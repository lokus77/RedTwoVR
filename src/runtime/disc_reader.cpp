// The disc image as the DVD drive sees it: reads on a thread of their own, with read-ahead.
//
// The game streams while it plays: in flight its music (Rogue2/dg000NN.vsp) takes two 18 KiB
// pieces at two places on the disc every second, voices (r2voice.vdy) a few KiB each, the menus'
// movies 18 KiB every 40 ms. The drive model used to read each piece from the image when the
// emulated transfer ended, on the game's thread: a disc image on a slow drive (a hard disk
// parking its heads, a USB or network drive) froze picture and sound for as long as Windows took
// - a third of a second, every second, in issue #25's log. Now a read starts on the reader thread
// when the game issues it and the 512 KiB after it come along (the next pieces of a stream are
// then in memory), and if the bytes are still not in when the emulated transfer ends, the drive
// finishes later, as a real drive with a slow seek would: the game keeps running meanwhile.
//
// Cache: 64 KiB blocks of the disc, the least recently used dropped past 32 MiB. Every read of
// the image (this thread's, the boot's synchronous ones) holds s_imageMu: the image keeps a file
// position and RVZ keeps decoded groups.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../common/disc_image.h"
#include "runtime.h"

namespace rt {

namespace {

constexpr u64 kBlock = 64 * 1024;
constexpr u64 kAhead = 512 * 1024;       // read past each read the game makes
constexpr size_t kMaxBlocks = 512;       // 32 MiB
constexpr u64 kMaxRun = 64;              // blocks in one read of the image (4 MiB)
constexpr size_t kMaxAheadWants = 16;    // older read-ahead requests are dropped past this

std::unique_ptr<disc::Image> s_image;
std::mutex s_imageMu;

struct Block {
    std::unique_ptr<u8[]> data;
    u32 size = 0;
    u64 used = 0;
};
struct Want {
    u64 first, last;  // block indices
    bool demand;      // the game is waiting for it (else read-ahead)
};

std::mutex s_mu;  // everything below
std::condition_variable s_cv;
std::unordered_map<u64, Block> s_cache;
std::deque<Want> s_wants;
u64 s_useClock = 0;
u64 s_readFirst = 1, s_readLast = 0;  // blocks being read now (empty when first > last)
u64 s_badFirst = 1, s_badLast = 0;    // blocks the image could not give
bool s_started = false;

u64 lastBlock() { return (s_image->size() - 1) / kBlock; }

bool cached(u64 b) { return s_cache.count(b) != 0; }

bool covered(u64 first, u64 last) {  // all cached
    for (u64 b = first; b <= last; b++)
        if (!cached(b)) return false;
    return true;
}

bool overlaps(u64 a0, u64 a1, u64 b0, u64 b1) { return a0 <= b1 && b0 <= a1; }

bool requested(u64 first, u64 last) {  // asked for or being read
    if (overlaps(first, last, s_readFirst, s_readLast)) return true;
    for (const Want& w : s_wants)
        if (overlaps(first, last, w.first, w.last)) return true;
    return false;
}

void evict() {
    while (s_cache.size() > kMaxBlocks) {
        auto oldest = s_cache.begin();
        for (auto it = s_cache.begin(); it != s_cache.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        s_cache.erase(oldest);
    }
}

// Slow reads of the image, for the log: at most 5 lines a minute, the rest counted.
struct SlowLog {
    double windowStart = -1e9;
    int lines = 0;
    u64 quiet = 0;  // not logged since the last line
    double worstQuiet = 0;
    // true: log this one (with *more = reads left out since the last line)
    bool allow(double now, double ms, u64* more, double* worst) {
        if (now - windowStart >= 60.0) {
            windowStart = now;
            lines = 0;
        }
        if (lines < 5) {
            lines++;
            *more = quiet;
            *worst = worstQuiet;
            quiet = 0;
            worstQuiet = 0;
            return true;
        }
        quiet++;
        if (ms > worstQuiet) worstQuiet = ms;
        return false;
    }
};
SlowLog s_slowRead, s_lateRead;

double secondsNow() { return double(tbNow()) / double(kTbClock); }

// Tests: R2_DISC_DELAY=<ms>[@<seconds>] makes every read the drive asks for take that much longer
// (a slow drive, issue #25: ~340 ms), from that many seconds after the start on (a mission is
// reached at the normal speed first); R2_DISC_SYNC=1 reads on the game's thread when the transfer
// ends, without the reader thread - how it was before, for comparison.
int delayMs() {
    static const double from = [] {
        const char* v = getenv("R2_DISC_DELAY");
        const char* at = v ? strchr(v, '@') : nullptr;
        return at ? atof(at + 1) : 0.0;
    }();
    static const int ms = [] {
        const char* v = getenv("R2_DISC_DELAY");
        return v ? atoi(v) : 0;
    }();
    static const u64 t0 = tbNow();
    return ms && double(tbNow() - t0) / double(kTbClock) >= from ? ms : 0;
}
bool syncMode() {
    static const bool on = envFlag("R2_DISC_SYNC");
    return on;
}

std::string moreNote(u64 more, double worst) {
    if (!more) return {};
    char b[96];
    snprintf(b, sizeof b, " (%llu more since the last such line, the slowest %.0f ms)", (unsigned long long)more, worst);
    return b;
}

void readerThread() {
    std::unique_lock lk(s_mu);
    for (;;) {
        s_cv.wait(lk, [] { return !s_wants.empty(); });
        Want& w = s_wants.front();
        u64 first = w.first;
        while (first <= w.last && cached(first)) first++;
        if (first > w.last) {
            s_wants.pop_front();
            continue;
        }
        u64 last = first;
        while (last < w.last && last - first + 1 < kMaxRun && !cached(last + 1)) last++;
        const bool demand = w.demand;
        s_readFirst = first;
        s_readLast = last;
        lk.unlock();

        const u64 off = first * kBlock;
        const u64 end = std::min((last + 1) * kBlock, s_image->size());
        auto buf = std::make_unique<u8[]>(size_t(end - off));
        const u64 t0 = tbNow();
        bool ok;
        {
            std::lock_guard ilk(s_imageMu);
            ok = s_image->read(off, buf.get(), u32(end - off));
        }
        if (delayMs()) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs()));
        const double ms = double(tbNow() - t0) * 1000.0 / double(kTbClock);

        lk.lock();
        s_readFirst = 1;
        s_readLast = 0;
        if (ok) {
            for (u64 b = first; b <= last; b++) {
                Block& blk = s_cache[b];
                blk.size = u32(std::min(kBlock, end - b * kBlock));
                blk.data = std::make_unique<u8[]>(blk.size);
                memcpy(blk.data.get(), buf.get() + (b - first) * kBlock, blk.size);
                blk.used = ++s_useClock;
            }
            evict();
        } else {
            s_badFirst = first;
            s_badLast = last;
            for (auto it = s_wants.begin(); it != s_wants.end();)  // (nothing more to try there)
                it = overlaps(it->first, it->last, first, last) ? s_wants.erase(it) : it + 1;
        }
        s_cv.notify_all();
        if (ms >= 100.0) {
            u64 more = 0;
            double worst = 0;
            if (s_slowRead.allow(secondsNow(), ms, &more, &worst)) {
                lk.unlock();
                logf("dvd", "reading the disc image took %.0f ms (%llu KiB at %s%s): the drive it is on is slow%s", ms,
                     (unsigned long long)((end - off) >> 10), discFileAt(off).c_str(), demand ? "" : ", read ahead",
                     moreNote(more, worst).c_str());
                lk.lock();
            }
        }
        if (!ok) {
            lk.unlock();
            logf("dvd", "the disc image could not be read at %llX (%llu KiB)", (unsigned long long)off,
                 (unsigned long long)((end - off) >> 10));
            lk.lock();
        }
    }
}

}  // namespace

std::unique_ptr<disc::Image> localizeDisc(std::unique_ptr<disc::Image> base, const std::string& spanishPath, std::string& why);

bool discOpen(const std::string& path, const std::string& spanish) {
    std::string err;
    auto img = disc::open(path, err);
    if (!img) {
        logf("disc", "%s", err.c_str());
        return false;
    }
    if (!spanish.empty()) {
        std::string why;
        img = localizeDisc(std::move(img), spanish, why);
        if (!why.empty()) logf("disc", "Spanish disc %s not used (the game stays in English): %s", spanish.c_str(), why.c_str());
    }
    logf("disc", "%s image %s (%llu MiB)", img->kind(), path.c_str(), (unsigned long long)(img->size() >> 20));
    const bool usable = img->size() > 0x440;
    std::lock_guard lk(s_mu);
    std::lock_guard ilk(s_imageMu);
    s_image = std::move(img);
    s_cache.clear();
    s_wants.clear();
    s_badFirst = 1;
    s_badLast = 0;
    return usable;
}

bool discRead(u64 off, void* dst, u32 len) {
    std::lock_guard ilk(s_imageMu);
    return s_image && s_image->read(off, dst, len);
}

void discPrefetch(u64 off, u32 len) {
    if (!s_image || !len || off + len > s_image->size() || syncMode()) return;  // (discTake fails it)
    std::lock_guard lk(s_mu);
    if (!s_started) {
        s_started = true;
        std::thread(readerThread).detach();
    }
    const u64 first = off / kBlock, last = (off + len - 1) / kBlock, end = lastBlock();
    if (!covered(first, last)) s_wants.push_front({first, last, true});
    const u64 aheadFirst = last + 1, aheadLast = std::min(end, (off + len + kAhead - 1) / kBlock);
    if (aheadFirst <= aheadLast && !covered(aheadFirst, aheadLast)) {
        size_t ahead = 0;
        for (const Want& w : s_wants) ahead += !w.demand;
        if (ahead >= kMaxAheadWants)  // a long run of scattered reads: the oldest guesses go
            for (auto it = s_wants.begin(); it != s_wants.end(); ++it)
                if (!it->demand) {
                    s_wants.erase(it);
                    break;
                }
        s_wants.push_back({aheadFirst, aheadLast, false});
    }
    s_cv.notify_all();
}

DiscData discTake(u64 off, void* dst, u32 len) {
    if (!s_image || off + len > s_image->size()) return DiscData::Failed;
    if (!len) return DiscData::Ready;
    if (syncMode()) {
        if (delayMs()) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs()));
        return discRead(off, dst, len) ? DiscData::Ready : DiscData::Failed;
    }
    const u64 first = off / kBlock, last = (off + len - 1) / kBlock;
    std::unique_lock lk(s_mu);
    if (!covered(first, last)) {
        if (overlaps(first, last, s_badFirst, s_badLast)) {  // the reader could not: try once here
            lk.unlock();
            return discRead(off, dst, len) ? DiscData::Ready : DiscData::Failed;
        }
        if (!requested(first, last)) {  // (a save state loaded with a read under way)
            s_wants.push_front({first, last, true});
            if (!s_started) {
                s_started = true;
                std::thread(readerThread).detach();
            }
            s_cv.notify_all();
        }
        return DiscData::Pending;
    }
    auto* out = static_cast<u8*>(dst);
    u64 pos = off;
    while (pos < off + len) {
        Block& blk = s_cache[pos / kBlock];
        blk.used = ++s_useClock;
        const u64 within = pos % kBlock;
        const u64 take = std::min<u64>(blk.size - within, off + len - pos);
        memcpy(out, blk.data.get() + within, size_t(take));
        out += take;
        pos += take;
    }
    return DiscData::Ready;
}

void discLate(u64 off, u32 len, double ms) {
    u64 more = 0;
    double worst = 0;
    {
        std::lock_guard lk(s_mu);
        if (!s_lateRead.allow(secondsNow(), ms, &more, &worst)) return;
    }
    logf("dvd", "a read of %s (%u KiB) came %.0f ms late: waiting for the disc image (the game kept running)%s",
         discFileAt(off).c_str(), len >> 10, ms, moreNote(more, worst).c_str());
}

}  // namespace rt
