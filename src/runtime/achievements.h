// RetroAchievements (achievements.cpp): rcheevos' rc_client (extern/rcheevos, MIT) in softcore
// mode. Everything runs on the game's thread: the retrace handler drives rc_client and hands it
// the server's replies (HTTP on worker threads); the menu (gfx/menu.cpp) logs in and lists.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rt::state {
struct Writer;
struct Reader;
}

namespace rt::ra {

void init(const std::string& discPath);  // main.cpp, after the settings (the disc may come later)
void setDisc(const std::string& discPath);  // the launcher's choice when init had none
void frame();                            // hw.cpp onVi: replies, then rc_client_do_frame
void idle();                             // the launcher (no game running yet): replies, rc_client_idle

// save states: the achievements' progress travels with them
void saveState(state::Writer& w);
void loadState(state::Reader& r);
void afterLoad();

// --- the menu --------------------------------------------------------------------------------
struct Status {
    bool enabled = false;
    bool busy = false;        // logging in or loading the game's achievements
    bool loggedIn = false;
    bool gameLoaded = false;  // the disc is known and its set is loaded
    std::string user;         // display name
    uint32_t score = 0;       // softcore points
    std::string game;         // the set's title
    uint32_t unlocked = 0, total = 0, points = 0, pointsTotal = 0;
    std::string message;      // the last thing worth telling (login failed, unknown disc, ...)
};
Status status();
void setEnabled(bool on);  // the setting changed
void login(const std::string& user, const std::string& password);
void logout();
bool importDolphinLogin(std::string& message);  // Dolphin's RetroAchievements.ini (user + token)
bool dolphinLoginAvailable();
// Sign-in in the player's browser (ra_web_login.cpp): a page Red Two serves on 127.0.0.1 for 10
// minutes; the name and password typed there go to login() on the game's thread.
bool webLoginStart(std::string& url, std::string& error);  // starts it (or keeps it): its address
void webLoginStop();
bool webLoginRunning();
void webLoginPoll();                                     // frame / idle: a login the page sent
void webLoginResult(bool ok, const std::string& text);  // onLogin: what the page shows next

struct Achievement {
    uint32_t id = 0;
    std::string title, description, badgeUrl;
    uint32_t points = 0;
    bool unlocked = false;
    std::string progress;  // "3/10" for measured ones
};
std::vector<Achievement> achievements();  // the loaded set, locked first in the game's order

// pop-ups for the overlay (unlocks, leaderboards, the game's start)
struct Popup {
    std::string title, text, badgeUrl;
    double shownAt = -1.0;  // the overlay's clock when it first drew it
};
std::vector<Popup>& popups();  // the overlay draws and expires them

// a badge or icon as an ImGui texture (ImTextureID), once downloaded; nullptr until then
void* image(const std::string& url);

}  // namespace rt::ra
