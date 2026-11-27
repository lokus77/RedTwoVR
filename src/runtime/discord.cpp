// Discord Rich Presence: what the player is doing, on their Discord profile ("Playing Red Two:
// Battle of Hoth, Flying"), through the Discord app's local connection (its IPC: \\.\pipe\discord-ipc-N
// on Windows, the discord-ipc-N socket in the runtime or temporary folder elsewhere, Flatpak's and
// Snap's included). No library: the protocol is a frame of an opcode, a length and JSON (0 handshake,
// 1 a command, 2 close, 3 ping, 4 pong). Nothing goes anywhere but the Discord app on this computer,
// and only while the setting discord_presence (GAME tab) is on. Same protocol code as msr2ds.
//   - the launcher: "In the launcher";
//   - the game: the level resource it read last from the disc (lNN of lvlPackNN.dat, main.cpp
//     discNoteRead) - a mission's name, the hangar or the menus - and in a mission whether the
//     player flies, paused or watches a cutscene (hle_game.cpp), with the time since the game started.
// A connection lost or not there (Discord closed, not installed) is looked for again every 20 s.
// The log says when it connects, what it shows and why it stopped; never the Discord account.
// Quitting ends the process (quit: TerminateProcess / _exit): the connection closes with it and
// Discord clears the presence itself.
#include "discord.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "runtime.h"
#include "settings.h"
#include "update.h"

// Red Two's application on Discord (discord.com/developers/applications): its name is what the
// profile says is being played, its Rich Presence art asset "icon" the picture (CMake
// R2_DISCORD_APP_ID; empty: no Rich Presence). R2_DISCORD_INVITE: the server's invite (a button).
#ifndef R2_DISCORD_APP_ID
#define R2_DISCORD_APP_ID ""
#endif
#ifndef R2_DISCORD_INVITE
#define R2_DISCORD_INVITE ""
#endif

namespace rt::discord {

// The level resources' missions, by number (lNN in lvlPackNN.dir). The game's own list of them
// (in MEM1 once the front end has loaded: "01_DS1", "02_ISON", "03_HOTH", "04_MAW", "05_RAZOR",
// "06_KOTHLIS", "07_PREFSBELT", "08_BESPIN", "09_ENDOR", "10_DS2", "11_ESCAPE", "12_ASTEROIDS",
// "13_ENDURANCE", "14_DEFENCE", "15_YAVIN") goes by the same numbers: test runs read l01 for Death
// Star Attack, l03 for Battle of Hoth and l04 for Prisons of the Maw (R2_DVD_LOG). l16 and l17
// are missions not in that list (unnamed here); l18-l21 the hangars of the four craft groups
// (mission ids 18-21, 0x7FD80AC0), l22 the front end (boot, title, menus), l00, l23 and l26
// not seen yet.
const char* missionName(int level) {
    static constexpr const char* kNames[] = {
        "",                      // l00
        "Death Star Attack",     // l01 01_DS1
        "Ison Corridor Ambush",  // l02 02_ISON
        "Battle of Hoth",        // l03 03_HOTH
        "Prisons of the Maw",    // l04 04_MAW
        "Razor Rendezvous",      // l05 05_RAZOR
        "Vengeance on Kothlis",  // l06 06_KOTHLIS
        "Imperial Academy Heist",  // l07 07_PREFSBELT (Prefsbelt IV)
        "Raid on Bespin",        // l08 08_BESPIN
        "Battle of Endor",       // l09 09_ENDOR
        "Strike at the Core",    // l10 10_DS2
        "Death Star Escape",     // l11 11_ESCAPE
        "The Asteroid Field",    // l12 12_ASTEROIDS
        "Endurance",             // l13 13_ENDURANCE
        "Triumph of the Empire",  // l14 14_DEFENCE
        "Revenge on Yavin",      // l15 15_YAVIN
    };
    return level >= 0 && level < int(std::size(kNames)) ? kNames[level] : "";
}

namespace {

constexpr int kFirstHangar = 18, kLastHangar = 21;  // (22: the front end)
bool isMission(int level) { return level >= 1 && level < kFirstHangar; }

std::atomic<bool> s_launcher{false};
std::atomic<int> s_phase{int(Phase::FrontEnd)};
std::atomic<int> s_level{-1};
std::atomic<std::int64_t> s_gameStart{0};  // when the game started (unix seconds), for "elapsed"

enum class Status : int { Unavailable, TestRun, Off, Looking, Connecting, Shown, Refused };
std::atomic<int> s_status{int(Status::Unavailable)};

#if !defined(__ANDROID__)

std::string json(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

// ---- the connection to the Discord app (only the worker thread uses it) ----
enum Op : uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

struct Connection {
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool open() const { return pipe != INVALID_HANDLE_VALUE; }
#else
    int fd = -1;
    bool open() const { return fd >= 0; }
#endif
    std::string in;  // bytes read, not yet a whole frame

    void shut() {
#ifdef _WIN32
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
#else
        if (fd >= 0) close(fd);
        fd = -1;
#endif
        in.clear();
    }

    bool connect() {
#ifdef _WIN32
        for (int i = 0; i < 10; ++i) {
            wchar_t name[64];
            swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) return true;
        }
        return false;
#else
        // the runtime folder, else the temporary ones; the app there, or as Flatpak (Discord,
        // Vesktop) or Snap runs it
        std::vector<std::string> bases;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
            if (const char* v = getenv(var); v && *v) bases.emplace_back(v);
        bases.emplace_back("/tmp");
        static constexpr const char* kSubfolders[] = {"", "app/com.discordapp.Discord/", "app/dev.vencord.Vesktop/",
                                                      "snap.discord/", ".flatpak/com.discordapp.Discord/xdg-run/",
                                                      "app/com.discordapp.DiscordCanary/", "snap.discord-canary/"};
        for (const auto& base : bases)
            for (const char* sub : kSubfolders)
                for (int i = 0; i < 10; ++i) {
                    std::string path = base;
                    if (path.back() != '/') path += '/';
                    path += std::string(sub) + "discord-ipc-" + std::to_string(i);
                    sockaddr_un address{};
                    address.sun_family = AF_UNIX;
                    if (path.size() >= sizeof address.sun_path) continue;
                    memcpy(address.sun_path, path.c_str(), path.size() + 1);
                    const int s = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (s < 0) return false;
                    fcntl(s, F_SETFD, FD_CLOEXEC);  // (a restart execv's: the new image connects anew)
#ifdef __APPLE__
                    const int one = 1;
                    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) {
                        fd = s;
                        return true;
                    }
                    close(s);
                }
        return false;
#endif
    }

    bool send(uint32_t op, const std::string& body) {
        std::string frame(8, '\0');
        const uint32_t length = uint32_t(body.size());
        memcpy(&frame[0], &op, 4);  // (little-endian: every platform here)
        memcpy(&frame[4], &length, 4);
        frame += body;
        size_t done = 0;
        while (done < frame.size()) {
#ifdef _WIN32
            DWORD n = 0;
            if (!WriteFile(pipe, frame.data() + done, DWORD(frame.size() - done), &n, nullptr) || !n) {
                shut();
                return false;
            }
#else
#ifdef MSG_NOSIGNAL
            const ssize_t n = ::send(fd, frame.data() + done, frame.size() - done, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(fd, frame.data() + done, frame.size() - done, 0);
#endif
            if (n <= 0) {
                shut();
                return false;
            }
#endif
            done += size_t(n);
        }
        return true;
    }

    // What has come in, without waiting (up to waitMs for the first byte); false once it is gone.
    // (Windows: only this thread uses the pipe and no read of its own is ever pending, so
    // PeekNamedPipe answers at once.)
    bool read(int waitMs) {
        char buf[4096];
        for (int waited = 0;;) {
#ifdef _WIN32
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
                shut();
                return false;
            }
            if (available) {
                DWORD n = 0;
                if (!ReadFile(pipe, buf, std::min<DWORD>(available, sizeof buf), &n, nullptr) || !n) {
                    shut();
                    return false;
                }
                in.append(buf, n);
                continue;
            }
#else
            pollfd p{fd, POLLIN, 0};
            const int ready = poll(&p, 1, 0);
            if (ready > 0) {
                const ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n <= 0) {
                    shut();
                    return false;
                }
                in.append(buf, size_t(n));
                continue;
            }
            if (ready < 0) {
                shut();
                return false;
            }
#endif
            if (waited >= waitMs) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            waited += 20;
        }
    }

    // The next whole frame read, if one is there
    bool next(uint32_t& op, std::string& body) {
        if (in.size() < 8) return false;
        uint32_t length;
        memcpy(&op, in.data(), 4);
        memcpy(&length, in.data() + 4, 4);
        if (length > (1u << 20)) {  // not Discord's
            shut();
            return false;
        }
        if (in.size() < 8 + length) return false;
        body = in.substr(8, length);
        in.erase(0, 8 + length);
        return true;
    }
};

// "key":"value" in a frame's JSON (the first one), for the log; "" when not there
std::string field(const std::string& body, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const auto at = body.find(k);
    if (at == std::string::npos) return {};
    const auto v = body.find_first_not_of(" :", at + k.size());
    if (v == std::string::npos) return {};
    if (body[v] == '"') {
        const auto end = body.find('"', v + 1);
        return end == std::string::npos ? std::string{} : body.substr(v + 1, end - v - 1);
    }
    const auto end = body.find_first_of(",}", v);
    return body.substr(v, end == std::string::npos ? std::string::npos : end - v);
}

// ---- what is shown ----
// The activity's JSON, and its words for the log
std::string activity(std::string& said) {
    std::string details, state;
    const bool launcher = s_launcher.load();
    const int level = s_level.load();
    const Phase phase = Phase(s_phase.load());
    const bool hangar = level >= kFirstHangar && level <= kLastHangar;
    if (launcher) {
        details = "In the launcher";
    } else if (isMission(level) && phase != Phase::FrontEnd) {
        const char* name = missionName(level);
        details = *name ? name : "In a mission";
        switch (phase) {
        case Phase::Flying: state = "Flying"; break;
        case Phase::Paused: state = "Paused"; break;
        case Phase::Cutscene: state = "Watching a cutscene"; break;
        default: break;  // (loading, the results)
        }
    } else if (hangar) {
        details = "In the hangar";
    } else {
        details = "In the menus";  // (the front end, l22; the boot screen before it; the attract demo)
    }
    said = details + (state.empty() ? "" : ", " + state);
    std::string a = "{\"details\":" + json(details);
    if (!state.empty()) a += ",\"state\":" + json(state);
    if (const std::int64_t t = s_gameStart.load(); !launcher && t) a += ",\"timestamps\":{\"start\":" + std::to_string(t) + "}";
    a += ",\"assets\":{\"large_image\":\"icon\",\"large_text\":" + json(std::string("Red Two ") + update::version()) + "}";
    a += ",\"buttons\":[{\"label\":\"Get Red Two\",\"url\":\"https://github.com/lokus77/RedTwoVR\"}";
    if (*R2_DISCORD_INVITE) a += ",{\"label\":\"Join the Discord\",\"url\":" + json(R2_DISCORD_INVITE) + "}";
    a += "]}";
    return a;
}

std::string command(const std::string& act) {  // SET_ACTIVITY; no activity clears it
    static unsigned nonce = 0;
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    std::string args = "{\"pid\":" + std::to_string(pid);
    if (!act.empty()) args += ",\"activity\":" + act;
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":" + args + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
}

// R2_DISCORD=log (tests): what would be shown, logged when it changes; no Discord app is talked to
void dryRun() {
    std::string shown;
    for (;; std::this_thread::sleep_for(std::chrono::seconds(1))) {
        std::string said;
        if (activity(said) != shown) {
            shown = activity(said);
            logf("discord", "test (not sent): \"%s\"", said.c_str());
        }
    }
}

void work() {
    using Clock = std::chrono::steady_clock;
    Connection c;
    Clock::time_point nextTry{}, lastSent{};
    std::string shown;  // the activity sent last ("" none)
    bool toldMissing = false, toldOff = false, toldTaken = false;
    int failures = 0;  // handshakes refused in a row (a wrong application ID: looked for less often)
    for (;; std::this_thread::sleep_for(std::chrono::seconds(1))) {
        const auto now = Clock::now();
        if (!settings::get().discordPresence) {
            if (c.open()) {
                c.send(Frame, command(""));
                c.shut();
                logf("discord", "Rich Presence turned off: cleared");
            } else if (!toldOff) {
                logf("discord", "Rich Presence is off (GAME > Discord Rich Presence)");
            }
            toldOff = true;
            shown.clear();
            nextTry = {};
            s_status = int(Status::Off);
            continue;
        }
        toldOff = false;
        if (!c.open()) {
            if (now < nextTry) continue;
            nextTry = now + std::chrono::seconds(failures >= 3 ? 120 : 20);
            if (!c.connect()) {
                if (!toldMissing) logf("discord", "no Discord app running on this computer (looked for every 20 s)");
                toldMissing = true;
                if (s_status != int(Status::Refused)) s_status = int(Status::Looking);
                continue;
            }
            // the handshake, then READY (or a close that says why not)
            s_status = int(Status::Connecting);
            bool ready = false;
            std::string why;
            if (c.send(Handshake, std::string("{\"v\":1,\"client_id\":") + json(R2_DISCORD_APP_ID) + "}")) {
                for (int waited = 0; waited < 5000 && c.open() && !ready && why.empty(); waited += 100) {
                    c.read(100);
                    uint32_t op;
                    std::string body;
                    while (c.next(op, body)) {
                        if (op == Frame && field(body, "evt") == "READY") ready = true;
                        else if (op == Close || field(body, "evt") == "ERROR")
                            why = field(body, "message") + " (" + field(body, "code") + ")";
                    }
                }
            }
            if (!ready) {
                c.shut();
                failures++;
                s_status = int(Status::Refused);
                logf("discord", "the Discord app did not take the connection%s%s", why.empty() ? "" : ": ", why.c_str());
                continue;
            }
            failures = 0;
            toldMissing = false;
            toldTaken = false;
            shown.clear();
            logf("discord", "connected to the Discord app: Rich Presence on");
        }
        // what came in: pings answered, a close or an error said once
        if (!c.read(0)) {
            logf("discord", "the Discord app closed the connection (looked for again in 20 s)");
            nextTry = now + std::chrono::seconds(20);
            s_status = int(Status::Looking);
            continue;
        }
        uint32_t op;
        std::string body;
        while (c.next(op, body)) {
            if (op == Ping) {
                c.send(Pong, body);
            } else if (op == Close) {
                logf("discord", "the Discord app closed the connection: %s", field(body, "message").c_str());
                c.shut();
            } else if (field(body, "evt") == "ERROR") {
                static int told = 0;
                if (told++ < 5)
                    logf("discord", "the Discord app refused the presence: %s (%s)", field(body, "message").c_str(),
                         field(body, "code").c_str());
            } else if (field(body, "cmd") == "SET_ACTIVITY" && !toldTaken && !shown.empty()) {
                toldTaken = true;  // (once a connection)
                logf("discord", "the Discord app shows it");
            }
        }
        if (!c.open()) {
            nextTry = now + std::chrono::seconds(20);
            s_status = int(Status::Looking);
            continue;
        }
        s_status = int(Status::Shown);
        std::string said;
        const std::string act = activity(said);
        // Discord takes 5 updates in 20 s: one every 4 s at most, the latest
        if (act != shown && now - lastSent >= std::chrono::seconds(4)) {
            if (c.send(Frame, command(act))) {
                shown = act;
                lastSent = now;
                logf("discord", "showing \"%s\"", said.c_str());
            }
        }
    }
}

#endif  // !__ANDROID__

}  // namespace

void init() {
#if !defined(__ANDROID__)
    const char* test = getenv("R2_DISCORD");
    if (test && !strcmp(test, "log")) {
        std::thread(dryRun).detach();
        return;
    }
    const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    if (!*R2_DISCORD_APP_ID) return;  // (a build without one)
    if (automated && !envFlag("R2_DISCORD")) {
        s_status = int(Status::TestRun);
        return;
    }
    s_status = int(Status::Off);
    std::thread(work).detach();
#endif
}

void setLauncher(bool open) { s_launcher = open; }

void gameFrame(Phase phase) {
    if (!s_gameStart.load(std::memory_order_relaxed)) s_gameStart = std::int64_t(time(nullptr));
    s_phase.store(int(phase), std::memory_order_relaxed);
}

void setLevel(int level) { s_level = level; }

const char* invite() { return R2_DISCORD_INVITE; }

std::string statusText() {
    switch (Status(s_status.load())) {
    case Status::Unavailable: return "Not available in this build.";
    case Status::TestRun: return "Off in automated test runs.";
    case Status::Off: return "Off: nothing goes to Discord.";
    case Status::Looking: return "The Discord app is not running on this PC (Red Two looks again every 20 s).";
    case Status::Connecting: return "Connecting to the Discord app...";
    case Status::Shown: return "Shown on your Discord profile.";
    case Status::Refused: return "The Discord app did not take the connection (the log says why).";
    }
    return "";
}

}  // namespace rt::discord
