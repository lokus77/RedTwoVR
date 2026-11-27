// In-app updates: see update.h.
//
// The check: GitHub's API for the repository's latest release (drafts and pre-releases are never
// "latest"), its tag (v1.3) against this build's version, and its assets: the Windows zip
// (*-windows-x64.zip) or the AppImage of this build's architecture (*-x86_64.AppImage,
// *-aarch64.AppImage), and SHA256SUMS.txt. macOS and Android only open the release page.
//
// The install, on a worker thread:
// - Windows: the zip goes to <program folder>\update\, is checked, and every file in it except
//   the user folder is unpacked to update\new\ (zlib, CRC-checked). Only then are they moved in:
//   each file it replaces is renamed to <name>.old first - Windows lets a running program and
//   its loaded DLLs be renamed, not overwritten - and the new one moved to its place; any failure
//   puts everything back. The next start (the new version) deletes the .old files and update\.
// - Linux: the AppImage downloads next to itself (<name>.download), is checked, made executable
//   and renamed over the running one: the running program keeps its mounted image, and a
//   restart starts the new file under the same name.
// The version replaced is written to the user folder (updated-from.txt) so the new version can
// say what happened.
//
// Test switches: R2_UPDATE_VERSION=<x.y> pretends this build is that version; R2_UPDATE_CHECK=1
// checks in automated runs too (they never do otherwise); R2_UPDATE_TEST=install installs what it
// finds at once and restarts into it (the menu's frame does the restart).
#include "update.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include "http.h"
#include "mods.h"
#include "settings.h"

#ifdef _WIN32
#include <windows.h>
#include <zlib.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef R2_VERSION
#define R2_VERSION "0.0"
#endif

namespace fs = std::filesystem;

namespace rt::update {
namespace {

constexpr const char* kApi = "https://api.github.com/repos/lokus77/RedTwoVR/releases/latest";

std::mutex s_mutex;
Status s_status;
std::string s_assetUrl, s_sumsUrl;
u64 s_assetSize = 0;
std::atomic<bool> s_busy{false};

void set(const std::function<void(Status&)>& f) {
    std::lock_guard lk(s_mutex);
    f(s_status);
}

// a path as UTF-8 text
fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string text(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string userAgent() { return std::string("RedTwo/") + version(); }

// "v1.2.3-rc" -> {1, 2, 3}
std::vector<int> parseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && isdigit(u8(v[i]))) {
        int n = 0;
        while (i < v.size() && isdigit(u8(v[i]))) n = n * 10 + (v[i++] - '0');
        out.push_back(n);
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
    return out;
}
bool newer(const std::string& a, const std::string& b) {  // a > b
    std::vector<int> x = parseVersion(a), y = parseVersion(b);
    const size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x > y;
}

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// --- SHA-256 (FIPS 180-4) ---------------------------------------------------------------------
struct Sha256 {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 buf[64] = {};
    size_t n = 0;
    u64 len = 0;
    static u32 rotr(u32 x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const u8* p) {
        static constexpr u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; i++) w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const u8* p, size_t size) {
        len += size;
        while (size) {
            const size_t take = std::min(size, size_t(64) - n);
            memcpy(buf + n, p, take);
            n += take;
            p += take;
            size -= take;
            if (n == 64) {
                block(buf);
                n = 0;
            }
        }
    }
    std::string hex() {
        const u64 bits = len * 8;
        const u8 one = 0x80, zero = 0;
        update(&one, 1);
        while (n != 56) update(&zero, 1);
        u8 l[8];
        for (int i = 0; i < 8; i++) l[i] = u8(bits >> (56 - 8 * i));
        update(l, 8);
        char out[65];
        for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
        return out;
    }
};

std::string sha256File(const fs::path& p) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(p.c_str(), L"rb");
#else
    f = fopen(p.c_str(), "rb");
#endif
    if (!f) return {};
    Sha256 h;
    std::vector<u8> buf(1 << 20);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) h.update(buf.data(), n);
    fclose(f);
    return h.hex();
}

FILE* openWrite(const fs::path& p) {
#ifdef _WIN32
    return _wfopen(p.c_str(), L"wb");
#else
    return fopen(p.c_str(), "wb");
#endif
}

void fail(const std::string& why) {
    logf("update", "failed: %s", why.c_str());
    set([&](Status& s) {
        s.phase = Phase::Failed;
        s.error = why;
    });
}

#ifdef _WIN32
fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return n ? fs::path(std::wstring(buf, n)).parent_path() : fs::path();
}

// --- the release zip -----------------------------------------------------------------------
u16 le16(const u8* p) { return u16(p[0] | p[1] << 8); }
u32 le32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

struct Entry {
    std::string name;  // inside the program folder ("red_two.exe", "sub/dir/file")
    u16 method;
    u32 crc;
    u64 compSize, size, localOffset;
};

// the zip's files, relative to its top folder ("Red Two/"), without the user folder
bool listZip(FILE* f, std::vector<Entry>& out, std::string& err) {
    if (_fseeki64(f, 0, SEEK_END) != 0) return false;
    const s64 fileSize = _ftelli64(f);
    const s64 tail = std::min<s64>(fileSize, 65536 + 22);
    std::vector<u8> t(static_cast<size_t>(tail));
    _fseeki64(f, fileSize - tail, SEEK_SET);
    if (fread(t.data(), 1, t.size(), f) != t.size()) return false;
    s64 eocd = -1;
    for (s64 i = tail - 22; i >= 0; i--)
        if (le32(&t[size_t(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        err = "the download is not a zip file";
        return false;
    }
    const u8* e = &t[size_t(eocd)];
    const u32 count = le16(e + 10), cdSize = le32(e + 12), cdOffset = le32(e + 16);
    std::vector<u8> cd(cdSize);
    _fseeki64(f, cdOffset, SEEK_SET);
    if (fread(cd.data(), 1, cd.size(), f) != cd.size()) return false;
    std::vector<Entry> all;
    for (size_t p = 0, i = 0; i < count; i++) {
        if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) {
            err = "the zip's directory is damaged";
            return false;
        }
        Entry en;
        en.method = le16(&cd[p + 10]);
        en.crc = le32(&cd[p + 16]);
        en.compSize = le32(&cd[p + 20]);
        en.size = le32(&cd[p + 24]);
        const u16 nameLen = le16(&cd[p + 28]), extraLen = le16(&cd[p + 30]), commentLen = le16(&cd[p + 32]);
        en.localOffset = le32(&cd[p + 42]);
        if (p + 46 + nameLen > cd.size()) return false;
        en.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        for (char& c : en.name)
            if (c == '\\') c = '/';
        p += 46 + nameLen + extraLen + commentLen;
        all.push_back(std::move(en));
    }
    // the top folder ("Red Two/") they all share
    std::string top;
    if (!all.empty()) {
        const size_t slash = all[0].name.find('/');
        if (slash != std::string::npos) top = all[0].name.substr(0, slash + 1);
        for (const Entry& en : all)
            if (en.name.compare(0, top.size(), top) != 0) top.clear();
    }
    for (Entry& en : all) {
        en.name.erase(0, top.size());
        if (en.name.empty() || en.name.back() == '/') continue;  // folders
        if (en.name.compare(0, 5, "user/") == 0) continue;     // the player's own folder
        if (en.name.find("..") != std::string::npos || en.name[0] == '/' || en.name.find(':') != std::string::npos) {
            err = "the zip holds a path outside the program's folder";
            return false;
        }
        if (en.method != 0 && en.method != 8) {
            err = "the zip uses a compression Red Two cannot unpack";
            return false;
        }
        out.push_back(std::move(en));
    }
    if (out.empty()) err = "the zip is empty";
    return !out.empty();
}

bool unpack(FILE* f, const Entry& en, const fs::path& dest, std::string& err) {
    u8 lh[30];
    _fseeki64(f, s64(en.localOffset), SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || le32(lh) != 0x04034b50) {
        err = "the zip is damaged";
        return false;
    }
    _fseeki64(f, s64(en.localOffset + 30 + le16(lh + 26) + le16(lh + 28)), SEEK_SET);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    FILE* out = openWrite(dest);
    if (!out) {
        err = "cannot write " + text(dest);
        return false;
    }
    std::vector<u8> in(1 << 20), buf(1 << 20);
    u64 left = en.compSize;
    uLong crc = crc32(0, nullptr, 0);
    bool ok = true;
    if (en.method == 0) {
        while (left && ok) {
            const size_t n = fread(in.data(), 1, size_t(std::min<u64>(left, in.size())), f);
            if (!n) ok = false;
            left -= n;
            crc = crc32(crc, in.data(), uInt(n));
            ok = ok && fwrite(in.data(), 1, n, out) == n;
        }
    } else {
        z_stream z{};
        ok = inflateInit2(&z, -15) == Z_OK;
        int rc = Z_OK;
        while (ok && rc != Z_STREAM_END) {
            if (!z.avail_in) {
                const size_t n = fread(in.data(), 1, size_t(std::min<u64>(left, in.size())), f);
                if (!n) {
                    ok = false;
                    break;
                }
                left -= n;
                z.next_in = in.data();
                z.avail_in = uInt(n);
            }
            z.next_out = buf.data();
            z.avail_out = uInt(buf.size());
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                ok = false;
                break;
            }
            const size_t made = buf.size() - z.avail_out;
            crc = crc32(crc, buf.data(), uInt(made));
            ok = fwrite(buf.data(), 1, made, out) == made;
        }
        inflateEnd(&z);
    }
    if (fclose(out) != 0) ok = false;
    if (ok && crc != en.crc) ok = false;
    if (!ok && err.empty()) err = "unpacking " + en.name + " failed";
    return ok;
}

// every file of the zip into the program's folder (see the top)
bool installZip(const fs::path& zip, const fs::path& dir, std::string& err) {
    FILE* f = _wfopen(zip.c_str(), L"rb");
    if (!f) {
        err = "cannot read the download";
        return false;
    }
    std::vector<Entry> entries;
    bool ok = listZip(f, entries, err);
    const fs::path stage = dir / "update" / "new";
    std::error_code ec;
    fs::remove_all(stage, ec);
    for (size_t i = 0; ok && i < entries.size(); i++) ok = unpack(f, entries[i], stage / pathOf(entries[i].name), err);
    fclose(f);
    if (!ok) return false;
    struct Done {
        fs::path dest, old, staged;
    };
    std::vector<Done> done;
    for (const Entry& en : entries) {
        Done d{dir / pathOf(en.name), {}, stage / pathOf(en.name)};
        fs::create_directories(d.dest.parent_path(), ec);
        if (fs::exists(d.dest, ec)) {
            // a .old left by an update whose cleanup failed (still in use): the next free name
            for (int k = 0; k < 10; k++) {
                d.old = d.dest;
                d.old += k ? L".old" + std::to_wstring(k) : std::wstring(L".old");
                if (!fs::exists(d.old, ec) || DeleteFileW(d.old.c_str())) break;
            }
            if (!MoveFileExW(d.dest.c_str(), d.old.c_str(), 0)) {
                err = "cannot replace " + en.name + " (Windows error " + std::to_string(GetLastError()) + ")";
                ok = false;
            }
        }
        if (ok && !MoveFileExW(d.staged.c_str(), d.dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = "cannot put " + en.name + " in place (Windows error " + std::to_string(GetLastError()) + ")";
            if (!d.old.empty()) MoveFileExW(d.old.c_str(), d.dest.c_str(), 0);
            ok = false;
        }
        if (!ok) break;
        done.push_back(d);
    }
    if (!ok) {  // everything back as it was
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            MoveFileExW(it->dest.c_str(), it->staged.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!it->old.empty()) MoveFileExW(it->old.c_str(), it->dest.c_str(), 0);
        }
        return false;
    }
    logf("update", "%zu files in place in %s", done.size(), text(dir).c_str());
    return true;
}
#endif

// --- the check and the install ------------------------------------------------------------------
void checkWorker() {
    http::Request req;
    req.url = kApi;
    req.userAgent = userAgent();
    req.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
    req.timeoutSeconds = 20;
    const http::Response res = http::fetch(req);
    s_busy = false;
    if (!res.ok || res.status != 200) {
        logf("update", "check failed: %s", res.ok ? ("GitHub answered " + std::to_string(res.status)).c_str() : "no connection");
        set([](Status& s) {
            if (s.phase == Phase::Checking) s.phase = Phase::Idle;
        });
        return;
    }
    nlohmann::json j = nlohmann::json::parse(res.body, nullptr, false);
    if (j.is_discarded() || !j.contains("tag_name")) {
        logf("update", "check failed: GitHub's answer is not a release");
        set([](Status& s) { s.phase = Phase::Idle; });
        return;
    }
    const std::string tag = j.value("tag_name", "");
    const std::string latest = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    std::string asset, assetUrl, sumsUrl;
    u64 assetSize = 0;
#ifdef _WIN32
    const char* suffix = "-windows-x64.zip";
#elif defined(__aarch64__)
    const char* suffix = "-aarch64.AppImage";  // (an ARM AppImage must never take the x86_64 one)
#else
    const char* suffix = "-x86_64.AppImage";
#endif
    if (j.contains("assets") && j["assets"].is_array())
        for (const auto& a : j["assets"]) {
            const std::string name = a.value("name", ""), url = a.value("browser_download_url", "");
            if (endsWith(name, suffix)) {
                asset = name;
                assetUrl = url;
                assetSize = a.value("size", u64(0));
            } else if (name == "SHA256SUMS.txt") {
                sumsUrl = url;
            }
        }
    const bool isNewer = newer(latest, version());
#ifdef _WIN32
#ifdef R2_RELEASE_BUILD
    const bool installable = true;
#else
    const bool installable = false;  // built from source: updated with git
#endif
#elif defined(__ANDROID__)
    const bool installable = false;
#else
    const bool installable = getenv("APPIMAGE") != nullptr;
#endif
    const bool can = installable && !assetUrl.empty() && !sumsUrl.empty();
    logf("update", "latest release %s, this is %s: %s%s", latest.c_str(), version(),
         isNewer ? "an update" : "up to date",
         isNewer ? (can ? (" (" + asset + ", " + std::to_string(assetSize >> 20) + " MiB)").c_str()
                        : " (this copy does not update itself: the release page)")
                 : "");
    {
        std::lock_guard lk(s_mutex);
        s_assetUrl = assetUrl;
        s_sumsUrl = sumsUrl;
        s_assetSize = assetSize;
        s_status.latest = latest;
        s_status.pageUrl = j.value("html_url", std::string("https://github.com/lokus77/RedTwoVR/releases/latest"));
        s_status.asset = asset;
        s_status.canInstall = can;
        if (s_status.phase == Phase::Checking) s_status.phase = isNewer ? Phase::Available : Phase::Current;
    }
    static const bool autoInstall = getenv("R2_UPDATE_TEST") && !strcmp(getenv("R2_UPDATE_TEST"), "install");
    if (isNewer && can && autoInstall) install();
}

void installWorker() {
    std::string assetUrl, sumsUrl, asset;
    u64 assetSize = 0;
    {
        std::lock_guard lk(s_mutex);
        assetUrl = s_assetUrl;
        sumsUrl = s_sumsUrl;
        asset = s_status.asset;
        assetSize = s_assetSize;
    }
    // the checksum first (small)
    http::Request sreq;
    sreq.url = sumsUrl;
    sreq.userAgent = userAgent();
    const http::Response sums = http::fetch(sreq);
    std::string expected;
    if (sums.ok && sums.status == 200) {
        size_t pos = 0;
        while (pos < sums.body.size()) {
            size_t end = sums.body.find('\n', pos);
            if (end == std::string::npos) end = sums.body.size();
            std::string line = sums.body.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t sp = line.find(' ');
            if (sp == 64 && endsWith(line, asset.c_str()) && (line.size() == 64 + 2 + asset.size() || line.size() == 64 + 1 + asset.size()))
                expected = line.substr(0, 64);
        }
    }
    if (expected.empty()) {
        fail("The release's checksum list could not be read. Try again later, or download it from the release page.");
        return;
    }
    // where it goes
    fs::path part, dest;
#ifdef _WIN32
    const fs::path dir = exeDir();
    std::error_code ec;
    fs::create_directories(dir / "update", ec);
    part = dir / "update" / pathOf(asset);
#elif !defined(__ANDROID__)
    dest = fs::path(getenv("APPIMAGE"));
    part = dest;
    part += ".download";
#endif
    FILE* f = openWrite(part);
    if (!f) {
#ifdef _WIN32
        fail("Red Two cannot write to its folder (" + text(dir) +
             "). Download the update from the release page, or move Red Two to a folder you can write to.");
#else
        fail("Red Two cannot write next to its AppImage (" + text(dest.parent_path()) +
             "). Download the update from the release page.");
#endif
        return;
    }
    logf("update", "downloading %s", asset.c_str());
    http::Request req;
    req.url = assetUrl;
    req.userAgent = userAgent();
    req.timeoutSeconds = 0;
    req.file = f;
    req.progress = [&](u64 got, u64 total) {
        set([&](Status& s) {
            s.got = got;
            s.total = total ? total : assetSize;
        });
        return true;
    };
    const http::Response res = http::fetch(req);
    fclose(f);
    std::error_code ec2;
    const u64 size = fs::file_size(part, ec2);
    if (!res.ok || res.status != 200 || (assetSize && size != assetSize)) {
        fs::remove(part, ec2);
        fail(!res.ok ? "The download stopped (no connection?). Try again." : "The download was incomplete. Try again.");
        return;
    }
    set([](Status& s) { s.phase = Phase::Installing; });
    const std::string got = sha256File(part);
    if (got != expected) {
        logf("update", "checksum %s, expected %s", got.c_str(), expected.c_str());
        fs::remove(part, ec2);
        fail("The download does not match the release's checksum. Try again.");
        return;
    }
    logf("update", "%s: %llu bytes, checksum matches", asset.c_str(), (unsigned long long)size);
#ifdef _WIN32
    std::string err;
    if (!installZip(part, dir, err)) {
        fail("Could not install the update: " + err + ". Nothing was changed; download it from the release page.");
        return;
    }
#elif !defined(__ANDROID__)
    chmod(part.c_str(), 0755);
    if (rename(part.c_str(), dest.c_str()) != 0) {
        fs::remove(part, ec2);
        fail("Could not replace the AppImage (" + text(dest) + "). Download it from the release page.");
        return;
    }
    logf("update", "%s replaced", dest.c_str());
#endif
    // the next start says what happened
    if (FILE* m = fopen((mods::userDir() + "updated-from.txt").c_str(), "w")) {
        fputs(version(), m);
        fclose(m);
    }
    set([](Status& s) { s.phase = Phase::Ready; });
}

}  // namespace

const char* version() {
    static const std::string v = getenv("R2_UPDATE_VERSION") ? getenv("R2_UPDATE_VERSION") : R2_VERSION;
    return v.c_str();
}

void init() {
    // what the update that started this version left behind
#ifdef _WIN32
    const fs::path dir = exeDir();
    std::error_code ec;
    int removed = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::wstring ext = e.path().extension().wstring();
        if (ext.rfind(L".old", 0) == 0 && fs::remove(e.path(), ec)) removed++;
    }
    if (fs::exists(dir / "update", ec)) fs::remove_all(dir / "update", ec);
    if (removed) logf("update", "the previous version's %d files removed", removed);
#elif !defined(__ANDROID__)
    if (const char* ai = getenv("APPIMAGE")) {
        std::error_code ec;
        fs::remove(std::string(ai) + ".download", ec);
    }
#endif
    const std::string marker = mods::userDir() + "updated-from.txt";
    if (FILE* m = fopen(marker.c_str(), "r")) {
        char from[32] = {};
        const size_t n = fread(from, 1, sizeof from - 1, m);
        fclose(m);
        remove(marker.c_str());
        from[n] = 0;
        logf("update", "updated from %s to %s", from, version());
        set([&](Status& s) {
            s.phase = Phase::Updated;
            s.from = from;
            s.latest = version();
            s.pageUrl = std::string("https://github.com/lokus77/RedTwoVR/releases/tag/v") + version();
        });
    }
    const bool automated = envFlag("R2_BACKGROUND") || envFlag("R2_HEADLESS") || getenv("R2_INPUT");
    if ((automated && !envFlag("R2_UPDATE_CHECK")) || !settings::get().checkUpdates) return;
    check();
}

void check() {
    if (s_busy.exchange(true)) return;
    set([](Status& s) {
        if (s.phase != Phase::Updated) s.phase = Phase::Checking;
    });
    std::thread(checkWorker).detach();
}

Status status() {
    std::lock_guard lk(s_mutex);
    return s_status;
}

void install() {
    {
        std::lock_guard lk(s_mutex);
        if (!s_status.canInstall || (s_status.phase != Phase::Available && s_status.phase != Phase::Failed)) return;
        s_status.phase = Phase::Downloading;
        s_status.got = s_status.total = 0;
        s_status.error.clear();
    }
    std::thread(installWorker).detach();
}

std::string restartProgram() {
#ifdef _WIN32
    return text(exeDir() / "red_two.exe");
#else
    const char* ai = getenv("APPIMAGE");
    return ai ? ai : "";
#endif
}

}  // namespace rt::update
