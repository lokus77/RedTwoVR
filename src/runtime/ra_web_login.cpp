// RetroAchievements sign-in in the player's browser (the menu's ACHIEVEMENTS > "Log in in your
// browser"). RetroAchievements has no web sign-in for programs (no OAuth or device codes: a
// program logs in with the user name and password and keeps the token the server answers with),
// so this page comes from Red Two itself: a small HTTP server on 127.0.0.1 at a random port, under
// a random path, serving one form. The browser posts the name and password to it; the game's
// thread (ra::frame / ra::idle -> webLoginPoll) logs in with them exactly as the menu's own Log in
// row does, and only the token is kept. The page asks for the result and shows it.
//
// Only this computer can reach it (loopback), and only through the address Red Two opened: the
// path carries 128 random bits, the Host header must be 127.0.0.1 / localhost at this port (no DNS
// rebinding) and a POST's Origin, when the browser sends one, must be the page's own. It stops 10
// minutes after it started, or a few seconds after a successful login (the page has had time to
// read it). The password is never logged or stored, and is overwritten once handed to rcheevos.
#include "achievements.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "runtime.h"
#include "settings.h"

namespace rt::ra {
namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { close(s); }
#endif

using Clock = std::chrono::steady_clock;
constexpr auto kLifetime = std::chrono::minutes(10);
constexpr auto kAfterOk = std::chrono::seconds(8);

enum class WebState { Waiting, Busy, Ok, Failed };

std::mutex s_mu;
std::thread s_thread;
std::atomic<bool> s_stop{false}, s_running{false};
std::string s_secret;  // the page's path
int s_port = 0;
// a login the page sent, for the game's thread
bool s_pending = false;
std::string s_user, s_pass;
// what the page shows
WebState s_state = WebState::Waiting;
std::string s_text;
Clock::time_point s_okAt{};

void wipe(std::string& s) {
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", unsigned(static_cast<unsigned char>(c)));
            o += b;
        } else o += c;
    }
    return o;
}

// application/x-www-form-urlencoded: one field's value ("" when absent)
std::string formValue(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        const std::string pair = body.substr(pos, amp - pos);
        pos = amp + 1;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos || pair.compare(0, eq, key) != 0 || eq != key.size()) continue;
        std::string v;
        for (size_t i = eq + 1; i < pair.size(); i++) {
            const char c = pair[i];
            if (c == '+') v += ' ';
            else if (c == '%' && i + 2 < pair.size() && std::isxdigit(static_cast<unsigned char>(pair[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(pair[i + 2]))) {
                v += char(std::stoi(pair.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else v += c;
        }
        return v;
    }
    return {};
}

const char* const kPage = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Red Two - RetroAchievements sign-in</title>
<style>
:root{color-scheme:dark}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
 background:radial-gradient(ellipse at top,#0b1a33,#03070f 70%);color:#dce6f5;
 font-family:Bahnschrift,"DIN Alternate","Segoe UI",system-ui,sans-serif}
.panel{width:min(440px,calc(100% - 32px));border:1.5px solid #ffd646;padding:28px 28px 22px;
 background:linear-gradient(#0a1830ee,#040a16f0);
 clip-path:polygon(18px 0,100% 0,100% calc(100% - 18px),calc(100% - 18px) 100%,0 100%,0 18px)}
h1{margin:0;color:#ffd646;letter-spacing:.18em;font-size:30px}
.sub{color:#7fd0ff;letter-spacing:.14em;font-size:12px;margin:4px 0 20px}
label{display:block;font-size:13px;letter-spacing:.12em;color:#ffd646;margin:14px 0 6px}
input{width:100%;box-sizing:border-box;padding:10px 12px;font:inherit;font-size:16px;color:#fff;
 background:#0a1730;border:1px solid #ffd64688;border-radius:0;outline:none}
input:focus{border-color:#ff9628}
button{margin-top:22px;width:100%;padding:12px;font:inherit;font-size:16px;letter-spacing:.18em;
 color:#1a0f00;background:#ff9628;border:0;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
.msg{min-height:1.4em;margin-top:14px;font-size:15px}
.err{color:#ff9628}.ok{color:#ffd646}
.note{margin-top:18px;font-size:12px;color:#8ea2bf;line-height:1.5}
</style></head><body><div class="panel">
<h1>RED TWO</h1><div class="sub">RETROACHIEVEMENTS SIGN-IN</div>
<form id="f">
<label for="u">USER NAME</label><input id="u" name="username" autocomplete="username" required autofocus>
<label for="p">PASSWORD</label><input id="p" name="password" type="password" autocomplete="current-password" required>
<button id="b" type="submit">LOG IN</button></form>
<div id="m" class="msg"></div>
<div class="note">Red Two sends your user name and password to retroachievements.org once and keeps only the
login token it gets back. This page comes from Red Two on your own computer and closes down after you sign in.</div>
</div><script>
const base=location.pathname.replace(/\/$/,'');
const f=document.getElementById('f'),m=document.getElementById('m'),b=document.getElementById('b');
function show(t,c){m.textContent=t;m.className='msg '+(c||'');}
const gone='Red Two is not answering. Is it still running? Open this page again from its ACHIEVEMENTS tab.';
async function poll(){
 try{const r=await fetch(base+'/status',{cache:'no-store'});const s=await r.json();
  if(s.state==='busy'){b.disabled=true;show('Signing in...');setTimeout(poll,500);return;}
  if(s.state==='ok'){f.style.display='none';show(s.message,'ok');return;}
  b.disabled=false;if(s.state==='failed')show(s.message,'err');
 }catch(e){b.disabled=false;show(gone,'err');}
}
f.addEventListener('submit',async e=>{
 e.preventDefault();b.disabled=true;show('Signing in...');
 try{const r=await fetch(base+'/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:new URLSearchParams(new FormData(f)).toString()});
  const s=await r.json();if(!r.ok){b.disabled=false;show(s.message,'err');return;}poll();
 }catch(e){b.disabled=false;show(gone,'err');}
});
poll();
</script></body></html>
)html";

void sendAll(Sock c, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;  // (a tab closed mid-answer must not end the program)
#else
        const int flags = 0;
#endif
        const int n = int(send(c, data.data() + off, int(std::min<size_t>(data.size() - off, 1 << 20)), flags));
        if (n <= 0) return;
        off += size_t(n);
    }
}

void respond(Sock c, int code, const char* type, const std::string& body) {
    const char* reason = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
                         : code == 409 ? "Conflict" : code == 405 ? "Method Not Allowed" : "Not Found";
    char head[640];
    snprintf(head, sizeof head,
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
             "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
             "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
             "connect-src 'self'; form-action 'self'; frame-ancestors 'none'\r\nConnection: close\r\n\r\n",
             code, reason, type, body.size());
    sendAll(c, head + body);
}

void respondJson(Sock c, int code, const char* state, const std::string& message) {
    respond(c, code, "application/json; charset=utf-8",
            std::string("{\"state\":\"") + state + "\",\"message\":\"" + jsonEscape(message) + "\"}");
}

void handle(Sock c) {
#ifdef _WIN32
    DWORD tv = 5000;
#else
    timeval tv{5, 0};
#endif
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
    std::string req;
    char buf[4096];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos && req.size() < 16384) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) return;
        req.append(buf, size_t(n));
        headEnd = req.find("\r\n\r\n");
    }
    if (headEnd == std::string::npos) return;
    // request line and headers
    const size_t lineEnd = req.find("\r\n");
    const std::string line = req.substr(0, lineEnd);
    const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;
    const std::string method = line.substr(0, sp1), path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string host, origin;
    size_t contentLength = 0;
    for (size_t pos = lineEnd + 2; pos < headEnd;) {
        const size_t nl = req.find("\r\n", pos);
        const std::string h = req.substr(pos, nl - pos);
        pos = nl + 2;
        const size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(h.substr(0, colon));
        std::string value = h.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        if (name == "host") host = lower(value);
        else if (name == "origin") origin = lower(value);
        else if (name == "content-length") contentLength = size_t(std::max(0L, std::atol(value.c_str())));
    }
    const std::string port = std::to_string(s_port);
    if (host != "127.0.0.1:" + port && host != "localhost:" + port) {
        respond(c, 403, "text/plain; charset=utf-8", "Forbidden");
        return;
    }
    const std::string root = "/" + s_secret;
    if (path == root || path == root + "/") {
        if (method != "GET") return respond(c, 405, "text/plain; charset=utf-8", "Method Not Allowed");
        return respond(c, 200, "text/html; charset=utf-8", kPage);
    }
    if (path == root + "/status") {
        std::lock_guard lk(s_mu);
        const char* st = s_state == WebState::Busy ? "busy" : s_state == WebState::Ok ? "ok"
                         : s_state == WebState::Failed ? "failed" : "waiting";
        return respondJson(c, 200, st, s_text);
    }
    if (path == root + "/login") {
        if (method != "POST") return respond(c, 405, "text/plain; charset=utf-8", "Method Not Allowed");
        if (!origin.empty() && origin != "http://127.0.0.1:" + port && origin != "http://localhost:" + port)
            return respond(c, 403, "text/plain; charset=utf-8", "Forbidden");
        if (contentLength > 4096) return respondJson(c, 400, "failed", "That is too long for a user name and password.");
        std::string body = req.substr(headEnd + 4);
        while (body.size() < contentLength) {
            const int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            body.append(buf, size_t(n));
        }
        std::string user = formValue(body, "username"), pass = formValue(body, "password");
        wipe(body);
        wipe(req);
        if (user.empty() || pass.empty()) {
            wipe(pass);
            return respondJson(c, 400, "failed", "Type your user name and password.");
        }
        {
            std::lock_guard lk(s_mu);
            if (s_pending || s_state == WebState::Busy) {
                wipe(pass);
                return respondJson(c, 409, "busy", "Signing in already...");
            }
            s_user = std::move(user);
            s_pass = std::move(pass);
            s_pending = true;
            s_state = WebState::Busy;
            s_text = "Signing in...";
        }
        return respondJson(c, 202, "busy", "Signing in...");
    }
    respond(c, 404, "text/plain; charset=utf-8", "Not Found");
}

void serve(Sock ls) {
    const auto start = Clock::now();
    while (!s_stop.load()) {
        const auto now = Clock::now();
        if (now - start > kLifetime) break;
        {
            std::lock_guard lk(s_mu);
            if (s_state == WebState::Ok && now - s_okAt > kAfterOk) break;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        timeval tv{0, 250000};
        if (select(int(ls) + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        const Sock c = accept(ls, nullptr, nullptr);
        if (c == kNoSock) continue;
        handle(c);
        closeSock(c);
    }
    closeSock(ls);
    {
        std::lock_guard lk(s_mu);
        wipe(s_pass);
        s_pending = false;
    }
    logf("ra", "browser sign-in page closed");
    s_running = false;
}

}  // namespace

bool webLoginStart(std::string& url, std::string& error) {
    if (s_running.load()) {
        url = "http://127.0.0.1:" + std::to_string(s_port) + "/" + s_secret;
        return true;
    }
    if (s_thread.joinable()) s_thread.join();  // (the last one ended by itself)
#ifdef _WIN32
    static bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa) {
        error = "Could not open the sign-in page (no network support).";
        return false;
    }
#endif
    const Sock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == kNoSock) {
        error = "Could not open the sign-in page (no socket).";
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this computer only
    a.sin_port = 0;                               // any free port
    socklen_t len = sizeof a;
    if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(ls, 8) != 0 ||
        getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        closeSock(ls);
        error = "Could not open the sign-in page (no free port on this computer).";
        return false;
    }
    std::random_device rd;
    char secret[33];
    for (int i = 0; i < 4; i++) snprintf(secret + i * 8, 9, "%08x", unsigned(rd()));
    {
        std::lock_guard lk(s_mu);
        s_secret = secret;
        s_port = ntohs(a.sin_port);
        s_state = WebState::Waiting;
        s_text.clear();
        s_pending = false;
    }
    s_stop = false;
    s_running = true;
    s_thread = std::thread(serve, ls);
    url = "http://127.0.0.1:" + std::to_string(s_port) + "/" + s_secret;
    logf("ra", "browser sign-in page open on 127.0.0.1 port %d (for 10 minutes)", s_port);
    return true;
}

void webLoginStop() {
    s_stop = true;
    if (s_thread.joinable()) s_thread.join();
}

bool webLoginRunning() { return s_running.load(); }

// the game's thread (frame / idle): a login the page sent goes to rcheevos like the menu's own
void webLoginPoll() {
    // R2_RA_WEB_TEST=1 (automated runs): the page opens at start and the log names its address
    static bool testChecked = false;
    if (!testChecked) {
        testChecked = true;
        if (envFlag("R2_RA_WEB_TEST") && (envFlag("R2_HEADLESS") || envFlag("R2_BACKGROUND"))) {
            std::string url, err;
            if (webLoginStart(url, err)) logf("ra", "web login test: %s", url.c_str());
            else logf("ra", "web login test: %s", err.c_str());
        }
    }
    if (!s_running.load()) return;
    std::string user, pass;
    {
        std::lock_guard lk(s_mu);
        if (!s_pending) return;
        s_pending = false;
        user.swap(s_user);
        pass.swap(s_pass);
    }
    settings::Settings& s = settings::get();
    if (!s.achievements) {  // (logging in switches achievements on, as the menu's Log in row does)
        s.achievements = true;
        settings::save();
        setEnabled(true);
    }
    logf("ra", "browser sign-in: logging in");
    login(user, pass);
    wipe(pass);
}

// achievements.cpp onLogin: what the page says next
void webLoginResult(bool ok, const std::string& text) {
    std::lock_guard lk(s_mu);
    if (ok) {
        s_state = WebState::Ok;
        s_text = text;
        s_okAt = Clock::now();
    } else if (s_state == WebState::Busy) {
        s_state = WebState::Failed;
        s_text = text;
    }
}

}  // namespace rt::ra
