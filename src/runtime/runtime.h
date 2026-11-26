// Runtime internals shared by the host side (not used by generated code).
#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "ctx.h"

namespace rt {

// --- logging --------------------------------------------------------------------------
void logf(const char* tag, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
[[noreturn]] void quit(int code);  // terminate now (no DLL teardown)
const char* symName(u32 addr);  // nearest known name or nullptr
std::string describeAddr(u32 addr);
bool envFlag(const char* name);
std::string traceRing(int n);  // recent guest function entries (R2_TRACE)
void printTraceRing(FILE* f, int n);
void debugInit();  // R2_LOGFN, R2_WATCH
void dumpThreads();  // guest OSThread list (debugging)
u32 peekGuest32(u32 a);  // a guest word without faulting (VM pages not mapped read as 0)
std::string guestBacktrace();  // threads.cpp: the running guest thread's call chain (fatal errors)
void printHostStack(FILE* f, int skip);  // main.cpp: the host call stack, symbolized (fatal errors)
#ifndef _WIN32
[[noreturn]] void hostCrash(int sig, void* siginfo, void* ucontext);  // main.cpp: a host crash (signal handlers)
#endif

// --- memory ---------------------------------------------------------------------------
constexpr u32 kMem1Size = 0x01800000;
constexpr u32 kVmBase = 0x7E000000, kVmSize = 0x02000000;
constexpr u32 kLockedCache = 0xE0000000, kLockedCacheSize = 0x4000;
// The port's own guest memory, past the end of MEM1 in the BAT window (no game code addresses
// it): enlarged copies of tables the game sizes for its 4:3 view (hle_game.cpp, port tables).
// Committed at start and saved in states.
constexpr u32 kPortArena = 0x8F000000, kPortArenaSize = 0x80000;
// The game's heap extension: 8 MiB of RAM right after MEM1 (physical 0x01800000-0x01FFFFFF,
// cached 0x81800000 and uncached 0xC1800000, mapped with MEM1 like a 32 MiB console), which the
// game is never told about (0x80000028 says 24 MiB) and only gets for an allocation its full
// heap would fail (hle_game.cpp, heap extension). Inside 32 MiB so every device register that
// holds a physical address reaches it (texture bases keep 20-21 bits of address >> 5).
constexpr u32 kMem1Ext = 0x00800000, kMem1Mapped = kMem1Size + kMem1Ext;
bool memInit();
// MEM1 or its extension (guest RAM at g_base + a)
inline bool isMem1(u32 a) { return (a >= 0x80000000u && a < 0x80000000u + kMem1Mapped) || (a >= 0xC0000000u && a < 0xC0000000u + kMem1Mapped); }
// Physical address (as DMA engines see it) -> host pointer into MEM1, or nullptr.
u8* physPtr(u32 phys, u32 len);

// --- VM region (vm.cpp) ---------------------------------------------------------------
void vmFlushAll();
void vmSetSdr1(u32 v);
void vmAramReport();  // R2_VM_SLOTS: the game's ARAM swap slots

// --- guest functions ------------------------------------------------------------------
GuestFn lookupFn(u32 addr);
GuestFn lookupOrig(u32 addr);  // translated body of a hooked function
void tableInit();
void interpret(Ctx& c, u32 addr);  // interp.cpp: code with no recompiled function
void profilerStart();  // profiler.cpp: R2_PROFILE=<seconds> (call on the guest's host thread)
// profiler.cpp, always on: stall reports (no frame for 250 ms: where the game thread is) and a
// perf line every 30 s. diagStart on the guest's host thread; diagFramePresented per frame.
void diagStart();
void diagFramePresented(u32 draws, u64 presentTb);  // presentTb: time in aurora_end_frame
void diagUiFrame();  // the launcher's / shader preparation's frame loop is alive (stall reports there too)
extern u64 g_idleTb;  // time the guest's host thread waited for the next event (idleWait)

// --- time and events ------------------------------------------------------------------
constexpr u64 kBusClock = 162000000, kCpuClock = 486000000, kTbClock = kBusClock / 4;
u64 tbNow();                       // guest time base (40.5 MHz)
void clockInit();                  // cpu.cpp: the console's clock (time base, RTC) at the PC's local time
u32 rtcSeconds();                  // cpu.cpp: the RTC (local seconds since 2000-01-01)
void schedule(int id, u64 atTb);   // (re)arm event id at time base value
void unschedule(int id);
bool scheduled(int id);
void eventsInit();
void runDueEvents();               // on the guest thread: fire every due event
void wakeGuest();                  // from any host thread: make the guest poll
enum Event : int {
    EV_VI = 0,
    EV_DEC,
    EV_DI,
    EV_AI,
    EV_ARAM,
    EV_DSP,
    EV_SI,
    EV_EXI0,
    EV_EXI1,
    EV_EXI2,
    EV_COUNT
};
void decWrite(u32 v);
u32 decRead();

// --- interrupts -----------------------------------------------------------------------
// PI interrupt sources (INTSR bits)
enum : u32 {
    PI_ERROR = 1 << 0,
    PI_RSW = 1 << 1,
    PI_DI = 1 << 2,
    PI_SI = 1 << 3,
    PI_EXI = 1 << 4,
    PI_AI = 1 << 5,
    PI_DSP = 1 << 6,
    PI_MEM = 1 << 7,
    PI_VI = 1 << 8,
    PI_PE_TOKEN = 1 << 9,
    PI_PE_FINISH = 1 << 10,
    PI_CP = 1 << 11,
    PI_DEBUG = 1 << 12,
    PI_HSP = 1 << 13,
};
void piSetIrq(u32 bits, bool on);
bool piPending();
bool decPending();
void decAck();

// --- threads (threads.cpp) ------------------------------------------------------------
void threadsInit(Ctx*& bootCtx);
Ctx& curCtx();
void deliverException(Ctx& c, u32 exc, u32 pc);

// --- devices --------------------------------------------------------------------------
// disc_reader.cpp: the disc image. discRead reads at once (boot); the DVD drive asks for a read
// with discPrefetch when it starts one (the reader thread brings it in, and what follows it) and
// takes the bytes with discTake when the emulated transfer ends - Pending: not in yet, finish
// later (discLate logs a long wait); Failed: past the disc or unreadable.
// spanish: the Spanish disc (GSWS64), whose texts, menus and voices the game then reads instead of
// the English ones (disc_language.cpp); empty, or not usable (logged): English
bool discOpen(const std::string& path, const std::string& spanish = {});
bool discRead(u64 off, void* dst, u32 len);
void discPrefetch(u64 off, u32 len);
enum class DiscData { Ready, Pending, Failed };
DiscData discTake(u64 off, void* dst, u32 len);
void discLate(u64 off, u32 len, double ms);
// main.cpp: whether an image is the disc Red Two needs (Rogue Leader, USA, GSWE64 revision 0):
// "" if so, else why not in a sentence; *what: what it is ("Star Wars Rogue Squadron II (GSWE64),
// RVZ image, 1.4 GiB") when it could be read
std::string discProblem(const std::string& path, std::string* what = nullptr);
std::string discFileAt(u64 off);  // "path+0xoffset" of the disc file containing off (debugging)
// main.cpp: a DVD read starting at off (hw.cpp diStart). Inside a level resource of
// Rogue2/lvlPackNN.dat (lNN in its .dir) the level the game loads: logged when it changes, and
// Discord Rich Presence names it.
void discNoteRead(u64 off);
int discLevel();  // the level the game read last (NN of lNN; -1 none yet): log lines
void discSetLevel(int level);  // (a save state's; logged when it changes)
void hwInit();
void viFrameHook();  // called once per guest frame (XFB handoff)
bool viIsFramebuffer(u32 addr);  // hw.cpp: has the VI been told to scan out this XFB?
void viSetRate(int fps);   // hw.cpp: retrace rate (60 = 59.94 Hz NTSC, or 90/120/144)
void viSetRateHz(double hz);  // hw.cpp: any retrace rate
// gx_bridge.cpp: the retrace rate to play at - the frame rate setting, or with V-Sync on the
// display's refresh (or a whole fraction of it) that matches it (see there)
double frameRateHz();
void presentPaced(bool on);  // gx_bridge.cpp: V-Sync as the setting says (on) or off (a cinematic skip)
double viRate();           // current retrace rate in Hz
u32 viRetraceCount();      // retraces so far
u32 viXfbLines();          // lines the VI scans out (304 for letterboxed cinematics, 450 in game)
void setAudioSpeed(double speed, bool mute);  // hw.cpp: AI (audio) clock speed-up, output muted
void setDvdBoost(double boost);               // hw.cpp: disc reads that much faster
bool cinematicSkip();                         // hle_game.cpp: a cinematic is being skipped
void setGameAspect(double aspect);  // hle_game.cpp: the game's camera aspect (widescreen)
// hw.cpp: a display copy to dest lands at this line of the VI's (front or back) frame buffer
bool viLocateCopy(u32 dest, u32 strideBytes, u32& line, u32& lines);
// hw.cpp: memory card images (the save importer, dolphin_import.cpp)
std::string cardImagePath(int slot);                       // the image file of slot A
bool cardCurrentImage(int slot, std::vector<u8>& out);     // the inserted card (false: none yet)
void cardDetach(int slot);  // the file was replaced: keep the game's later writes in memory only
void cardFlushNow();        // card writes not yet in the image file go there now (before quitting)
// main.cpp: start a new instance with the same arguments, then quit. The new instance goes
// straight into the game, or to the launcher with toLauncher (R2_LAUNCHER=1/0 in its environment).
void restartGame(bool toLauncher = false);
void resetGame(bool toLauncher = false);
void setRestartProgram(const std::string& path);  // main.cpp: restarts start this program instead (an update)  // main.cpp: the menu's reset (card writes flushed, then restartGame)
// main.cpp: the window was closed or the menu's quit chosen: a save state being written and the
// memory card's last writes reach their files, then the process ends
[[noreturn]] void quitToDesktop();

// hle_game.cpp: the game's own passcode unlocks (two flag words of its progress block, kept in
// its save), for the menu's cheats page
namespace unlocks {
enum Item : int { AllMissions, TechUpgrades, AceMode, ArtGallery, MusicHall, Documentary, Commentary, Credits, BlackWhite, Count };
const char* name(int item);
const char* help(int item);
int state(int item);          // 1 on, 0 off, -1 not known yet (the game has not run a frame)
void set(int item, bool on);  // applied at the game's next frame
}  // namespace unlocks

namespace gfx {
bool menuOpen();  // gfx/menu.cpp: the settings menu has the controls (the game sees idle pads)
bool launcherShowing();  // gfx/menu.cpp: the launcher is up (the game has not started)
// gfx/touch.cpp: on-screen touch controls (Android): every SDL event, drawn inside a frame,
// merged into port 1's PADStatus (false: not in use)
void touchEvent(const void* sdlEvent);
void touchDraw();
bool touchApply(void* padStatus);
void setHudAnchoring(bool on);  // gfx/menu.cpp: widescreen HUD pieces at the screen edges (in flight)
void noteWidenedAspect(float aspect);  // gfx/menu.cpp: a projection aspect a camera was widened to
void buttonIconsUpdate();  // gfx/button_icons.cpp: Xbox / PlayStation button prompts per the setting, controller and bindings
void buttonIconsNoteTexture(u32 fmt, u32 w, u32 h, const u8* data, u32 bytes);  // a bound texture (baked prompts)
void setMenuTiling(bool on);    // gfx/gx_bridge.cpp: widescreen menu backgrounds tiled into the sides
void resendStereoViews();       // gfx/gx_bridge.cpp: VR: the eyes' views again (the camera turned with the head)
}

namespace input {
void init(bool pads);  // pads: Aurora PAD (after the window exists); false = R2_INPUT script only
void poll();  // once per frame: Aurora pads/keyboard -> SI controller state
void rumble(int chan, u32 cmd);  // SI motor bits: 0 stop, 1 rumble, 2 hard stop
bool isPlayStation(int sdlGamepadType);  // an SDL_GamepadType of a PS3 / PS4 / PS5 controller
bool port1PlayStation();  // port 1's controller is a PlayStation one (as of the last poll; button prompts)
// the settings' deadzone (round, rescaled) and sensitivity of the control stick or the C-stick; -1..1
void shapeStick(float& x, float& y, bool mainStick);
// The control stick (-1..1, as the controller reports it) as the game gets it: raw PADStatus
// values, which the game's own PADClamp then takes 15 off and limits to 72 (stick menu meter: the
// turn / pitch rate that gives in flight, 0..1 of the craft's maximum, in rateX / rateY).
void mainStickToGame(float x, float y, s8& rawX, s8& rawY, float* rateX = nullptr, float* rateY = nullptr);
// The C-stick likewise (PADClamp: 15 off, at most 59, 31 each on the diagonal); gameX / gameY: the
// fraction of its range the game reads (stick menu meter).
void cStickToGame(float x, float y, s8& rawX, s8& rawY, float* gameX = nullptr, float* gameY = nullptr);
// Port 1's triggers for the menu's trigger meter: how far each is pulled (0..1, from SDL), what
// the game gets (analog 0..255 after the travel curve; the click bit) and where the click sits on
// the travel (0: not on the trigger - the bumpers have it). 1: read; 0: no controller on port 1;
// -1: the input thread has the controllers (a device scan): keep the last reading.
struct TriggerView {
    float travel[2];
    u8 analog[2];
    bool click[2];
    float clickAt[2];
};
int port1Triggers(TriggerView& v);
// The game thread may call SDL's controller API while this is held. Not held: the input thread
// is updating the controllers, maybe a device scan of seconds - use the previous state instead.
class JoystickAccess {
public:
    JoystickAccess();
    ~JoystickAccess();
    JoystickAccess(const JoystickAccess&) = delete;
    JoystickAccess& operator=(const JoystickAccess&) = delete;
    explicit operator bool() const { return m_held; }

private:
    bool m_held = false;
};
}  // namespace input

}  // namespace rt
