// Controllers: Aurora reads gamepads (SDL3, GameCube adapters) and the keyboard; the result
// feeds the SI model (hw/hw.cpp), which answers the game's own controller polling.
#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <dolphin/pad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gyro.h"
#include "hw/gx_fifo.h"
#include "hotkeys.h"
#include "runtime.h"
#include "settings.h"

namespace rt::input {

namespace {
bool s_ready = false;

// R2_INPUT="<vblank>:<BUTTON>[*frames],...": scripted presses for automated runs.
struct Press {
    u64 start, frames;
    u32 button;  // GameCube buttons; kStick* / kTrig* above them
    u8 value = 0;  // kTrig*: the analog trigger value
};
// script names SUP / SDOWN / SLEFT / SRIGHT: the main stick pushed fully that way; LT<n> / RT<n>:
// the left / right trigger's analog value n (0-255) without the digital L / R press
constexpr u32 kStickUp = 1u << 16, kStickDown = 1u << 17, kStickLeft = 1u << 18, kStickRight = 1u << 19;
constexpr u32 kTrigL = 1u << 20, kTrigR = 1u << 21;
constexpr u32 kCUp = 1u << 22, kCDown = 1u << 23;  // CUP / CDOWN: the C-stick
std::vector<Press> s_script;
u64 s_vblank = 0;

void parseScript(const char* s) {
    static const struct {
        const char* name;
        u32 bit;
    } names[] = {{"LEFT", 0x0001},      {"RIGHT", 0x0002},     {"DOWN", 0x0004},      {"UP", 0x0008},
                 {"Z", 0x0010},         {"R", 0x0020},         {"L", 0x0040},         {"A", 0x0100},
                 {"B", 0x0200},         {"X", 0x0400},         {"Y", 0x0800},         {"START", 0x1000},
                 {"SUP", kStickUp},     {"SDOWN", kStickDown}, {"SLEFT", kStickLeft}, {"SRIGHT", kStickRight},
                 {"CUP", kCUp},         {"CDOWN", kCDown}};
    while (*s) {
        char* end;
        const u64 start = strtoull(s, &end, 10);
        if (*end != ':') break;
        s = end + 1;
        std::string name;
        while (*s && *s != '*' && *s != ',') name += *s++;
        u64 frames = 6;
        if (*s == '*') frames = strtoull(s + 1, const_cast<char**>(&s), 10);
        for (auto& n : names)
            if (name == n.name) s_script.push_back({start, frames, n.bit});
        if (name.size() > 2 && (name[0] == 'L' || name[0] == 'R') && name[1] == 'T' && isdigit(u8(name[2])))
            s_script.push_back({start, frames, name[0] == 'L' ? kTrigL : kTrigR, u8(std::min(255, atoi(name.c_str() + 2)))});
        while (*s == ',') s++;
    }
    logf("input", "%zu scripted presses", s_script.size());
}

void defaultKeyboard() {
    using aurora::binding::Binding;
    using aurora::binding::PhysicalInput;
    const auto& c = aurora::pad::controls();
    std::vector<Binding> keys;
    auto key = [&](aurora::binding::ControlId target, SDL_Scancode k, float scale = 1.f) {
        keys.push_back({.input = {.control = PhysicalInput::Key{k}}, .target = target, .scale = scale});
    };
    // Flight: WASD or the arrow keys; C-stick: IJKL.
    key(c.leftX, SDL_SCANCODE_A, -1.f);
    key(c.leftX, SDL_SCANCODE_D, 1.f);
    key(c.leftY, SDL_SCANCODE_W, 1.f);
    key(c.leftY, SDL_SCANCODE_S, -1.f);
    key(c.leftX, SDL_SCANCODE_LEFT, -1.f);
    key(c.leftX, SDL_SCANCODE_RIGHT, 1.f);
    key(c.leftY, SDL_SCANCODE_UP, 1.f);
    key(c.leftY, SDL_SCANCODE_DOWN, -1.f);
    key(c.rightX, SDL_SCANCODE_J, -1.f);
    key(c.rightX, SDL_SCANCODE_L, 1.f);
    key(c.rightY, SDL_SCANCODE_I, 1.f);
    key(c.rightY, SDL_SCANCODE_K, -1.f);
    key(c.a, SDL_SCANCODE_SPACE);
    key(c.b, SDL_SCANCODE_LSHIFT);
    key(c.x, SDL_SCANCODE_E);
    key(c.y, SDL_SCANCODE_Q);
    key(c.z, SDL_SCANCODE_R);
    key(c.l, SDL_SCANCODE_1);
    key(c.triggerL, SDL_SCANCODE_1);
    key(c.r, SDL_SCANCODE_2);
    key(c.triggerR, SDL_SCANCODE_2);
    key(c.start, SDL_SCANCODE_RETURN);
    key(c.up, SDL_SCANCODE_T);
    key(c.down, SDL_SCANCODE_G);
    key(c.left, SDL_SCANCODE_F);
    key(c.right, SDL_SCANCODE_H);
    aurora::pad::set_default_keyboard_bindings(0, std::move(keys));
}

// --- controller updates off the game thread ---------------------------------------------------
// SDL rescans its controllers inside SDL_UpdateJoysticks when Windows reports a device change:
// DirectInput, SDL's HIDAPI driver and its raw input driver each open every HID device and ask
// it for its names (on the user's machine 52 interfaces, ~0.5 s a pass: a Steam Controller
// puck's 13 at 18 ms each, wireless receivers, RGB controllers), holding SDL's joystick lock.
// SDL's event pump ran that on the game's thread: frames of 3-4 s at random moments, mid-cutscene
// too (Battle of Hoth; the profile showed hid.dll under dinput8.dll and SDL3.dll). The input
// thread now updates the controllers (SDL_HINT_AUTO_UPDATE_JOYSTICKS is off, gx_bridge.cpp)
// under s_joyMutex, and the game thread's own SDL controller calls (pads, rumble, hotkeys, menu)
// only try that mutex (JoystickAccess): while a scan runs they keep the previous state. Aurora's
// controller state itself comes from SDL events, which the window's event pump still delivers.
std::mutex s_joyMutex;

void joystickThread() {
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -20000;  // 2 ms (100 ns units), then every 2 ms: 500 Hz
    SetWaitableTimer(timer, &due, 2, nullptr, nullptr, FALSE);
#else
    auto next = std::chrono::steady_clock::now();
#endif
    u64 lastLog = 0;
    for (;;) {
#ifdef _WIN32
        WaitForSingleObject(timer, INFINITE);
#else
        next += std::chrono::milliseconds(2);  // 500 Hz
        std::this_thread::sleep_until(next);
#endif
        // A controller came or went: Aurora opens or closes it on the game thread (SDL calls that
        // wait for the joystick lock), so no new scan starts before it has.
        if (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMAPPED) > 0) continue;
        const u64 t0 = tbNow();
        {
            std::lock_guard lock(s_joyMutex);
            SDL_UpdateJoysticks();
        }
        const u64 t1 = tbNow();
        if (t1 - t0 > kTbClock / 10 && t1 - lastLog > kTbClock) {
            lastLog = t1;
            logf("input", "controller scan: %.0f ms on the input thread (the game kept running)",
                 double(t1 - t0) * 1000.0 / double(kTbClock));
        }
    }
}

// R2_JOY_SCAN_TEST=<seconds>: from then on, every 5 s the input thread holds SDL's joystick lock
// for 3 s, as a slow device scan does (the game thread must keep its frame rate)
void scanTest() {
    const char* e = getenv("R2_JOY_SCAN_TEST");
    if (!e) return;
    const double start = atof(e);
    std::thread([start] {
        std::this_thread::sleep_for(std::chrono::duration<double>(start));
        for (int i = 0; i < 6; i++) {
            {
                std::lock_guard lock(s_joyMutex);
                SDL_LockJoysticks();
                logf("input", "scan test: joystick lock held for 3 s");
                std::this_thread::sleep_for(std::chrono::seconds(3));
                SDL_UnlockJoysticks();
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }).detach();
}

PADStatus s_lastPads[PAD_CHANMAX] = {};  // what PADRead gave last (reused while a scan runs)
u32 s_rumbleWant[PAD_CHANMAX] = {}, s_rumbleSent[PAD_CHANMAX] = {};

void applyRumble() {  // JoystickAccess held
    for (u32 i = 0; i < PAD_CHANMAX; i++)
        if (s_rumbleWant[i] != s_rumbleSent[i]) {
            s_rumbleSent[i] = s_rumbleWant[i];
            PADControlMotor(i, s_rumbleWant[i]);
        }
}

// Port 1 follows the controller in use: one on no port that has a button pressed (or a trigger
// or stick pushed most of the way) takes port 1 over. Aurora's automatic assignment gives port 1
// to the first controller it saw, and the order in which SDL finds them (XInput, Bluetooth, USB
// receivers, Steam's virtual pads) is arbitrary: the user's 8BitDo on its USB receiver did nothing
// next to an Elite 2. The takeover pins port 1 to that controller in Aurora; a pin left in
// input.json (the menu saves the profiles) is undone at the next start, and a pin whose
// controller went away goes back to automatic.
void followActiveController() {  // JoystickAccess held
    const aurora::input::SourceId current = aurora::pad::port_controller(0);
    if (!current && aurora::pad::port_preference(0).state == aurora::pad::PortAssignment::Controller) aurora::pad::set_port_auto(0);
    static std::vector<std::pair<SDL_JoystickID, bool>> s_active;  // per gamepad: in use at the last poll
    decltype(s_active) now;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        const aurora::input::SourceId src = g ? aurora::input::source_for_gamepad(ids[i]) : 0;
        if (!src) continue;
        bool active = false;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !active; b++) active = SDL_GetGamepadButton(g, SDL_GamepadButton(b));
        for (SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
            active |= SDL_GetGamepadAxis(g, a) > 16000;
        for (SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY})
            active |= std::abs(int(SDL_GetGamepadAxis(g, a))) > 24000;
        // a controller seen for the first time counts as in use: one connecting with a button held
        // (or a stick resting off-centre) takes nothing over until it is let go and used again
        bool was = true;
        for (const auto& [id, act] : s_active)
            if (id == ids[i]) was = act;
        now.emplace_back(ids[i], active);
        if (!active || was || src == current) continue;
        bool otherPort = false;
        for (u32 p = 1; p < 4; p++) otherPort |= aurora::pad::port_controller(p) == src;
        if (otherPort || !aurora::pad::set_port_controller(0, src)) continue;
        logf("input", "port 1: '%s' (vid %04x, pid %04x) took over, a button was pressed", SDL_GetGamepadName(g),
             SDL_GetGamepadVendor(g), SDL_GetGamepadProduct(g));
    }
    SDL_free(ids);
    s_active = std::move(now);
}

bool s_port1PlayStation = false;  // port 1's controller is a PlayStation one (button prompts)

// the session log says which controller port 1 has whenever that changes (automatic or taken over)
void logPort1() {  // JoystickAccess held
    static aurora::input::SourceId s_logged = 0;
    const auto src = aurora::pad::port_controller(0);
    if (src == s_logged) return;
    s_logged = src;
    SDL_Gamepad* g = src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
    const SDL_GamepadType type = g ? SDL_GetGamepadType(g) : SDL_GAMEPAD_TYPE_UNKNOWN;
    s_port1PlayStation = isPlayStation(type);
    const char* typeName = SDL_GetGamepadStringForType(type);
    if (g)
        logf("input", "port 1 controller: '%s' (vid %04x, pid %04x, type %s)", SDL_GetGamepadName(g), SDL_GetGamepadVendor(g),
             SDL_GetGamepadProduct(g), typeName ? typeName : "standard");
    else
        logf("input", "port 1 controller: none (keyboard only)");
}

// Aurora's stick deadzone works on each axis alone and cuts without rescaling: with its default
// (24%) an axis jumped from 0 to a quarter of its travel as the stick left the deadzone, and
// small aiming moves snapped to the axes ("jittery, not smooth": issue #9). It is turned off for
// every controller (the profile's useDeadzones, once per controller and session) and shapeStick
// applies the settings instead.
// Triggers: Aurora presses the digital L / R (the GameCube trigger's click) at the profile's
// trigger threshold (95% of the travel). A trigger rebound in the Controls page before 1.1 bound
// the click to the trigger's axis at the default threshold (0.5): full boost / brake from half
// travel on ("only full-press boosts and full brakes": issue #6). Such bindings get the
// profile's threshold here; the menu binds new ones with it (menu.cpp bindRow).
// s_curveTriggers: the port's controller has emulated triggers (not a GameCube adapter's).
bool s_curveTriggers[4] = {};
std::vector<aurora::input::SourceId> s_setUp;  // controllers set up this session
// --- the triggers' clicks on the bumpers (settings bumper_clicks) ---------------------------------
// What players set up in Dolphin for this game (issue #6): a trigger pulled all the way clicks -
// full boost, which closes the S-foils (no firing), or full brake - easy to do by accident on a
// trigger with no click to feel. With the option the triggers are analog only, over their whole
// travel (curveTrigger), and LB / RB give the clicks (Aurora reads a digital L / R as the full
// press, 180); Z, on RB by default (a chase camera while held), moves to the right stick click.
// Done in the controller's bindings, so the Controls page shows them, and undone when the option
// goes off unless the player changed them since. L / R without bindings of their own are read
// from the triggers past the profile's threshold (Aurora's emulated triggers).
using aurora::binding::Binding;
using PhysIn = aurora::binding::PhysicalInput;
bool s_bumperClicks[4];  // per port: the clicks are on the bumpers (the triggers' whole travel is analog)

Binding buttonBinding(SDL_GamepadButton b) {
    Binding x;
    x.input.control = PhysIn::GamepadButton{b};
    return x;
}
bool onlyButton(const std::vector<Binding>& list, SDL_GamepadButton b) {
    if (list.size() != 1) return false;
    const auto* gb = list[0].input.control.get_if<PhysIn::GamepadButton>();
    return gb && gb->button == b;
}

void placeClicks(aurora::pad::ProfileTarget target, bool bumpers, u32 port) {
    const auto& c = aurora::pad::controls();
    const auto l = aurora::pad::bindings_for_control(target, c.l), r = aurora::pad::bindings_for_control(target, c.r);
    auto z = aurora::pad::bindings_for_control(target, c.z);
    if (bumpers) {
        if (onlyButton(l, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) && onlyButton(r, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) return;
        aurora::pad::set_binding(target, c.l, {buttonBinding(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)});
        aurora::pad::set_binding(target, c.r, {buttonBinding(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});
        const auto onRb = [](const Binding& b) {
            const auto* gb = b.input.control.get_if<PhysIn::GamepadButton>();
            return gb && gb->button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        };
        if (std::erase_if(z, onRb)) {
            if (z.empty()) z.push_back(buttonBinding(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
            aurora::pad::set_binding(target, c.z, std::move(z));
        }
        logf("input", "port %u: the triggers' clicks on LB / RB, Z on the right stick click", port + 1);
    } else if (onlyButton(l, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) && onlyButton(r, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
        aurora::pad::set_binding(target, c.l, {});  // read from the triggers again
        aurora::pad::set_binding(target, c.r, {});
        if (onlyButton(z, SDL_GAMEPAD_BUTTON_RIGHT_STICK))
            aurora::pad::set_binding(target, c.z, {buttonBinding(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)});
        logf("input", "port %u: the triggers' clicks back on the triggers, Z on RB", port + 1);
    }
}

void setUpControllers() {  // JoystickAccess held
    auto& s_done = s_setUp;
    // the option changed: every controller is set up again
    static int s_bumpersApplied = -1;
    const bool bumpers = settings::get().bumperClicks;
    if (int(bumpers) != s_bumpersApplied) {
        s_bumpersApplied = int(bumpers);
        s_done.clear();
    }
    for (u32 port = 0; port < 4; port++) {
        const auto src = aurora::pad::port_controller(port);
        const auto target = aurora::pad::ProfileTarget::controller(src);
        const aurora::pad::Profile* prof = src ? aurora::pad::profile(target) : nullptr;
        s_curveTriggers[port] = prof && prof->settings.emulateTriggers;
        s_bumperClicks[port] = s_curveTriggers[port] && bumpers;
        if (!src || std::find(s_done.begin(), s_done.end(), src) != s_done.end()) continue;
        s_done.push_back(src);
        aurora::pad::ProfileSettings s = prof ? prof->settings : aurora::pad::ProfileSettings{};
        if (s.useDeadzones) {
            s.useDeadzones = false;
            if (!aurora::pad::set_settings(target, s)) logf("input", "port %u: Aurora's stick deadzone could not be turned off", port + 1);
        }
        const auto& c = aurora::pad::controls();
        for (const bool left : {true, false}) {
            const float threshold = left ? s.leftTriggerThreshold : s.rightTriggerThreshold;
            auto list = aurora::pad::bindings_for_control(target, left ? c.l : c.r);
            bool changed = false;
            for (auto& b : list)
                if (const auto* ax = b.input.control.get_if<aurora::binding::PhysicalInput::GamepadAxis>();
                    ax && (ax->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ax->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                    b.threshold < threshold) {
                    b.threshold = threshold;
                    changed = true;
                }
            if (changed && aurora::pad::set_binding(target, left ? c.l : c.r, std::move(list)))
                logf("input", "port %u: the %s trigger's click moved to %.0f%% of its travel (was a rebind at half)", port + 1,
                     left ? "left" : "right", threshold * 100.f);
        }
        if (s.emulateTriggers) placeClicks(target, bumpers, port);
    }
}

// A controller's analog trigger spread over its travel up to the click. The game takes a trigger
// through the SDK's PADClamp (values up to 30 read 0, 170 and more full: (v - 30) / 140, measured
// in flight) as an analog throttle and brake (0x7FC31D30: half a trigger = half throttle, the
// click adds the boost / full brake). Aurora reports the travel as 0-255, so the throttle was
// full at 67% and the last quarter before the click did nothing; a GameCube trigger builds up
// until it clicks. Now 4% of the travel reads 0 and the click's threshold full (170) - or the
// whole travel when the clicks are on the bumpers (wholeTravel).
void curveTrigger(u8& v, bool clicked, bool wholeTravel = false) {
    if (clicked) return;  // Aurora's full press with the click (180)
    constexpr float kRest = 0.04f, kClick = 31151.f / 32767.f;  // Aurora's trigger threshold
    const float t = float(v) / 255.f, full = wholeTravel ? 1.f : kClick;
    v = t <= kRest ? 0 : u8(std::min(170l, std::lround(30.f + (t - kRest) / (full - kRest) * 140.f)));
}

// R2_TRIGGER_TEST=<seconds> (automated runs, with R2_BG_PADS=1): a virtual gamepad takes port 1
// with a full right trigger, then holds it at several depths and logs what port 1 gets; then
// the right trigger's click is bound at half travel (a rebind from before 1.1) and set up again:
// at 60% the click must stay off.
void triggerTest(const PADStatus& p1) {  // JoystickAccess held
    static const double start = getenv("R2_TRIGGER_TEST") ? atof(getenv("R2_TRIGGER_TEST")) : -1.0;
    if (start < 0) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - start;
    static SDL_Joystick* joy = nullptr;
    static SDL_JoystickID id = 0;
    static int step = -1;
    constexpr int kSteps = 14, kRebind = 9;
    if (t < 0 || step >= kSteps) return;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two trigger test pad";
        id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("input", "trigger test: virtual gamepad %s", joy ? "attached" : SDL_GetError());
        if (!joy) step = 99;
        return;
    }
    // every 0.5 s from 1 s on: set the trigger, and log what port 1 got from the previous one
    static const float depth[kSteps] = {1.f, 0.f, 0.02f, 0.25f, 0.5f, 0.75f, 0.9f, 0.94f, 0.97f, 0.f, 0.f, 0.6f, 0.97f, 0.f};
    const int now = int((t - 1.0) / 0.5);
    if (now <= step || now < 0) return;
    if (step >= 0)
        logf("input", "trigger test: right trigger at %3.0f%%: port 1 trigger R %3u, R click %d", depth[step] * 100.f,
             p1.triggerRight, int((p1.button & PAD_TRIGGER_R) != 0));
    step = now;
    if (step >= kSteps) return;
    if (step == kRebind) {  // an old rebind: the click on the trigger's axis at half travel
        const auto src = aurora::pad::port_controller(0);
        const auto target = aurora::pad::ProfileTarget::controller(src);
        aurora::binding::Binding b;
        b.input.control = aurora::binding::PhysicalInput::GamepadAxis{SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                                                       aurora::binding::PhysicalInput::GamepadAxis::Direction::Positive};
        const bool ok = aurora::pad::set_binding(target, aurora::pad::controls().r, {b});
        std::erase(s_setUp, src);  // set up again at the next poll
        logf("input", "trigger test: R click bound at half travel (%s), controller %llu", ok ? "ok" : "failed",
             (unsigned long long)src);
    }
    // (a virtual joystick's trigger axis spans -32768 (released) to 32767)
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, Sint16(std::lround(-32768.0 + depth[step] * 65535.0)));
}

// R2_BUMPER_TEST=<seconds> (automated runs, with R2_BG_PADS=1 and bumper_clicks=1): a virtual
// gamepad takes port 1; every 0.5 s from 1 s on one step (the right trigger, LB, RB, the right stick
// click), port 1's L / R / Z and triggers logged for each; halfway the option goes off (the clicks
// back on the triggers, Z on RB) and the same steps run again.
void bumperTest(const PADStatus& p1) {  // JoystickAccess held
    static const double start = getenv("R2_BUMPER_TEST") ? atof(getenv("R2_BUMPER_TEST")) : -1.0;
    if (start < 0) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - start;
    static SDL_Joystick* joy = nullptr;
    static int step = -1;
    struct Step {
        const char* what;
        float rt;
        int button;  // SDL_GamepadButton held, -1 none
    };
    static const Step kSteps[] = {
        {"nothing", 0.f, -1},
        {"right trigger 50%", 0.5f, -1},
        {"right trigger 100%", 1.f, -1},
        {"RB", 0.f, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"LB", 0.f, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"right stick click", 0.f, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"nothing", 0.f, -1},
    };
    constexpr int kN = int(std::size(kSteps));
    if (t < 0 || step >= 2 * kN) return;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two bumper test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("input", "bumper test: virtual gamepad %s", joy ? "attached" : SDL_GetError());
        if (!joy) step = 99;
        return;
    }
    const int now = int((t - 1.0) / 0.5);
    if (now <= step || now < 0) return;
    if (step >= 0)
        logf("input", "bumper test (option %s): %-18s -> L %d R %d Z %d, triggers %3u %3u", step < kN ? "on" : "off",
             kSteps[step % kN].what, int((p1.button & PAD_TRIGGER_L) != 0), int((p1.button & PAD_TRIGGER_R) != 0),
             int((p1.button & PAD_TRIGGER_Z) != 0), p1.triggerLeft, p1.triggerRight);
    step = now;
    if (step >= 2 * kN) return;
    if (step == kN) {
        settings::get().bumperClicks = false;
        logf("input", "bumper test: option off");
    }
    const Step& s = kSteps[step % kN];
    for (int b : {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_STICK})
        SDL_SetJoystickVirtualButton(joy, b, b == s.button);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, Sint16(std::lround(-32768.0 + s.rt * 65535.0)));
}

// R2_STICK_TEST=<seconds> (automated runs, with R2_BG_PADS=1; R2_GYRO_LOG=1 logs the craft's
// turn rate meanwhile): a virtual gamepad takes port 1 and holds its left stick right at several
// deflections, 3 s each; what port 1 got and the turn rate expected are logged for each.
void stickTest(const PADStatus& p1) {  // JoystickAccess held
    static const double start = getenv("R2_STICK_TEST") ? atof(getenv("R2_STICK_TEST")) : -1.0;
    if (start < 0) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - start;
    static SDL_Joystick* joy = nullptr;
    static int step = -1;
    static const float kDefl[] = {0.f, 0.2f, 0.35f, 0.5f, 0.65f, 0.8f, 0.9f, 1.f, 0.f};
    constexpr int kN = int(std::size(kDefl));
    if (step >= kN) return;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two stick test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("input", "stick test: virtual gamepad %s", joy ? "attached" : SDL_GetError());
        if (!joy) step = 99;
        return;
    }
    if (t < 0) return;
    const int now = int(t / 3.0);
    if (now <= step) return;
    if (step >= 0) {
        s8 rx = 0, ry = 0;
        float rate = 0.f;
        mainStickToGame(kDefl[step], 0.f, rx, ry, &rate);
        logf("input", "stick test (%s): stick %3.0f%% -> port 1 x %d, turn rate expected %3.0f%%",
             settings::stickCurveName(settings::get().stickCurve), kDefl[step] * 100.f, p1.stickX, rate * 100.f);
    }
    step = now;
    if (step < kN) SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, Sint16(std::lround(kDefl[step] * 32767.f)));
}

}  // namespace

// --- the control stick as the game takes it (issues #9, #20: "small movements barely do anything,
// then a sudden jump") ---
// The game's PADClamp (the SDK's, ClampRegion r13-0x51C0) takes 15 units off each axis and limits
// the stick to an octagon of 72 (40 each on the diagonal), and in flight the craft turns and
// pitches at half the stick's fraction up to 80% of it, then three times as steep to its maximum
// (measured for gyro.cpp: X-wing, Death Star Attack, both axes). Aurora reports a controller's
// travel as -127..127: on top of the settings' deadzone the clamp's 15 left the first quarter of
// the travel dead, 87 was full at 73% of it, and the game's steep last fifth - 40% to 100% of the
// turn rate - was a tenth of the stick's travel. Now the travel past the deadzone is the game's
// range (15 put back in front, full at full), and the rate follows the stick by settings
// stick_curve: Smooth (deflection^1.5), Linear, or the game's own curve (GameCube).
constexpr int kStickDead = 15, kStickMax = 72, kStickXY = 40;
constexpr int kSubMax = 59, kSubXY = 31;  // the C-stick's (the SDK's ClampRegion, read from the game's copy)
float gameStickResponse(float g) {  // the craft's rate (fraction of its maximum) for a stick fraction
    const float a = std::min(std::fabs(g), 1.f);
    return std::copysign(a <= 0.8f ? a * 0.5f : 0.4f + (a - 0.8f) * 3.f, g);
}
float gameStickInverse(float r) {
    const float a = std::min(std::fabs(r), 1.f);
    return std::copysign(a <= 0.4f ? a * 2.f : 0.8f + (a - 0.4f) / 3.f, r);
}
void gameStickClamp(int& x, int& y, int max = kStickMax, int xy = kStickXY) {  // the SDK's ClampStick
    const int sx = x < 0 ? -1 : 1, sy = y < 0 ? -1 : 1;
    x = std::abs(x) <= kStickDead ? 0 : std::abs(x) - kStickDead;
    y = std::abs(y) <= kStickDead ? 0 : std::abs(y) - kStickDead;
    if (x == 0 && y == 0) return;
    const int d = y <= x ? xy * x + (max - xy) * y : xy * y + (max - xy) * x;
    if (xy * max < d) {
        x = xy * max * x / d;
        y = xy * max * y / d;
    }
    x *= sx;
    y *= sy;
}

// a fraction of a stick's range (-1..1) as a raw value: the clamp's 15 in front, full at full
s8 stickRaw(float v, int max) {
    const int u = int(std::lround(std::fabs(v) * float(max)));
    return u ? s8(v < 0 ? -(kStickDead + u) : kStickDead + u) : 0;
}

void cStickToGame(float x, float y, s8& rawX, s8& rawY, float* gameX, float* gameY) {
    shapeStick(x, y, false);  // its deadzone and sensitivity
    rawX = stickRaw(x, kSubMax);
    rawY = stickRaw(y, kSubMax);
    if (gameX || gameY) {
        int cx = rawX, cy = rawY;
        gameStickClamp(cx, cy, kSubMax, kSubXY);
        if (gameX) *gameX = float(cx) / float(kSubMax);
        if (gameY) *gameY = float(cy) / float(kSubMax);
    }
}

void mainStickToGame(float x, float y, s8& rawX, s8& rawY, float* rateX, float* rateY) {
    shapeStick(x, y, true);  // deadzone and sensitivity
    const float m = std::min(1.f, std::sqrt(x * x + y * y));
    float g[2] = {0.f, 0.f};
    if (m > 0.f) {
        const auto curve = settings::get().stickCurve;
        const float target = curve == settings::StickCurve::Smooth ? std::pow(m, 1.5f) : m;
        const float n = std::sqrt(x * x + y * y);
        const float t[2] = {x / n * target, y / n * target};
        for (int i = 0; i < 2; i++) g[i] = curve == settings::StickCurve::GameCube ? t[i] : gameStickInverse(t[i]);
    }
    rawX = stickRaw(g[0], kStickMax);
    rawY = stickRaw(g[1], kStickMax);
    if (rateX || rateY) {
        int cx = rawX, cy = rawY;
        gameStickClamp(cx, cy);
        if (rateX) *rateX = gameStickResponse(float(cx) / float(kStickMax));
        if (rateY) *rateY = gameStickResponse(float(cy) / float(kStickMax));
    }
}

// A round deadzone (settings stick_deadzone) the output rescales from, so it starts at 0 at the
// deadzone's edge and keeps its direction, then the control stick's sensitivity; x, y in -1..1
// (a keyboard's diagonal reaches past 1: it stays full on both axes).
void shapeStick(float& x, float& y, bool mainStick) {
    const auto& s = settings::get();
    const float dz = float(std::clamp(mainStick ? s.stickDeadzone : s.cStickDeadzone, 0, 50)) / 100.f;
    const float sens = float(std::clamp(mainStick ? s.stickSensitivity : s.cStickSensitivity, 25, 200)) / 100.f;
    const float m = std::sqrt(x * x + y * y);
    if (m <= dz || m <= 0.f) {
        x = y = 0.f;
        return;
    }
    const float k = (m - dz) / (1.f - dz) / m * sens;
    x = std::clamp(x * k, -1.f, 1.f);
    y = std::clamp(y * k, -1.f, 1.f);
}

bool isPlayStation(int sdlGamepadType) {
    return sdlGamepadType == SDL_GAMEPAD_TYPE_PS3 || sdlGamepadType == SDL_GAMEPAD_TYPE_PS4 ||
           sdlGamepadType == SDL_GAMEPAD_TYPE_PS5;
}
bool port1PlayStation() { return s_port1PlayStation; }

JoystickAccess::JoystickAccess() : m_held(s_joyMutex.try_lock()) {}
JoystickAccess::~JoystickAccess() {
    if (m_held) s_joyMutex.unlock();
}

bool s_pads = false;  // Aurora PAD available (not with R2_NO_GFX)

void init(bool pads) {
    s_pads = pads;
    for (auto& e : s_lastPads) e.err = PAD_ERR_NO_CONTROLLER;
    if (pads) {
        defaultKeyboard();
        PADInit();
        // a port 1 pin is followActiveController's, from the last session: start automatic
        if (aurora::pad::port_preference(0).state == aurora::pad::PortAssignment::Controller)
            aurora::pad::set_port_auto(0);
        // The mouse pointer hides after 2 s without mouse input (Aurora's AutoHide; any movement,
        // click or wheel shows it again), windowed or fullscreen. A passive input layer under the
        // game's states that preference; ImGui's layer above keeps the pointer while the menu
        // has the mouse.
        (void)aurora::input::register_layer({
            .label = "red_two.pointer",
            .priority = aurora::input::kGameLayerPriority - 1,
            .onEvent = [](const aurora::input::InputEvent&, void*) { return aurora::input::EventResult::Pass; },
            .pointerMode = [](void*) { return aurora::input::PointerMode::AutoHide; },
        });
        gyro::init();
        std::thread(joystickThread).detach();
        scanTest();
    }
    if (const char* s = getenv("R2_INPUT")) parseScript(s);
    s_ready = true;
}

void rumble(int chan, u32 cmd) {
    if (!s_pads || chan < 0 || chan > 3) return;
    s_rumbleWant[chan] = cmd;
    if (JoystickAccess joy; joy) applyRumble();  // else at the next poll
}

namespace {
PADStatus s_port1Game{};  // port 1 as the last poll shaped it (the trigger meter)
u64 s_port1Tb = 0;

void curvePort(PADStatus& e, int i) {
    if (!s_curveTriggers[i]) return;
    curveTrigger(e.triggerLeft, e.button & PAD_TRIGGER_L, s_bumperClicks[i]);
    curveTrigger(e.triggerRight, e.button & PAD_TRIGGER_R, s_bumperClicks[i]);
}
}  // namespace

int port1Triggers(TriggerView& v) {
    if (!s_ready || !s_pads) return 0;
    JoystickAccess joy;
    if (!joy) return -1;
    const auto src = aurora::pad::port_controller(0);
    SDL_Gamepad* g = src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
    if (!g) return 0;
    PADStatus p = s_port1Game;
    if (!s_port1Tb || tbNow() - s_port1Tb > kTbClock / 10) {  // no game reading the pads (the launcher)
        setUpControllers();
        PADStatus st[PAD_CHANMAX] = {};
        PADRead(st);
        curvePort(st[0], 0);
        p = st[0];
    }
    const aurora::pad::Profile* prof = aurora::pad::profile(aurora::pad::ProfileTarget::controller(src));
    for (int i = 0; i < 2; i++) {
        v.travel[i] = float(SDL_GetGamepadAxis(g, i ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / 32767.f;
        v.analog[i] = i ? p.triggerRight : p.triggerLeft;
        v.click[i] = p.button & (i ? PAD_TRIGGER_R : PAD_TRIGGER_L);
        v.clickAt[i] = !prof || !prof->settings.emulateTriggers || s_bumperClicks[0] ? 0.f
                       : i ? prof->settings.rightTriggerThreshold : prof->settings.leftTriggerThreshold;
    }
    return 1;
}

void poll() {
    if (!s_ready) return;
    s_vblank++;
    PADStatus st[PAD_CHANMAX] = {};
    // the sticks' deadzone and sensitivity (settings) and the triggers' travel, then gyro aiming
    // on port 1's main stick (its own scale: not through the thumb's deadzone and sensitivity)
    auto shape = [&st] {
        for (int i = 0; i < PAD_CHANMAX; i++)
            if (auto& e = st[i]; e.err == PAD_ERR_NONE) {
                // in flight the control stick's up / down may be reversed (setting stick_invert_y)
                const bool invertY = settings::get().stickInvertY && gyro::flying();
                const float stickY = float(e.stickY) / 127.f;
                mainStickToGame(float(e.stickX) / 127.f, invertY ? -stickY : stickY, e.stickX, e.stickY);
                cStickToGame(float(e.substickX) / 127.f, float(e.substickY) / 127.f, e.substickX, e.substickY);
                curvePort(e, i);
            }
        s_port1Game = st[0];
        s_port1Tb = tbNow();
    };
    if (!s_pads) {
        for (auto& e : st) e.err = PAD_ERR_NO_CONTROLLER;
    } else if (JoystickAccess joy; joy) {
        followActiveController();
        logPort1();
        setUpControllers();
        PADRead(st);
        if (s_curveTriggers[0] && st[0].err == PAD_ERR_NONE) {  // (the test's log: after the curve)
            PADStatus p1 = st[0];
            curveTrigger(p1.triggerRight, p1.button & PAD_TRIGGER_R);
            triggerTest(p1);
        } else {
            triggerTest(st[0]);
        }
        memcpy(s_lastPads, st, sizeof st);
        applyRumble();
        shape();
        bumperTest(st[0]);
        stickTest(st[0]);
        gyro::apply(st[0], true);
    } else {
        memcpy(st, s_lastPads, sizeof st);
        shape();
        gyro::apply(st[0], false);
    }
    // a hotkey's modifier (Back) is down, or a hotkey just fired: its buttons are not the game's
    if (hotkeys::muteGame())
        for (auto& e : st) {
            e.button = 0;
            e.triggerLeft = e.triggerRight = 0;
        }
    gfx::touchApply(&st[0]);  // on-screen touch controls (Android), with or without a controller
    u32 scripted = 0;
    // script times count 59.94 Hz retraces, whatever the retrace rate (settings fps; it also
    // changes while a cinematic is skipped)
    static double scriptTime = 0;
    scriptTime += (60000.0 / 1001.0) / viRate();
    const u64 t = u64(scriptTime);
    u8 trigL = 0, trigR = 0;
    for (auto& p : s_script)
        if (t >= p.start && t < p.start + p.frames) {
            scripted |= p.button;
            if (p.button == kTrigL) trigL = std::max(trigL, p.value);
            if (p.button == kTrigR) trigR = std::max(trigR, p.value);
        }
    if (scripted) {
        st[0].err = PAD_ERR_NONE;
        st[0].button |= u16(scripted);
        if (scripted & kTrigL) st[0].triggerLeft = trigL;
        if (scripted & kTrigR) st[0].triggerRight = trigR;
        if (scripted & kStickUp) st[0].stickY = 127;
        if (scripted & kStickDown) st[0].stickY = -127;
        if (scripted & kStickLeft) st[0].stickX = -127;
        if (scripted & kStickRight) st[0].stickX = 127;
        if (scripted & kCUp) st[0].substickY = 127;
        if (scripted & kCDown) st[0].substickY = -127;
    }
    // In flight Y (the targeting computer, shown while held) toggles: a press turns it on, the
    // next one off (setting targeting_toggle)
    if (settings::get().targetingToggle) {
        static bool yWas = false, yOn = false;
        const bool flying = gyro::flying() && st[0].err == PAD_ERR_NONE;
        const bool y = flying && (st[0].button & PAD_BUTTON_Y);
        if (!flying) yOn = false;
        else if (y && !yWas) yOn = !yOn;
        yWas = y;
        if (flying) st[0].button = u16(yOn ? st[0].button | PAD_BUTTON_Y : st[0].button & ~PAD_BUTTON_Y);
    }
    static const bool padLog = envFlag("R2_PAD_LOG");
    if (padLog && s_vblank % 30 == 0) {
        for (int i = 0; i < 2; i++) {
            const auto src = s_pads ? aurora::pad::port_controller(u32(i)) : 0;
            logf("pad", "port %d: err %d buttons %04X stick %d,%d c %d,%d triggers %u,%u controller %llu", i, st[i].err,
                 st[i].button, st[i].stickX, st[i].stickY, st[i].substickX, st[i].substickY, st[i].triggerLeft,
                 st[i].triggerRight, (unsigned long long)src);
        }
    }
    const bool menu = gfx::menuOpen();
    // After the menu closes, a controller stays idle until its buttons are let go: B (which
    // closes the menu) or A must not reach the game as a press.
    static bool heldFromMenu[PAD_CHANMAX] = {};
    for (int i = 0; i < PAD_CHANMAX; i++) {
        PadState& p = padState(i);
        if (menu) heldFromMenu[i] = true;
        else if (heldFromMenu[i] && (st[i].err != PAD_ERR_NONE || (st[i].button & 0x1F7F) == 0)) heldFromMenu[i] = false;
        const bool idle = menu || heldFromMenu[i];
        if (st[i].err == PAD_ERR_NO_CONTROLLER || idle) {
            // Port 1 always has a controller: the game asks for one otherwise.
            p.connected = i == 0 || (idle && st[i].err != PAD_ERR_NO_CONTROLLER);
            p.buttons = 0x0080;
            p.stickX = p.stickY = p.substickX = p.substickY = 0x80;
            p.triggerL = p.triggerR = 0;
            continue;
        }
        p.connected = true;
        p.buttons = u16((st[i].button & 0x1F7F) | 0x0080);  // PAD_USE_ORIGIN
        p.stickX = u8(st[i].stickX + 128);
        p.stickY = u8(st[i].stickY + 128);
        p.substickX = u8(st[i].substickX + 128);
        p.substickY = u8(st[i].substickY + 128);
        p.triggerL = st[i].triggerLeft;
        p.triggerR = st[i].triggerRight;
    }
}

}  // namespace rt::input
