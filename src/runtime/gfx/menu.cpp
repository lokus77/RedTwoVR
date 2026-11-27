// In-game settings menu: Dear ImGui (through Aurora), drawn over the game's frame. F1 or the
// controller's Back/Select button (the GameCube pad has none, so the game never sees it) opens
// and closes it - Back when it is let go without another button pressed meanwhile, since Back
// also starts the save state hotkeys (Back + RB); Escape or the controller's B closes it. While
// it is open the game sees idle controllers (input.cpp, which also keeps buttons held when it
// closes from the game).
//
// Tabs run along the top: LB / RB (Q / E, Tab) switch them. The d-pad or the left stick (arrow
// keys) moves through a page's rows and left / right changes the selected value; A (Enter)
// toggles or picks, X (Delete) clears a binding. The mouse works everywhere too.
//
// The look: a holo-display over a starfield - a chamfered navy panel with glowing gold edges,
// corner brackets and scanlines, Star Wars crawl gold for titles, the radar's amber for
// highlights, Bahnschrift (Windows' DIN-style face) for type.
#include "gfx/xr.h"
#include "gx_bridge.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../hotkeys.h"
#include "../hw/musyx_dsp.h"
#include "../mods.h"
#include "../achievements.h"
#include "../discord.h"
#include "../update.h"
#include "../gyro.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../settings.h"

namespace rt::gfx {
namespace {

// ---------------------------------------------------------------------------------------
// theme
// ---------------------------------------------------------------------------------------
constexpr ImU32 kGold = IM_COL32(255, 214, 70, 255);    // the crawl's yellow
constexpr ImU32 kAmber = IM_COL32(255, 150, 40, 255);   // the radar frame
constexpr ImU32 kHolo = IM_COL32(110, 205, 255, 255);   // hologram blue
constexpr ImU32 kText = IM_COL32(226, 233, 245, 255);
constexpr ImU32 kDim = IM_COL32(128, 146, 176, 255);
constexpr ImU32 kPanelTop = IM_COL32(10, 20, 40, 238);
constexpr ImU32 kPanelBottom = IM_COL32(2, 5, 13, 246);
constexpr ImU32 kFrame = IM_COL32(13, 28, 54, 235);

ImU32 alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (ImU32(std::clamp(a, 0, 255)) << IM_COL32_A_SHIFT); }
ImVec4 vec(ImU32 c, float a = -1.f) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (a >= 0.f) v.w = a;
    return v;
}

ImFont* s_font = nullptr;     // body text
ImFont* s_fontBig = nullptr;  // titles
constexpr float kFontPx = 34.f, kBigPx = 72.f;

void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.ChildRounding = 0.f;
    st.FrameRounding = 2.f;
    st.PopupRounding = 2.f;
    st.GrabRounding = 2.f;
    st.WindowBorderSize = 0.f;
    st.ChildBorderSize = 0.f;
    st.FrameBorderSize = 1.f;
    st.PopupBorderSize = 1.f;
    st.ScrollbarSize = 10.f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = vec(kText);
    c[ImGuiCol_TextDisabled] = vec(kDim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.07f, 0.14f, 0.98f);
    c[ImGuiCol_Border] = vec(kGold, 0.35f);
    c[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.11f, 0.21f, 0.90f);
    c[ImGuiCol_FrameBgHovered] = vec(kHolo, 0.22f);
    c[ImGuiCol_FrameBgActive] = vec(kHolo, 0.35f);
    c[ImGuiCol_Button] = ImVec4(0.06f, 0.13f, 0.25f, 0.92f);
    c[ImGuiCol_ButtonHovered] = vec(kHolo, 0.30f);
    c[ImGuiCol_ButtonActive] = vec(kAmber, 0.65f);
    c[ImGuiCol_Header] = vec(kAmber, 0.22f);
    c[ImGuiCol_HeaderHovered] = vec(kHolo, 0.28f);
    c[ImGuiCol_HeaderActive] = vec(kAmber, 0.45f);
    c[ImGuiCol_CheckMark] = vec(kGold);
    c[ImGuiCol_SliderGrab] = vec(kAmber);
    c[ImGuiCol_Separator] = vec(kGold, 0.25f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = vec(kGold, 0.25f);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kGold, 0.45f);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kAmber, 0.7f);
    c[ImGuiCol_NavCursor] = vec(kAmber);
    c[ImGuiCol_TextSelectedBg] = vec(kHolo, 0.35f);
}

// text with letter spacing (tracking) and an optional glow, at a size in pixels
// The menu's language (setting language): English as written here, or its Spanish from
// menu_es.inc (a text it has no entry for stays English)
const char* loc(const char* en) {
    if (!en || settings::get().language != 1) return en;
    static const std::unordered_map<std::string_view, const char*> es = [] {
        static const std::pair<const char*, const char*> table[] = {
#include "gfx/menu_es.inc"
        };
        std::unordered_map<std::string_view, const char*> m;
        for (const auto& [k, v] : table) m.emplace(k, v);
        return m;
    }();
    const auto it = es.find(en);
    return it != es.end() ? it->second : en;
}
std::string loc(const std::string& en) { return loc(en.c_str()); }

// the bytes of the UTF-8 character starting at s (the Spanish menu's accents)
int utf8Len(const char* s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    const int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    for (int i = 1; i < n; i++)
        if (!s[i]) return i;
    return n;
}

void drawText(ImDrawList* dl, ImFont* font, float px, ImVec2 at, ImU32 col, const char* text, float tracking = 0.f,
              ImU32 glow = 0) {
    if (!font) font = ImGui::GetFont();
    text = loc(text);
    auto pass = [&](ImVec2 p, ImU32 c) {
        for (const char* s = text; *s;) {
            const int n = utf8Len(s);
            char buf[5] = {};
            memcpy(buf, s, size_t(n));
            dl->AddText(font, px, p, c, buf);
            p.x += font->CalcTextSizeA(px, FLT_MAX, 0.f, buf).x + tracking;
            s += n;
        }
    };
    if (glow) {
        const float r = std::max(1.f, px * 0.05f);
        for (int i = 0; i < 8; i++) {
            const float a = float(i) * 0.785398f;
            pass(ImVec2(at.x + std::cos(a) * r, at.y + std::sin(a) * r), glow);
        }
    }
    pass(at, col);
}
float textWidth(ImFont* font, float px, const char* text, float tracking = 0.f) {
    if (!font) font = ImGui::GetFont();
    text = loc(text);
    size_t n = 0;
    for (const char* s = text; *s; s += utf8Len(s)) n++;
    return font->CalcTextSizeA(px, FLT_MAX, 0.f, text).x + tracking * float(n > 0 ? n - 1 : 0);
}

// a panel with two cut corners (top-left, bottom-right), a vertical gradient and a glowing edge
void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 edge, float thick) {
    const ImVec2 pts[6] = {ImVec2(a.x + cut, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - cut),
                           ImVec2(b.x - cut, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + cut)};
    const int start = dl->VtxBuffer.Size;
    dl->AddConvexPolyFilled(pts, 6, top);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
    if (edge) {
        dl->AddPolyline(pts, 6, alpha(edge, 40), ImDrawFlags_Closed, thick * 5.f);
        dl->AddPolyline(pts, 6, alpha(edge, 70), ImDrawFlags_Closed, thick * 2.5f);
        dl->AddPolyline(pts, 6, edge, ImDrawFlags_Closed, thick);
    }
}

void brackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float len, ImU32 col, float thick) {
    const float o = thick * 3.f;
    const ImVec2 tr[3] = {ImVec2(b.x - len, a.y - o), ImVec2(b.x + o, a.y - o), ImVec2(b.x + o, a.y + len)};
    const ImVec2 bl[3] = {ImVec2(a.x - o, b.y - len), ImVec2(a.x - o, b.y + o), ImVec2(a.x + len, b.y + o)};
    dl->AddPolyline(tr, 3, col, ImDrawFlags_None, thick);
    dl->AddPolyline(bl, 3, col, ImDrawFlags_None, thick);
}

// slowly drifting, twinkling stars behind the panel
void starfield(ImDrawList* dl, ImVec2 size, float t) {
    struct Star {
        float x, y, r, phase, speed;
    };
    static std::vector<Star> stars;
    if (stars.empty()) {
        u32 seed = 0x5EED2001;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 8) / float(1 << 24);
        };
        for (int i = 0; i < 260; i++)
            stars.push_back({rnd(), rnd(), 0.4f + rnd() * rnd() * 1.6f, rnd() * 6.28f, 0.004f + rnd() * 0.012f});
    }
    const float scale = size.y / 1080.f;
    for (const Star& s : stars) {
        const float x = std::fmod(s.x + t * s.speed, 1.f) * size.x;
        const float tw = 0.55f + 0.45f * std::sin(t * 2.2f + s.phase);
        dl->AddCircleFilled(ImVec2(x, s.y * size.y), s.r * scale * (0.8f + 0.4f * tw), alpha(kText, int(200 * tw)), 8);
    }
}

// ---------------------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------------------
bool s_open = false;
bool s_focus = false;  // just opened: focus the window
// The launcher: this menu before the game boots (gx_bridge.cpp runLauncher), with the game's
// start on its first row; B / Esc / F1 / Back do not close it. s_launch: start was chosen.
bool s_launcher = false, s_launch = false;
u64 s_fpsFrames = 0, s_fpsTb = 0;
float s_fps = 0.f;
// automated runs: nobody's controller drives the menu (or opens it over a scripted test)
bool automatedRun() {
    static const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    return automated;
}
// R2_MENU_PAD_TEST=1 (automated runs): a virtual gamepad drives the menu (padSelfTest)
bool padTest() {
    static const bool on = envFlag("R2_MENU_PAD_TEST") && automatedRun();
    return on;
}

// (no achievements page in this build: RetroAchievements is off unless set in settings.ini)
enum Section { Display, Gameplay, Audio, Controls, Cheats, Mods, Vr, SectionCount };
const char* const kSectionNames[SectionCount] = {"DISPLAY", "GAMEPLAY", "AUDIO", "CONTROLS", "CHEATS", "MODS", "VR"};
int s_section = 0;
int s_rowSel[SectionCount] = {};    // each page's selected row
int s_rowCount[SectionCount] = {};  // the rows each page had when it was last drawn

// --- navigation: controllers and keyboard ------------------------------------------------
// The menu reads controllers itself: every gamepad SDL has open (Aurora opens each one as it
// connects). ImGui's SDL backend lists gamepads once and refreshes the list only on connection
// events, which Aurora's input router keeps from it, so a controller that showed up after
// startup (Steam Input's virtual pad, a wireless pad waking up) never reached the menu.
enum Nav { NavUp, NavDown, NavLeft, NavRight, NavAccept, NavBack, NavClear, NavPrevTab, NavNextTab, NavStart, NavCount };
struct NavKey {
    bool held = false;
    double next = 0.0;  // directions: when the held press repeats
};
NavKey s_navKey[NavCount];
bool s_nav[NavCount];    // this frame: pressed (or repeated, for directions)
bool s_padPress[NavCount] = {};  // this frame's controller presses, also while they are not for navigating (typing)
bool s_navFresh = true;  // just opened: buttons already held are not presses
bool s_usingPad = false; // the last input came from a controller (button hints)
// that controller's type (the hints name a PlayStation controller's buttons): the last one
// pressed since the menu opened, else port 1's
SDL_GamepadType s_padType = SDL_GAMEPAD_TYPE_UNKNOWN;
bool s_padPsPressed = false;
const char* padName(const char* xbox, const char* ps) { return input::isPlayStation(s_padType) ? ps : xbox; }
const char* padBackName(bool upper = true) {  // the Back button's name on the controller
    return s_padType == SDL_GAMEPAD_TYPE_PS5 ? (upper ? "CREATE" : "Create")
         : s_padType == SDL_GAMEPAD_TYPE_PS4 ? (upper ? "SHARE" : "Share")
         : s_padType == SDL_GAMEPAD_TYPE_PS3 ? (upper ? "SELECT" : "Select")
                                             : (upper ? "BACK" : "Back");
}
const char* padStartName() { return padName("START", s_padType == SDL_GAMEPAD_TYPE_PS3 ? "START" : "OPTIONS"); }
// this frame's press is a new one, not the repeat of a held direction: ON / OFF switches take
// only these (a held d-pad / stick / arrow key repeats every 0.09 s, and on a switch row it
// flipped the setting back and forth - the user's log: fast_disc 22 times in 1.4 s)
bool s_navFirst[NavCount];

void readPads(bool held[NavCount]) {
    static bool last[NavCount] = {};  // while the input thread updates the controllers (a device scan)
    input::JoystickAccess joy;
    if (!joy) {
        for (int k = 0; k < NavCount; k++) held[k] |= last[k];
        return;
    }
    struct Keep {  // what this read found, for the frames a scan blocks reading
        bool* held;
        ~Keep() { std::copy(held, held + NavCount, last); }
    } keep{held};
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        auto btn = [g](SDL_GamepadButton b) { return SDL_GetGamepadButton(g, b); };
        const int lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        const int ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        constexpr int kDead = 16000;
        const bool vertical = std::abs(ly) >= std::abs(lx);  // only the stick's main direction
        bool mine[NavCount] = {};
        mine[NavUp] = btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || (vertical && ly < -kDead);
        mine[NavDown] = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (vertical && ly > kDead);
        mine[NavLeft] = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (!vertical && lx < -kDead);
        mine[NavRight] = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (!vertical && lx > kDead);
        mine[NavAccept] = btn(SDL_GAMEPAD_BUTTON_SOUTH);
        mine[NavBack] = btn(SDL_GAMEPAD_BUTTON_EAST);
        mine[NavClear] = btn(SDL_GAMEPAD_BUTTON_WEST);
        mine[NavPrevTab] = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        mine[NavNextTab] = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        mine[NavStart] = btn(SDL_GAMEPAD_BUTTON_START);  // (the launcher: start the game)
        bool any = false;
        for (int k = 0; k < NavCount; k++) {
            held[k] |= mine[k];
            any |= mine[k];
        }
        if (any) {  // (button hints: this controller's names)
            s_padType = SDL_GetGamepadType(g);
            s_padPsPressed = true;
        }
    }
    SDL_free(ids);
    if (!s_padPsPressed) {  // nothing pressed since the menu opened: port 1's controller
        const auto src = aurora::pad::port_controller(0);
        SDL_Gamepad* g = src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
        s_padType = g ? SDL_GetGamepadType(g) : SDL_GAMEPAD_TYPE_UNKNOWN;
    }
}

// this frame's presses; `enabled` false (a rebind in progress) still tracks what is held
void pollNav(bool enabled) {
    bool held[NavCount] = {};
    if (!automatedRun() || padTest() || getenv("R2_HOTKEY_TEST")) readPads(held);
    const double now = double(SDL_GetTicksNS()) * 1e-9;
    for (int k = 0; k < NavCount; k++) {
        NavKey& st = s_navKey[k];
        const bool repeats = k <= NavRight;
        bool fire = false, again = false;
        if (held[k] && !st.held) {
            fire = !s_navFresh;
            st.next = now + 0.40;
        } else if (held[k] && repeats && now >= st.next) {
            fire = !s_navFresh;
            again = true;
            st.next = now + 0.09;
        }
        st.held = held[k];
        s_padPress[k] = fire;
        s_nav[k] = fire && enabled;
        s_navFirst[k] = s_nav[k] && !again;
        if (s_nav[k]) s_usingPad = true;
    }
    s_navFresh = false;
    if (!enabled) return;
    // keyboard (ImGui does the key repeat)
    const bool shift = ImGui::GetIO().KeyShift;
    bool kb[NavCount] = {};
    kb[NavUp] = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
    kb[NavDown] = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
    kb[NavLeft] = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
    kb[NavRight] = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    kb[NavAccept] = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space, false);
    kb[NavClear] = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    kb[NavPrevTab] = ImGui::IsKeyPressed(ImGuiKey_Q, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    kb[NavNextTab] = ImGui::IsKeyPressed(ImGuiKey_E, false) || (!shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    // (ImGui's repeats: pressed with repeat but not without it)
    const ImGuiKey dirKeys[] = {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow};
    for (int k = 0; k < NavCount; k++)
        if (kb[k]) {
            s_nav[k] = true;
            if (k > NavRight || ImGui::IsKeyPressed(dirKeys[k], false)) s_navFirst[k] = true;
            s_usingPad = false;
        }
}

// A / Enter / Space still down (a rebind starts once it is released, or it would bind it)
bool acceptHeld() {
    return s_navKey[NavAccept].held || ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
           ImGui::IsKeyDown(ImGuiKey_Space);
}

// --- control remapping ---------------------------------------------------------------------
struct ControlRow {
    const char* name;
    aurora::binding::ControlId control, second;  // second: the analog half of L/R
    int sign;                                    // stick directions: +1 / -1 (0: a button)
};

std::vector<ControlRow> controlRows() {
    const auto& c = aurora::pad::controls();
    return {
        {"A", c.a, 0, 0},
        {"B", c.b, 0, 0},
        {"X", c.x, 0, 0},
        {"Y", c.y, 0, 0},
        {"Z", c.z, 0, 0},
        {"START", c.start, 0, 0},
        {"L TRIGGER", c.l, c.triggerL, 0},
        {"R TRIGGER", c.r, c.triggerR, 0},
        {"D-PAD UP", c.up, 0, 0},
        {"D-PAD DOWN", c.down, 0, 0},
        {"D-PAD LEFT", c.left, 0, 0},
        {"D-PAD RIGHT", c.right, 0, 0},
        {"STICK UP", c.leftY, 0, +1},
        {"STICK DOWN", c.leftY, 0, -1},
        {"STICK LEFT", c.leftX, 0, -1},
        {"STICK RIGHT", c.leftX, 0, +1},
        {"C-STICK UP", c.rightY, 0, +1},
        {"C-STICK DOWN", c.rightY, 0, -1},
        {"C-STICK LEFT", c.rightX, 0, -1},
        {"C-STICK RIGHT", c.rightX, 0, +1},
    };
}

bool s_padDevice = false;  // controls page: false keyboard, true controller

aurora::pad::ProfileTarget profileTarget() {
    return s_padDevice ? aurora::pad::ProfileTarget::controller(aurora::pad::port_controller(0))
                       : aurora::pad::ProfileTarget::keyboard(0);
}
SDL_Gamepad* targetGamepad() {
    if (!s_padDevice) return nullptr;
    const auto src = aurora::pad::port_controller(0);
    return src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
}


// which output sign a binding produces for an axis control (0: both)
int bindingSign(const aurora::binding::Binding& b) {
    using Dir = aurora::binding::PhysicalInput::GamepadAxis::Direction;
    if (const auto* ax = b.input.control.get_if<aurora::binding::PhysicalInput::GamepadAxis>()) {
        if (ax->direction == Dir::Full) return 0;
    }
    return b.scale < 0.f ? -1 : +1;
}

std::string bindingText(const ControlRow& row) {
    std::string out;
    SDL_Gamepad* pad = targetGamepad();
    for (const auto& b : aurora::pad::bindings_for_control(profileTarget(), row.control)) {
        if (row.sign != 0) {
            const int s = bindingSign(b);
            if (s != 0 && s != row.sign) continue;
        }
        if (!out.empty()) out += " / ";
        out += aurora::binding::input_label(b.input, pad);
    }
    return out.empty() ? std::string("-") : out;
}

// apply a captured input to a row
void bindRow(const ControlRow& row, const aurora::binding::PhysicalInput& in) {
    using namespace aurora::binding;
    using Dir = PhysicalInput::GamepadAxis::Direction;
    const auto target = profileTarget();
    if (row.sign == 0) {
        // L / R on a controller's trigger: its click (the digital L / R) at the profile's trigger
        // threshold, as Aurora binds it by default - the binding's own 0.5 clicked at half travel
        // (issue #6; input.cpp setUpControllers mends older rebinds)
        Binding b{.input = in};
        const auto* ax = in.control.get_if<PhysicalInput::GamepadAxis>();
        if (row.second && ax && (ax->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ax->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            const aurora::pad::Profile* prof = aurora::pad::profile(target);
            const aurora::pad::ProfileSettings s = prof ? prof->settings : aurora::pad::ProfileSettings{};
            b.threshold = row.control == aurora::pad::controls().l ? s.leftTriggerThreshold : s.rightTriggerThreshold;
        }
        aurora::pad::rebind(target, row.control, b);
        if (row.second) aurora::pad::rebind(target, row.second, in, aurora::pad::ConflictPolicy::Allow);
    } else {
        // keep the other direction's bindings (splitting a full axis into its other half)
        std::vector<Binding> list;
        for (auto b : aurora::pad::bindings_for_control(target, row.control)) {
            const int s = bindingSign(b);
            if (s == row.sign) continue;
            if (s == 0) {
                auto ax = *b.input.control.get_if<PhysicalInput::GamepadAxis>();
                const int other = -row.sign;
                ax.direction = float(other) * b.scale > 0.f ? Dir::Positive : Dir::Negative;
                b.input.control = ax;
                b.scale = float(other);
            }
            list.push_back(b);
        }
        Binding nb{.input = in, .target = row.control};
        nb.scale = float(row.sign);
        list.push_back(nb);
        aurora::pad::set_binding(target, row.control, std::move(list));
    }
    aurora::pad::save_profiles();
}

void clearRow(const ControlRow& row) {
    const auto target = profileTarget();
    std::vector<aurora::binding::Binding> keep;
    if (row.sign != 0)
        for (const auto& b : aurora::pad::bindings_for_control(target, row.control))
            if (bindingSign(b) == -row.sign) keep.push_back(b);
    aurora::pad::set_binding(target, row.control, std::move(keep));
    if (row.second) aurora::pad::set_binding(target, row.second, {});
    aurora::pad::save_profiles();
}

// capture in progress: the row, and the input once it arrives (from the event loop)
int s_captureRow = -1;
int s_pendingCapture = -1;  // chosen with A / Enter: starts once that is released
bool s_captured = false, s_captureCancel = false, s_captureRearm = false;
aurora::binding::PhysicalInput s_capturedInput;
u64 s_navBackTb = 0;  // keyboard/controller navigation resumes after a capture (not on its press)

void startCapture(int row) {
    s_captureRow = row;
    s_captured = s_captureCancel = false;
    aurora::binding::capture_next([](const aurora::binding::PhysicalInput& in) {
        using namespace aurora::binding;
        if (const auto* k = in.control.get_if<PhysicalInput::Key>()) {
            if (k->scancode == SDL_SCANCODE_ESCAPE || k->scancode == SDL_SCANCODE_F1) {
                s_captureCancel = true;
                return;
            }
        }
        if (const auto* g = in.control.get_if<PhysicalInput::GamepadButton>()) {
            if (g->button == SDL_GAMEPAD_BUTTON_BACK) {
                s_captureCancel = true;
                return;
            }
        }
        if (in.control.get_if<PhysicalInput::MouseButton>()) {  // a click elsewhere: cancel
            s_captureCancel = true;
            return;
        }
        // keys for the keyboard page, buttons and sticks for the controller page
        const bool isKey = in.control.get_if<PhysicalInput::Key>() != nullptr;
        if (isKey == s_padDevice) {
            s_captureRearm = true;
            return;
        }
        s_capturedInput = in;
        s_captured = true;
    });
}

void finishCapture(const std::vector<ControlRow>& rows) {
    if (s_captureRow >= 0 && s_captureRearm) {  // the other device's input: keep listening
        s_captureRearm = false;
        startCapture(s_captureRow);
        return;
    }
    if (s_captureRow < 0 || !(s_captured || s_captureCancel)) return;
    if (s_captured && s_captureRow < int(rows.size())) bindRow(rows[size_t(s_captureRow)], s_capturedInput);
    s_captureRow = -1;
    s_captured = s_captureCancel = false;
    s_navBackTb = tbNow() + kTbClock / 4;
}

// hotkey rows (hotkeys.cpp captures a combination: held together, then let go)
int s_pendingHotkey = -1;  // chosen with A / Enter: the capture starts once that is released
bool s_hotkeyWasCapturing = false;

void cancelCapture() {
    if (s_captureRow >= 0) aurora::binding::cancel_capture();
    s_captureRow = s_pendingCapture = -1;
    s_pendingHotkey = -1;
    hotkeys::captureCancel();
}

bool rebindingNow() {
    return s_captureRow >= 0 || s_pendingCapture >= 0 || s_pendingHotkey >= 0 || hotkeys::capturing() ||
           (s_navBackTb && tbNow() < s_navBackTb);
}

// ---------------------------------------------------------------------------------------
// rows
// ---------------------------------------------------------------------------------------
// A page is a list of rows: a label on the left, its value from s_valueX. One row is selected
// (the controller's and keyboard's cursor; hovering with a moving mouse selects too) and gets
// this frame's left/right, accept and clear presses.
float s_ui = 1.f;      // size of the UI: the display's height / 1080
float s_valueX = 0.f;  // where values start in a row
struct RowInput {
    int dx = 0;       // left -1 / right +1 (repeating while held)
    int dxFirst = 0;  // the same, new presses only
    bool accept = false, clear = false;
} s_in;
int s_row = 0;               // rows drawn so far on this page
bool s_scrollToSel = false;  // the selection moved: bring it into view
const char* s_help = nullptr;  // the selected row's description

void bodyFont(float px) { ImGui::SetWindowFontScale(px / kFontPx); }

bool rowBegin(const char* text, const char* help) {
    const int idx = s_row++;
    int& selRow = s_rowSel[s_section];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight(), padY = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    const ImVec2 a(p.x, p.y - padY), b(p.x + ImGui::GetContentRegionAvail().x, p.y + h + padY);
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f) && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b))
        selRow = idx;
    const bool sel = idx == selRow;
    if (sel) {
        dl->AddRectFilledMultiColor(a, b, alpha(kHolo, 50), alpha(kHolo, 4), alpha(kHolo, 4), alpha(kHolo, 50));
        dl->AddRectFilled(a, ImVec2(a.x + 4.f * s_ui, b.y), kAmber);
        // (copied: some pages build their descriptions on the fly)
        static std::string helpText;
        helpText = help ? loc(help) : "";
        s_help = help ? helpText.c_str() : nullptr;
        if (s_scrollToSel) {
            s_scrollToSel = false;
            if (idx == 0) ImGui::SetScrollY(0.f);  // the page's first heading too
            else ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(a, b), ImGuiScrollFlags_KeepVisibleEdgeY);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 20.f * s_ui, p.y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? vec(kGold) : vec(kText));
    ImGui::TextUnformatted(loc(text));
    ImGui::PopStyleColor();
    ImGui::SameLine(s_valueX);
    return sel;
}

// a framed box for a value
void valueFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sel, bool hot) {
    dl->AddRectFilled(a, b, hot ? IM_COL32(24, 52, 92, 240) : kFrame, 2.f);
    dl->AddRect(a, b, sel ? alpha(kAmber, 220) : alpha(kGold, hot ? 140 : 70), 2.f, 0, 1.f);
}

// ON / OFF switch
bool onOff(const char* id, bool& v, bool sel) {
    const float w = 150.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##onoff", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    if (v) {
        dl->AddRectFilledMultiColor(p, b, alpha(kAmber, 170), alpha(kAmber, 120), alpha(kAmber, 90), alpha(kAmber, 140));
        dl->AddRect(p, b, sel ? kGold : alpha(kAmber, 220), 2.f, 0, 1.f);
    } else {
        valueFrame(dl, p, b, sel, hot);
    }
    const char* text = loc(v ? "ON" : "OFF");
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                v ? IM_COL32(255, 245, 220, 255) : kDim, text);
    if (clicked) v = !v;
    return clicked;
}

bool toggleRow(const char* text, bool& v, const char* help = nullptr) {
    const bool sel = rowBegin(text, help);
    bool changed = onOff(text, v, sel);
    if (sel && (s_in.accept || s_in.dxFirst != 0)) {  // (held: once)
        v = !v;
        changed = true;
    }
    return changed;
}

// a value with arrows either side: the mouse clicks one half, the controller presses left/right.
// Returns the step clicked (-1 / +1, 0 none).
int selector(const char* id, const char* text, bool canPrev, bool canNext, bool sel) {
    text = loc(text);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##selector", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot);
    const float ah = h * 0.2f, cy = p.y + h * 0.5f, inset = 18.f * s_ui;
    auto arrow = [&](float x, float dir, bool on) {
        const ImU32 c = on ? (sel ? kGold : kText) : alpha(kDim, 70);
        dl->AddTriangleFilled(ImVec2(x + dir * ah, cy), ImVec2(x - dir * ah, cy - ah * 1.3f), ImVec2(x - dir * ah, cy + ah * 1.3f), c);
    };
    arrow(p.x + inset, -1.f, canPrev);
    arrow(p.x + w - inset, +1.f, canNext);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                sel ? kGold : kText, text);
    if (!clicked) return 0;
    return ImGui::GetIO().MousePos.x < p.x + w * 0.5f ? -1 : +1;
}

// one of `n` choices: left/right step (stopping at the ends), A goes round
int choiceStep(int cur, int n, int clickStep, bool sel) {
    int next = std::clamp(cur + clickStep + (sel ? s_in.dx : 0), 0, n - 1);
    if (sel && s_in.accept) next = (cur + 1) % n;
    return next;
}

template <typename E>
bool enumRow(const char* text, E& value, const char* (*name)(E), const char* help = nullptr) {
    const bool sel = rowBegin(text, help);
    const int n = int(E::Count), cur = int(value);
    const int next = choiceStep(cur, n, selector(text, name(value), cur > 0, cur < n - 1, sel), sel);
    if (next == cur) return false;
    value = E(next);
    return true;
}

bool fpsRow(const char* text, int& fps, const char* help) {
    const bool sel = rowBegin(text, help);
    constexpr int n = int(std::size(settings::kFpsChoices));
    int cur = 0;
    for (int i = 0; i < n; i++)
        if (settings::kFpsChoices[i] == fps) cur = i;
    char buf[16];
    snprintf(buf, sizeof buf, "%d FPS", fps);
    const int next = choiceStep(cur, n, selector(text, buf, cur > 0, cur < n - 1, sel), sel);
    if (settings::kFpsChoices[next] == fps) return false;
    fps = settings::kFpsChoices[next];
    return true;
}

// 0..100: a bar the mouse drags, left/right step 5
// The Controls page's stick meter: port 1's control stick as it is (dot), the deadzone (shaded)
// and what the game gets after the deadzone and sensitivity (gold), to tune both by eye.
const char* stickCurveLabel(settings::StickCurve c) {
    switch (c) {
    case settings::StickCurve::Linear: return "Linear";
    case settings::StickCurve::GameCube: return "GameCube";
    default: return "Smooth";
    }
}

// port 1's control stick (the controller's left stick) or C-stick (its right one): where it is, the
// deadzone, and what the game gets - the turn / pitch rate in flight for the control stick, the
// fraction of its range for the C-stick
void stickMeter(bool mainStick) {
    static float sx[2] = {}, sy[2] = {};
    static bool pads[2] = {};
    const int k = mainStick ? 0 : 1;
    float& rx = sx[k];
    float& ry = sy[k];
    bool& pad = pads[k];
    if (input::JoystickAccess joy; joy) {  // (else the last reading: the input thread is scanning)
        const auto src = aurora::pad::port_controller(0);
        SDL_Gamepad* g = src ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(src)) : nullptr;
        pad = g != nullptr;
        if (g) {
            rx = float(SDL_GetGamepadAxis(g, mainStick ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX)) / 32767.f;
            ry = float(SDL_GetGamepadAxis(g, mainStick ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY)) / 32767.f;
        }
    }
    if (static const char* test = getenv("R2_STICK_METER_TEST"); test) {  // "x,y[,cx,cy]" (screenshots)
        float v[4] = {};
        const int n = sscanf(test, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
        if (n >= 2 * (k + 1)) {
            pad = true;
            rx = v[2 * k];
            ry = v[2 * k + 1];
        }
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 54.f * s_ui, size = 2.f * r + 16.f * s_ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + s_valueX + r + 8.f * s_ui, p.y + size * 0.5f);
    if (!pad) {
        dl->AddText(ImVec2(p.x + 18.f * s_ui, p.y), kDim,
                    mainStick ? "Connect a controller to see its control stick here." : "Connect a controller to see its C-stick here.");
        ImGui::Dummy(ImVec2(0.f, ImGui::GetFrameHeight()));
        return;
    }
    const auto& st = settings::get();
    const float dz = float(mainStick ? st.stickDeadzone : st.cStickDeadzone) / 100.f;
    // the dot: the turn / pitch rate the stick gives the craft in flight (0..1 of its maximum), or
    // the C-stick's fraction of its range
    float ox = 0.f, oy = 0.f;
    s8 gx = 0, gy = 0;
    if (mainStick) input::mainStickToGame(rx, ry, gx, gy, &ox, &oy);
    else input::cStickToGame(rx, ry, gx, gy, &ox, &oy);
    auto at = [&](float x, float y) {
        const float m = std::sqrt(x * x + y * y), k = m > 1.05f ? 1.05f / m : 1.f;  // (a keyboard's diagonal)
        return ImVec2(c.x + x * k * r, c.y + y * k * r);
    };
    dl->AddCircleFilled(c, r, IM_COL32(12, 22, 40, 220), 64);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), alpha(kDim, 50), 1.f);
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), alpha(kDim, 50), 1.f);
    dl->AddCircle(c, r, alpha(kGold, 110), 64, 1.5f);
    if (dz > 0.f) {
        dl->AddCircleFilled(c, r * dz, alpha(kAmber, 45), 48);
        dl->AddCircle(c, r * dz, alpha(kAmber, 150), 48, 1.f);
    }
    dl->AddLine(c, at(ox, oy), alpha(kGold, 140), 2.f * s_ui);
    dl->AddCircleFilled(at(rx, ry), 4.f * s_ui, kText, 16);
    dl->AddCircleFilled(at(ox, oy), 6.f * s_ui, kGold, 16);
    char buf[96];
    if (mainStick)
        snprintf(buf, sizeof buf, "STICK %3.0f%%   TURN %3.0f%%   PITCH %3.0f%%", std::min(1.f, std::sqrt(rx * rx + ry * ry)) * 100.f,
                 std::fabs(ox) * 100.f, std::fabs(oy) * 100.f);
    else
        snprintf(buf, sizeof buf, "STICK %3.0f%%   TO THE GAME %3.0f%%", std::min(1.f, std::sqrt(rx * rx + ry * ry)) * 100.f,
                 std::min(1.f, std::sqrt(ox * ox + oy * oy)) * 100.f);
    dl->AddText(ImVec2(c.x + r + 24.f * s_ui, c.y - ImGui::GetFontSize() * 0.5f), kDim, buf);
    ImGui::Dummy(ImVec2(0.f, size));
}

// Port 1's triggers, like Dolphin's controller page: how far each is pulled, where it clicks (the
// tick; none when the bumpers have the clicks), the click lighting up, and what the game gets
// (its PADClamp reads 30 and less as nothing and 170 as all: (v - 30) / 140).
void triggerMeter() {
    static input::TriggerView v{};
    static bool pad = false;
    static u64 lastTb = 0;
    if (const u64 now = tbNow(); now - lastTb > kTbClock / 120) {  // (read once a frame at most)
        lastTb = now;
        input::TriggerView n{};
        const int got = input::port1Triggers(n);
        if (got > 0) v = n;
        if (got >= 0) pad = got > 0;  // (-1: the input thread is scanning - the last reading)
    }
    if (static const char* test = getenv("R2_TRIGGER_METER_TEST"); test) {  // "l,r" travel (screenshots)
        float l = 0.f, r = 0.f;
        if (sscanf(test, "%f,%f", &l, &r) == 2) {
            pad = true;
            const float t[2] = {l, r};
            for (int i = 0; i < 2; i++) {
                v.travel[i] = t[i];
                v.clickAt[i] = settings::get().bumperClicks ? 0.f : 0.95f;
                v.click[i] = v.clickAt[i] > 0.f && t[i] >= v.clickAt[i];
                const float full = v.clickAt[i] > 0.f ? v.clickAt[i] : 1.f;
                v.analog[i] = v.click[i] ? 180 : t[i] <= 0.04f ? 0 : u8(std::lround(30.f + (t[i] - 0.04f) / (full - 0.04f) * 140.f));
            }
        }
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!pad) {
        dl->AddText(ImVec2(p0.x + 18.f * s_ui, p0.y), kDim, "Connect a controller to see its triggers here.");
        ImGui::Dummy(ImVec2(0.f, ImGui::GetFrameHeight()));
        return;
    }
    const float h = ImGui::GetFrameHeight(), w = 300.f * s_ui, gap = 6.f * s_ui;
    static const char* const kNames[2] = {"L trigger (brake)", "R trigger (throttle)"};
    for (int i = 0; i < 2; i++) {
        const ImVec2 p(p0.x, p0.y + float(i) * (h + gap));
        const float cy = p.y + h * 0.5f;
        dl->AddText(ImVec2(p.x + 18.f * s_ui, cy - ImGui::GetFontSize() * 0.5f), kText, kNames[i]);
        const float x0 = p.x + s_valueX + 14.f * s_ui, x1 = p.x + s_valueX + w - 14.f * s_ui;
        const float th = 12.f * s_ui, t = std::clamp(v.travel[i], 0.f, 1.f), fx = x0 + (x1 - x0) * t;
        dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), IM_COL32(30, 52, 86, 255), th * 0.3f);
        if (t > 0.f) {
            const ImU32 a = v.click[i] ? IM_COL32(255, 120, 60, 255) : alpha(kAmber, 200);
            const ImU32 b = v.click[i] ? IM_COL32(255, 70, 40, 255) : kGold;
            dl->AddRectFilledMultiColor(ImVec2(x0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), a, b, b, a);
        }
        if (v.clickAt[i] > 0.f) {  // where the click is
            const float cx = x0 + (x1 - x0) * v.clickAt[i];
            dl->AddLine(ImVec2(cx, cy - th * 1.1f), ImVec2(cx, cy + th * 1.1f), alpha(kText, 200), 2.f * s_ui);
        }
        dl->AddRect(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), alpha(kGold, 90), th * 0.3f, 0, 1.f);
        const float game = std::clamp((float(v.analog[i]) - 30.f) / 140.f, 0.f, 1.f);
        char buf[64];
        snprintf(buf, sizeof buf, "%3.0f%%   GAME %3.0f%%", t * 100.f, game * 100.f);
        const float tx = p.x + s_valueX + w + 10.f * s_ui;
        dl->AddText(ImVec2(tx, cy - ImGui::GetFontSize() * 0.5f), kDim, buf);
        // the click: full brake / boost
        const char* tag = i ? "BOOST" : "BRAKE";
        const ImVec2 ts = ImGui::CalcTextSize(tag);
        const float bx = tx + ImGui::CalcTextSize(buf).x + 14.f * s_ui, pad2 = 6.f * s_ui;
        const ImVec2 b0(bx, cy - ts.y * 0.5f - 2.f * s_ui), b1(bx + ts.x + 2.f * pad2, cy + ts.y * 0.5f + 2.f * s_ui);
        if (v.click[i]) dl->AddRectFilled(b0, b1, IM_COL32(255, 90, 50, 230), 3.f * s_ui);
        dl->AddRect(b0, b1, v.click[i] ? IM_COL32(255, 160, 120, 255) : alpha(kDim, 90), 3.f * s_ui, 0, 1.f);
        dl->AddText(ImVec2(bx + pad2, cy - ts.y * 0.5f), v.click[i] ? IM_COL32(255, 255, 255, 255) : alpha(kDim, 140), tag);
    }
    ImGui::Dummy(ImVec2(0.f, 2.f * h + gap));
}

// a bar from lo to hi percent (steps of `step` from the controller / keys, any value by mouse)
bool percentRow(const char* text, int& v, const char* help, int lo = 0, int hi = 100, int step = 5) {
    const bool sel = rowBegin(text, help);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    ImGui::InvisibleButton("##bar", ImVec2(w, h));
    const bool active = ImGui::IsItemActive(), hot = ImGui::IsItemHovered();
    ImGui::PopID();
    const float x0 = p.x + 14.f * s_ui, x1 = p.x + w - 14.f * s_ui, cy = p.y + h * 0.5f;
    int nv = v;
    if (active)
        nv = lo + int(std::lround(std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.f, 1.f) * float(hi - lo)));
    if (sel && s_in.dx > 0) nv = std::min(hi, lo + ((v - lo) / step + 1) * step);
    if (sel && s_in.dx < 0) nv = std::max(lo, lo + ((v - lo + step - 1) / step - 1) * step);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot || active);
    const float th = 6.f * s_ui, fx = x0 + (x1 - x0) * float(nv - lo) / float(hi - lo);
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), IM_COL32(30, 52, 86, 255), th * 0.5f);
    for (int i = 1; i < 10; i++) {
        const float x = x0 + (x1 - x0) * float(i) / 10.f;
        dl->AddLine(ImVec2(x, cy - th * 1.4f), ImVec2(x, cy + th * 1.4f), alpha(kDim, 60), 1.f);
    }
    if (nv > lo)
        dl->AddRectFilledMultiColor(ImVec2(x0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), alpha(kAmber, 200), kGold, kGold,
                                    alpha(kAmber, 200));
    dl->AddCircleFilled(ImVec2(fx, cy), th * 1.5f, sel || active ? kGold : kText, 16);
    dl->AddCircle(ImVec2(fx, cy), th * 2.4f, alpha(kAmber, sel ? 160 : 60), 20, 1.f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? vec(kGold) : vec(kText));
    ImGui::Text("%d%%", nv);
    ImGui::PopStyleColor();
    if (nv == v) return false;
    v = nv;
    return true;
}

// gyro sensitivity: percent in steps of 10
bool gyroSpeedRow(const char* text, int& v, const char* help) {
    const bool sel = rowBegin(text, help);
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", v);
    int step = selector(text, buf, v > settings::kGyroSpeedMin, v < settings::kGyroSpeedMax, sel);
    if (sel) step += s_in.dx;
    const int nv = std::clamp(v + step * 10, settings::kGyroSpeedMin, settings::kGyroSpeedMax);
    if (nv == v) return false;
    v = nv;
    return true;
}

// a button with a caption; true when pressed (click or A)
bool buttonRow(const char* text, const char* caption, const char* help, ImU32 captionCol = kText) {
    const bool sel = rowBegin(text, help);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##button", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, hot);
    caption = loc(caption);
    const ImVec2 ts = ImGui::CalcTextSize(caption);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                sel ? kGold : captionCol, caption);
    return clicked || (sel && s_in.accept);
}

// a gold heading with a rule after it
void heading(const char* text) {
    text = loc(text);
    ImGui::Dummy(ImVec2(0, 4.f * s_ui));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float px = 21.f * s_ui;
    drawText(dl, s_font, px, p, kGold, text, 3.f * s_ui);
    const float w = textWidth(s_font, px, text, 3.f * s_ui);
    const float y = p.y + px * 0.55f;
    dl->AddLine(ImVec2(p.x + w + 12.f * s_ui, y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, y), alpha(kGold, 70), 1.f);
    ImGui::Dummy(ImVec2(0, px + 4.f * s_ui));
}

// ---------------------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------------------
bool s_changed = false, s_display = false;
bool s_confirmQuit = false;   // quitting asks for a second press
bool s_confirmReset = false;  // so does resetting
bool s_confirmLauncher = false;  // and going back to the launcher

// a button that asks for a second press (the selection moving away cancels it)
bool confirmRow(const char* text, const char* caption, const char* again, bool& armed, const char* help) {
    const int row = s_row;
    const bool pressed = buttonRow(text, armed ? again : caption, help, armed ? kAmber : kText);
    if (s_rowSel[s_section] != row) armed = false;
    if (!pressed) return false;
    if (!armed) {
        armed = true;
        return false;
    }
    armed = false;
    return true;
}

// (the game disc's page and the browser come further down)
bool discReady();
void discPageOpen();
void pageDisc(settings::Settings& s);
bool discPageShown();
bool discBrowserShown();
std::string discRowText();
void pageBrowse(settings::Settings& s);

// --- updates (update.cpp): a newer release, its download and install, the restart into it ---
void infoRow(const char* text, const std::string& value, const char* help, ImU32 col);  // (below)
void openUrl(const std::string& url) {
    if (!SDL_OpenURL(url.c_str())) logf("menu", "cannot open %s: %s", url.c_str(), SDL_GetError());
    else logf("menu", "opened %s", url.c_str());
}

bool s_confirmUpdateRestart = false;

void restartIntoUpdate() {
    logf("menu", "restarting into Red Two %s", update::status().latest.c_str());
    setRestartProgram(update::restartProgram());
    if (s_launcher) restartGame(true);  // (returns only if it could not start the new version)
    else resetGame(true);
}

// a bar that only shows progress (not a value to change)
void progressRow(const char* text, float fraction, const std::string& caption, const char* help) {
    rowBegin(text, help);
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = p.x + 14.f * s_ui, x1 = p.x + w - 14.f * s_ui, cy = p.y + h * 0.5f, th = 10.f * s_ui;
    dl->AddRectFilled(ImVec2(x0, cy - th * 0.5f), ImVec2(x1, cy + th * 0.5f), IM_COL32(30, 52, 86, 255), th * 0.3f);
    const float fx = x0 + (x1 - x0) * std::clamp(fraction, 0.f, 1.f);
    if (fx > x0) dl->AddRectFilledMultiColor(ImVec2(x0, cy - th * 0.5f), ImVec2(fx, cy + th * 0.5f), alpha(kAmber, 200), kGold, kGold, alpha(kAmber, 200));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(caption.c_str());
}

void updateSection() {
    using P = update::Phase;
    const update::Status u = update::status();
    if (u.phase != P::Available && u.phase != P::Downloading && u.phase != P::Installing && u.phase != P::Ready &&
        u.phase != P::Failed && u.phase != P::Updated)
        return;
    heading("UPDATE");
    char buf[192];
    switch (u.phase) {
    case P::Available:
        snprintf(buf, sizeof buf, "Red Two %s is out (you have %s)", u.latest.c_str(), update::version());
        infoRow("New version", buf, "A newer Red Two is on GitHub. Its release notes say what changed.", kAmber);
        if (u.canInstall) {
            if (buttonRow("Update", "DOWNLOAD AND INSTALL",
                          "Downloads the new version, checks it and puts it in place of this one; then Red Two restarts "
                          "into it. Your settings, controls, memory card, save states and texture packs stay as they are "
                          "(save states only load into the version that made them).",
                          kAmber))
                update::install();
        } else if (buttonRow("Update", "OPEN THE DOWNLOAD PAGE",
                             "This copy of Red Two cannot replace itself (built from source, or not the release download): "
                             "get the new version from its release page.",
                             kAmber)) {
            openUrl(u.pageUrl);
        }
        break;
    case P::Downloading: {
        const float frac = u.total ? float(double(u.got) / double(u.total)) : 0.f;
        snprintf(buf, sizeof buf, "%.0f / %.0f MB", double(u.got) / 1e6, double(u.total) / 1e6);
        progressRow("Downloading", frac, buf, "The new version is downloading. You can keep playing meanwhile.");
        break;
    }
    case P::Installing:
        infoRow("Installing", "Checking the download and putting it in place...", "Almost done.", kText);
        break;
    case P::Ready:
        snprintf(buf, sizeof buf, "RESTART INTO %s", u.latest.c_str());
        if (s_launcher ? buttonRow("Update installed", buf, "Red Two restarts into the new version.", kAmber)
                       : confirmRow("Update installed", buf, "PRESS AGAIN: RESTART", s_confirmUpdateRestart,
                                    "Red Two restarts into the new version, in the launcher. What the game saved to its "
                                    "memory card and your save states are kept; a mission in progress is not. Or keep "
                                    "playing: the next start is the new version."))
            restartIntoUpdate();
        break;
    case P::Failed: {
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kAmber));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(("The update did not work. " + u.error).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (buttonRow("Update", "TRY AGAIN", "Download and install the new version again.", kAmber)) update::install();
        break;
    }
    case P::Updated:
        snprintf(buf, sizeof buf, "Red Two %s (updated from %s)", u.latest.c_str(), u.from.c_str());
        infoRow("Updated", buf, "This is the new version. Its release notes say what changed.", kGold);
        break;
    default:
        break;
    }
    if (!u.pageUrl.empty() && buttonRow("What's new", "RELEASE NOTES", "Opens the release's page on GitHub in your browser."))
        openUrl(u.pageUrl);
}

// R2_UPDATE_TEST=install (tests): restart into the update as soon as it is in place
void updatePoll() {
    static const bool autoRestart = getenv("R2_UPDATE_TEST") && !strcmp(getenv("R2_UPDATE_TEST"), "install");
    static bool done = false;
    if (autoRestart && !done && update::status().phase == update::Phase::Ready) {
        done = true;
        restartIntoUpdate();
    }
}

// the menu's language (also on the disc page: a first start, before there is a disc)
void languageRow(settings::Settings& s) {
    const bool sel = rowBegin("Language", "The language of this menu (the launcher and the in-game menu). The game's "
                                          "own language: Game language.");
    static const char* const names[] = {"English", "Español"};
    const int cur = std::clamp(s.language, 0, 1);
    const int next = choiceStep(cur, 2, selector("Language", names[cur], cur > 0, cur < 1, sel), sel);
    if (next != cur) {
        s.language = next;
        s_changed = true;
    }
}

void pageDisplay(settings::Settings& s) {
    if (discBrowserShown()) {
        pageBrowse(s);
        return;
    }
    if (discPageShown()) {
        pageDisc(s);
        return;
    }
    updateSection();
    heading("GAME");
    if (s_launcher) {
        if (buttonRow("Start game", discReady() ? "LAUNCH" : "CHOOSE THE DISC FIRST",
                      "Start Rogue Leader with these settings. The controller's START button does this from any page.",
                      kAmber)) {
            if (discReady()) {
                logf("menu", "launcher: start");
                s_launch = true;
            } else {
                discPageOpen();
            }
        }
    } else {
        if (confirmRow("Reset game", "RESET", "PRESS AGAIN: RESET", s_confirmReset,
                       "Start the game over from the boot screen, like the console's reset button. What the game saved to "
                       "its memory card and your save states are kept; a mission in progress is not.")) {
            logf("menu", "reset");
            resetGame();
        }
        if (confirmRow("Back to launcher", "RESTART", "PRESS AGAIN: RESTART", s_confirmLauncher,
                       "Start over in the launcher, where every setting can be changed before the game starts. The "
                       "memory card and your save states are kept; a mission in progress is not.")) {
            logf("menu", "back to launcher");
            resetGame(true);
        }
    }
    if (buttonRow("Game disc", discRowText().c_str(),
                  s_launcher ? "Your Rogue Leader disc image: where it is, or choose another."
                             : "Your Rogue Leader disc image: where it is, or choose another (from the next start).",
                  discReady() ? kText : kAmber))
        discPageOpen();
    languageRow(s);
    {
        const bool sel = rowBegin("Game language", "The game's texts, menus and radio voices. Spanish needs, in the same "
                                                   "folder as the USA disc, your disc of the Spanish release (GSWS64) or the "
                                                   "folder red_two_es made from it (red_two --extract-spanish <disc>). "
                                                   "From the next start.");
        static const char* const names[] = {"English", "Español"};
        const int cur = std::clamp(s.gameLanguage, 0, 1);
        const int next = choiceStep(cur, 2, selector("Game language", names[cur], cur > 0, cur < 1, sel), sel);
        if (next != cur) {
            s.gameLanguage = next;
            s_changed = true;
        }
    }
    if (toggleRow("Show launcher at start", s.launcher,
                  s_launcher ? "Off: from the next start Red Two goes straight into the game. The launcher comes back "
                               "with this switch in the in-game menu (F1 or the controller's Back)."
                             : "On: Red Two opens the launcher before the game starts. Off: straight into the game."))
        s_changed = true;
    if (confirmRow("Quit to desktop", "QUIT", "PRESS AGAIN: QUIT", s_confirmQuit,
                   "Close Red Two. What the game saved to its memory card and your save states are kept; a mission in "
                   "progress is not.")) {
        logf("menu", "quit to desktop");
        quitToDesktop();
    }
    heading("SCREEN");
    if (toggleRow("Fullscreen", s.fullscreen, "Also F11 or Alt+Enter.")) s_changed = s_display = true;
    if (enumRow("Internal resolution", s.resolution, settings::resolutionName,
                "What the game renders at. Window size follows the window; 4K renders 2160 lines whatever the window size."))
        s_changed = s_display = true;
    {
        // MSAA: Aurora sets it up when it starts (WebGPU has 1 or 4 samples only)
        static const int startMsaa = s.msaa;  // (what this session runs with)
        const bool sel = rowBegin("Anti-aliasing", s.msaa != startMsaa
                                                       ? "Smooths the edges of 3D objects (4x MSAA). TAKES EFFECT AT THE NEXT START."
                                                       : "Smooths the edges of 3D objects (4x MSAA). Takes effect at the next start.");
        const int cur = s.msaa >= 4 ? 1 : 0;
        const char* text = cur ? (startMsaa >= 4 ? "4x MSAA" : "4x MSAA (NEXT START)") : (startMsaa >= 4 ? "OFF (NEXT START)" : "OFF");
        const int next = choiceStep(cur, 2, selector("msaa", text, cur > 0, cur < 1, sel), sel);
        if (next != cur) {
            s.msaa = next ? 4 : 1;
            s_changed = true;
        }
    }
    {
        static const int kLevels[] = {1, 2, 4, 8, 16};
        static const char* const kNames[] = {"OFF", "2x", "4x", "8x", "16x"};
        const bool sel = rowBegin("Anisotropic filtering",
                                  "Keeps textures sharp at steep angles (the ground ahead, the Death Star's surface). "
                                  "Off: only where the game asks for it.");
        int cur = 0;
        while (cur < 4 && kLevels[cur] < s.anisotropy) cur++;
        const int next = choiceStep(cur, 5, selector("aniso", kNames[cur], cur > 0, cur < 4, sel), sel);
        if (next != cur) {
            s.anisotropy = kLevels[next];
            s_changed = s_display = true;
        }
    }
    if (enumRow("Aspect ratio", s.aspect, settings::aspectName,
                "Widescreen widens the 3D view and fills the screen in cutscenes. At most 21:9: a wider window "
                "gets bars at the sides."))
        s_changed = s_display = true;
    if (toggleRow("HUD at screen edges", s.hudEdges, "Widescreen: the flight HUD sits at the edges of the screen."))
        s_changed = true;
    if (toggleRow("Fill menu backgrounds", s.tileMenus,
                  "Widescreen: the menus' panel backgrounds repeat into the sides of the screen."))
        s_changed = true;
    if (toggleRow("Dither pattern", s.gameDither,
                  "The game blends a dither pattern over the picture, made for the GameCube's 640x480. At higher "
                  "resolutions it shows as a grid (Hoth's sky). Off, like Dolphin's \"Disable Dithering\" code."))
        s_changed = true;
    heading("PERFORMANCE");
    if (fpsRow("Frame rate", s.fps, "Above 60 the game runs more frames per second at its normal speed."))
        s_changed = s_display = true;
    {
        std::string help = "Shows each frame in step with your display's refresh: no tearing, a little more input lag. "
                           "Best with the frame rate at your display's refresh rate";
        const int hz = displayRefreshHz();
        help += hz > 0 ? " (this display: " + std::to_string(hz) + " Hz)." : ".";
        if (hz > 0 && s.vsync && s.fps > hz + 1)
            help += " WITH V-SYNC ON THE GAME RUNS AT " + std::to_string(hz) + " FRAMES A SECOND.";
        if (toggleRow("V-Sync", s.vsync, help.c_str())) s_changed = s_display = true;
    }
    if (toggleRow("Show frame rate", s.showFps, "Frames per second in the top-left corner.")) s_changed = true;
    if (toggleRow("Prepare shaders at start", s.precompileShaders,
                  s_launcher ? "Before the game starts, compiles every shader it has used on this device (PREPARING "
                               "SHADERS). Off: the game starts at once and objects may pop in while their shaders compile."
                             : "Before the game starts, compiles every shader it has used on this device (PREPARING "
                               "SHADERS). Off: the game starts at once and objects may pop in while their shaders "
                               "compile. From the next start."))
        s_changed = true;
    if (toggleRow("Ubershaders", s.ubershaders,
                  "A shader seen for the first time is drawn with a generic one while it compiles: no pop-in, but more "
                  "GPU work."))
        s_changed = s_display = true;
}

void pageGameplay(settings::Settings& s) {
    heading("LOADING");
    if (toggleRow("Fast loading", s.fastDisc,
                  "Disc reads at SSD speed instead of the GameCube drive's 3 MB/s: a mission loads 10-15 s sooner."))
        s_changed = true;
    heading("CINEMATICS");
    if (toggleRow("Skip intro logos", s.skipLogos, "Reach the title screen in seconds.")) s_changed = true;
    if (toggleRow("Skippable cutscenes", s.skipCutscenes,
                  "Press B during the opening crawl or a cutscene to skip to the gameplay."))
        s_changed = true;
}

const char* speakersLabel(settings::Speakers sp) {
    switch (sp) {
    case settings::Speakers::Stereo: return "Stereo";
    case settings::Speakers::Surround51: return "5.1 surround";
    case settings::Speakers::Surround71: return "7.1 surround";
    default: {
        const int n = audio::outputChannels();
        return n == 8 ? "Automatic (7.1)" : n == 6 ? "Automatic (5.1)" : "Automatic (stereo)";
    }
    }
}

template <typename T>
bool valueRow(const char* text, T& v, const T* values, const char* const* labels, int n, const char* help);  // (below)

void pageAudio(settings::Settings& s) {
    heading("VOLUME");
    if (percentRow("Master volume", s.volume,
                   "Everything the game plays. Music, effects and speech have their own levels in the game's pause "
                   "menu (Sound Settings)."))
        s_changed = true;
    heading("OUTPUT");
    if (enumRow("Speakers", s.speakers, speakersLabel,
                "Stereo plays the game's Pro Logic II surround matrix-encoded, for a receiver's decoder. 5.1 and "
                "7.1 play it on speakers of their own (the game's sound setting must be STEREO, not MONO).")) {
        s_changed = true;
        audio::setSpeakers(int(s.speakers));
    }
    {
        static const int levels[] = {0, 1, 2};
        static const char* const labels[] = {"NORMAL", "LARGE", "HUGE"};
        if (valueRow("Audio buffer", s.audioBuffer, levels, labels, 3,
                     "How much sound is kept ready ahead (50, 100 or 160 ms). If the sound crackles or cuts out, a "
                     "larger buffer rides out the moments the PC falls behind; the sound comes a little later.")) {
            s_changed = true;
            audio::setBuffer(s.audioBuffer);
        }
    }
    if (toggleRow("Sound in background", s.backgroundAudio,
                  "Keep playing sound while another window has the focus. Off: silent until you come back."))
        s_changed = true;
}

void infoRow(const char* text, const std::string& value, const char* help, ImU32 col);  // (below)

void pageCheats(settings::Settings& s) {
    heading("REBEL ALLIANCE CLEARANCE");
    if (toggleRow("Invincibility", s.invincible, "Your ship cannot be destroyed.")) s_changed = true;
    if (toggleRow("Infinite lives", s.infiniteLives, "Losing a ship costs no life.")) s_changed = true;
    if (toggleRow("Infinite bombs", s.infiniteAmmo, "Torpedoes, bombs and missiles never run out.")) s_changed = true;
    if (toggleRow("Perfect mission stats", s.perfectStats,
                  "The mission results say 0:01, every enemy destroyed, 100% accuracy, no friendlies or lives lost and 100% "
                  "targeting computer efficiency; the medals follow."))
        s_changed = true;
    if (toggleRow("All craft", s.allCraft,
                  "Every craft a mission allows waits in its hangar: the passcode craft (Millennium Falcon, Slave I, TIE "
                  "fighter, Naboo starfighter, Imperial shuttle, ...) and the medal rewards. Your save is not changed."))
        s_changed = true;
    heading("PASSCODES");
    if (s_launcher) {  // (the game loads its save at boot, over anything set now)
        infoRow("Passcode unlocks", "IN THE GAME",
                "The passcode unlocks live in the game's save, which it loads once it has started: switch them in the "
                "in-game menu (F1 or the controller's Back).", kDim);
        return;
    }
    // the game's own unlocks: kept in its save like codes entered in Options > Passcodes
    for (int i = 0; i < unlocks::Count; i++) {
        bool v = unlocks::state(i) == 1;
        if (toggleRow(unlocks::name(i), v, unlocks::help(i))) unlocks::set(i, v);
    }
}

// --- achievements: RetroAchievements (achievements.cpp) ---------------------------------------
// A text value: A / Enter starts typing (a keyboard; Steam Deck's on-screen keyboard comes up
// with SDL's text input), Enter or the controller's A ends it, Esc or B cancels.
bool s_textEditing = false;
int s_textRow = -1, s_textSection = -1, s_textFrames = 0;
char s_textBuf[256] = {};

bool textRow(const char* text, std::string& value, bool password, const char* help) {
    const bool sel = rowBegin(text, help);
    const int idx = s_row - 1;
    const float w = 380.f * s_ui, h = ImGui::GetFrameHeight();
    const bool editing = s_textEditing && s_textRow == idx && s_textSection == s_section;
    ImGui::PushID(text);
    bool changed = false;
    if (editing) {
        ImGui::SetNextItemWidth(w);
        if (s_textFrames++ == 0) ImGui::SetKeyboardFocusHere();
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                          (password ? ImGuiInputTextFlags_Password : 0);
        const bool enter = ImGui::InputText("##text", s_textBuf, sizeof s_textBuf, flags);
        const bool lost = s_textFrames > 2 && !ImGui::IsItemActive();
        if (enter || s_padPress[NavAccept]) {
            value = s_textBuf;
            changed = true;
            s_textEditing = false;
        } else if (lost || s_padPress[NavBack] || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_textEditing = false;
        }
        if (!s_textEditing) memset(s_textBuf, 0, sizeof s_textBuf);
    } else {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##textbox", ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        valueFrame(dl, p, ImVec2(p.x + w, p.y + h), sel, ImGui::IsItemHovered());
        std::string shown = value.empty() ? std::string(sel ? loc("PRESS A / ENTER TO TYPE") : "-")
                                          : password ? std::string(std::min<size_t>(value.size(), 24), '*') : value;
        // a long value (a path) shows its end: "...\Rogue Leader (USA).rvz"
        const float room = w - 28.f * s_ui;
        if (ImGui::CalcTextSize(shown.c_str()).x > room) {
            size_t cut = 0;
            while (cut < shown.size() && ImGui::CalcTextSize(("..." + shown.substr(cut)).c_str()).x > room)
                do cut++;
                while (cut < shown.size() && (u8(shown[cut]) & 0xC0) == 0x80);  // (whole UTF-8 characters)
            shown = "..." + shown.substr(cut);
        }
        const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 14.f * s_ui, p.y + (h - ts.y) * 0.5f),
                    value.empty() ? kDim : (sel ? kGold : kText), shown.c_str());
        if (clicked || (sel && s_in.accept)) {
            s_textEditing = true;
            s_textRow = idx;
            s_textSection = s_section;
            s_textFrames = 0;
            snprintf(s_textBuf, sizeof s_textBuf, "%s", value.c_str());
        }
    }
    ImGui::PopID();
    return changed;
}

// a row that only shows something
void infoRow(const char* text, const std::string& value, const char* help, ImU32 col = kText) {
    const bool sel = rowBegin(text, help);
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? vec(kGold) : vec(col));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

std::string s_raUser, s_raPassword;  // typed into the login rows (the password is never stored)
bool s_raUserInit = false;
std::string s_raNote;                // the menu's own message (Dolphin's login)
bool s_confirmLogout = false;
int s_raDolphin = -1;                // Dolphin has a login: -1 not looked yet

void pageAchievements(settings::Settings& s) {
    const ra::Status st = ra::status();
    heading("RETROACHIEVEMENTS");
    if (toggleRow("Achievements", s.achievements,
                  "Earn achievements from retroachievements.org as you play, with your account there. Softcore: save "
                  "states, cheats and the options stay allowed (hardcore is not offered).")) {
        s_changed = true;
        ra::setEnabled(s.achievements);
    }
    if (!st.loggedIn) {
        if (!s_raUserInit) {
            s_raUser = s.raUser;
            s_raUserInit = true;
        }
        textRow("Username", s_raUser, false, "Your retroachievements.org user name.");
        textRow("Password", s_raPassword, true,
                "Only sent to log in: Red Two keeps the login token the server answers with, never the password.");
        if (buttonRow("Log in", st.busy ? "LOGGING IN..." : "LOG IN",
                      "Log in to retroachievements.org with the name and password above.") &&
            !st.busy) {
            if (s_raUser.empty() || s_raPassword.empty()) {
                s_raNote = "Type your user name and password first.";
            } else {
                if (!s.achievements) {
                    s.achievements = true;
                    s_changed = true;
                }
                s_raNote.clear();
                ra::login(s_raUser, s_raPassword);
                s_raPassword.clear();
            }
        }
        if (s_raDolphin < 0) s_raDolphin = ra::dolphinLoginAvailable() ? 1 : 0;
        if (s_raDolphin == 1 && buttonRow("Dolphin's login", "IMPORT",
                                          "Dolphin is logged in to RetroAchievements on this computer: use the same "
                                          "account (its login token from RetroAchievements.ini).")) {
            ra::importDolphinLogin(s_raNote);
            s_changed = false;  // (saved by the import)
        }
    } else {
        char b[160];
        snprintf(b, sizeof b, "%s  (%u points)", st.user.c_str(), st.score);
        infoRow("Account", b, "The retroachievements.org account unlocks go to (softcore points).");
        if (confirmRow("Log out", "LOG OUT", "PRESS AGAIN: LOG OUT", s_confirmLogout,
                       "Forget this account's login on this computer."))
            ra::logout();
    }
    if (toggleRow("Notifications", s.raNotifications, "Pop-ups for unlocks, leaderboards and the game's start."))
        s_changed = true;
    if (toggleRow("Encore mode", s.raEncore,
                  "Achievements you already have can trigger (and pop up) again. Takes effect when the game's "
                  "achievements load: at the next start or login."))
        s_changed = true;
    const std::string note = !s_raNote.empty() ? s_raNote : st.message;
    if (!note.empty()) infoRow("Status", note, note.c_str(), kAmber);

    heading("THIS GAME");
    if (!st.enabled) {
        infoRow("Achievements", "Off", "Switch achievements on above.", kDim);
        return;
    }
    if (!st.gameLoaded) {
        infoRow("Achievements", st.busy ? "Loading..." : st.loggedIn ? "-" : "Log in to see them", nullptr, kDim);
        return;
    }
    char sum[160];
    snprintf(sum, sizeof sum, "%u of %u unlocked, %u of %u points", st.unlocked, st.total, st.points, st.pointsTotal);
    infoRow("Progress", sum, st.game.c_str());
    for (const ra::Achievement& a : ra::achievements()) {
        static std::string help;
        help = a.description;
        const bool sel = rowBegin(a.title.c_str(), help.c_str());
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (void* tex = ra::image(a.badgeUrl))
            dl->AddImage(ImTextureID(uintptr_t(tex)), p, ImVec2(p.x + h, p.y + h), ImVec2(0, 0), ImVec2(1, 1),
                         a.unlocked ? IM_COL32_WHITE : IM_COL32(150, 150, 150, 200));
        ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine();
        char v[96];
        if (a.unlocked) snprintf(v, sizeof v, "UNLOCKED  -  %u POINTS", a.points);
        else if (!a.progress.empty()) snprintf(v, sizeof v, "%s  -  %u POINTS", a.progress.c_str(), a.points);
        else snprintf(v, sizeof v, "%u POINTS", a.points);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, a.unlocked ? vec(kGold) : sel ? vec(kText) : vec(kDim));
        ImGui::TextUnformatted(v);
        ImGui::PopStyleColor();
    }
}

// --- mods: texture packs, codes, importing from Dolphin -------------------------------------
mods::DolphinScan s_dolphin;       // what Dolphin has for this game (the last scan's findings)
bool s_dolphinHave = false;        // a scan has finished
bool s_dolphinStale = true;        // scan again when the page shows next (menu opened, import done)
std::future<mods::DolphinScan> s_dolphinJob;
bool s_confirmSave = false;        // the save import asks for a second press

// Scanning Dolphin reads its folders, its texture packs file by file (and Red Two's copy of
// them): 1.4 s for the user's 6 GB pack, and on the game thread the game showed no frame
// meanwhile whenever the menu opened on the MODS page. A thread of its own scans now; the page
// shows the last scan's findings, or "looking", until it is done.
void dolphinScanPoll() {
    if (s_dolphinJob.valid() && s_dolphinJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        s_dolphin = s_dolphinJob.get();
        s_dolphinHave = true;
    }
    if (s_dolphinStale && !s_dolphinJob.valid()) {
        s_dolphinStale = false;
        s_dolphinJob = std::async(std::launch::async, [] {
            const u64 t0 = tbNow();
            mods::DolphinScan s;
            try {
                s = mods::scanDolphin();
            } catch (const std::exception& e) {
                logf("menu", "Dolphin scan failed: %s", e.what());
            }
            logf("menu", "Dolphin scan: %.0f ms, %s, %zu saves, %zu texture packs, %zu codes", double(tbNow() - t0) * 1000.0 / double(kTbClock),
                 s.found ? "found" : "not found", s.saves.size(), s.packs.size(), s.codes.size());
            return s;
        });
    }
}
std::string s_modsStatus;          // the last import's outcome

std::string gigabytes(uint64_t b) {
    char t[32];
    // (binary units, as Explorer shows them)
    snprintf(t, sizeof t, b >= (1ull << 30) ? "%.1f GB" : "%.0f MB", b >= (1ull << 30) ? double(b) / double(1ull << 30) : double(b) / double(1ull << 20));
    return t;
}

// a folder in the system's file manager. Linux paths start with '/', so file:// + the path (it was
// "file:///" + "/home/...": four slashes); every byte but unreserved ones and separators is
// percent-encoded ("Red Two", non-ASCII names, '#', '%').
void openFolder(const std::string& dir) {
#ifdef _WIN32
    std::string url = "file:///";
#else
    std::string url = "file://";
#endif
    static const char kHex[] = "0123456789ABCDEF";
    for (const unsigned char c : dir) {
        if (c == '\\') url += '/';
        else if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') url += char(c);
        else {
            url += '%';
            url += kHex[c >> 4];
            url += kHex[c & 15];
        }
    }
    if (!SDL_OpenURL(url.c_str())) logf("menu", "cannot open %s: %s", url.c_str(), SDL_GetError());
    else logf("menu", "opened %s", url.c_str());
}

// text shortened with "..." to fit `w` pixels in the current font
std::string fitText(const std::string& text, float w) {
    if (ImGui::CalcTextSize(text.c_str()).x <= w) return text;
    std::string t = text;
    while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > w) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();  // (whole UTF-8 characters)
        if (!t.empty() && static_cast<unsigned char>(t.back()) >= 0xC0) t.pop_back();
    }
    return t + "...";
}

// --- choosing a texture pack folder ----------------------------------------------------------
// The MODS page's folder browser: a pack can stay where it was unpacked (~/Downloads, an SD card)
// instead of going into Red Two's own folder. It is part of the menu, driven like every page: a
// system file dialog or file manager does not show in the Steam Deck's Game Mode.
// A opens a folder (or picks "Use this folder"), X / Backspace goes up, B / Esc goes back.
struct BrowseEntry {
    std::string name, path;
    bool pack = false;  // named GSWE64 / GSW, or holding such a folder
    // the disc browser's image files: Rogue Leader (USA) or not, and what it is / why not
    bool file = false, ok = false;
    std::string help;
};
// what the browser picks: a texture pack folder (MODS) or the game disc image (DISPLAY's disc page)
enum class BrowseMode { Textures, Disc };
BrowseMode s_browseMode = BrowseMode::Textures;
int s_browseSection = 0;                 // the page it is shown on (Mods / Display)
bool s_browsing = false;
std::string s_browseDir;                 // the folder shown
std::string s_browseParent;              // its parent ("" at a root)
std::vector<BrowseEntry> s_browseList;   // its folders
std::vector<BrowseEntry> s_browsePlaces; // home, Downloads, SD cards / drives
std::string s_browseNote;                // what choosing it loads
std::string s_browseError;               // why it could not be listed
int s_browseReturnRow = 0;               // the page's row to go back to
bool s_browseLeft = false;               // left this frame: the page's rows were the browser's

std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string u8str(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
// without a trailing separator, but for a root ("/", "C:\")
std::filesystem::path tidyPath(const std::string& dir) {
    std::filesystem::path p = u8path(dir).lexically_normal();
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();
    return p;
}

std::vector<BrowseEntry> browsePlaces() {
    namespace fs = std::filesystem;
    std::vector<BrowseEntry> out;
    std::vector<std::string> seen;  // canonical paths (an SD card can be reached through a link too)
    const auto add = [&](const std::string& name, const char* dir) {
        std::error_code ec;
        if (!dir || !*dir || !fs::is_directory(u8path(dir), ec)) return;
        const fs::path p = tidyPath(dir);
        const std::string key = u8str(fs::weakly_canonical(p, ec));
        for (const auto& s : seen)
            if (s == key) return;
        seen.push_back(key);
        out.push_back({name, u8str(p), false});
    };
    add("Home", SDL_GetUserFolder(SDL_FOLDER_HOME));
    add("Downloads", SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS));
    add("Desktop", SDL_GetUserFolder(SDL_FOLDER_DESKTOP));
#ifdef _WIN32
    for (char d = 'C'; d <= 'Z'; d++) {
        const char root[4] = {d, ':', '\\', 0};
        add(std::string("Drive ") + d + ":", root);
    }
#else
    // SD cards and USB drives: SteamOS (udisks) mounts them in /run/media/<user>/<label>
    const char* user = getenv("USER");
    for (const std::string base : {std::string("/run/media/") + (user ? user : ""), std::string("/run/media")}) {
        std::error_code ec;
        for (fs::directory_iterator it(u8path(base), fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            const std::string name = u8str(it->path().filename());
            if (!it->is_directory(e2) || name == "root" || (user && name == user)) continue;  // (users' mount folders)
            add("SD card / drive: " + name, u8str(it->path()).c_str());
        }
    }
#endif
    add("Red Two's folder", mods::userDir().c_str());
    return out;
}

void browseOpen(const std::string& dir) {
    namespace fs = std::filesystem;
    const fs::path p = tidyPath(dir);
    s_browseDir = u8str(p);
    s_browseParent = p.has_relative_path() ? u8str(p.parent_path()) : std::string();
    s_browseList.clear();
    s_browseError.clear();
    std::error_code ec;
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        const std::string name = u8str(it->path().filename());
        if (name.empty() || name[0] == '.') continue;  // hidden
        const std::string path = u8str(it->path());
        if (!it->is_directory(e2)) {  // (links to folders count)
            // the disc browser: image files, each looked at (its header: which game it is)
            if (s_browseMode != BrowseMode::Disc || !it->is_regular_file(e2)) continue;
            std::string ext = u8str(it->path().extension());
            for (auto& ch : ext) ch = char(tolower(static_cast<unsigned char>(ch)));
            if (ext != ".iso" && ext != ".gcm" && ext != ".rvz" && ext != ".wia") continue;
            BrowseEntry e{name, path};
            e.file = true;
            std::string what;
            const std::string problem = discProblem(path, &what);
            e.ok = problem.empty();
            e.help = e.ok ? what : problem;
            s_browseList.push_back(std::move(e));
            continue;
        }
        if (s_browseMode == BrowseMode::Disc) {
            s_browseList.push_back({name, path});
            continue;
        }
        const auto kind = mods::textureFolderKind(path);
        s_browseList.push_back({name, path, kind == mods::FolderKind::Pack || kind == mods::FolderKind::HoldsPack});
    }
    if (ec) s_browseError = "Cannot read this folder: " + ec.message();
    // (disc images first: Rogue Leader before the rest)
    std::sort(s_browseList.begin(), s_browseList.end(), [](const BrowseEntry& a, const BrowseEntry& b) {
        if (a.file != b.file) return a.file;
        if (a.ok != b.ok) return a.ok;
        return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    s_rowSel[s_browseSection] = 0;
    s_scrollToSel = true;
    if (s_browseMode == BrowseMode::Disc) return;
    std::string pack;
    switch (mods::textureFolderKind(s_browseDir, &pack)) {
    case mods::FolderKind::Pack: s_browseNote = "This is a texture pack folder: its textures load."; break;
    case mods::FolderKind::HoldsPack: s_browseNote = "Holds a texture pack: " + pack + " loads."; break;
    default:
        s_browseNote = "No GSWE64 folder here: open your pack's folder, or use this one only if the pack's files are "
                       "inside it.";
        break;
    }
}

// the browser on a page (the folder given, else Downloads, else home)
void browseBegin(BrowseMode mode, int section, std::string start) {
    std::error_code ec;
    s_browseMode = mode;
    s_browseSection = section;
    s_browseReturnRow = s_rowSel[section];
    s_browsePlaces = browsePlaces();
    if (start.empty() || !std::filesystem::is_directory(u8path(start), ec)) {
        const char* dl = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
        const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
        start = dl && std::filesystem::is_directory(u8path(dl), ec) ? dl : home ? home : "/";
    }
    s_browsing = true;
    browseOpen(start);
}

void browseStart(const settings::Settings& s) { browseBegin(BrowseMode::Textures, Mods, s.texturesFolder); }

void browseClose() {
    if (!s_browsing) return;
    s_browsing = false;
    s_browseLeft = true;
    s_rowSel[s_browseSection] = s_browseReturnRow;
    s_scrollToSel = true;
}

// a full-width row: a name (shortened to fit) and a tag at the right; true when picked
bool folderRow(const char* text, const char* tag, const char* help, ImU32 tagCol = kText) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    const bool sel = rowBegin("", help);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##folder", ImVec2(rowW, h));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float tagW = ImGui::CalcTextSize(tag).x, pad = 20.f * s_ui;
    const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
    const std::string label = fitText(text, rowW - tagW - 3.f * pad);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + pad, ty), sel ? kGold : kText, label.c_str());
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + rowW - tagW - pad, ty), sel ? kGold : tagCol, tag);
    return clicked || (sel && s_in.accept);
}

// 1175 -> "1,175"
std::string withCommas(size_t n) {
    std::string d = std::to_string(n);
    for (int i = int(d.size()) - 3; i > 0; i -= 3) d.insert(size_t(i), ",");
    return d;
}

// The textures loaded from the folder chosen in the menu, and why there are none (mods' note)
size_t chosenPackTextures(std::string* note = nullptr) {
    size_t n = 0;
    for (const auto& p : mods::loadedPacks())
        if (p.chosen) {
            n += p.textures;
            if (note && note->empty()) *note = p.note;
        }
    return n;
}

void browseChoose(settings::Settings& s) {
    s.texturesFolder = s_browseDir;
    s.customTextures = true;  // (a folder chosen: textures wanted)
    s_changed = true;
    browseClose();
    mods::setTexturesEnabled(true);  // (loads Red Two's folder and the chosen one again)
    std::string note;
    const size_t n = chosenPackTextures(&note);
    s_modsStatus = n ? withCommas(n) + " replacement textures loaded from " + s.texturesFolder
                     : "No replacement textures in " + s.texturesFolder + (note.empty() ? "." : ": " + note + ".");
    logf("menu", "texture folder chosen: %s (%zu textures)", s.texturesFolder.c_str(), n);
}

// --- the game disc (DISPLAY: the launcher's first page) ------------------------------------
// The disc image the game boots from: --disc, else the one chosen here before (setting
// disc_path), else one found next to the program or in the user folder (main.cpp). With none (or
// one that is not Rogue Leader, USA, revision 0) the launcher opens on the disc page and the game
// cannot start until one is chosen: a path typed or pasted, the system's file dialog, or the
// browser (folders and disc images; it works with a controller and in the Steam Deck's Game
// Mode, where no system dialog shows). In game a new choice applies from the next start.
std::string s_discPath;     // the disc in use (the launcher: the one the game will start with)
std::string s_discWhat;     // what it is (discProblem)
std::string s_discProblem;  // why s_discPath cannot be used ("" if it can)
std::string s_discNote;     // why the launcher asks (main.cpp: a chosen disc that went away, ...)
std::string s_discError;    // the last choice that was refused, and why
std::string s_discEdit;     // the path row's text
bool s_discPage = false;    // the disc page instead of DISPLAY's rows
bool s_discChanged = false; // chosen in game: for the next start
int s_discReturnRow = 0;

bool discReady() { return !s_discPath.empty() && s_discProblem.empty(); }

void discPageOpen() {
    if (s_discPage) return;
    s_discReturnRow = s_rowSel[Display];
    s_discPage = true;
    s_discError.clear();
    s_discEdit = s_discPath;
    s_rowSel[Display] = 0;
    s_scrollToSel = true;
    s_section = Display;
}

void discPageClose() {
    s_discPage = false;
    s_browseLeft = true;  // (DISPLAY's own rows again next frame)
    s_rowSel[Display] = s_discReturnRow;
    s_scrollToSel = true;
}

// a path the player chose: kept when it is the disc (and remembered for the next starts)
bool discChoose(const std::string& path) {
    std::string what;
    std::string p = path;
    while (!p.empty() && (p.back() == ' ' || p.back() == '"')) p.pop_back();  // (a pasted "quoted path")
    while (!p.empty() && (p.front() == ' ' || p.front() == '"')) p.erase(p.begin());
    const std::string problem = discProblem(p, &what);
    if (!problem.empty()) {
        s_discError = p + ": " + problem;
        logf("menu", "disc refused: %s", s_discError.c_str());
        return false;
    }
    s_discError.clear();
    const bool changed = p != s_discPath;
    s_discPath = p;
    s_discWhat = what;
    s_discProblem.clear();
    s_discNote.clear();
    s_discEdit = p;
    settings::get().discPath = p;
    settings::save();
    if (changed && !s_launcher) s_discChanged = true;
    logf("menu", "disc chosen: %s (%s)", p.c_str(), what.c_str());
    if (s_launcher) s_rowSel[Display] = 0;  // (Start game, at the top of the disc page)
    return true;
}

// the system's file dialog (SDL_ShowOpenFileDialog: Windows' own, a desktop portal on Linux):
// its answer comes on another thread
std::mutex s_dialogMutex;
bool s_dialogOpen = false, s_dialogDone = false, s_dialogFailed = false;
std::string s_dialogFile, s_dialogErrorText;

void discDialogOpen() {
    if (s_dialogOpen) return;
    static const SDL_DialogFileFilter filters[] = {{"GameCube disc images (.iso, .gcm, .rvz, .wia)", "iso;gcm;rvz;wia"},
                                                   {"All files", "*"}};
    std::string start;
    if (!s_discPath.empty()) start = u8str(u8path(s_discPath).parent_path());
    if (start.empty()) {
        const char* dl = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
        start = dl ? dl : "";
    }
    s_dialogOpen = true;
    logf("menu", "disc: file dialog");
    SDL_ShowOpenFileDialog(
        [](void*, const char* const* files, int) {
            std::lock_guard lk(s_dialogMutex);
            s_dialogDone = true;
            s_dialogFailed = files == nullptr;
            if (!files) s_dialogErrorText = SDL_GetError();
            else s_dialogFile = files[0] ? files[0] : "";
        },
        nullptr, aurora_get_window(), filters, 2, start.empty() ? nullptr : start.c_str(), false);
}

void discDialogPoll() {
    std::string file;
    {
        std::lock_guard lk(s_dialogMutex);
        if (!s_dialogDone) return;
        s_dialogDone = s_dialogOpen = false;
        if (s_dialogFailed) {
            s_discError = "No file dialog here (" + s_dialogErrorText + "): use Find it below.";
            logf("menu", "disc: file dialog failed: %s", s_dialogErrorText.c_str());
            return;
        }
        file = s_dialogFile;
    }
    if (!file.empty()) discChoose(file);  // (empty: canceled)
}

// a disc drawn next to the status: gold when ready
void drawDiscIcon(ImDrawList* dl, ImVec2 c, float r, bool ready) {
    const ImU32 col = ready ? kGold : alpha(kDim, 160);
    dl->AddCircleFilled(c, r, ready ? IM_COL32(40, 34, 18, 255) : IM_COL32(20, 26, 40, 255), 48);
    for (int i = 1; i <= 3; i++) dl->AddCircle(c, r * (0.45f + 0.17f * float(i)), alpha(col, 40 + 25 * i), 48, 1.f);
    dl->PathArcTo(c, r * 0.82f, -2.4f, -1.3f, 16);
    dl->PathStroke(alpha(kText, ready ? 120 : 50), 0, 3.f * s_ui);  // (a highlight)
    dl->AddCircle(c, r, col, 48, 2.f * s_ui);
    dl->AddCircleFilled(c, r * 0.18f, IM_COL32(8, 12, 22, 255), 24);
    dl->AddCircle(c, r * 0.3f, alpha(col, 160), 24, 1.5f * s_ui);
}

void pageDisc(settings::Settings& s) {
    discDialogPoll();
    const bool ready = discReady();
    heading("GAME DISC");
    // the status: a disc, READY / what is wrong, what it is and where
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 118.f * s_ui, r = 42.f * s_ui;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 18, 34, 200), 6.f * s_ui);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), alpha(ready ? kGold : kAmber, ready ? 110 : 70), 6.f * s_ui, 0, 1.f);
        drawDiscIcon(dl, ImVec2(p.x + 20.f * s_ui + r, p.y + h * 0.5f), r, ready);
        const float tx = p.x + 40.f * s_ui + 2.f * r, tw = w - (tx - p.x) - 16.f * s_ui;
        const char* title = ready ? "READY" : s_discPath.empty() ? "NO DISC IMAGE YET" : "THIS IS NOT THE DISC";
        drawText(dl, s_fontBig ? s_fontBig : s_font, 30.f * s_ui, ImVec2(tx, p.y + 14.f * s_ui), ready ? kGold : kAmber,
                 title, 3.f * s_ui);
        const std::string line1 = ready ? s_discWhat
                                  : s_discPath.empty() ? std::string(loc("Choose your Rogue Leader disc image below."))
                                                       : s_discProblem;
        const float px1 = ImGui::GetFontSize() * 0.8f, px2 = ImGui::GetFontSize() * 0.7f;
        dl->AddText(ImGui::GetFont(), px1, ImVec2(tx, p.y + 56.f * s_ui), kText,
                    fitText(line1.c_str(), tw * ImGui::GetFontSize() / px1).c_str());
        dl->AddText(ImGui::GetFont(), px2, ImVec2(tx, p.y + 84.f * s_ui), kDim,
                    fitText(s_discPath.c_str(), tw * ImGui::GetFontSize() / px2).c_str());
        ImGui::Dummy(ImVec2(w, h + 6.f * s_ui));
    }
    const std::string message = !s_discError.empty() ? s_discError
                                : s_discChanged     ? std::string("Red Two starts with this disc from the next start "
                                                                  "(Back to launcher restarts now).")
                                                    : s_discNote;
    if (!message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, vec(!s_discError.empty() ? kAmber : kHolo));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }
    if (s_launcher && ready &&
        buttonRow("Start game", "LAUNCH", "Start Rogue Leader with this disc. The controller's START button does this "
                                          "from any page.", kAmber)) {
        logf("menu", "launcher: start (disc page)");
        s_launch = true;
    }
    if (textRow("Path", s_discEdit, false,
                "Type or paste the full path of your disc image (Ctrl+V), then Enter. Quotes around it are fine."))
        discChoose(s_discEdit);
    if (buttonRow("Choose the file", s_dialogOpen ? "OPEN..." : "BROWSE",
                  "Pick the image in your system's file dialog. On a Steam Deck in Game Mode use Find it below."))
        discDialogOpen();
    if (buttonRow("Find it", "OPEN", "Look through your folders here, with the controller or the keyboard: Rogue Leader "
                                     "images are marked."))
        browseBegin(BrowseMode::Disc, Display, s_discPath.empty() ? std::string() : u8str(u8path(s_discPath).parent_path()));
    if ((!s_launcher || ready) &&
        buttonRow(s_launcher ? "Other settings" : "Back", "DONE", "Back to the rest of this page (B / Esc too)."))
        discPageClose();
    languageRow(s);
    ImGui::Dummy(ImVec2(0, 8.f * s_ui));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(kDim));
    ImGui::TextWrapped("%s", loc("Red Two plays from your own disc image of Star Wars: Rogue Squadron II - Rogue Leader for the "
                       "GameCube (USA, GSWE64, revision 0): an .iso, .gcm, .rvz or .wia file, as Dolphin uses them. "
                       "Choose it once; Red Two remembers where it is."));
    ImGui::PopStyleColor();
}

bool discPageShown() { return s_discPage; }
bool discBrowserShown() { return s_browsing && s_browseMode == BrowseMode::Disc; }

// the GAME row's value: the image's file name (or what is wrong)
std::string discRowText() {
    if (s_discPath.empty()) return "CHOOSE";
    if (!s_discProblem.empty()) return "NOT USABLE: CHOOSE";
    const std::string name = u8str(u8path(s_discPath).filename());
    return fitText(name, 330.f * s_ui);
}

// main.cpp (through gx_bridge.cpp): the disc the game starts with, and why the launcher asks for one
void discSet(const std::string& path, const std::string& note) {
    s_discPath = path;
    s_discNote = note;
    s_discWhat.clear();
    s_discProblem = path.empty() ? std::string() : discProblem(path, &s_discWhat);
    s_discEdit = path;
}

// B / Esc inside the menu: out of the folder browser, then off the disc page (true: handled)
bool menuStepBack() {
    if (s_browsing) {
        browseClose();
        return true;
    }
    if (s_discPage && (!s_launcher || discReady())) {
        discPageClose();
        return true;
    }
    return false;
}

void pageBrowse(settings::Settings& s) {
    const bool disc = s_browseMode == BrowseMode::Disc;
    heading(disc ? "FIND YOUR DISC IMAGE" : "CHOOSE A TEXTURE PACK FOLDER");
    ImGui::PushStyleColor(ImGuiCol_Text, vec(kHolo));
    ImGui::TextWrapped("%s", s_browseDir.c_str());
    ImGui::PopStyleColor();
    std::string go;  // (opened after the rows: they read the list)
    if (!disc && folderRow("Use this folder", "SELECT", s_browseNote.c_str(), kAmber)) {
        browseChoose(s);
        return;
    }
    if (!s_browseParent.empty() && folderRow(".. (parent folder)", "UP", "Back up one folder (X / Backspace too).")) go = s_browseParent;
    size_t images = 0;
    for (const BrowseEntry& e : s_browseList) images += e.file;
    if (disc) {
        heading("DISC IMAGES");
        if (!images) ImGui::TextDisabled("No .iso, .gcm, .rvz or .wia files here.");
        if (!s_discError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, vec(kAmber));
            ImGui::TextWrapped("%s", s_discError.c_str());
            ImGui::PopStyleColor();
        }
    }
    for (size_t i = 0; i < s_browseList.size(); i++) {
        const BrowseEntry& e = s_browseList[i];
        if (!e.file) continue;
        ImGui::PushID(int(i));
        const bool picked =
            folderRow(e.name.c_str(), e.ok ? "ROGUE LEADER" : "NOT THIS ONE", e.help.c_str(), e.ok ? kAmber : kDim);
        ImGui::PopID();
        if (picked && discChoose(e.path)) {
            browseClose();
            return;
        }
    }
    heading("FOLDERS");
    if (s_browseList.size() == images)
        ImGui::TextDisabled("%s", s_browseError.empty() ? "No folders here." : s_browseError.c_str());
    for (size_t i = 0; i < s_browseList.size(); i++) {
        const BrowseEntry& e = s_browseList[i];
        if (e.file) continue;
        ImGui::PushID(int(i));
        if (folderRow(e.name.c_str(), e.pack ? "TEXTURE PACK" : "OPEN",
                      e.pack ? "A Rogue Leader texture pack (GSWE64): open it, then Use this folder." : "Open this folder.",
                      e.pack ? kAmber : kDim))
            go = e.path;
        ImGui::PopID();
    }
    heading("PLACES");
    for (size_t i = 0; i < s_browsePlaces.size(); i++) {
        const BrowseEntry& e = s_browsePlaces[i];
        ImGui::PushID(int(1000 + i));
        if (folderRow(e.name.c_str(), "GO", e.path.c_str(), kDim)) go = e.path;
        ImGui::PopID();
    }
    if (s_in.clear && !s_browseParent.empty()) go = s_browseParent;
    if (!go.empty()) browseOpen(go);
}

// a texture pack copy finished (the menu may be closed by then): load what it brought
void modsPoll() {
    mods::CopyProgress& copy = mods::packCopy();
    if (!copy.done.exchange(false)) return;
    s_modsStatus = copy.message;
    if (!copy.failed) mods::reloadTextures();
    s_dolphinStale = true;  // (now up to date)
}

void pageMods(settings::Settings& s) {
    dolphinScanPoll();
    modsPoll();
    if (s_browsing && s_browseMode == BrowseMode::Textures) {
        pageBrowse(s);
        return;
    }
    const mods::CopyProgress& copy = mods::packCopy();
    // the last import's / folder choice's outcome (also when Dolphin is not there: the page ends early then)
    const auto status = [] {
        if (s_modsStatus.empty()) return;
        ImGui::Dummy(ImVec2(0, 4.f * s_ui));
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kHolo));
        ImGui::TextWrapped("%s", s_modsStatus.c_str());
        ImGui::PopStyleColor();
    };

    heading("TEXTURE PACKS");
    {
        const auto& packs = mods::loadedPacks();
        std::string help;
        if (!s.customTextures)
            help = "Replacement textures in Dolphin's format: from Red Two's textures folder and the texture pack folder below.";
        else if (packs.empty()) help = "No texture pack loaded: choose your pack's folder below (Texture pack folder).";
        else help = std::to_string(mods::loadedTextureCount()) + " replacement textures loaded (Dolphin's format).";
        if (s.customTextures && !packs.empty())
            help += mods::texturesHidden() ? " HIDDEN by the texture pack hotkey: press it again to show them."
                                           : " The texture pack hotkey (" + hotkeys::label(settings::HkToggleTextures, true) + " / " +
                                                 hotkeys::label(settings::HkToggleTextures, false) +
                                                 ", Controls page) hides and shows them.";
        if (toggleRow("Custom textures", s.customTextures, help.c_str())) {
            s_changed = true;
            mods::setTexturesEnabled(s.customTextures);
        }
    }
    if (toggleRow("Prefetch textures", s.prefetchTextures,
                  s_launcher ? "Loads the texture packs into memory before the game starts (up to half the RAM, as "
                               "Dolphin): textures appear sharp the first time."
                             : "Loads the texture packs into memory before the game starts (up to half the RAM, as "
                               "Dolphin): textures appear sharp the first time. Takes effect at the next start.")) {
        s_changed = true;
        if (s_launcher) mods::reloadTextures();  // (the game has not started: this start gets it)
    }
    {  // a pack wherever it was unpacked
        const int row = s_row;
        std::string caption = "CHOOSE", help;
        if (s.texturesFolder.empty()) {
            help = "Pick the folder of a texture pack you downloaded, e.g. in Downloads: the one holding GSWE64, or "
                   "GSWE64 itself. No need to copy it anywhere.";
        } else {
            // the folder's name and what loaded from it, on the row itself (issue #42: the count
            // was only in this row's description, and NONE FOUND further down - Dolphin's import -
            // read as "your pack was not found")
            std::string note;
            const size_t n = chosenPackTextures(&note);
            std::error_code ec;
            const bool there = std::filesystem::is_directory(u8path(s.texturesFolder), ec);
            const std::string count = !there ? "NOT FOUND" : !s.customTextures ? "" : n ? withCommas(n) + " TEXTURES" : "NO TEXTURES";
            const std::string name = tidyPath(s.texturesFolder).filename().empty() ? s.texturesFolder
                                                                                   : u8str(tidyPath(s.texturesFolder).filename());
            caption = count.empty() ? fitText(name, 360.f * s_ui) : fitText(name, 200.f * s_ui) + "  -  " + count;
            std::string what;
            if (!there) what = ": NOT FOUND.";
            else if (s.customTextures) what = n ? ": " + withCommas(n) + " textures loaded." : ": no textures loaded" + (note.empty() ? std::string(".") : " - " + note + ".");
            help = s.texturesFolder + what +
                   (s_usingPad ? " A: choose another, X: stop using it." : " Enter: choose another, Delete: stop using it.");
        }
        if (buttonRow("Texture pack folder", caption.c_str(), help.c_str(), s.texturesFolder.empty() ? kText : kAmber)) {
            browseStart(s);
            return;
        }
        if (s_rowSel[s_section] == row && s_in.clear && !s.texturesFolder.empty()) {
            logf("menu", "texture folder forgotten: %s", s.texturesFolder.c_str());
            s.texturesFolder.clear();
            s_changed = true;
            s_modsStatus.clear();
            mods::reloadTextures();
        }
    }
    std::string ownHelp =
        "Opens Red Two's own textures folder in the file manager (packs go in a GSWE64 folder inside, as in Dolphin). "
        "Not shown in the Steam Deck's Game Mode: use Texture pack folder there.";
    if (s.customTextures) {  // what loaded from it
        const std::string own = mods::texturesDir();
        size_t n = 0, packs = 0;
        std::string note;
        for (const auto& p : mods::loadedPacks())
            if (p.path.size() >= own.size() && SDL_strncasecmp(p.path.c_str(), own.c_str(), own.size()) == 0) {
                packs++;
                n += p.textures;
                if (note.empty()) note = p.note;
            }
        ownHelp += !packs ? " Nothing in it yet (no GSWE64 folder)."
                   : n    ? " " + withCommas(n) + " textures loaded from it."
                          : " No textures loaded from it" + (note.empty() ? std::string(".") : " - " + note + ".");
    }
    if (buttonRow("Red Two's textures folder", "OPEN", ownHelp.c_str())) {
        const std::string dir = mods::texturesDir();  // (one string: begin and end of the same one)
        std::error_code ec;
        std::filesystem::create_directories(u8path(dir), ec);
        if (ec) logf("menu", "cannot create %s: %s", dir.c_str(), ec.message().c_str());
        openFolder(dir);
    }

    heading("CODES");
    const auto list = mods::codesSnapshot();
    if (list.empty())
        ImGui::TextDisabled("None yet: import Dolphin's below, or add Gecko / Action Replay codes to codes.ini.");
    for (size_t i = 0; i < list.size(); i++) {
        const mods::Code& c = list[i];
        ImGui::PushID(int(i));
        static const char* kinds[] = {"Gecko code", "Action Replay code", "Patch"};
        std::string help = std::string(kinds[int(c.kind)]) + (c.creator.empty() ? "" : " by " + c.creator) +
                           (c.notes.empty() ? "." : ": " + c.notes);
        if (c.supported) {
            bool v = c.enabled;
            if (toggleRow(c.name.c_str(), v, help.c_str())) mods::setCodeEnabled(i, v);
        } else {
            buttonRow(c.name.c_str(), "NOT SUPPORTED", c.why.c_str(), kDim);
        }
        ImGui::PopID();
    }

    heading("IMPORT FROM DOLPHIN");
    if (!s_dolphinHave) {
        ImGui::TextDisabled("Looking for Dolphin...");
        status();
        return;
    }
    if (!s_dolphin.found) {
        ImGui::TextDisabled("Dolphin was not found on this PC.");
        status();
        return;
    }
    {  // save
        const bool have = !s_dolphin.saves.empty();
        std::string help = "No Rogue Leader save in Dolphin's memory cards.";
        if (have) {
            const mods::DolphinSave& sv = s_dolphin.saves.front();
            help = "Saved " + sv.when + " in " + sv.where + ". Replaces Red Two's save (old card kept in saves\\backup), restarts.";
        }
        const int row = s_row;
        const bool pressed = buttonRow("Save data", !have ? "NOT IN DOLPHIN" : s_confirmSave ? "PRESS AGAIN: REPLACE" : "IMPORT",
                                       help.c_str(), have ? (s_confirmSave ? kAmber : kText) : kDim);
        if (s_rowSel[s_section] != row) s_confirmSave = false;
        if (pressed && have) {
            if (!s_confirmSave) {
                s_confirmSave = true;
            } else {
                s_confirmSave = false;
                std::string msg;
                if (mods::importSave(s_dolphin.saves.front(), msg)) {
                    restartGame(s_launcher);  // (returns only if it could not start the new instance)
                    msg += " Restart Red Two to play with it.";
                }
                s_modsStatus = msg;
            }
        }
    }
    {  // texture packs
        const bool have = !s_dolphin.packs.empty();
        size_t files = 0;
        uint64_t bytes = 0;
        for (const auto& p : s_dolphin.packs) files += p.files, bytes += p.bytes;
        bool imported = have;
        for (const auto& p : s_dolphin.packs) imported &= p.imported;
        std::string caption = !have ? "NOT IN DOLPHIN" : imported ? "UP TO DATE" : "IMPORT";
        if (copy.running) {
            const uint64_t t = copy.bytesTotal, d = copy.bytesDone;
            caption = "COPYING " + std::to_string(t ? int(d * 100 / t) : 0) + "%";
        }
        std::string help = "Dolphin's Load\\Textures folder has no Rogue Leader pack to copy. This row only imports "
                           "from Dolphin: a pack in Red Two's textures folder or the Texture pack folder above loads "
                           "without it.";
        if (have)
            help = std::to_string(files) + " files (" + gigabytes(bytes) + ")" + (imported ? ", already copied from " : ", copied from ") +
                   s_dolphin.packs.front().path;
        if (buttonRow("Texture pack", caption.c_str(), help.c_str(), have ? (copy.running ? kAmber : kText) : kDim) && have &&
            !copy.running) {
            s_modsStatus.clear();
            mods::startPackImport(s_dolphin.packs);
        }
    }
    {  // codes
        const bool have = !s_dolphin.codes.empty();
        size_t on = 0;
        for (const auto& c : s_dolphin.codes) on += c.enabled;
        const size_t n = s_dolphin.codes.size();
        const std::string help = have ? std::to_string(n) + (n == 1 ? " code" : " codes") + " for this game in Dolphin (" +
                                            std::to_string(on) + " enabled there), kept on or off as they are there."
                                      : "Dolphin has no codes for Rogue Leader.";
        if (buttonRow("Codes", have ? "IMPORT" : "NOT IN DOLPHIN", help.c_str(), have ? kText : kDim) && have) {
            std::string msg;
            mods::importCodes(s_dolphin, msg);
            s_modsStatus = msg;
        }
    }
    status();
}

void pageControls() {
    const auto rows = controlRows();
    finishCapture(rows);
    if (s_pendingCapture >= 0 && !acceptHeld()) {
        startCapture(s_pendingCapture);
        s_pendingCapture = -1;
    }
    if (s_pendingHotkey >= 0 && !acceptHeld()) {
        hotkeys::captureStart(s_pendingHotkey, s_padDevice);
        s_pendingHotkey = -1;
    } else if (hotkeys::capturing() &&
               (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        hotkeys::captureCancel();  // a click elsewhere
    }
    hotkeys::captureTick();
    if (s_hotkeyWasCapturing && !hotkeys::capturing()) s_navBackTb = tbNow() + kTbClock / 4;  // (its release is no menu press)
    s_hotkeyWasCapturing = hotkeys::capturing();
    heading("CONTROL STICK");
    {
        settings::Settings& st = settings::get();
        if (percentRow("Sensitivity", st.stickSensitivity,
                       "How much the control stick (your left stick) steers: below 100% finer aiming (full "
                       "deflection is never reached), above it full deflection comes sooner. The meter below "
                       "shows the result.",
                       25, 200, 5))
            s_changed = true;
        if (enumRow("Response", st.stickCurve, stickCurveLabel,
                    "How fast the craft turns for how far the stick goes. Smooth: gentle near the center, building "
                    "up evenly to full speed. Linear: in proportion to the stick. GameCube: the game's own curve, "
                    "half speed up to 80% of the stick, then a jump to full."))
            s_changed = true;
        if (toggleRow("Invert Y axis", st.stickInvertY,
                      "Off: pushing the control stick forward dives, as on the GameCube. On: forward climbs and "
                      "back dives, like a plane's control column. Flight only: the menus keep the stick as it is."))
            s_changed = true;
        if (percentRow("Deadzone", st.stickDeadzone,
                       "How far the control stick moves before anything happens (round, and the output grows from "
                       "zero at its edge). Raise it if the craft drifts with the stick let go.",
                       0, 50, 1))
            s_changed = true;
        stickMeter(true);
    }
    heading("C-STICK");
    {
        settings::Settings& st = settings::get();
        if (percentRow("C-stick sensitivity", st.cStickSensitivity,
                       "How much the C-stick (your right stick) moves: below 100% it never reaches the edge, above "
                       "it the edge comes sooner. The meter below shows the result.",
                       25, 200, 5))
            s_changed = true;
        if (percentRow("C-stick deadzone", st.cStickDeadzone,
                       "How far the C-stick moves before anything happens (round, and the output grows from zero at "
                       "its edge).",
                       0, 50, 1))
            s_changed = true;
        stickMeter(false);
    }
    heading("TRIGGERS");
    if (toggleRow("Trigger clicks on LB / RB", settings::get().bumperClicks,
                  "A GameCube trigger clicks at the end of its pull: full boost (wings closed, no firing) or full brake. "
                  "On: the triggers only throttle and brake, LB / RB click (as players set up in Dolphin), and Z moves "
                  "from RB to the right stick click. Changes the controller's bindings below."))
        s_changed = true;
    triggerMeter();
    heading("ON-SCREEN BUTTONS");
    {
        settings::Settings& st = settings::get();
        const bool sel = rowBegin("Button prompts", "The buttons the game shows in its prompts (the button each GameCube "
                                                    "button is bound to): Automatic shows PlayStation symbols with a "
                                                    "PlayStation controller and Xbox buttons otherwise; or the original GameCube ones.");
        static const char* const kNames[] = {"AUTOMATIC", "XBOX", "PLAYSTATION", "GAMECUBE"};
        constexpr int n = int(settings::ButtonPrompts::Count);
        static_assert(std::size(kNames) == n);
        const int cur = int(st.buttonPrompts);
        const int next = choiceStep(cur, n, selector("prompts", kNames[cur], cur > 0, cur < n - 1, sel), sel);
        if (next != cur) {
            st.buttonPrompts = settings::ButtonPrompts(next);
            s_changed = true;
            buttonIconsUpdate();
        }
    }
    heading("GYRO AIMING");
    {
        settings::Settings& st = settings::get();
        const std::string pad = gyro::controllerName();
        std::string help = "Turn and tilt the controller to steer the craft in flight, on top of the stick (menus, the pause "
                           "screen and cutscenes are left alone). ";
        help += pad.empty() ? "No controller with a gyro found (a Steam Deck, DualSense, DualShock 4 or Switch Pro "
                              "Controller has one; Xbox controllers do not). Steam Input hides it: turn Steam Input off "
                              "for Red Two (Properties > Controller)."
                            : "Gyro: " + pad + ".";
        if (enumRow("Gyro aiming", st.gyroMode, settings::gyroModeName, help.c_str())) s_changed = true;
    }
    {
        settings::Settings& st = settings::get();
        const bool sel = rowBegin("Gyro button", "The button for While held, Button toggles and Off while held, from those "
                                                 "the gyro's controller has. Touches keep the gyro on while a finger rests: "
                                                 "the Steam Deck's left stick touch (the stick you fly with), a DualSense's "
                                                 "or DualShock 4's touchpad.");
        // the buttons the controller has (all of them while none is found), in kButtons' order
        const u32 avail = gyro::availableButtons();
        const int eff = gyro::effectiveButton();  // the setting's, or what stands in for it
        int list[gyro::kButtonCount], n = 0, shown = -1;
        for (int i = 0; i < gyro::kButtonCount; i++)
            if (avail >> i & 1) {
                if (i == eff) shown = n;
                list[n++] = i;
            }
        const char* label = eff >= 0 ? gyro::kButtons[eff].label : st.gyroButton.c_str();  // (one set in settings.ini)
        const int cur = std::max(shown, 0);
        const int next = n ? choiceStep(cur, n, selector("gyrobutton", label, cur > 0, cur < n - 1, sel), sel) : cur;
        if (n && (next != cur || (shown < 0 && sel && (s_in.dx || s_in.accept)))) {
            st.gyroButton = gyro::kButtons[list[next]].sdlName;
            s_changed = true;
        }
    }
    if (gyroSpeedRow("Gyro turning", settings::get().gyroSpeedX,
                     "How far the craft turns left / right for a turn of the controller: 100% as far as the controller "
                     "(up to the craft's own limit)."))
        s_changed = true;
    if (gyroSpeedRow("Gyro pitch", settings::get().gyroSpeedY,
                     "How far the nose goes up / down for a tilt of the controller: 100% as far as the controller."))
        s_changed = true;
    if (toggleRow("Invert gyro pitch", settings::get().gyroInvertY,
                  "Off: tilting the top of the controller toward you raises the nose. On: it lowers it."))
        s_changed = true;
    heading("SAVE STATES");
    {
        settings::Settings& st = settings::get();
        const bool sel = rowBegin("State slot", "The slot the save state hotkeys use (the next / previous slot hotkeys "
                                                "change it too). States are kept in your Red Two folder, states.");
        const std::string info = state::slotInfo(st.stateSlot);
        char buf[64];
        snprintf(buf, sizeof buf, "SLOT %d   %s", st.stateSlot, info.empty() ? "EMPTY" : info.c_str());
        const int cur = st.stateSlot - 1;
        const int next = choiceStep(cur, state::kSlots, selector("slot", buf, cur > 0, cur < state::kSlots - 1, sel), sel);
        if (next != cur) {
            st.stateSlot = next + 1;
            s_changed = true;
        }
    }
    if (buttonRow("Save state now", "SAVE", "Saves the game as it is right now into the slot."))
        state::request(state::Op::Save, settings::get().stateSlot);
    if (buttonRow("Load state now", "LOAD", "Puts the game back the way it was saved in the slot."))
        state::request(state::Op::Load, settings::get().stateSlot);
    {  // both devices' hotkeys at a glance (the HOTKEYS rows below show and change one device's)
        const std::string padSave = hotkeys::label(settings::HkSaveState, true),
                          padLoad = hotkeys::label(settings::HkLoadState, true),
                          keySave = hotkeys::label(settings::HkSaveState, false),
                          keyLoad = hotkeys::label(settings::HkLoadState, false);
        ImGui::PushStyleColor(ImGuiCol_Text, vec(kHolo));
        ImGui::Text("   Controller: %s saves, %s loads        Keyboard: %s saves, %s loads", padSave.c_str(), padLoad.c_str(),
                    keySave.c_str(), keyLoad.c_str());
        ImGui::PopStyleColor();
    }
    heading("INPUT DEVICE");
    {
        const bool sel = rowBegin("Remap", "Which device's bindings this page shows and changes.");
        const int step = selector("device", s_padDevice ? "CONTROLLER" : "KEYBOARD", s_padDevice, !s_padDevice, sel);
        if (step || (sel && (s_in.dx || s_in.accept)) ) {
            const bool want = step ? step > 0 : (s_in.dx ? s_in.dx > 0 : !s_padDevice);
            s_padDevice = want;
        }
    }
    const bool noPad = s_padDevice && !aurora::pad::port_controller(0);
    if (buttonRow("Reset to defaults", "RESET", "Puts back the original bindings and hotkeys of the device shown.") &&
        !noPad) {
        aurora::pad::reset_to_defaults(profileTarget());
        aurora::pad::save_profiles();
        hotkeys::resetDefaults(s_padDevice);
    }
    if (noPad) {
        ImGui::TextDisabled("No controller connected.");
        return;
    }
    if (s_padDevice) {
        SDL_Gamepad* pad = targetGamepad();
        const char* name = pad ? SDL_GetGamepadName(pad) : nullptr;
        ImGui::TextDisabled("%s", name ? name : "Controller");
    }
    heading("HOTKEYS");
    static const char* const kHotkeyNames[settings::HkCount] = {"Save state", "Load state", "Next slot", "Previous slot",
                                                                "Texture packs on / off"};
    for (int a = 0; a < settings::HkCount; a++) {
        ImGui::PushID(100 + a);
        const bool capturing = hotkeys::captureAction() == a || s_pendingHotkey == a;
        const std::string text = capturing ? std::string("HOLD, THEN LET GO...")
                                           : hotkeys::label(a, s_padDevice);
        const bool idle = !rebindingNow();
        const std::string padHelp = std::string("Press ") + padName("A", "Cross") +
                                    " (or click), then hold the buttons together - " + padBackName(false) + " + " + padName("RB", "R1") +
                                    " - and let go. " + padName("X", "Square") + " or a right-click clears it.";
        const bool pressed = buttonRow(kHotkeyNames[a], text.c_str(),
                                       s_padDevice ? padHelp.c_str()
                                                   : "Press Enter (or click), then hold the keys together and let go. Delete "
                                                     "or a right-click clears it. Esc cancels.",
                                       capturing ? kGold : kText);
        const bool sel = s_rowSel[s_section] == s_row - 1;
        if (pressed && idle) s_pendingHotkey = a;  // listens once the click / A / Enter is released
        if (idle && (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (sel && s_in.clear))) hotkeys::clear(a, s_padDevice);
        ImGui::PopID();
    }
    heading("GAMECUBE CONTROLLER");
    for (int i = 0; i < int(rows.size()); i++) {
        const ControlRow& row = rows[size_t(i)];
        ImGui::PushID(i);
        const bool capturing = s_captureRow == i || s_pendingCapture == i;
        const std::string text = capturing ? std::string("PRESS A KEY OR BUTTON...") : bindingText(row);
        const bool idle = s_captureRow < 0 && s_pendingCapture < 0 && s_pendingHotkey < 0 && !hotkeys::capturing();
        const std::string help = std::string("Press ") + padName("A", "Cross") +
                                 " / Enter (or click), then the new key or button. " + padName("X", "Square") +
                                 " / Delete or a right-click clears it. " + padBackName(false) + " / Esc cancels.";
        const bool pressed = buttonRow(row.name, text.c_str(), help.c_str(), capturing ? kGold : kText);
        const bool sel = s_rowSel[s_section] == s_row - 1;
        if (pressed && idle) s_pendingCapture = i;  // listens once the click / A / Enter is released
        if (idle && (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (sel && s_in.clear))) clearRow(row);
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------------------
void switchSection(int i) {
    if (i == s_section) return;
    browseClose();
    s_section = i;
    s_scrollToSel = true;
    s_confirmQuit = false;
    s_confirmReset = false;
    s_confirmLauncher = false;
    s_textEditing = false;
    s_pendingCapture = -1;
    s_pendingHotkey = -1;
    hotkeys::captureCancel();
}

// the controller's Back: down now, and whether another button went down meanwhile (menuEvent)
bool s_backDown = false, s_backChord = false;
// a short message at the top of the screen (save states)
std::string s_toast;
u64 s_toastTb = 0;

// a small key cap: the button that does something here
void keyCap(ImDrawList* dl, ImVec2 c, const char* text, bool lit) {
    const float px = 17.f * s_ui;
    const float tw = textWidth(s_font, px, text, 1.f * s_ui);
    const ImVec2 half(std::max(tw * 0.5f + 10.f * s_ui, 20.f * s_ui), 15.f * s_ui);
    const ImVec2 a(c.x - half.x, c.y - half.y), b(c.x + half.x, c.y + half.y);
    dl->AddRectFilled(a, b, lit ? alpha(kAmber, 200) : IM_COL32(10, 22, 44, 220), 6.f * s_ui);
    dl->AddRect(a, b, lit ? kGold : alpha(kGold, 140), 6.f * s_ui, 0, 1.5f * s_ui);
    drawText(dl, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.55f), lit ? IM_COL32(20, 12, 0, 255) : kGold, text, 1.f * s_ui);
}

// the tabs: across the top, LB / RB (Q / E) caps at the ends
// one of `n` labelled values: the index of the one nearest `v` is shown; true when another was
// chosen (v takes its value)
template <typename T>
bool valueRow(const char* text, T& v, const T* values, const char* const* labels, int n, const char* help) {
    const bool sel = rowBegin(text, help);
    int cur = 0;
    for (int i = 1; i < n; i++)
        if (std::abs(double(values[i]) - double(v)) < std::abs(double(values[cur]) - double(v))) cur = i;
    const int next = choiceStep(cur, n, selector(text, labels[cur], cur > 0, cur < n - 1, sel), sel);
    if (next == cur) return false;
    v = values[next];
    return true;
}

// VR (--xr; gfx/xr.cpp): the headset's refresh rate, stereo 3D, the HUD, the view and the screen
void pageVr(settings::Settings& s) {
    bool vr = false;  // (applied to the running session: xr::applySettings)
    heading("HEADSET");
    if (toggleRow("Play in VR", s.vr,
                  "On: Red Two starts in your VR headset (OpenXR: Virtual Desktop, Quest Link, SteamVR), as with --xr. "
                  "Off: on the monitor (--xr still starts in VR). From the next start."))
        s_changed = true;
    {
        static const int rates[] = {0, 72, 80, 90, 120};
        static const char* const labels[] = {"HEADSET'S OWN", "72 HZ", "80 HZ", "90 HZ", "120 HZ"};
        if (valueRow("Refresh rate", s.xrRefreshRate, rates, labels, 5,
                     "The headset's refresh rate, asked of the OpenXR runtime (Virtual Desktop, Quest Link, SteamVR). "
                     "The game itself keeps its own frame rate (Display: 60 FPS); the headset fills the frames in "
                     "between, turned with your head. Applies at once in the headset."))
            s_changed = vr = true;
    }
    {
        static const int lines[] = {0, -1, 1728, 2016, 2240, 2400, 2560, 2720, 2880, 3040, 3200};
        static const char* const labels[] = {"AS DISPLAY", "HEADSET'S OWN", "LOW (1728)", "MEDIUM (2016)", "2240",
                                             "HIGH (2400)", "2560", "2720", "ULTRA (2880)", "3040", "GODLIKE (3200)"};
        if (valueRow("Resolution", s.xrResolution, lines, labels, 11,
                     "What the game renders at in the headset, in lines per eye (in flight; the virtual screen in the "
                     "menus). Low to Godlike follow Virtual Desktop's levels for Quest 3 and Pico 4. Headset's own: "
                     "what the OpenXR runtime asks for. As display: the Display page's internal resolution. Higher is "
                     "sharper and asks more of the graphics card. Applies at once."))
            s_changed = s_display = true;
    }
    if (toggleRow("Stereo 3D in flight", s.xrStereo,
                  "On: in flight the world is around you in 3D, seen through your own eyes. Off: the game on a flat "
                  "virtual screen all the time. From the next start."))
        s_changed = true;
    heading("FLIGHT");
    {
        static const float dist[] = {5.f, 10.f, 20.f, 40.f};
        static const char* const labels[] = {"NEAR", "MEDIUM", "FAR", "VERY FAR"};
        if (valueRow("HUD distance", s.xrHudDistance, dist, labels, 4,
                     "How far ahead the HUD (reticle, radar, counters) floats, fixed to the game's view. Nearer: "
                     "bigger and easier to read; farther: easier to focus with the world behind it."))
            s_changed = vr = true;
    }
    {
        static const int ranges[] = {3, 4, 5, 6, 8};
        static const char* const labels[] = {"3X", "4X", "5X", "6X", "8X"};
        if (valueRow("View range", s.xrViewRange, ranges, labels, 5,
                     "How far round the game's own view the world is drawn: the headset shows far more than the "
                     "game's camera. More: no ground missing under you or to the sides; less: fewer distant "
                     "buildings appearing late."))
            s_changed = true;
    }
    if (buttonRow("Reset camera zoom", "RESET",
                  "Puts the camera back where the game has it: undoes the zoom set with the right grip and the "
                  "right thumbstick up / down in flight."))
        xr::resetCameraZoom(), s_changed = true;
    if (toggleRow("Targeting computer toggles", s.targetingToggle,
                  "On: Y turns the targeting computer on, the next Y off. Off: it shows while Y is held, as on the "
                  "GameCube."))
        s_changed = true;
    heading("VIRTUAL SCREEN");
    {
        static const float widths[] = {2.4f, 3.2f, 4.2f, 5.4f};
        static const char* const labels[] = {"SMALL", "MEDIUM", "LARGE", "HUGE"};
        if (valueRow("Screen size", s.xrScreenWidth, widths, labels, 4,
                     "The flat screen the menus, the hangar and the cinematics show on, 2.5 m away: Medium is about "
                     "65 degrees across."))
            s_changed = vr = true;
    }
    {
        static const float depths[] = {0.f, 1.f, 2.f, 3.f, 4.f};
        static const char* const labels[] = {"OFF", "LOW", "MEDIUM", "HIGH", "VERY HIGH"};
        if (valueRow("Screen depth", s.xrScreenDepth, depths, labels, 5,
                     "The 3D depth of the screen in the hangar and cinematics (also: hold the right grip and move "
                     "the right thumbstick left / right there)."))
            s_changed = vr = true;
    }
    if (vr) xr::applySettings();
}

void tabBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = 54.f * s_ui, capW = 64.f * s_ui, gap = 10.f * s_ui;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tabsX = p.x + capW + gap, tabsW = avail - 2.f * (capW + gap);
    const float tw = (tabsW - gap * float(SectionCount - 1)) / float(SectionCount);
    for (int i = 0; i < SectionCount; i++) {
        const ImVec2 a(tabsX + float(i) * (tw + gap), p.y), b(a.x + tw, p.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, h)) && s_captureRow < 0) switchSection(i);
        const bool hot = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = s_section == i;
        panel(dl, a, b, 12.f * s_ui, on ? IM_COL32(74, 48, 10, 235) : IM_COL32(10, 22, 44, hot ? 230 : 170),
              on ? IM_COL32(30, 18, 4, 235) : IM_COL32(4, 10, 22, 200),
              on ? kAmber : (hot ? alpha(kHolo, 200) : alpha(kGold, 60)), 1.f * s_ui);
        if (on) dl->AddRectFilled(ImVec2(a.x + 14.f * s_ui, b.y - 4.f * s_ui), ImVec2(b.x - 14.f * s_ui, b.y), kAmber);
        // (smaller where a name does not fit: seven tabs on a narrow screen)
        float px = 22.f * s_ui, tr = 4.f * s_ui;
        const char* name = loc(kSectionNames[i]);
        const float fit = tw - 16.f * s_ui, full = textWidth(s_font, px, name, tr);
        if (full > fit) px *= fit / full, tr *= fit / full;
        const float w = textWidth(s_font, px, name, tr);
        drawText(dl, s_font, px, ImVec2(a.x + (tw - w) * 0.5f, a.y + h * 0.5f - px * 0.55f),
                 on ? kGold : (hot ? kText : kDim), name, tr, on ? alpha(kAmber, 40) : 0);
    }
    const float cy = p.y + h * 0.5f;
    keyCap(dl, ImVec2(p.x + capW * 0.5f, cy), s_usingPad ? padName("LB", "L1") : "Q",
           s_navKey[NavPrevTab].held || ImGui::IsKeyDown(ImGuiKey_Q));
    keyCap(dl, ImVec2(p.x + avail - capW * 0.5f, cy), s_usingPad ? padName("RB", "R1") : "E",
           s_navKey[NavNextTab].held || ImGui::IsKeyDown(ImGuiKey_E));
    const float ruleY = p.y + h + 12.f * s_ui;
    dl->AddLine(ImVec2(p.x, ruleY), ImVec2(p.x + avail, ruleY), alpha(kGold, 60), 1.f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, ruleY + 14.f * s_ui));
}

// returns false when the menu closed
bool drawMenu(settings::Settings& s, float t) {
    // input first: B closes, LB / RB switch tabs, up / down move the selection
    const bool rebinding = rebindingNow();
    pollNav(!rebinding && !s_textEditing);  // (typing: the text field takes the keys)
    if (s_nav[NavBack]) {
        // (the folder browser, then the disc page: back a step; the in-game menu otherwise closes)
        if (!menuStepBack() && !s_launcher) return false;
        s_nav[NavBack] = false;
    }
    if (s_launcher && s_nav[NavStart] && !s_textEditing) {
        logf("menu", "launcher: controller START");
        if (discReady()) s_launch = true;
        else discPageOpen();
    }
    if (s_nav[NavPrevTab]) switchSection((s_section + SectionCount - 1) % SectionCount);
    if (s_nav[NavNextTab]) switchSection((s_section + 1) % SectionCount);
    {
        const int count = s_rowCount[s_section];
        int& sel = s_rowSel[s_section];
        if (count > 0 && (s_nav[NavUp] || s_nav[NavDown])) {
            sel = (sel + (s_nav[NavUp] ? count - 1 : 1)) % count;
            s_scrollToSel = true;
        }
    }
    s_in = {};
    s_in.dx = (s_nav[NavRight] ? 1 : 0) - (s_nav[NavLeft] ? 1 : 0);
    s_in.dxFirst = (s_navFirst[NavRight] ? 1 : 0) - (s_navFirst[NavLeft] ? 1 : 0);
    s_in.accept = s_nav[NavAccept];
    s_in.clear = s_nav[NavClear];

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 disp = io.DisplaySize;
    s_ui = std::clamp(disp.y / 1080.f, 0.6f, 3.f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    // the game behind: dimmed, stars over it (the launcher: nothing behind yet)
    bg->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 3, 10, s_launcher ? 255 : 205));
    starfield(bg, disp, t);

    const float w = std::min(disp.x * 0.86f, disp.y * 1.45f), h = disp.y * 0.86f;
    const ImVec2 a((disp.x - w) * 0.5f, (disp.y - h) * 0.5f), b(a.x + w, a.y + h);
    panel(bg, a, b, 34.f * s_ui, kPanelTop, kPanelBottom, alpha(kGold, 200), 1.5f * s_ui);
    brackets(bg, a, b, 46.f * s_ui, kAmber, 2.f * s_ui);
    for (float y = a.y + 2.f; y < b.y; y += 4.f * s_ui)
        bg->AddLine(ImVec2(a.x + 2.f, y), ImVec2(b.x - 2.f, y), IM_COL32(120, 190, 255, 9), 1.f);
    // a slow scan line
    const float scanY = a.y + std::fmod(t * 0.12f, 1.f) * h;
    bg->AddRectFilledMultiColor(ImVec2(a.x, scanY - 18.f * s_ui), ImVec2(b.x, scanY), IM_COL32(110, 205, 255, 0),
                                IM_COL32(110, 205, 255, 0), IM_COL32(110, 205, 255, 22), IM_COL32(110, 205, 255, 22));

    // header
    const float pad = 44.f * s_ui;
    const float titlePx = 50.f * s_ui;
    const float titleY = a.y + pad * 0.55f;
    drawText(bg, s_fontBig, titlePx, ImVec2(a.x + pad, titleY), kGold, "RED TWO", 10.f * s_ui, alpha(kAmber, 50));
    drawText(bg, s_font, 18.f * s_ui, ImVec2(a.x + pad + 2.f, titleY + titlePx * 1.02f), kHolo,
             loc(s_launcher ? "ROGUE SQUADRON II  //  PRE-FLIGHT CHECK" : "ROGUE SQUADRON II  //  FLIGHT SYSTEMS CONFIGURATION"),
             3.f * s_ui);
    const float blink = 0.5f + 0.5f * std::sin(t * 3.f);
    const ImVec2 lamp(b.x - pad - 8.f * s_ui, titleY + titlePx * 0.45f);
    bg->AddCircleFilled(lamp, 6.f * s_ui, alpha(kAmber, int(120 + 135 * blink)));
    bg->AddCircle(lamp, 11.f * s_ui, alpha(kAmber, 90), 24, 1.f);
    const char* status = loc(s_launcher ? "AWAITING LAUNCH" : "SYSTEMS ONLINE");
    const float statusPx = 17.f * s_ui;
    drawText(bg, s_font, statusPx, ImVec2(lamp.x - 22.f * s_ui - textWidth(s_font, statusPx, status, 2.f * s_ui), lamp.y - 9.f * s_ui),
             kDim, status, 2.f * s_ui);
    {
        const update::Status us = update::status();
        const bool news = us.phase == update::Phase::Available || us.phase == update::Phase::Ready;
        const std::string ver = news ? loc("UPDATE ") + us.latest + loc(" AVAILABLE") : std::string(loc("VERSION ")) + update::version() + " By LoKuS";
        const float vpx = 14.f * s_ui;
        drawText(bg, s_font, vpx, ImVec2(lamp.x - 22.f * s_ui - textWidth(s_font, vpx, ver.c_str(), 2.f * s_ui), lamp.y + 12.f * s_ui),
                 news ? kAmber : alpha(kDim, 150), ver.c_str(), 2.f * s_ui);
    }
    const float headerBottom = titleY + titlePx * 1.02f + 30.f * s_ui;

    // footer: the selected row's description, then the buttons
    const float footY = b.y - pad * 0.8f;
    const float helpY = footY - 40.f * s_ui;
    bg->AddLine(ImVec2(a.x + pad, helpY - 12.f * s_ui), ImVec2(b.x - pad, helpY - 12.f * s_ui), alpha(kGold, 50), 1.f);
    // (a PlayStation controller's buttons by their names: cross, circle, square, L1 / R1, ...)
    const std::string accept = padName("A", "CROSS"), back = padName("B", "CIRCLE"), clear = padName("X", "SQUARE");
    std::string hint = s_usingPad ? std::string(padName("LB / RB", "L1 / R1")) + loc("  TABS      D-PAD / STICK  MOVE, CHANGE      ") + accept + loc("  SELECT")
                                  : loc("Q / E  TABS      ARROWS  MOVE, CHANGE      ENTER  SELECT");
    if (s_section == Controls) hint += s_usingPad ? "      " + clear + loc("  CLEAR") : loc("      DEL  CLEAR");
    if (s_launcher) hint += s_usingPad ? std::string("      ") + padStartName() + loc("  LAUNCH") : "";
    else hint += s_usingPad ? "      " + back + " / " + padBackName() + loc("  CLOSE") : loc("      ESC / F1  CLOSE");
    if (s_browsing && s_section == s_browseSection)
        hint = s_usingPad ? loc("D-PAD / STICK  MOVE      ") + accept + loc("  OPEN, SELECT      ") + clear + loc("  PARENT FOLDER      ") + back + loc("  BACK")
                          : loc("ARROWS  MOVE      ENTER  OPEN, SELECT      BACKSPACE  PARENT FOLDER      ESC  BACK");
    drawText(bg, s_font, 16.f * s_ui, ImVec2(a.x + pad, footY), kDim, hint.c_str(), 2.f * s_ui);

    // the interactive part: an invisible window over the panel body
    const float top = headerBottom;
    ImGui::SetNextWindowPos(ImVec2(a.x + pad, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w - 2.f * pad, helpY - 24.f * s_ui - top), ImGuiCond_Always);
    if (s_focus) ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollWithMouse;
    s_help = nullptr;
    if (ImGui::Begin("##redtwo_menu", nullptr, flags)) {
        // (a child's font scale multiplies its parent's: this window keeps 1, the page sets it)
        if (s_font) ImGui::PushFont(s_font);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f * s_ui, 7.f * s_ui));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f * s_ui, 10.f * s_ui));

        tabBar();

        ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_None);
        bodyFont(23.f * s_ui);
        s_valueX = std::min(380.f * s_ui, ImGui::GetContentRegionAvail().x * 0.42f);
        s_changed = s_display = false;
        s_row = 0;
        switch (s_section) {
        case Display: pageDisplay(s); break;
        case Gameplay: pageGameplay(s); break;
        case Audio: pageAudio(s); break;
        case Controls: pageControls(); break;
        case Cheats: pageCheats(s); break;
        case Mods: pageMods(s); break;
        case Vr: pageVr(s); break;
        }
        ImGui::Dummy(ImVec2(0, 8.f * s_ui));
        if (!s_browseLeft) {  // (left the folder browser mid-page: keep the MODS row to return to)
            s_rowCount[s_section] = s_row;
            if (s_row > 0) s_rowSel[s_section] = std::clamp(s_rowSel[s_section], 0, s_row - 1);
        }
        s_browseLeft = false;
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
        if (s_font) ImGui::PopFont();
        if (s_changed) settings::save();
        if (s_display) applyDisplaySettings();
    }
    ImGui::End();
    if (s_help) {
        // one line, smaller where it is long; two lines (split at the space nearest the middle)
        // where it would get too small
        const float avail = w - 2.f * pad, tr = 0.5f * s_ui;
        float px = 19.f * s_ui;
        const float full = textWidth(s_font, px, s_help, tr);
        if (full <= avail || px * avail / full >= 15.f * s_ui) {
            if (full > avail) px *= avail / full;
            drawText(bg, s_font, px, ImVec2(a.x + pad, helpY), alpha(kHolo, 220), s_help, tr);
        } else {
            const std::string text = s_help;
            size_t split = std::string::npos;
            for (size_t i = 0; i < text.size(); i++)
                if (text[i] == ' ' && (split == std::string::npos || std::abs(int(i) - int(text.size() / 2)) <
                                                                         std::abs(int(split) - int(text.size() / 2))))
                    split = i;
            const std::string l1 = text.substr(0, split), l2 = split == std::string::npos ? "" : text.substr(split + 1);
            px = 17.f * s_ui;
            const float wide = std::max(textWidth(s_font, px, l1.c_str(), tr), textWidth(s_font, px, l2.c_str(), tr));
            if (wide > avail) px *= avail / wide;
            drawText(bg, s_font, px, ImVec2(a.x + pad, helpY - px * 0.55f), alpha(kHolo, 220), l1.c_str(), tr);
            drawText(bg, s_font, px, ImVec2(a.x + pad, helpY + px * 0.6f), alpha(kHolo, 220), l2.c_str(), tr);
        }
    }
    s_focus = false;
    if (s_captureRow < 0 && s_navBackTb && tbNow() >= s_navBackTb) s_navBackTb = 0;
    return true;
}

// R2_MENU_BIND_TEST=1 (with R2_MENU=1 R2_MENU_SECTION=3, automated runs only: their bindings
// live in build/test_cache): rebinds A and STICK UP on the keyboard through synthetic key
// presses, logs the results, then resets the keyboard profile.
void pushKey(SDL_Scancode sc) {
    for (int down = 1; down >= 0; down--) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.timestamp = SDL_GetTicksNS();
        e.key.windowID = aurora_get_window() ? SDL_GetWindowID(aurora_get_window()) : 0;
        e.key.scancode = sc;
        e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = down != 0;
        SDL_PushEvent(&e);
    }
}
// R2_MENU_TEXT_TEST=1 (with R2_MENU=1 R2_MENU_SECTION=5 R2_MENU_ROW=1 and R2_INPUT set): types
// into the achievements page's user name row through synthetic key and text events
void pushText(const char* text) {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.timestamp = SDL_GetTicksNS();
    e.text.windowID = aurora_get_window() ? SDL_GetWindowID(aurora_get_window()) : 0;
    e.text.text = SDL_strdup(text);  // (SDL keeps it for the event's lifetime; a test, leaked)
    SDL_PushEvent(&e);
}
void textSelfTest() {
    static const bool on = envFlag("R2_MENU_TEXT_TEST") && getenv("R2_INPUT");
    if (!on) return;
    static int k = 0;
    switch (++k) {
    case 30: logf("menu", "text test: user '%s', editing %d", s_raUser.c_str(), s_textEditing); pushKey(SDL_SCANCODE_RETURN); break;
    case 40: logf("menu", "text test: editing %d after Enter", s_textEditing); pushText("Wedge"); break;
    case 50: pushText("Antilles"); break;
    case 60: pushKey(SDL_SCANCODE_RETURN); break;
    case 70: logf("menu", "text test: user '%s', editing %d", s_raUser.c_str(), s_textEditing); pushKey(SDL_SCANCODE_RETURN); break;
    case 80: pushText("x"); break;
    case 90: pushKey(SDL_SCANCODE_ESCAPE); break;
    case 100: logf("menu", "text test: after Esc: user '%s', editing %d, menu open %d", s_raUser.c_str(), s_textEditing, s_open); break;
    }
}

void bindSelfTest() {
    textSelfTest();
    static const bool on = envFlag("R2_MENU_BIND_TEST") && getenv("R2_INPUT");
    if (!on) return;
    static int k = 0;
    const auto rows = controlRows();
    auto show = [&](const char* when) {
        logf("menu", "bind test %s: A = %s, STICK UP = %s, STICK DOWN = %s", when, bindingText(rows[0]).c_str(),
             bindingText(rows[12]).c_str(), bindingText(rows[13]).c_str());
    };
    switch (++k) {
    case 30: s_padDevice = false; show("before"); startCapture(0); break;
    case 40: pushKey(SDL_SCANCODE_K); break;
    case 60: show("after A=K"); startCapture(12); break;
    case 70: pushKey(SDL_SCANCODE_I); break;
    case 90: show("after STICK UP=I"); startCapture(1); break;
    case 100: pushKey(SDL_SCANCODE_ESCAPE); break;
    case 120: show("after Esc (B unchanged)"); logf("menu", "bind test: B = %s, menu open %d", bindingText(rows[1]).c_str(), s_open); break;
    case 130:
        aurora::pad::reset_to_defaults(profileTarget());
        aurora::pad::save_profiles();
        show("after reset");
        break;
    default: break;
    }
}

// R2_MENU_PAD_TEST=1 (with R2_MENU=1, automated runs): an SDL virtual gamepad - the same path
// as a real controller - switches tabs, moves, changes the volume, toggles, then closes the
// menu with B; the log shows where each step got to.
void padSelfTest() {
    if (!padTest()) return;
    static SDL_Joystick* joy = nullptr;
    static int k = 0;
    if (!joy) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Red Two test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("menu", "pad test: virtual gamepad %u %s", unsigned(id), joy ? "attached" : SDL_GetError());
        if (!joy) return;
    }
    const settings::Settings& s = settings::get();
    auto show = [&](const char* step) {
        logf("menu", "pad test %-22s: open %d tab %s row %d volume %d background %d", step, s_open, kSectionNames[s_section],
             s_rowSel[s_section], s.volume, s.backgroundAudio);
    };
    auto press = [&](int at, SDL_GamepadButton b) {
        if (k == at) SDL_SetJoystickVirtualButton(joy, b, true);
        if (k == at + 4) SDL_SetJoystickVirtualButton(joy, b, false);
    };
    auto stick = [&](int at, SDL_GamepadAxis a, Sint16 v) {
        if (k == at) SDL_SetJoystickVirtualAxis(joy, a, v);
        if (k == at + 4) SDL_SetJoystickVirtualAxis(joy, a, 0);
    };
    k++;
    press(40, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // Display -> Gameplay
    press(60, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);   // -> Audio
    press(80, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);    // -> Gameplay
    press(100, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);  // -> Audio
    stick(120, SDL_GAMEPAD_AXIS_LEFTY, 32767);      // stick down: row 1
    press(140, SDL_GAMEPAD_BUTTON_DPAD_UP);         // row 0, master volume
    press(160, SDL_GAMEPAD_BUTTON_DPAD_LEFT);       // 95
    stick(180, SDL_GAMEPAD_AXIS_LEFTX, -32768);     // 90
    press(200, SDL_GAMEPAD_BUTTON_DPAD_DOWN);       // row 1
    press(220, SDL_GAMEPAD_BUTTON_SOUTH);           // A: sound in background off
    press(260, SDL_GAMEPAD_BUTTON_EAST);            // B: close (the launcher stays)
    static bool launcherStart = false;               // the launcher (R2_LAUNCHER=1): START starts the game
    if (k == 300 && s_launcher) SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, launcherStart = true);
    if (k == 304 && launcherStart) SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_START, false);
    switch (k) {
    case 30: show("start"); break;
    case 55: show("RB"); break;
    case 75: show("RB"); break;
    case 95: show("LB"); break;
    case 115: show("RB"); break;
    case 135: show("stick down"); break;
    case 155: show("d-pad up"); break;
    case 175: show("d-pad left"); break;
    case 195: show("stick left"); break;
    case 215: show("d-pad down"); break;
    case 235: show("A"); break;
    case 275: show("B"); break;
    case 310: if (launcherStart) logf("menu", "pad test START: launcher %s", s_launcher ? "still open" : "closed, game started"); break;
    default: break;
    }
}

void setOpen(bool open) {
    if (!open) cancelCapture();
    if (!open) browseClose();
    if (!open && s_discPage) discPageClose();
    if (open) s_dolphinStale = true;  // Dolphin's files may have changed meanwhile
    s_confirmSave = false;
    s_confirmQuit = false;
    s_confirmReset = false;
    s_confirmLauncher = false;
    s_confirmLogout = false;
    s_textEditing = false;
    if (open) s_raDolphin = -1;  // (Dolphin may have logged in meanwhile)
    if (open) s_padPsPressed = false;  // (the hints name port 1's controller's buttons until a press)
    s_open = open;
    s_focus = open;
    s_navFresh = true;
    s_scrollToSel = open;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// interface
// ---------------------------------------------------------------------------------------
bool menuOpen() { return s_open; }
bool launcherShowing() { return s_launcher; }

void launcherOpen() {
    s_launcher = true;
    s_launch = false;
    s_section = Display;
    s_rowSel[Display] = 0;  // Start game: Enter / A right away
    s_usingPad = SDL_HasGamepad();  // (button hints; the first input decides after that)
    s_padDevice = s_usingPad && aurora::pad::port_controller(0) != 0;  // (the Controls page's device)
    setOpen(true);
    if (!discReady()) discPageOpen();  // (no disc yet: that first)
}

void launcherSetDisc(const std::string& path, const std::string& note) { discSet(path, note); }
std::string launcherDisc() { return s_discPath; }

// one frame of the launcher, inside an ImGui frame; true: the player started the game
bool launcherFrame() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return false;
    modsPoll();
    updatePoll();
    const float t = float(double(SDL_GetTicks()) / 1000.0);
    drawMenu(settings::get(), t);
    if (s_open) bindSelfTest();
    padSelfTest();
    // R2_LAUNCHER_AUTO=<seconds> (tests): start the game that long after the launcher opened
    static const char* autoEnv = getenv("R2_LAUNCHER_AUTO");
    static const u64 openedTb = tbNow();
    // R2_LAUNCHER_DISC=<path> (tests): that path entered on the disc page 1 s after it opened
    static const char* discEnv = getenv("R2_LAUNCHER_DISC");
    static bool discDone = false;
    if (discEnv && !discDone && double(tbNow() - openedTb) / double(kTbClock) >= 1.0) {
        discDone = true;
        logf("menu", "launcher: R2_LAUNCHER_DISC %s: %s", discEnv, discChoose(discEnv) ? "taken" : "refused");
    }
    // R2_LAUNCHER_BROWSE=<folder> (screenshots): the disc browser on that folder
    static const char* browseEnv = getenv("R2_LAUNCHER_BROWSE");
    static bool browseDone = false;
    if (browseEnv && !browseDone) {
        browseDone = true;
        discPageOpen();
        browseBegin(BrowseMode::Disc, Display, browseEnv);
    }
    if (autoEnv && double(tbNow() - openedTb) / double(kTbClock) >= atof(autoEnv)) {
        logf("menu", "launcher: R2_LAUNCHER_AUTO start");
        s_launch = true;
    }
    if (s_launch && !discReady()) {  // (no game without its disc)
        s_launch = false;
        discPageOpen();
    }
    if (!s_launch) return false;
    s_launcher = s_launch = false;
    setOpen(false);
    return true;
}

void setHudAnchoring(bool on) {
    static bool last = false;
    if (on != last) AuroraSetHudAnchoring(last = on);
}

// Perspective aspects the game's cameras were widened to (hle_CameraSetup): Aurora shows those
// projections as they are. The latest 8; Aurora hears of each new one once.
void noteWidenedAspect(float aspect) {
    static float list[8] = {};
    static u32 count = 0, next = 0;
    for (u32 i = 0; i < count; i++)
        if (std::abs(list[i] - aspect) < 0.0005f) return;
    list[next] = aspect;
    next = (next + 1) % 8;
    count = std::min<u32>(count + 1, 8);
    AuroraSetWidenedAspects(list, count);
    logf("wide", "widened camera aspect %.4f", aspect);
}

void menuImGuiInit(const void*) {
    ImGuiIO& io = ImGui::GetIO();
    // the menu does its own navigation (above); ImGui's would move a second cursor
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    // Windows' Bahnschrift; on Linux one shipped next to the program (the AppImage's
    // share/red-two/font.ttf, a condensed DejaVu) or the system's DejaVu / Noto
    std::vector<std::string> fonts;
    if (const char* f = getenv("R2_FONT")) fonts.push_back(f);
#if defined(_WIN32)
    fonts.push_back("C:\\Windows\\Fonts\\bahnschrift.ttf");
#elif defined(__ANDROID__)
    for (const char* f : {"/system/fonts/RobotoCondensed-Regular.ttf", "/system/fonts/Roboto-Regular.ttf",
                          "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf"})
        fonts.push_back(f);
#elif defined(__APPLE__)
    // one shipped in the app (Contents/Resources/font.ttf, the base path), else the system's
    if (const char* base = SDL_GetBasePath()) fonts.push_back(std::string(base) + "font.ttf");
    for (const char* f : {"/System/Library/Fonts/Supplemental/Arial Narrow.ttf", "/System/Library/Fonts/SFCompact.ttf",
                          "/System/Library/Fonts/SFNS.ttf"})
        fonts.push_back(f);
#else
    if (const char* base = SDL_GetBasePath()) {
        fonts.push_back(std::string(base) + "../share/red-two/font.ttf");
        fonts.push_back(std::string(base) + "font.ttf");
    }
    for (const char* f : {"/usr/share/fonts/TTF/DejaVuSansCondensed.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf",
                          "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"})
        fonts.push_back(f);
#endif
    for (const std::string& path : fonts) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        fclose(f);
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        s_font = io.Fonts->AddFontFromFileTTF(path.c_str(), kFontPx, &cfg);
        s_fontBig = io.Fonts->AddFontFromFileTTF(path.c_str(), kBigPx, &cfg);
        if (s_font && s_fontBig) break;
    }
    if (!s_font) s_font = s_fontBig = io.Fonts->AddFontDefault();
    applyStyle();
}

void menuEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    // the window moved to another display, or the display's mode changed: with V-Sync the
    // retrace follows its refresh
    if ((e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) &&
        settings::get().vsync && !cinematicSkip()) {
        logf("gfx", "display now %d Hz", displayRefreshHz());
        viSetRateHz(frameRateHz());
    }
    if (rebindingNow()) return;  // rebinding takes these
    if (s_launcher) {  // no opening or closing: Esc only leaves the folder browser / the disc page
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE && !s_textEditing) menuStepBack();
        return;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.key == SDLK_F1 || e.key.key == SDLK_AC_BACK) {  // (AC_BACK: Android's back button / gesture)
            logf("menu", "F1: %s", s_open ? "close" : "open");
            s_usingPad = false;
            if (!s_open) s_padDevice = false;  // Controls shows the keyboard's
            setOpen(!s_open);
        } else if (e.key.key == SDLK_ESCAPE && s_open && !s_textEditing) {
            if (!menuStepBack()) {  // (the folder browser / the disc page: back a step)
                logf("menu", "Esc: close");
                setOpen(false);
            }
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        // Back toggles the menu when it is let go with nothing else pressed meanwhile: Back + RB
        // and the other hotkeys hold it too
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            s_backDown = true;
            s_backChord = false;
        } else if (s_backDown) {
            s_backChord = true;
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && s_backDown && e.gaxis.value > 16000 &&
               (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        s_backChord = true;
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
        const bool toggle = s_backDown && !s_backChord && (!automatedRun() || getenv("R2_HOTKEY_TEST"));
        s_backDown = false;
        if (toggle) {
            logf("menu", "controller Back: %s", s_open ? "close" : "open");
            s_usingPad = true;
            if (!s_open) s_padDevice = aurora::pad::port_controller(0) != 0;  // Controls shows the controller's
            setOpen(!s_open);
        }
    }
}

void toast(const std::string& text) {
    s_toast = text;
    s_toastTb = tbNow();
}

// The aspect Aurora renders at: the setting's, or the window's (-1) up to settings::kMaxAspect -
// a wider window gets a fixed 21:9 frame with bars at the sides.
static float s_appliedAspect = 0.f;

static float displayAspect() {
    const float a = settings::aspectValue(settings::get().aspect);
    if (a > 0.f) return std::min(a, settings::kMaxAspect);
    int w = 0, h = 0;
    if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSizeInPixels(win, &w, &h);
    return w > 0 && h > 0 && float(w) / float(h) > settings::kMaxAspect ? settings::kMaxAspect : -1.f;
}

static void applyDisplayAspect() {
    const float a = displayAspect();
    if (a == s_appliedAspect) return;  // (AuroraSetDisplayAspect drains the GX worker)
    s_appliedAspect = a;
    AuroraSetDisplayAspect(a);
}

// ubershaders (Aurora patch 0021): R2_UBER=0/1/2 (off, hybrid, every draw: testing) overrides
int uberShaderMode() {
    static const char* uberEnv = getenv("R2_UBER");
    return uberEnv ? atoi(uberEnv) : settings::get().ubershaders ? 1 : 0;
}

void applyDisplaySettings() {
    const settings::Settings& s = settings::get();
    // (in VR the VR page's resolution, lines per eye over the game's 480, unless it says the same
    // as here)
    float scale = settings::resolutionScale(s.resolution);
    if (xr::enabled() && s.xrResolution != 0) {
        const int lines = s.xrResolution < 0 ? xr::recommendedEyeHeight() : s.xrResolution;
        if (lines > 0) scale = float(std::clamp(lines, 480, 4096)) / 480.f;
    }
    static float logged = -1.f;
    if (scale != logged) {
        logged = scale;
        logf("gfx", "frame buffer scale %g (%s)", scale,
             xr::enabled() && s.xrResolution != 0 ? "the VR page's resolution" : "the internal resolution");
    }
    VISetFrameBufferScale(scale);
    applyDisplayAspect();
    AuroraSetUberShaders(uberShaderMode());
    AuroraSetForcedAnisotropy(u16(s.anisotropy));  // (Aurora patch 0030)
    aurora_enable_vsync(s.vsync);  // (the surface is set up again on the render worker when it changes)
    // widescreen: letterboxed cinematics fill the screen (4:3 keeps the original bars)
    AuroraSetLetterboxFill(s.aspect != settings::Aspect::R4_3);
    if (SDL_Window* w = aurora_get_window()) {
        const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
        if (full != s.fullscreen) SDL_SetWindowFullscreen(w, s.fullscreen);
    }
    viSetRateHz(frameRateHz());  // (the frame rate setting; with V-Sync in step with the display)
}

// Before the game starts, every pipeline in the cache is built (gx_bridge.cpp precompileShaders);
// frames of its own show how far it got.
void drawShaderProgress(u32 done, u32 total, u64 texDone, u64 texTotal, u64 texBytes) {
    if (!GImGui || !GImGui->WithinFrameScope) return;  // (no UI frame before the first XFB exists)
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float ui = std::clamp(disp.y / 1080.f, 0.6f, 3.f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 255));
    const float cx = disp.x * 0.5f, cy = disp.y * 0.5f;
    const bool textures = texTotal != 0;
    const char* title = loc(textures ? "PREPARING SHADERS AND TEXTURES" : "PREPARING SHADERS");
    const float tpx = 38.f * ui, track = 5.f * ui;
    const float top = textures ? cy - 120.f * ui : cy - 80.f * ui;
    drawText(dl, s_fontBig, tpx, ImVec2(cx - textWidth(s_fontBig, tpx, title, track) * 0.5f, top), kGold, title, track);
    const float w = std::min(disp.x * 0.5f, 720.f * ui), h = 8.f * ui;
    const float px = 22.f * ui;
    // one bar with its caption under it
    const auto bar = [&](float y, float f, const char* caption) {
        const ImVec2 a(cx - w * 0.5f, y - h * 0.5f), b(cx + w * 0.5f, y + h * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(30, 52, 86, 255), h * 0.5f);
        f = std::clamp(f, 0.f, 1.f);
        if (f > 0.f) dl->AddRectFilled(a, ImVec2(a.x + w * f, b.y), kAmber, h * 0.5f);
        drawText(dl, s_font, px, ImVec2(cx - textWidth(s_font, px, caption) * 0.5f, b.y + 14.f * ui), kText, caption);
        return b.y;
    };
    char line[96];
    snprintf(line, sizeof line, "Shaders  %u / %u", done, total);
    float y = textures ? cy - 40.f * ui : cy;
    float below = bar(y, total ? float(done) / float(total) : 1.f, line);
    if (textures) {
        snprintf(line, sizeof line, "Textures  %llu / %llu  (%.1f GiB)", (unsigned long long)texDone,
                 (unsigned long long)texTotal, double(texBytes) / double(1ull << 30));
        below = bar(cy + 50.f * ui, float(texDone) / float(texTotal), line);
    }
    const char* note = textures ? "Shaders the game has used on this PC compile and the texture pack loads into memory."
                                : "Every shader the game has used on this PC is compiled before it starts.";
    const char* note2 = "Shaders take long only after a graphics driver update.";
    const float npx = 19.f * ui;
    drawText(dl, s_font, npx, ImVec2(cx - textWidth(s_font, npx, note) * 0.5f, below + 66.f * ui), kDim, note);
    drawText(dl, s_font, npx, ImVec2(cx - textWidth(s_font, npx, note2) * 0.5f, below + 92.f * ui), kDim, note2);
}

void menuDraw() {
    // VR: the headset shows the window's picture, which has no system cursor: ImGui draws one
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = xr::active();
    // widescreen: the game's cameras take the aspect the frame is rendered at (so they also cull
    // for it); 4:3 restores the original
    applyDisplayAspect();  // match window: a resize may cross the 21:9 limit
    {
        u32 w = 0, h = 0;
        AuroraGetRenderSize(&w, &h);
        if (s_appliedAspect > 0.f) setGameAspect(s_appliedAspect);
        else if (w && h) setGameAspect(std::min(double(w) / double(h), double(settings::kMaxAspect)));
    }
    settings::Settings& s = settings::get();
    modsPoll();
    updatePoll();
    // button prompts follow the bindings (a controller connected, a rebind): checked twice a second
    {
        static u64 lastIconsTb = 0;
        const u64 now = tbNow();
        if (now - lastIconsTb >= kTbClock / 2) {
            // (reads controller names from SDL: not while the input thread updates the controllers)
            if (input::JoystickAccess joy; joy) {
                lastIconsTb = now;
                buttonIconsUpdate();
            }
        }
    }
    // volume: (volume / 100)^2 roughly follows loudness; silent in the background if asked
    {
        SDL_Window* win = aurora_get_window();
        const bool focused = !win || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const float v = float(std::clamp(s.volume, 0, 100)) / 100.f;
        audio::setGain(!s.backgroundAudio && !focused ? 0.f : v * v);
    }
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;
    static bool testOpen = envFlag("R2_MENU");  // screenshots of the menu in automated runs
    if (testOpen) {
        testOpen = false;
        setOpen(true);
        if (const char* sec = getenv("R2_MENU_SECTION")) s_section = std::clamp(atoi(sec), 0, int(SectionCount) - 1);
        if (const char* row = getenv("R2_MENU_ROW")) s_rowSel[s_section] = std::max(0, atoi(row));
        s_usingPad = envFlag("R2_MENU_PAD");
        s_padDevice = envFlag("R2_MENU_PAD");  // (the Controls page shows the controller)
        // R2_MENU_PAD=ps: a PlayStation controller's button names in the hints (automated runs
        // read no controllers in the menu)
        if (const char* v = getenv("R2_MENU_PAD"); v && !strcmp(v, "ps")) {
            s_padType = SDL_GAMEPAD_TYPE_PS5;
            s_padPsPressed = true;
        }
    }

    // presented frames per second (half-second window)
    s_fpsFrames++;
    const u64 now = tbNow();
    if (now - s_fpsTb >= kTbClock / 2) {
        s_fps = s_fpsTb ? float(double(s_fpsFrames) * double(kTbClock) / double(now - s_fpsTb)) : 0.f;
        s_fpsFrames = 0;
        s_fpsTb = now;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float ui = std::clamp(io.DisplaySize.y / 1080.f, 0.6f, 3.f);
    const float t = float(double(SDL_GetTicks()) / 1000.0);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (s.showFps && !s_open) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f FPS", s_fps);
        const float px = 20.f * ui;
        const ImVec2 at(14.f * ui, 12.f * ui);
        const float tw = textWidth(s_font, px, buf, 2.f * ui);
        panel(fg, ImVec2(at.x - 8.f * ui, at.y - 5.f * ui), ImVec2(at.x + tw + 10.f * ui, at.y + px + 5.f * ui), 6.f * ui,
              IM_COL32(6, 14, 30, 170), IM_COL32(2, 6, 14, 190), alpha(kGold, 140), 1.f);
        drawText(fg, s_font, px, at, kGold, buf, 2.f * ui);
    }
    // save state messages: 2.5 s at the top, fading out
    if (!s_toast.empty()) {
        const double age = double(now - s_toastTb) / double(kTbClock);
        if (age < 2.5) {
            const float a = age < 2.0 ? 1.f : float((2.5 - age) / 0.5);
            const float px = 24.f * ui;
            const float tw = textWidth(s_font, px, s_toast.c_str(), 3.f * ui);
            const ImVec2 c(io.DisplaySize.x * 0.5f, 64.f * ui);
            const ImVec2 pa(c.x - tw * 0.5f - 22.f * ui, c.y - px * 0.5f - 12.f * ui),
                pb(c.x + tw * 0.5f + 22.f * ui, c.y + px * 0.5f + 12.f * ui);
            panel(fg, pa, pb, 10.f * ui, alpha(IM_COL32(8, 18, 38, 255), int(225 * a)), alpha(IM_COL32(2, 6, 14, 255), int(235 * a)),
                  alpha(kGold, int(200 * a)), 1.5f * ui);
            drawText(fg, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.55f), alpha(kGold, int(255 * a)), s_toast.c_str(),
                     3.f * ui);
        } else {
            s_toast.clear();
        }
    }
    // RetroAchievements pop-ups: top right, stacked, 5 s each (only the first few at a time)
    {
        auto& list = ra::popups();
        const double secs = double(now) / double(kTbClock);
        constexpr double kShow = 5.0;
        float y = 18.f * ui;
        int shown = 0;
        for (auto it = list.begin(); it != list.end() && shown < 3; shown++) {
            if (it->shownAt < 0) it->shownAt = secs;
            const double age = secs - it->shownAt;
            if (age > kShow) {
                it = list.erase(it);
                continue;
            }
            const float a = float(std::min({1.0, age / 0.25, (kShow - age) / 0.5}));
            const float px = 19.f * ui, tpx = 16.f * ui, badge = 64.f * ui, padIn = 12.f * ui;
            // the text: a title line, then the text's lines (split at '\n')
            std::vector<std::string> lines;
            for (size_t p0 = 0; p0 <= it->text.size();) {
                const size_t nl = it->text.find('\n', p0);
                lines.push_back(it->text.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0));
                if (nl == std::string::npos) break;
                p0 = nl + 1;
            }
            float tw = textWidth(s_font, tpx, it->title.c_str(), 2.f * ui);
            for (const auto& l : lines) tw = std::max(tw, textWidth(s_font, px, l.c_str(), 0.f));
            void* tex = ra::image(it->badgeUrl);
            const float textX = it->badgeUrl.empty() ? padIn : padIn * 2.f + badge;
            const float w = std::min(textX + tw + padIn * 1.5f, io.DisplaySize.x * 0.6f);
            const float h = std::max(badge, tpx + 6.f * ui + float(lines.size()) * (px + 4.f * ui)) + padIn * 2.f;
            const float slide = (1.f - std::min(1.f, float(age / 0.25))) * 40.f * ui;
            const ImVec2 pa(io.DisplaySize.x - w - 18.f * ui + slide, y), pb(pa.x + w, y + h);
            panel(fg, pa, pb, 10.f * ui, alpha(IM_COL32(8, 18, 38, 255), int(230 * a)), alpha(IM_COL32(2, 6, 14, 255), int(240 * a)),
                  alpha(kGold, int(200 * a)), 1.5f * ui);
            if (tex)
                fg->AddImage(ImTextureID(uintptr_t(tex)), ImVec2(pa.x + padIn, pa.y + padIn),
                             ImVec2(pa.x + padIn + badge, pa.y + padIn + badge), ImVec2(0, 0), ImVec2(1, 1), alpha(IM_COL32_WHITE, int(255 * a)));
            float ty = pa.y + padIn;
            drawText(fg, s_font, tpx, ImVec2(pa.x + textX, ty), alpha(kAmber, int(255 * a)), it->title.c_str(), 2.f * ui);
            ty += tpx + 6.f * ui;
            for (size_t i = 0; i < lines.size(); i++) {
                drawText(fg, s_font, px, ImVec2(pa.x + textX, ty), alpha(i == 0 ? kGold : kText, int(255 * a)), lines[i].c_str(), 0.f);
                ty += px + 4.f * ui;
            }
            y = pb.y + 10.f * ui;
            ++it;
        }
    }
    if (cinematicSkip()) {
        const char* text = loc("SKIPPING");
        const float px = 34.f * ui;
        const float tw = textWidth(s_font, px, text, 8.f * ui);
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float pulse = 0.6f + 0.4f * std::sin(t * 6.f);
        drawText(fg, s_font, px, ImVec2(c.x - tw * 0.5f, c.y - px * 0.5f), alpha(kGold, int(255 * pulse)), text, 8.f * ui,
                 alpha(kAmber, 40));
        const float bw = tw * 1.2f, y = c.y + px * 0.9f;
        fg->AddRectFilled(ImVec2(c.x - bw * 0.5f, y), ImVec2(c.x + bw * 0.5f, y + 2.f * ui), alpha(kGold, 60));
        const float x = c.x - bw * 0.5f + std::fmod(t * 0.8f, 1.f) * bw * 0.8f;
        fg->AddRectFilled(ImVec2(x, y), ImVec2(x + bw * 0.2f, y + 2.f * ui), kAmber);
    }
    if (s_open && !drawMenu(s, t)) {
        logf("menu", "controller B: close");
        setOpen(false);
    }
    if (s_open) bindSelfTest();
    padSelfTest();
}

}  // namespace rt::gfx
