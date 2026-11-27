// Hotkeys (hotkeys.h): save / load state and the slot, on combinations of controller buttons
// or keys held together.
//
// A combination fires when its last button goes down while the others are held; when one
// combination contains another (Back + RB and RB), only the larger fires. Controllers are read
// straight from SDL (any gamepad, like the settings menu). A modifier - a button of a
// combination that plays no GameCube control, Back by default - mutes the game's buttons and
// triggers while it is held, and so does a combination that fired until it is let go: Back + RB
// must not also press what RB plays in the game.
#include "hotkeys.h"

#include <aurora/binding.hpp>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gyro.h"
#include "hw/gx_fifo.h"
#include "mods.h"
#include "runtime.h"
#include "savestate.h"
#include "settings.h"

namespace rt::hotkeys {
namespace {

using Set = std::vector<int>;  // pad: SDL_GamepadButton, or kAxis + a trigger axis; keys: SDL_Scancode
constexpr int kAxis = 100;
constexpr int kDevices = 2;    // 0 controller, 1 keyboard
constexpr size_t kMaxItems = 4;

bool automated() {
    static const bool a = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    return a;
}

std::string& setting(int action, bool pad) {
    settings::Settings& s = settings::get();
    return pad ? s.hotkeyPad[action] : s.hotkeyKeys[action];
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t sep = s.find(" + ", start);
        std::string part = s.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        while (!part.empty() && part.back() == ' ') part.pop_back();
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        if (!part.empty()) out.push_back(part);
        if (sep == std::string::npos) break;
        start = sep + 3;
    }
    return out;
}

Set parse(const std::string& s, bool pad) {
    Set out;
    for (const std::string& name : split(s)) {
        int item = -1;
        if (pad) {
            const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name.c_str());
            if (b != SDL_GAMEPAD_BUTTON_INVALID) item = int(b);
            else if (const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name.c_str());
                     a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
                item = kAxis + int(a);
        } else {
            const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc != SDL_SCANCODE_UNKNOWN) item = int(sc);
        }
        if (item < 0) return {};  // a name this SDL does not know: the whole combination is off
        if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
    }
    return out;
}

std::string itemName(int item, bool pad) {
    if (!pad) return SDL_GetScancodeName(SDL_Scancode(item));
    const char* n = item >= kAxis ? SDL_GetGamepadStringForAxis(SDL_GamepadAxis(item - kAxis))
                                  : SDL_GetGamepadStringForButton(SDL_GamepadButton(item));
    return n ? n : "";
}

bool contains(const Set& held, const Set& combo) {
    for (int x : combo)
        if (std::find(held.begin(), held.end(), x) == held.end()) return false;
    return true;
}

Set heldPad() {
    static Set last;  // while the input thread updates the controllers (a device scan can take seconds)
    input::JoystickAccess joy;
    if (!joy) return last;
    Set s;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return last = s;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g, SDL_GamepadButton(b))) s.push_back(b);
        for (SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
            if (SDL_GetGamepadAxis(g, a) > 16000) s.push_back(kAxis + int(a));
    }
    SDL_free(ids);
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    return last = s;
}

Set heldKeys() {
    Set s;
    int n = 0;
    const bool* k = SDL_GetKeyboardState(&n);
    for (int i = 0; k && i < n; i++)
        if (k[i]) s.push_back(i);
    return s;
}

// parsed combinations, refreshed when the settings change
struct Parsed {
    std::string text;
    Set combo;
};
Parsed s_parsed[settings::HkCount][kDevices];
const Set& combo(int action, int device) {
    Parsed& p = s_parsed[action][device];
    const std::string& t = setting(action, device == 0);
    if (p.text != t || (p.combo.empty() && !t.empty())) {
        p.text = t;
        p.combo = parse(t, device == 0);
    }
    return p.combo;
}

bool s_was[settings::HkCount][kDevices] = {};
Set s_modifiers;        // controller buttons that only serve as hotkey modifiers
Set s_mutedUntilUp;     // a combination that fired: muted until all of it is up
bool s_mute = false;
const void* s_bindingsSeen = nullptr;
std::string s_modifierKey;  // the combinations the modifiers were computed for

// Modifiers: the buttons of multi-button combinations that play nothing in the game (port 1).
void refreshModifiers() {
    const auto set = aurora::pad::binding_set(0);
    std::string key;
    for (int a = 0; a < settings::HkCount; a++) key += combo(a, 0).size() > 1 ? setting(a, true) + ";" : ";";
    if (set.get() == s_bindingsSeen && key == s_modifierKey) return;
    s_bindingsSeen = set.get();
    s_modifierKey = key;
    Set bound;
    if (set) {
        using aurora::binding::PhysicalInput;
        // GameCube controls only (Aurora also binds Back, Guide, ... to extended buttons the game
        // never sees)
        const auto& c = aurora::pad::controls();
        const aurora::binding::ControlId gc[] = {c.a,  c.b,    c.x,    c.y,     c.z,     c.start, c.l,      c.r,       c.up,
                                                 c.down, c.left, c.right, c.leftX, c.leftY, c.rightX, c.rightY, c.triggerL, c.triggerR};
        for (const auto& b : set->bindings) {
            if (std::find(std::begin(gc), std::end(gc), b.target) == std::end(gc)) continue;
            if (const auto* g = b.input.control.get_if<PhysicalInput::GamepadButton>()) bound.push_back(int(g->button));
            if (const auto* x = b.input.control.get_if<PhysicalInput::GamepadAxis>()) bound.push_back(kAxis + int(x->axis));
        }
    }
    s_modifiers.clear();
    for (int a = 0; a < settings::HkCount; a++) {
        const Set& c = combo(a, 0);
        if (c.size() < 2) continue;
        for (int x : c)
            if (std::find(bound.begin(), bound.end(), x) == bound.end() &&
                std::find(s_modifiers.begin(), s_modifiers.end(), x) == s_modifiers.end())
                s_modifiers.push_back(x);
    }
    std::string names;
    for (int x : s_modifiers) names += " " + itemName(x, true);
    logf("hotkey", "modifiers (the game's buttons are muted while held):%s", names.empty() ? " none" : names.c_str());
}

void fire(int action) {
    settings::Settings& s = settings::get();
    switch (action) {
    case settings::HkSaveState: state::request(state::Op::Save, s.stateSlot); break;
    case settings::HkLoadState: state::request(state::Op::Load, s.stateSlot); break;
    case settings::HkNextSlot:
    case settings::HkPrevSlot: {
        s.stateSlot = action == settings::HkNextSlot ? s.stateSlot % state::kSlots + 1
                                                     : (s.stateSlot + state::kSlots - 2) % state::kSlots + 1;
        settings::save();
        const std::string info = state::slotInfo(s.stateSlot);
        char buf[64];
        snprintf(buf, sizeof buf, "SLOT %d  %s", s.stateSlot, info.empty() ? "(EMPTY)" : info.c_str());
        gfx::toast(buf);
        break;
    }
    case settings::HkToggleTextures: gfx::toast(mods::toggleTexturesShown()); break;
    case settings::HkCommentary: commentaryToggle(); break;  // (hle_game.cpp, at the game's next frame)
    default: break;
    }
}

struct Capture {
    bool active = false;
    int action = -1;
    bool pad = true;
    bool armed = false;  // everything was let go once (the button that started it)
    Set best, prev;
    u64 startTb = 0;
} s_cap;

// R2_HOTKEY_TEST=<seconds> (automated runs, with R2_BG_PADS=1): from then on a virtual gamepad
// holds Back + RB (save), Back + LB (load), Back + d-pad right (next slot), then taps Back alone
// twice (the menu opens and closes on its release); logs what the game's port 1 saw meanwhile.
void selfTest() {
    static const double start = getenv("R2_HOTKEY_TEST") ? atof(getenv("R2_HOTKEY_TEST")) : -1.0;
    if (start < 0) return;
    static const u64 t0 = tbNow();
    const double t = double(tbNow() - t0) / double(kTbClock) - start;
    if (t < 0) return;
    static SDL_Joystick* joy = nullptr;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two hotkey test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("hotkey", "test: virtual gamepad %s", joy ? "attached" : SDL_GetError());
        if (!joy) return;
    }
    struct Step {
        double at;
        SDL_GamepadButton button;
        bool down;
        const char* what;
    };
    // R2_HOTKEY_TEST_BIND=1 (with R2_MENU=1 R2_MENU_SECTION=4 R2_MENU_PAD=1 R2_MENU_ROW=14): A on
    // the save state row, then Back + left trigger: the new combination
    static const bool bind = envFlag("R2_HOTKEY_TEST_BIND");
    if (bind) {
        static int stage = 0;
        auto at = [&](double when) { return t >= when && stage == int(when * 10); };
        if (stage == 0 && t >= 1.0) {
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
            stage = 12;
        } else if (at(1.2)) {
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
            stage = 25;
        } else if (at(2.5)) {
            logf("hotkey", "test: capturing %d (action %d)", int(capturing()), captureAction());
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_BACK, true);
            stage = 28;
        } else if (at(2.8)) {
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
            stage = 32;
        } else if (at(3.2)) {
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
            stage = 34;
        } else if (at(3.4)) {
            SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_BACK, false);
            stage = 40;
        } else if (at(4.0)) {
            logf("hotkey", "test: capturing %d, save state is now '%s' (%s), menu open %d", int(capturing()),
                 settings::get().hotkeyPad[settings::HkSaveState].c_str(), label(settings::HkSaveState, true).c_str(),
                 int(gfx::menuOpen()));
            stage = 99;
        }
        return;
    }
    static const Step steps[] = {
        {2.0, SDL_GAMEPAD_BUTTON_BACK, true, "back down"},           {2.3, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true, "rb down"},
        {2.6, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false, "rb up"},    {2.8, SDL_GAMEPAD_BUTTON_BACK, false, "back up"},
        {7.0, SDL_GAMEPAD_BUTTON_BACK, true, "back down"},           {7.3, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true, "lb down"},
        {7.6, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false, "lb up"},     {7.8, SDL_GAMEPAD_BUTTON_BACK, false, "back up"},
        {10.0, SDL_GAMEPAD_BUTTON_BACK, true, "back down"},          {10.3, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true, "right down"},
        {10.6, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false, "right up"},    {10.8, SDL_GAMEPAD_BUTTON_BACK, false, "back up"},
        {13.0, SDL_GAMEPAD_BUTTON_BACK, true, "back alone down"},    {13.2, SDL_GAMEPAD_BUTTON_BACK, false, "back alone up"},
        {16.0, SDL_GAMEPAD_BUTTON_BACK, true, "back alone down"},    {16.2, SDL_GAMEPAD_BUTTON_BACK, false, "back alone up"},
    };
    static size_t next = 0;
    while (next < std::size(steps) && t >= steps[next].at) {
        const Step& s = steps[next++];
        SDL_SetJoystickVirtualButton(joy, s.button, s.down);
        logf("hotkey", "test: %s (menu %d, game muted %d, port 1 buttons %04X)", s.what, int(gfx::menuOpen()), int(s_mute),
             padState(0).buttons);
    }
}

}  // namespace

void tick() {
    static const bool test = envFlag("R2_HOTKEY_TEST");
    // R2_TEXTURE_TOGGLE_TEST=<seconds>: the texture pack hotkey's action once, then (automated
    // runs take no hotkeys)
    if (static const char* tt = getenv("R2_TEXTURE_TOGGLE_TEST"); tt) {
        static const u64 t0 = tbNow();
        static bool done = false;
        if (!done && double(tbNow() - t0) / double(kTbClock) >= atof(tt)) {
            done = true;
            logf("hotkey", "texture toggle test");
            fire(settings::HkToggleTextures);
        }
    }
    // R2_COMMENTARY_TEST=<seconds>[,<seconds>...]: the commentary hotkey's action at those times
    if (static const char* ct = getenv("R2_COMMENTARY_TEST"); ct) {
        static const u64 t0 = tbNow();
        static const char* next = ct;
        if (next && *next && double(tbNow() - t0) / double(kTbClock) >= atof(next)) {
            logf("hotkey", "commentary test");
            fire(settings::HkCommentary);
            const char* comma = strchr(next, ',');
            next = comma ? comma + 1 : nullptr;
        }
    }
    selfTest();
    refreshModifiers();
    const bool enabled = !gfx::menuOpen() && !s_cap.active && (!automated() || test);
    const Set held[kDevices] = {enabled ? heldPad() : Set{}, enabled ? heldKeys() : Set{}};
    bool sat[settings::HkCount][kDevices] = {};
    for (int d = 0; d < kDevices; d++)
        for (int a = 0; a < settings::HkCount; a++) sat[a][d] = !combo(a, d).empty() && contains(held[d], combo(a, d));
    for (int d = 0; d < kDevices; d++)
        for (int a = 0; a < settings::HkCount; a++) {
            if (!sat[a][d] || s_was[a][d]) continue;
            bool covered = false;  // a larger combination holding this one is down too: that one counts
            for (int o = 0; o < settings::HkCount && !covered; o++)
                covered = o != a && sat[o][d] && combo(o, d).size() > combo(a, d).size() && contains(combo(o, d), combo(a, d));
            if (covered) continue;
            // the commentary's L3 is also the gyro button of a controller without stick touch or a
            // touchpad (a Switch Pro Controller): with gyro aiming on, the gyro keeps it
            if (a == settings::HkCommentary && d == 0 && settings::get().gyroMode != settings::GyroMode::Off) {
                const int g = gyro::effectiveButton();
                if (g >= 0 && combo(a, d).size() == 1 && combo(a, d)[0] == SDL_GetGamepadButtonFromString(gyro::kButtons[g].sdlName))
                    continue;
            }
            logf("hotkey", "%s: %s", label(a, d == 0).c_str(),
                 a == settings::HkSaveState         ? "save state"
                 : a == settings::HkLoadState      ? "load state"
                 : a == settings::HkToggleTextures ? "texture packs"
                 : a == settings::HkCommentary     ? "audio commentary"
                                                   : "slot");
            fire(a);
            if (d == 0) s_mutedUntilUp = combo(a, d);
        }
    for (int d = 0; d < kDevices; d++)
        for (int a = 0; a < settings::HkCount; a++) s_was[a][d] = sat[a][d];
    // the game's buttons: muted while a modifier is down, or a fired combination is not all up
    bool any = false;
    for (int x : s_mutedUntilUp) any |= std::find(held[0].begin(), held[0].end(), x) != held[0].end();
    if (!any) s_mutedUntilUp.clear();
    bool mod = false;
    for (int x : s_modifiers) mod |= std::find(held[0].begin(), held[0].end(), x) != held[0].end();
    s_mute = mod || !s_mutedUntilUp.empty();
}

bool muteGame() { return s_mute; }

std::string label(int action, bool pad) {
    const Set& c = combo(action, pad ? 0 : 1);
    if (c.empty()) return "-";
    SDL_Gamepad* g = nullptr;
    if (pad)
        if (const auto src = aurora::pad::port_controller(0)) g = SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src));
    std::string out;
    using aurora::binding::PhysicalInput;
    for (int x : c) {
        if (!out.empty()) out += " + ";
        PhysicalInput in;
        if (!pad) in.control = PhysicalInput::Key{SDL_Scancode(x)};
        else if (x >= kAxis) in.control = PhysicalInput::GamepadAxis{SDL_GamepadAxis(x - kAxis), PhysicalInput::GamepadAxis::Direction::Positive};
        else in.control = PhysicalInput::GamepadButton{SDL_GamepadButton(x)};
        out += aurora::binding::input_label(in, g);
    }
    return out;
}

void captureStart(int action, bool pad) {
    s_cap = Capture{};
    s_cap.active = true;
    s_cap.action = action;
    s_cap.pad = pad;
    s_cap.startTb = tbNow();
}

bool capturing() { return s_cap.active; }
int captureAction() { return s_cap.active ? s_cap.action : -1; }
void captureCancel() { s_cap = Capture{}; }

void captureTick() {
    if (!s_cap.active) return;
    Set held = s_cap.pad ? heldPad() : heldKeys();
    if (!s_cap.pad) {
        if (std::find(held.begin(), held.end(), int(SDL_SCANCODE_ESCAPE)) != held.end()) return captureCancel();
        std::erase(held, int(SDL_SCANCODE_F1));  // (the menu's own key)
    }
    if (tbNow() - s_cap.startTb > 10 * kTbClock) return captureCancel();
    if (!s_cap.armed) {
        if (held.empty()) s_cap.armed = true;
        return;
    }
    bool released = false;
    for (int x : s_cap.prev) released |= std::find(held.begin(), held.end(), x) == held.end();
    if (released && !s_cap.best.empty()) {
        std::string text;
        for (int x : s_cap.best) text += (text.empty() ? "" : " + ") + itemName(x, s_cap.pad);
        setting(s_cap.action, s_cap.pad) = text;
        settings::save();
        logf("hotkey", "%s bound to %s", s_cap.pad ? "controller" : "keyboard", text.c_str());
        s_cap = Capture{};
        return;
    }
    for (int x : held)  // in the order they went down
        if (s_cap.best.size() < kMaxItems && std::find(s_cap.best.begin(), s_cap.best.end(), x) == s_cap.best.end())
            s_cap.best.push_back(x);
    s_cap.prev = held;
}

void clear(int action, bool pad) {
    setting(action, pad).clear();
    settings::save();
}

void resetDefaults(bool pad) {
    for (int a = 0; a < settings::HkCount; a++) setting(a, pad) = settings::hotkeyDefault(a, pad);
    settings::save();
}

}  // namespace rt::hotkeys
