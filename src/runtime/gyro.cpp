// Gyro aiming: the controller's gyro steers the craft in flight, on top of the main stick.
//
// Sensors: the Steam Deck's built-in controller through SDL's HIDAPI driver (gyro and
// accelerometer at 250 Hz; the Deck's firmware already reads 0 at rest), or any controller SDL
// reads a gyro from (DualSense, DualShock 4, Switch Pro Controller, Joy-Cons, ...; Xbox
// controllers have none). With Steam Input on, Steam hides the controller behind a virtual pad
// without sensors: Red Two needs it off. The gyro button is one the controller has (updateButton). The samples
// arrive as Aurora input events (a passive layer: SensorChanged with the sensor's timestamps)
// from the game thread's event pump; input::poll asks for the mean rate of the last 16 ms once
// per retrace, so neither the pump's nor the game's timing shows.
//
// Axes: SDL's gamepad frame - x right, y out of the controls' face, z toward the player. A Deck
// held as usual (screen toward the player) has the world's vertical mostly along -z: turning the
// body is mostly SDL's roll there, its yaw on a pad held flat. So turning is taken in "player
// space" (GyroWiki, JoyShockMapper): rotation about gravity (the accelerometer's direction,
// carried along by the gyro between samples), relaxed by 1.41 so a steering-wheel tilt turns too.
// Pitch is the controller's own x axis: its far edge up = nose up. Measured on the Deck on its
// stand: accelerometer (0, 0.36, -0.89) g in SDL's frame - screen leaning back ~22 degrees.
//
// Output: in flight the stick sets the craft's turn rate, so the controller's rotation rate
// becomes a turn rate: at 100% the craft turns as far as the controller (1:1) up to its limit.
// The game's response, measured with R2_GYRO_TEST and R2_GYRO_LOG (X-wing, Death Star Attack,
// steady state; heading 4.2 / 8.7 / 18 / 27 / 30 / 42 / 52 / 60 / 70 / 72 deg/s at 9 / 18 / 36 / 54
// / 58 / 62 / 65 / 68 / 71 / 72 of 72 stick units; pitch 7 / 14 / 58 at 18 / 36 / 72): the rate is
// half the stick's fraction of its maximum up to 80% of the stick, then three times as steep to
// the maximum at the edge (72 deg/s heading, 58 pitch). The gyro's rate goes through the inverse
// of that, added to what the physical stick asks for. The SDK's PADClamp (0x8003D3E0,
// ClampRegion at r13-0x51C0) drops a dead zone of 15 units and limits the stick to an octagon of
// 72 (40 on the diagonal) before the game sees it: the sum is given back with the dead zone in
// front (an anti-dead zone, or slow turns of the controller would do nothing), fractions of a
// unit carried into the next retrace. Only while the player's craft flies (hle_game.cpp): menus,
// the pause screen and cutscenes read the stick too.
#include "gyro.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <aurora/input.hpp>
#include <aurora/pad.hpp>
#include <dolphin/pad.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "settings.h"

namespace rt::gyro {
namespace {
using aurora::input::InputEvent;
using aurora::input::SensorKind;
using aurora::input::SourceId;

constexpr float kRadToDeg = 57.2957795f;
// the craft's rate at full stick (deg/s; the X-wing's - other craft are a little faster or slower)
constexpr float kCraftRateX = 72.f, kCraftRateY = 58.f;
constexpr float kTighten = 1.f;  // deg/s: slower rates are scaled down toward 0 (sensor noise)
constexpr float kRelax = 1.41f;  // player space: a pure yaw or roll still reaches full speed
constexpr u64 kWindowNs = 16'000'000;  // the rate: mean of the samples this long before the newest
constexpr u64 kStaleNs = 100'000'000;  // no sample for this long: no rate
constexpr int kDeadZone = 15, kStickMax = 72, kStickXY = 40;  // the SDK's ClampRegion (main stick)

struct Sample {
    u64 ns;  // sensor time
    float turn, pitch;  // deg/s: right, nose up
};
constexpr int kRing = 64;
Sample s_ring[kRing];
int s_count = 0, s_head = 0;  // s_head: the next one written

SourceId s_source = 0;  // the controller whose sensors are read
std::string s_name;
u32 s_available = ~0u;        // kButtons that controller has (bit i)
int s_effective = -1;         // the gyro button used (index into kButtons; -1: the setting's own name)
std::string s_buttonSetting;  // the setting s_effective was worked out for (empty: again)
bool s_sensorsOn = false;
u64 s_lastArrivalNs = 0, s_lastSensorNs = 0;
float s_accel[3] = {};  // m/s^2, the newest sample
bool s_haveAccel = false;
float s_up[3] = {0.f, 1.f, 0.f};  // the world's up in the controller's frame (unit)
bool s_haveUp = false;

// Auto calibration: a controller lying still (every gyro axis within 1.2 deg/s and every
// accelerometer axis within 0.3 m/s^2 for a second) reads its bias, the mean of that second.
float s_bias[3] = {};
struct Still {
    u64 startNs = 0;
    int n = 0;
    float sum[3]{}, gmin[3]{}, gmax[3]{}, amin[3]{}, amax[3]{};
} s_still;

bool s_flight = false;  // the player's craft flies (hle_game.cpp)
bool s_toggle = false;  // GyroMode::Toggle: on
bool s_buttonWas = false;
u32 s_samples = 0;  // gyro samples handled (R2_GYRO_LOG)
float s_carry[2] = {};  // fractions of a stick unit not sent yet

float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void resetSamples() {
    s_count = s_head = 0;
    s_lastArrivalNs = s_lastSensorNs = 0;
    s_haveAccel = s_haveUp = false;
    s_up[0] = s_up[2] = 0.f;
    s_up[1] = 1.f;
    s_still = {};
}

void calibrate(const float* w, u64 ns) {
    Still& st = s_still;
    bool fits = st.n > 0 && ns > st.startNs;
    for (int i = 0; i < 3 && fits; i++) {
        if (std::max(st.gmax[i], w[i]) - std::min(st.gmin[i], w[i]) > 1.2f) fits = false;
        if (s_haveAccel && std::max(st.amax[i], s_accel[i]) - std::min(st.amin[i], s_accel[i]) > 0.3f) fits = false;
    }
    if (!fits) {
        st = {};
        st.startNs = ns;
        for (int i = 0; i < 3; i++) {
            st.gmin[i] = st.gmax[i] = w[i];
            st.amin[i] = st.amax[i] = s_accel[i];
        }
    }
    st.n++;
    for (int i = 0; i < 3; i++) {
        st.sum[i] += w[i];
        st.gmin[i] = std::min(st.gmin[i], w[i]);
        st.gmax[i] = std::max(st.gmax[i], w[i]);
        st.amin[i] = std::min(st.amin[i], s_accel[i]);
        st.amax[i] = std::max(st.amax[i], s_accel[i]);
    }
    if (ns - st.startNs < 1'000'000'000 || st.n < 50) return;
    float m[3];
    bool plausible = true;
    for (int i = 0; i < 3; i++) {
        m[i] = st.sum[i] / float(st.n);
        plausible &= std::fabs(m[i]) < 6.f;
    }
    if (plausible) {
        const bool changed = std::fabs(m[0] - s_bias[0]) + std::fabs(m[1] - s_bias[1]) + std::fabs(m[2] - s_bias[2]) > 0.2f;
        memcpy(s_bias, m, sizeof m);
        if (changed) logf("gyro", "calibrated at rest: bias %.2f %.2f %.2f deg/s", m[0], m[1], m[2]);
    }
    st = {};
}

void gyroSample(const float* rad, u64 sensorNs, u64 arrivalNs) {
    const float dt = s_lastSensorNs && sensorNs > s_lastSensorNs
                         ? std::min(float(sensorNs - s_lastSensorNs) * 1e-9f, 0.05f)
                         : 0.004f;
    s_lastSensorNs = sensorNs;
    s_lastArrivalNs = arrivalNs ? arrivalNs : SDL_GetTicksNS();
    float w[3];  // deg/s about x (pitch), y (yaw), z (roll), counter-clockwise seen from the axis' end
    for (int i = 0; i < 3; i++) w[i] = rad[i] * kRadToDeg;
    calibrate(w, sensorNs);
    for (int i = 0; i < 3; i++) w[i] -= s_bias[i];

    // gravity: turned along with the controller (a world vector seen from the controller's frame:
    // du/dt = -w x u), pulled toward the accelerometer's direction over ~0.3 s (the hands' own
    // accelerations average out; strong ones are skipped)
    if (s_haveAccel) {
        float a[3] = {s_accel[0], s_accel[1], s_accel[2]};
        const float len = std::sqrt(dot3(a, a));
        if (len > 1.f) {
            for (float& v : a) v /= len;
            if (!s_haveUp) {
                memcpy(s_up, a, sizeof a);
                s_haveUp = true;
            }
            const float wr[3] = {w[0] / kRadToDeg, w[1] / kRadToDeg, w[2] / kRadToDeg};
            const float cx = wr[1] * s_up[2] - wr[2] * s_up[1], cy = wr[2] * s_up[0] - wr[0] * s_up[2],
                        cz = wr[0] * s_up[1] - wr[1] * s_up[0];
            s_up[0] -= cx * dt;
            s_up[1] -= cy * dt;
            s_up[2] -= cz * dt;
            if (std::fabs(len - 9.80665f) < 3.f) {
                const float k = std::min(1.f, dt / 0.3f);
                for (int i = 0; i < 3; i++) s_up[i] += (a[i] - s_up[i]) * k;
            }
            const float n = std::sqrt(dot3(s_up, s_up));
            if (n > 0.1f)
                for (float& v : s_up) v /= n;
        }
    }
    // player space: rotation about the world's vertical (counter-clockwise from above = left)
    const float worldYaw = s_up[1] * w[1] + s_up[2] * w[2];
    const float turnLeft = std::copysign(std::min(std::fabs(worldYaw) * kRelax, std::hypot(w[1], w[2])), worldYaw);
    s_samples++;
    s_ring[s_head] = {sensorNs, -turnLeft, w[0]};
    s_head = (s_head + 1) % kRing;
    s_count = std::min(s_count + 1, kRing);
}

aurora::input::EventResult onEvent(const InputEvent& e, void*) {
    const auto* s = e.payload.get_if<InputEvent::SensorChanged>();
    if (s && s_source && e.source.id == s_source) {
        if (s->sensor == SensorKind::Accel) {
            memcpy(s_accel, s->values.data(), sizeof s_accel);
            s_haveAccel = true;
        } else if (s->sensor == SensorKind::Gyro) {
            gyroSample(s->values.data(), s->sensorTimestampNs, e.timestampNs);
        }
    }
    return aurora::input::EventResult::Pass;
}

// mean rate of the samples within kWindowNs of the newest; false: none lately
bool rate(float& turn, float& pitch) {
    if (!s_count || SDL_GetTicksNS() - s_lastArrivalNs > kStaleNs) return false;
    const u64 newest = s_ring[(s_head + kRing - 1) % kRing].ns;
    float t = 0.f, p = 0.f;
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        const Sample& x = s_ring[(s_head + kRing - 1 - i) % kRing];
        if (newest - x.ns >= kWindowNs) break;
        t += x.turn;
        p += x.pitch;
        n++;
    }
    turn = t / float(n);
    pitch = p / float(n);
    return true;
}

// port 1's controller if it has a gyro, else any controller that has one (Steam Input's virtual
// pad on port 1 beside the Deck's own, or a Deck docked while another pad plays: it lies still)
SourceId pickSource() {
    const SourceId port1 = aurora::pad::port_controller(0);
    if (port1 && aurora::input::has_sensor(port1, SensorKind::Gyro)) return port1;
    for (const auto& src : aurora::input::sources())
        if (src.kind == aurora::input::InputSource::Kind::Controller && aurora::input::has_sensor(src.id, SensorKind::Gyro))
            return src.id;
    return 0;
}

void enableSensors(SourceId src, bool on) {
    aurora::input::set_sensor_enabled(src, SensorKind::Gyro, on);
    if (aurora::input::has_sensor(src, SensorKind::Accel)) aurora::input::set_sensor_enabled(src, SensorKind::Accel, on);
}

void selectSource(bool want) {  // JoystickAccess held
    static u32 s_polls = 0;
    // (looked for again twice a second: controllers come and go, port 1 changes hands)
    const bool look = s_polls++ % 30 == 0 || (want && !s_sensorsOn);
    if (!look) return;
    const SourceId src = pickSource();
    static bool s_missingLogged = false;
    if (want && !src && !s_source && !s_missingLogged) {
        s_missingLogged = true;
        logf("gyro", "no controller with a gyro (Xbox controllers have none; Steam Input hides one: turn it off for Red Two)");
    }
    if (src != s_source) {
        if (s_source && s_sensorsOn) enableSensors(s_source, false);
        s_source = src;
        s_sensorsOn = false;
        s_name = src ? aurora::input::source_name(src) : std::string();
        resetSamples();
        if (src) logf("gyro", "gyro of '%s'", s_name.c_str());
        else logf("gyro", "the controller with the gyro went away");
        s_missingLogged = src != 0;
        s_buttonSetting.clear();  // its buttons: worked out again
    }
    if (s_source && want != s_sensorsOn) {
        enableSensors(s_source, want);
        s_sensorsOn = want;
        resetSamples();
    }
}

// --- the gyro button ----------------------------------------------------------------------------
// The setting's button may not be on this controller: the default is the Steam Deck's left stick
// touch, which a DualSense or a Switch Pro Controller lacks (While held never turned on, Off while
// held never off). Then the first of these it has is used.
constexpr const char* kFallbacks[] = {"misc3", "touch", "leftstick"};

SDL_Gamepad* sourcePad() {  // JoystickAccess held
    return s_source ? SDL_GetGamepadFromID(aurora::input::gamepad_for_source(s_source)) : nullptr;
}

bool padHas(SDL_Gamepad* g, const char* name) {
    if (!std::strcmp(name, "touch")) return SDL_GetNumGamepadTouchpads(g) > 0;
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    return b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GamepadHasButton(g, b);
}

void updateButton(const std::string& setting, bool force) {  // JoystickAccess held
    static u32 s_polls = 0;
    if (!force && setting == s_buttonSetting && s_polls++ % 30 != 0) return;
    SDL_Gamepad* g = sourcePad();
    u32 avail = 0;
    for (int i = 0; i < kButtonCount; i++)
        if (!g || padHas(g, kButtons[i].sdlName)) avail |= 1u << i;
    int want = -1;
    for (int i = 0; i < kButtonCount; i++)
        if (setting == kButtons[i].sdlName) want = i;
    int eff = want;
    const bool missing = g && (want >= 0 ? !(avail >> want & 1) : !padHas(g, setting.c_str()));
    if (missing)
        for (const char* f : kFallbacks) {
            int i = 0;
            while (i < kButtonCount && std::strcmp(kButtons[i].sdlName, f) != 0) i++;
            if (i < kButtonCount && (avail >> i & 1)) {
                eff = i;
                break;
            }
        }
    if (eff != s_effective && missing && eff >= 0)
        logf("gyro", "'%s' has no %s: the gyro button is %s", s_name.c_str(), setting.c_str(), kButtons[eff].label);
    s_available = avail;
    s_effective = eff;
    s_buttonSetting = setting;
}

bool buttonHeld() {  // JoystickAccess held
    SDL_Gamepad* g = sourcePad();
    if (!g) return false;
    const char* name = s_effective >= 0 ? kButtons[s_effective].sdlName : s_buttonSetting.c_str();
    if (!std::strcmp(name, "touch")) {  // any finger on any touchpad
        for (int t = 0, nt = SDL_GetNumGamepadTouchpads(g); t < nt; t++)
            for (int f = 0, nf = SDL_GetNumGamepadTouchpadFingers(g, t); f < nf; f++) {
                bool down = false;
                float x, y, p;
                if (SDL_GetGamepadTouchpadFinger(g, t, f, &down, &x, &y, &p) && down) return true;
            }
        return false;
    }
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    return b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(g, b);
}

// R2_GYRO_PAD_TEST=<seconds> (automated runs, with R2_BG_PADS=1 and gyro=2, While held): a virtual
// controller like a DualSense - gyro and accelerometer at 250 Hz, a touchpad, no Steam Deck stick
// touches - lies flat from the start; from <seconds> on it turns right at 40 deg/s for 2 s with no
// finger on the touchpad, 2 s with one, then raises its far edge at 30 deg/s for 2 s with one, then
// lies still: what port 1's main stick got is logged in each phase (the default button, left stick
// touch, is not on it: the touchpad must take over).
struct PadTest {
    double start = -1.0;
    SDL_Joystick* joy = nullptr;
    int phase = -1;
    int n = 0;
    float sx = 0.f, sy = 0.f;
} s_padTest;

bool SDLCALL padTestSensors(void*, bool) { return true; }

void padTestUpdate() {  // JoystickAccess held; before the gyro is applied
    PadTest& t = s_padTest;
    static bool once = false;
    if (!once) {
        once = true;
        const char* e = getenv("R2_GYRO_PAD_TEST");
        t.start = e ? atof(e) : -1.0;
        if (t.start < 0) return;
        static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_GYRO, 250.f}, {SDL_SENSOR_ACCEL, 250.f}};
        static const SDL_VirtualJoystickTouchpadDesc touchpad = {2, {}};
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = 0xFFFFu | (1u << SDL_GAMEPAD_BUTTON_TOUCHPAD);  // face ... misc1, touchpad click
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.ntouchpads = 1;
        desc.touchpads = &touchpad;
        desc.nsensors = 2;
        desc.sensors = sensors;
        desc.SetSensorsEnabled = padTestSensors;
        desc.name = "Red Two gyro test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        t.joy = id ? SDL_OpenJoystick(id) : nullptr;
        logf("gyro", "pad test: virtual controller %s", t.joy ? "attached" : SDL_GetError());
    }
    if (!t.joy) return;
    static const u64 t0 = SDL_GetTicksNS();
    const u64 now = SDL_GetTicksNS();
    const double secs = double(now - t0) * 1e-9 - t.start;
    const int phase = secs < 0 ? -1 : secs < 2 ? 0 : secs < 4 ? 1 : secs < 6 ? 2 : secs < 8 ? 3 : 4;
    if (phase != t.phase) {
        static const char* const kNames[] = {"turning right, no touch", "turning right, finger on the touchpad",
                                             "raising the far edge, finger on the touchpad", "still"};
        if (t.phase >= 0 && t.phase < 4)
            logf("gyro", "pad test: %-44s -> port 1 stick %+.0f, %+.0f (mean)", kNames[t.phase], t.n ? t.sx / t.n : 0.f,
                 t.n ? t.sy / t.n : 0.f);
        t.phase = phase;
        t.n = 0;
        t.sx = t.sy = 0.f;
        SDL_SetJoystickVirtualTouchpad(t.joy, 0, 0, phase == 1 || phase == 2, 0.5f, 0.5f, phase == 1 || phase == 2 ? 1.f : 0.f);
    }
    // (rad/s in SDL's frame: x right, y out of the face, z toward the player; flat: up = +y)
    const float turn = phase == 0 || phase == 1 ? -40.f / kRadToDeg : 0.f;  // about +y: counter-clockwise = left
    const float lift = phase == 2 ? 30.f / kRadToDeg : 0.f;
    static u64 last = 0;
    if (!last) last = now;
    for (u64 ts = last + 4'000'000; ts <= now; ts += 4'000'000) {  // 250 Hz up to now
        const float gyro[3] = {lift, turn, 0.f}, accel[3] = {0.f, 9.80665f, 0.f};
        SDL_SendJoystickVirtualSensorData(t.joy, SDL_SENSOR_GYRO, ts, gyro, 3);
        SDL_SendJoystickVirtualSensorData(t.joy, SDL_SENSOR_ACCEL, ts, accel, 3);
        last = ts;
    }
}

void padTestResult(const PADStatus& st) {
    if (s_padTest.phase < 0 || s_padTest.phase > 3) return;
    s_padTest.sx += st.stickX;
    s_padTest.sy += st.stickY;
    s_padTest.n++;
}

// R2_GYRO_TEST="<from s>-<to s>:<turn>,<pitch>[;...]": the gyro reads these rates (deg/s, right
// and nose up) between those times since the start, as if on (automated runs: no sensors)
struct TestWindow {
    double from, to;
    float turn, pitch;
};
std::vector<TestWindow> parseTest() {
    std::vector<TestWindow> v;
    const char* p = getenv("R2_GYRO_TEST");
    while (p && *p) {
        TestWindow t{};
        char* end;
        t.from = strtod(p, &end);
        if (*end != '-') break;
        t.to = strtod(end + 1, &end);
        if (*end != ':') break;
        t.turn = strtof(end + 1, &end);
        if (*end == ',') t.pitch = strtof(end + 1, &end);
        v.push_back(t);
        p = end;
        while (*p == ';') p++;
    }
    if (!v.empty()) logf("gyro", "%zu test windows", v.size());
    return v;
}

bool testRate(float& turn, float& pitch) {
    static const std::vector<TestWindow> s_test = parseTest();
    if (s_test.empty()) return false;
    static const u64 s_start = tbNow();
    const double t = double(tbNow() - s_start) / double(kTbClock);
    for (const auto& w : s_test)
        if (t >= w.from && t < w.to) {
            turn = w.turn;
            pitch = w.pitch;
            return true;
        }
    return false;
}

// the SDK's ClampStick for the main stick: what the game makes of a raw stick position
void clampStick(int& x, int& y) {
    const int sx = x < 0 ? -1 : 1, sy = y < 0 ? -1 : 1;
    x = std::abs(x) <= kDeadZone ? 0 : std::abs(x) - kDeadZone;
    y = std::abs(y) <= kDeadZone ? 0 : std::abs(y) - kDeadZone;
    if (x == 0 && y == 0) return;
    const int d = y <= x ? kStickXY * x + (kStickMax - kStickXY) * y : kStickXY * y + (kStickMax - kStickXY) * x;
    if (kStickXY * kStickMax < d) {
        x = kStickXY * kStickMax * x / d;
        y = kStickXY * kStickMax * y / d;
    }
    x *= sx;
    y *= sy;
}

// a value the game's clamp leaves at v (|v| <= kStickMax): the dead zone put back in front
s8 unclamp(int v) { return s8(v == 0 ? 0 : v + (v < 0 ? -kDeadZone : kDeadZone)); }

// the craft's turn rate (fraction of its maximum) for a stick fraction s, and back
float response(float s) {
    const float a = std::min(std::fabs(s), 1.f);
    return std::copysign(a <= 0.8f ? a * 0.5f : 0.4f + (a - 0.8f) * 3.f, s);
}
float inverseResponse(float f) {
    const float a = std::min(std::fabs(f), 1.f);
    return std::copysign(a <= 0.4f ? a * 2.f : 0.8f + (a - 0.4f) / 3.f, f);
}

}  // namespace

void init() {
    (void)aurora::input::register_layer({
        .label = "red_two.gyro",
        .priority = aurora::input::kGameLayerPriority - 2,
        .onEvent = onEvent,
    });
}

void setFlight(bool flying) { s_flight = flying; }
bool flying() { return s_flight; }
std::string controllerName() {
    // (the menu: also before the game runs and while gyro aiming is off, when apply looks for none)
    if (input::JoystickAccess joy; joy) {
        const SourceId src = pickSource();
        return src ? aurora::input::source_name(src) : std::string();
    }
    return s_name;
}

u32 availableButtons() { return s_available; }
int effectiveButton() {
    if (s_buttonSetting != settings::get().gyroButton) {  // changed in the menu: until the next poll
        for (int i = 0; i < kButtonCount; i++)
            if (settings::get().gyroButton == kButtons[i].sdlName) return i;
        return -1;
    }
    return s_effective;
}

void applyGyro(PADStatus& st, bool joy);

void apply(PADStatus& st, bool joy) {
    if (joy) padTestUpdate();
    applyGyro(st, joy);
    padTestResult(st);
}

void applyGyro(PADStatus& st, bool joy) {
    const settings::Settings& s = settings::get();
    float turn = 0.f, pitch = 0.f;
    const bool test = testRate(turn, pitch);
    const settings::GyroMode mode = s.gyroMode;
    if (joy) selectSource(mode != settings::GyroMode::Off);
    bool held = s_buttonWas;
    if (joy) {
        updateButton(s.gyroButton, s_buttonSetting.empty());
        held = buttonHeld();
    }
    if (mode == settings::GyroMode::Toggle) {
        if (held && !s_buttonWas) {
            s_toggle = !s_toggle;
            logf("gyro", "toggled %s", s_toggle ? "on" : "off");
        }
    } else {
        s_toggle = false;
    }
    s_buttonWas = held;
    static const bool log = envFlag("R2_GYRO_LOG");
    static u32 s_statusPolls = 0;
    if (log && s_source && s_statusPolls++ % 120 == 0)
        logf("gyro", "'%s': sensors %s, %u samples, button %s, flight %d, up %.2f %.2f %.2f, bias %.2f %.2f %.2f",
             s_name.c_str(), s_sensorsOn ? "on" : "off", s_samples, held ? "held" : "up", s_flight, s_up[0], s_up[1],
             s_up[2], s_bias[0], s_bias[1], s_bias[2]);
    bool on = mode == settings::GyroMode::Always || (mode == settings::GyroMode::Held && held) ||
              (mode == settings::GyroMode::Toggle && s_toggle) || (mode == settings::GyroMode::Paused && !held);
    if (!test) on = on && rate(turn, pitch);
    const bool active = (on || test) && s_flight && st.err == PAD_ERR_NONE;
    if (!active) {
        s_carry[0] = s_carry[1] = 0.f;
        return;
    }
    // small rates shrink toward 0 (what the sensor reads of a hand holding still)
    const float mag = std::hypot(turn, pitch);
    if (mag < kTighten) {
        turn *= mag / kTighten;
        pitch *= mag / kTighten;
    }
    // the craft's rate the gyro asks for, as a fraction of its maximum: right = +x, nose up =
    // stick pulled back = -y (flight controls)
    const float g[2] = {turn * float(s.gyroSpeedX) / 100.f / kCraftRateX,
                        (s.gyroInvertY ? pitch : -pitch) * float(s.gyroSpeedY) / 100.f / kCraftRateY};
    int phys[2] = {st.stickX, st.stickY};
    clampStick(phys[0], phys[1]);
    int out[2];
    for (int i = 0; i < 2; i++) {
        const float want = response(float(phys[i]) / float(kStickMax)) + g[i];
        const float total = inverseResponse(want) * float(kStickMax) + s_carry[i];
        out[i] = std::clamp(int(std::lround(total)), -kStickMax, kStickMax);
        s_carry[i] = std::abs(out[i]) < kStickMax ? total - float(out[i]) : 0.f;
    }
    static u32 s_logPolls = 0;
    if (log && s_logPolls++ % 30 == 0)
        logf("gyro", "%s%.1f right %.1f up deg/s -> stick %d,%d (physical %d,%d) up %.2f %.2f %.2f", test ? "test " : "",
             turn, pitch, out[0], out[1], st.stickX, st.stickY, s_up[0], s_up[1], s_up[2]);
    st.stickX = unclamp(out[0]);
    st.stickY = unclamp(out[1]);
}

}  // namespace rt::gyro
