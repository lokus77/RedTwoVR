// RetroAchievements through rcheevos' rc_client (extern/rcheevos, MIT), softcore only: save
// states, cheats and the port's options stay usable, and nothing is reported as hardcore.
//
// The game's memory as RetroAchievements sees a GameCube (rcheevos consoleinfo.c): addresses
// 0x00000000-0x017FFFFF are MEM1, big-endian bytes as stored - here at guest 0x80000000, the
// same physical layout as on hardware and in Dolphin (whose sets these are). The VM region the
// game runs from (0x7F000000+) is paged into MEM1 frames by the game itself, as on hardware, so
// what RetroAchievements reads is what it reads in Dolphin.
//
// Threads: everything rc_client does runs on the game's thread - the retrace handler (hw.cpp
// onVi) hands it the server's replies and calls rc_client_do_frame (which reads memory), the menu
// (gfx/menu.cpp, also the game's thread) logs in and lists. HTTP requests run on worker threads
// (WinHTTP on Windows, the system's libcurl on Linux, loaded at run time), badges are decoded
// there (libpng) and become ImGui textures on the game's thread.
#include "achievements.h"
#include "http.h"

#include "mods.h"
#include "runtime.h"
#include "savestate.h"
#include "settings.h"
#include "../common/disc_image.h"

#include <aurora/imgui.h>
#include <png.h>
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#else
#include <dlfcn.h>
#endif

namespace rt::ra {
namespace {

rc_client_t* s_client = nullptr;
std::string s_discPath;
std::string s_userAgent;
std::string s_hash;                  // the disc's RetroAchievements hash, once computed
std::atomic<bool> s_hashReady{false};
std::thread s_hashThread;
bool s_loginStarted = false, s_gameStarted = false;
bool s_busy = false;
std::string s_message;
std::vector<Popup> s_popups;

double now() { return double(tbNow()) / double(kTbClock); }

void popup(const std::string& title, const std::string& text, const std::string& badge = {}) {
    if (!settings::get().raNotifications) return;
    if (s_popups.size() >= 6) s_popups.erase(s_popups.begin());
    s_popups.push_back({title, text, badge, -1.0});
}

// Two workers: the API (unlocks, pings, logins: small and in order) and images, so a slow
// badge never holds up an unlock.
struct Job {
    std::string url, post, type;
    std::function<void(bool ok, int status, std::string body)> done;  // on the worker
};
struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::thread thread;
};
Queue s_api, s_images;
std::mutex s_repliesMutex;
std::vector<std::function<void()>> s_replies;  // run on the game's thread (frame)

void runQueue(Queue* q) {
    for (;;) {
        Job j;
        {
            std::unique_lock lk(q->m);
            q->cv.wait(lk, [&] { return !q->jobs.empty(); });
            j = std::move(q->jobs.front());
            q->jobs.pop_front();
        }
        http::Request req;
        req.url = j.url;
        req.post = j.post;
        req.contentType = j.type;
        req.userAgent = s_userAgent;
        http::Response res = http::fetch(req);
        j.done(res.ok, res.status, std::move(res.body));
    }
}

void enqueue(Queue& q, Job j) {
    {
        std::lock_guard lk(q.m);
        if (!q.thread.joinable()) q.thread = std::thread(runQueue, &q);
        q.jobs.push_back(std::move(j));
    }
    q.cv.notify_one();
}

void RC_CCONV serverCall(const rc_api_request_t* req, rc_client_server_callback_t cb, void* data, rc_client_t*) {
    Job j;
    j.url = req->url;
    j.post = req->post_data ? req->post_data : "";
    j.type = req->content_type ? req->content_type : "";
    j.done = [cb, data](bool ok, int status, std::string body) {
        auto reply = [cb, data, ok, status, body = std::move(body)] {
            rc_api_server_response_t r{};
            r.body = body.data();
            r.body_length = body.size();
            // no answer at all (no network): rc_client retries those
            r.http_status_code = ok ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
            cb(&r, data);
        };
        std::lock_guard lk(s_repliesMutex);
        s_replies.push_back(std::move(reply));
    };
    enqueue(s_api, std::move(j));
}

// --- images (badges) ---------------------------------------------------------------------------
struct Image {
    enum State { Loading, Decoded, Ready, Failed } state = Loading;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    ImTextureID tex{};
};
std::mutex s_imagesMutex;
std::map<std::string, std::shared_ptr<Image>> s_imageCache;

bool decodePng(const std::string& data, Image& img) {
    png_image p{};
    p.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&p, data.data(), data.size())) return false;
    p.format = PNG_FORMAT_RGBA;
    img.rgba.resize(PNG_IMAGE_SIZE(p));
    if (!png_image_finish_read(&p, nullptr, img.rgba.data(), 0, nullptr)) {
        png_image_free(&p);
        return false;
    }
    img.w = p.width;
    img.h = p.height;
    return img.w && img.h;
}

// --- disc hash: rcheevos' GameCube hash, read through the disc image reader (RVZ too) ------------
struct DiscFile {
    std::unique_ptr<disc::Image> img;
    int64_t pos = 0;
};
void* RC_CCONV discOpen(const char* path) {
    std::string err;
    auto img = disc::open(path, err);
    if (!img) return nullptr;
    return new DiscFile{std::move(img), 0};
}
void RC_CCONV discSeek(void* h, int64_t off, int origin) {
    auto* f = static_cast<DiscFile*>(h);
    const int64_t size = int64_t(f->img->size());
    f->pos = origin == SEEK_SET ? off : origin == SEEK_CUR ? f->pos + off : size + off;
    f->pos = std::clamp<int64_t>(f->pos, 0, size);
}
int64_t RC_CCONV discTell(void* h) { return static_cast<DiscFile*>(h)->pos; }
size_t RC_CCONV discReadFn(void* h, void* buf, size_t n) {
    auto* f = static_cast<DiscFile*>(h);
    const int64_t size = int64_t(f->img->size());
    const size_t len = size_t(std::max<int64_t>(0, std::min<int64_t>(int64_t(n), size - f->pos)));
    if (!len || !f->img->read(uint64_t(f->pos), buf, uint32_t(len))) return 0;
    f->pos += int64_t(len);
    return len;
}
void RC_CCONV discClose(void* h) { delete static_cast<DiscFile*>(h); }
void RC_CCONV hashMessage(const char* msg, const rc_hash_iterator_t*) { logf("ra", "hash: %s", msg); }

void computeHash() {
    rc_hash_iterator_t it;
    rc_hash_initialize_iterator(&it, s_discPath.c_str(), nullptr, 0);
    it.callbacks.verbose_message = hashMessage;
    it.callbacks.error_message = hashMessage;
    it.callbacks.filereader = {discOpen, discSeek, discTell, discReadFn, discClose};
    char hash[33] = {};
    const bool ok = rc_hash_generate(hash, RC_CONSOLE_GAMECUBE, &it) != 0;
    rc_hash_destroy_iterator(&it);
    if (ok) {
        s_hash = hash;
        logf("ra", "disc hash %s", hash);
    } else {
        logf("ra", "could not hash the disc %s", s_discPath.c_str());
    }
    s_hashReady = true;
}

// --- rc_client ---------------------------------------------------------------------------------
uint32_t RC_CCONV readMemory(uint32_t addr, uint8_t* buf, uint32_t n, rc_client_t*) {
    if (addr >= kMem1Size) return 0;
    n = std::min<uint32_t>(n, kMem1Size - addr);
    memcpy(buf, hp(0x80000000u + addr), n);
    return n;
}

// rcheevos' messages. The log is meant to be attached to bug reports as it is, so it names no
// account: rc_client's "Attempting to log in <user> (with password|token)" loses the name.
void RC_CCONV logMessage(const char* msg, const rc_client_t*) {
    static const char kLogin[] = "Attempting to log in ";
    if (!strncmp(msg, kLogin, sizeof kLogin - 1)) {
        const char* with = strstr(msg, " (with ");
        logf("ra", "Attempting to log in%s", with ? with : "");
        return;
    }
    logf("ra", "%s", msg);
}

std::string badgeUrl(const rc_client_achievement_t* a) {
    if (a->badge_url) return a->badge_url;
    char b[256];
    return rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, b, sizeof b) == RC_OK ? b : "";
}

void RC_CCONV onEvent(const rc_client_event_t* e, rc_client_t* client) {
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        const rc_client_achievement_t* a = e->achievement;
        logf("ra", "achievement unlocked: %s (%u points)", a->title, a->points);
        char t[64];
        snprintf(t, sizeof t, "ACHIEVEMENT UNLOCKED  -  %u POINTS", a->points);
        popup(t, std::string(a->title) + "\n" + a->description, badgeUrl(a));
        break;
    }
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        popup("LEADERBOARD ATTEMPT STARTED", e->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
        popup("LEADERBOARD ATTEMPT FAILED", e->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        popup("LEADERBOARD SUBMITTED", std::string(e->leaderboard->title) + ": " + e->leaderboard->tracker_value);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t* g = rc_client_get_game_info(client);
        popup("SET COMPLETE", std::string("Every achievement of ") + (g ? g->title : "the game") + " unlocked.");
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        logf("ra", "server error (%s): %s", e->server_error->api, e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        popup("RETROACHIEVEMENTS", "No connection to the server: unlocks are kept and sent later.");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        popup("RETROACHIEVEMENTS", "Connected again: every unlock has been sent.");
        break;
    default:
        break;
    }
}

void startGame();

void RC_CCONV onGameLoaded(int result, const char* error, rc_client_t* client, void*) {
    s_busy = false;
    if (result != RC_OK) {
        s_message = result == RC_NO_GAME_LOADED ? "RetroAchievements does not know this disc (hash " + s_hash + ")."
                                                : std::string("Could not load the achievements: ") + (error ? error : "?");
        logf("ra", "%s", s_message.c_str());
        return;
    }
    const rc_client_game_t* g = rc_client_get_game_info(client);
    rc_client_user_game_summary_t sum{};
    rc_client_get_user_game_summary(client, &sum);
    char b[160];
    snprintf(b, sizeof b, "%u of %u achievements unlocked, %u of %u points.", sum.num_unlocked_achievements,
             sum.num_core_achievements, sum.points_unlocked, sum.points_core);
    s_message.clear();
    logf("ra", "game %u \"%s\": %s", g ? g->id : 0, g ? g->title : "?", b);
    char icon[256] = {};
    if (g) rc_client_game_get_image_url(g, icon, sizeof icon);
    popup(g ? g->title : "RetroAchievements", b, icon);
}

void startGame() {
    if (s_gameStarted || !s_hashReady || !rc_client_get_user_info(s_client)) return;
    s_gameStarted = true;
    if (s_hash.empty()) {
        s_message = "The disc could not be read for RetroAchievements.";
        return;
    }
    s_busy = true;
    rc_client_set_encore_mode_enabled(s_client, settings::get().raEncore);
    rc_client_begin_load_game(s_client, s_hash.c_str(), onGameLoaded, nullptr);
}

void RC_CCONV onLogin(int result, const char* error, rc_client_t* client, void*) {
    s_busy = false;
    const rc_client_user_t* u = rc_client_get_user_info(client);
    if (result != RC_OK || !u) {
        s_message = std::string("Login failed: ") + (error ? error : "?");  // (rc_client logs it)
        webLoginResult(false, s_message);
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) {
            settings::get().raToken.clear();  // a token that no longer works: log in again
            settings::save();
        }
        s_loginStarted = false;
        return;
    }
    settings::Settings& s = settings::get();
    if (s.raUser != u->username || s.raToken != u->token) {
        s.raUser = u->username;
        s.raToken = u->token;
        settings::save();
    }
    s_message.clear();
    logf("ra", "logged in (%u softcore points)", u->score_softcore);  // (no account name in the log)
    char b[96];
    snprintf(b, sizeof b, "Logged in as %s (%u points).", u->display_name, u->score_softcore);
    popup("RETROACHIEVEMENTS", b);
    webLoginResult(true, std::string("Signed in as ") + u->display_name +
                             ". You can close this tab and go back to Red Two.");
    startGame();
}

void ensureClient() {
    if (s_client) return;
    s_client = rc_client_create(readMemory, serverCall);
    rc_client_enable_logging(s_client, RC_CLIENT_LOG_LEVEL_INFO, logMessage);
    rc_client_set_event_handler(s_client, onEvent);
    rc_client_set_hardcore_enabled(s_client, 0);  // softcore only
    rc_client_set_unofficial_enabled(s_client, 0);
    char clause[64] = {};
    rc_client_get_user_agent_clause(s_client, clause, sizeof clause);
#if defined(_WIN32)
    s_userAgent = std::string("RedTwo/1.0 (Windows) ") + clause;
#elif defined(__ANDROID__)
    s_userAgent = std::string("RedTwo/1.0 (Android) ") + clause;
#else
    s_userAgent = std::string("RedTwo/1.0 (Linux) ") + clause;
#endif
}

void startHash() {
    if (s_hashThread.joinable() || s_hashReady || s_discPath.empty()) return;
    s_hashThread = std::thread(computeHash);
}

void startLogin() {
    const settings::Settings& s = settings::get();
    if (s_loginStarted || s.raUser.empty() || s.raToken.empty()) return;
    s_loginStarted = true;
    s_busy = true;
    rc_client_begin_login_with_token(s_client, s.raUser.c_str(), s.raToken.c_str(), onLogin, nullptr);
}

std::string iniValue(const std::string& text, const std::string& key) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
            size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) i++;
            return v.substr(i);
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return {};
}

// Dolphin's login: <config folder>/RetroAchievements.ini, [Achievements] Username / ApiToken
bool dolphinLogin(std::string& user, std::string& token) {
    for (const std::string& dir : mods::dolphinConfigDirs()) {
        const std::filesystem::path p = std::filesystem::path(std::u8string(dir.begin(), dir.end())) / "RetroAchievements.ini";
        FILE* f = nullptr;
#ifdef _WIN32
        f = _wfopen(p.c_str(), L"rb");
#else
        f = fopen(p.c_str(), "rb");
#endif
        if (!f) continue;
        std::string text;
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
        user = iniValue(text, "Username");
        token = iniValue(text, "ApiToken");
        if (!user.empty() && !token.empty()) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
void init(const std::string& discPath) {
    s_discPath = discPath;
    ensureClient();
    if (!settings::get().achievements) return;
    startHash();
    startLogin();
}

// the disc the launcher ended up with (init had none, or another): hashed now
void setDisc(const std::string& discPath) {
    if (discPath == s_discPath || s_hashThread.joinable() || s_hashReady) return;
    s_discPath = discPath;
    if (settings::get().achievements) startHash();
}

void frame() {
    if (!s_client) return;
    std::vector<std::function<void()>> replies;
    {
        std::lock_guard lk(s_repliesMutex);
        replies.swap(s_replies);
    }
    for (auto& r : replies) r();
    webLoginPoll();
    if (!settings::get().achievements) return;
    startGame();  // once both the login and the hash are there
    if (rc_client_is_game_loaded(s_client)) rc_client_do_frame(s_client);
    else rc_client_idle(s_client);
}

// The launcher (gfx/menu.cpp), before the game runs: the server's replies (a login from the
// ACHIEVEMENTS page) and rc_client's idle work, but no frame - nothing to evaluate yet.
void idle() {
    if (!s_client) return;
    std::vector<std::function<void()>> replies;
    {
        std::lock_guard lk(s_repliesMutex);
        replies.swap(s_replies);
    }
    for (auto& r : replies) r();
    webLoginPoll();
    if (!settings::get().achievements) return;
    startGame();
    rc_client_idle(s_client);
}

Status status() {
    Status st;
    st.enabled = settings::get().achievements;
    st.busy = s_busy || (st.enabled && !s_hashReady && s_loginStarted);
    st.message = s_message;
    if (!s_client) return st;
    if (const rc_client_user_t* u = rc_client_get_user_info(s_client)) {
        st.loggedIn = true;
        st.user = u->display_name;
        st.score = u->score_softcore;
    }
    if (rc_client_is_game_loaded(s_client)) {
        st.gameLoaded = true;
        if (const rc_client_game_t* g = rc_client_get_game_info(s_client)) st.game = g->title;
        rc_client_user_game_summary_t sum{};
        rc_client_get_user_game_summary(s_client, &sum);
        st.unlocked = sum.num_unlocked_achievements;
        st.total = sum.num_core_achievements;
        st.points = sum.points_unlocked;
        st.pointsTotal = sum.points_core;
    }
    return st;
}

void setEnabled(bool on) {
    ensureClient();
    if (on) {
        startHash();
        startLogin();
        return;
    }
    if (rc_client_is_game_loaded(s_client)) rc_client_unload_game(s_client);
    s_gameStarted = false;
    s_message.clear();
}

void login(const std::string& user, const std::string& password) {
    ensureClient();
    startHash();
    s_loginStarted = true;
    s_busy = true;
    s_message.clear();
    rc_client_begin_login_with_password(s_client, user.c_str(), password.c_str(), onLogin, nullptr);
}

void logout() {
    if (!s_client) return;
    if (rc_client_is_game_loaded(s_client)) rc_client_unload_game(s_client);
    rc_client_logout(s_client);
    s_loginStarted = s_gameStarted = false;
    settings::Settings& s = settings::get();
    s.raToken.clear();
    settings::save();
    s_message = "Logged out.";
    logf("ra", "logged out");
}

bool dolphinLoginAvailable() {
    std::string u, t;
    return dolphinLogin(u, t);
}

bool importDolphinLogin(std::string& message) {
    std::string user, token;
    if (!dolphinLogin(user, token)) {
        message = "Dolphin has no RetroAchievements login on this computer.";
        return false;
    }
    ensureClient();
    if (rc_client_get_user_info(s_client)) {
        if (rc_client_is_game_loaded(s_client)) rc_client_unload_game(s_client);
        rc_client_logout(s_client);
    }
    settings::Settings& s = settings::get();
    s.raUser = user;
    s.raToken = token;
    s.achievements = true;
    settings::save();
    s_loginStarted = s_gameStarted = false;
    startHash();
    startLogin();
    message = "Logging in as " + user + " with Dolphin's login...";
    return true;
}

std::vector<Achievement> achievements() {
    std::vector<Achievement> out;
    if (!s_client || !rc_client_is_game_loaded(s_client)) return out;
    rc_client_achievement_list_t* list =
        rc_client_create_achievement_list(s_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return out;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        const rc_client_achievement_bucket_t& bucket = list->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; i++) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            Achievement x;
            x.id = a->id;
            x.title = a->title;
            x.description = a->description;
            x.points = a->points;
            x.unlocked = a->unlocked != RC_CLIENT_ACHIEVEMENT_UNLOCKED_NONE;
            x.progress = a->measured_progress;
            if (x.unlocked || !a->badge_locked_url) x.badgeUrl = badgeUrl(a);
            else x.badgeUrl = a->badge_locked_url;
            out.push_back(std::move(x));
        }
    }
    rc_client_destroy_achievement_list(list);
    return out;
}

std::vector<Popup>& popups() { return s_popups; }

void* image(const std::string& url) {
    if (url.empty()) return nullptr;
    std::shared_ptr<Image> img;
    {
        std::lock_guard lk(s_imagesMutex);
        auto& slot = s_imageCache[url];
        if (!slot) {
            slot = std::make_shared<Image>();
            std::weak_ptr<Image> weak = slot;
            Job j;
            j.url = url;
            j.done = [weak](bool ok, int status, std::string body) {
                auto im = weak.lock();
                if (!im) return;
                Image decoded;
                const bool good = ok && status == 200 && decodePng(body, decoded);
                std::lock_guard lk2(s_imagesMutex);
                if (good) {
                    im->w = decoded.w, im->h = decoded.h, im->rgba = std::move(decoded.rgba);
                    im->state = Image::Decoded;
                } else {
                    im->state = Image::Failed;
                }
            };
            enqueue(s_images, std::move(j));
        }
        img = slot;
        if (img->state == Image::Decoded) {
            img->tex = aurora_imgui_add_texture(img->w, img->h, img->rgba.data());
            img->rgba.clear();
            img->rgba.shrink_to_fit();
            img->state = Image::Ready;
        }
    }
    return img->state == Image::Ready ? reinterpret_cast<void*>(img->tex) : nullptr;
}

// --- save states --------------------------------------------------------------------------------
namespace {
std::vector<u8> s_loadedProgress;
bool s_haveLoadedProgress = false;
}

void saveState(state::Writer& w) {
    w.section("RA  ");
    std::vector<u8> blob;
    if (s_client && rc_client_is_game_loaded(s_client)) {
        blob.resize(rc_client_progress_size(s_client));
        if (rc_client_serialize_progress_sized(s_client, blob.data(), blob.size()) != RC_OK) blob.clear();
    }
    w.vec(blob);
}

void loadState(state::Reader& r) {
    r.section("RA  ");
    r.vec(s_loadedProgress);
    s_haveLoadedProgress = true;
}

void afterLoad() {
    if (!s_haveLoadedProgress) return;
    s_haveLoadedProgress = false;
    if (s_client && rc_client_is_game_loaded(s_client)) {
        // a state of another session or without progress: every achievement starts over
        if (s_loadedProgress.empty()) rc_client_deserialize_progress_sized(s_client, nullptr, 0);
        else rc_client_deserialize_progress_sized(s_client, s_loadedProgress.data(), s_loadedProgress.size());
    }
    s_loadedProgress.clear();
    s_loadedProgress.shrink_to_fit();
}

}  // namespace rt::ra
