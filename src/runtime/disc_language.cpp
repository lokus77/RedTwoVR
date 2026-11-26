// Rogue Leader in Spanish: the texts, menus and voices of the Spanish release, with the USA program.
//
// Red Two is made from the USA disc's program (GSWE64); the Spanish disc (GSWS64, the PAL release
// for Spain) has a program of its own and cannot run here. Its data can: Rogue2/data.dat there is
// the USA one plus each language's pieces (text/Spanish beside text/English, staticGame/sp beside
// staticGame/us, frontendSP beside frontend), all else byte for byte the same, and its voices
// (audio/voiceDir, Rogue2/r2voice.vdy) are the Spanish ones. The level packs differ by a byte here
// and there (built with the PAL program) and stay the USA ones.
//
// This image is the USA disc as the drive reads it, with those pieces taken from the Spanish disc:
//   - data.dat's own directory (its last 0x7840 bytes; 64-byte rows: offset, length, a folder's
//     span, the name) and resDir (44-byte rows: unpacked size, length, a hash) point the English
//     pieces at the Spanish ones;
//   - the Spanish pieces too long for the English one's place sit past the end of the disc (a
//     folder is read whole: text is 11744 bytes and its Spanish 12263);
//   - the FST in the disc's header says data.dat reaches that far, and r2voice.vdy's length.
// The game looks its strings up by name, so a table with its own order works.
//
// The Spanish pieces come from the disc, or from a folder holding just them (about 140 MB), made by
// red_two --extract-spanish <the Spanish disc> [<folder>] (red_two_es beside that disc by default).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common/disc_image.h"
#include "runtime.h"

namespace rt {

namespace {

u32 rd32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
void wr32be(u8* p, u32 v) {
    p[0] = u8(v >> 24);
    p[1] = u8(v >> 16);
    p[2] = u8(v >> 8);
    p[3] = u8(v);
}

// A disc's file system: each file's place, and where its FST row is (for the length)
struct Fst {
    u32 off = 0, size = 0;  // of the FST on the disc
    std::vector<u8> bytes;
    struct File {
        u32 off, size, row;  // row: the byte of its FST row
    };
    std::map<std::string, File> files;
};

bool readFst(disc::Image& img, Fst& f) {
    u8 hdr[0x440];
    if (!img.read(0, hdr, sizeof hdr)) return false;
    f.off = rd32(hdr + 0x424);
    f.size = rd32(hdr + 0x428);
    if (f.size < 12 || f.size > (16u << 20)) return false;
    f.bytes.resize(f.size);
    if (!img.read(f.off, f.bytes.data(), f.size)) return false;
    const u32 n = rd32(&f.bytes[8]);
    if (u64(n) * 12 > f.size) return false;
    const char* names = reinterpret_cast<const char*>(&f.bytes[n * 12]);
    std::vector<std::pair<std::string, u32>> dirs{{"", n}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().second) dirs.pop_back();
        const u8* e = &f.bytes[i * 12];
        const u32 nameOff = rd32(e) & 0xFFFFFF;
        if (n * 12 + nameOff >= f.size) return false;
        const std::string path = dirs.back().first + (names + nameOff);
        if (e[0]) dirs.push_back({path + "/", rd32(e + 8)});
        else f.files[path] = {rd32(e + 4), rd32(e + 8), i * 12};
    }
    return true;
}

// data.dat's own directory: the rows by path ("text/English")
struct Dat {
    u32 dirOff = 0, dirSize = 0;  // in the file
    std::vector<u8> dir;
    struct Row {
        u32 at;  // byte of the row in dir
        u32 off, size;
        bool folder;
    };
    std::map<std::string, Row> rows;
    std::vector<u8> resDir;
    u32 resDirOff = 0;
};

bool readDat(disc::Image& img, u32 fileOff, u32 fileSize, Dat& d) {
    u8 h[16];
    if (!img.read(fileOff, h, sizeof h)) return false;
    d.dirOff = rd32(h + 4);
    d.dirSize = rd32(h + 12);
    if (d.dirSize % 0x40 || d.dirSize > (1u << 20) || u64(d.dirOff) + d.dirSize > fileSize) return false;
    d.dir.resize(d.dirSize);
    if (!img.read(fileOff + d.dirOff, d.dir.data(), d.dirSize)) return false;
    std::vector<std::pair<std::string, u32>> stack;  // folder, end of its rows
    for (u32 i = 0; i < d.dirSize; i += 0x40) {
        const u8* e = &d.dir[i];
        const u32 flags = rd32(e + 24), span = rd32(e + 28);
        char name[33] = {};
        memcpy(name, e + 32, 32);
        while (!stack.empty() && i >= stack.back().second) stack.pop_back();
        std::string path;
        for (const auto& s : stack) path += s.first + "/";
        path += name;
        const bool folder = (flags & 0x80000000u) != 0;
        d.rows[path] = {i, rd32(e + 4), rd32(e + 12), folder};
        if (folder) stack.push_back({name, i + span});
    }
    const auto r = d.rows.find("resDir");
    if (r == d.rows.end() || u64(r->second.off) + r->second.size > fileSize || r->second.size > (1u << 20)) return false;
    d.resDirOff = r->second.off;
    d.resDir.resize(r->second.size);
    return img.read(fileOff + d.resDirOff, d.resDir.data(), r->second.size);
}

u8* resRow(std::vector<u8>& rd, const char* name) {
    for (size_t i = 0; i + 0x2C <= rd.size(); i += 0x2C)
        if (!strncmp(reinterpret_cast<const char*>(&rd[i]), name, 32)) return &rd[i];
    return nullptr;
}

#ifdef _WIN32
int seek64(FILE* f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
int64_t tell64(FILE* f) { return _ftelli64(f); }
#else
int seek64(FILE* f, int64_t off, int whence) { return fseeko(f, off_t(off), whence); }
int64_t tell64(FILE* f) { return int64_t(ftello(f)); }
#endif

// A plain file read as an image (the pieces in the folder --extract-spanish makes)
class FileImage final : public disc::Image {
public:
    explicit FileImage(FILE* f) : m_f(f) {
        seek64(m_f, 0, SEEK_END);
        m_size = u64(tell64(m_f));
    }
    ~FileImage() override { fclose(m_f); }
    bool read(uint64_t off, void* dst, uint32_t len) override {
        if (off + len > m_size || seek64(m_f, int64_t(off), SEEK_SET)) return false;
        return fread(dst, 1, len, m_f) == len;
    }
    uint64_t size() const override { return m_size; }
    const char* kind() const override { return "file"; }

private:
    FILE* m_f;
    u64 m_size = 0;
};

// The Spanish pieces, wherever they are: the disc, or the folder made from it
struct Spanish {
    std::vector<std::unique_ptr<disc::Image>> images;  // (owned)
    struct Src {
        disc::Image* img = nullptr;
        u64 off = 0;
        u32 len = 0;
    };
    // text/Spanish, staticGame/sp, frontendSP, audio/voiceDir, r2voice.vdy
    Src text, statics, frontend, voiceDir, voices;
    u8 res[3][12] = {};  // the resDir rows (unpacked size, length, hash) of the first three
};

// The folder's files (the order of Spanish's pieces)
const char* const kPieceFiles[] = {"text_Spanish.bin", "staticGame_sp.bin", "frontendSP.bin", "voiceDir.bin", "r2voice.vdy"};
const char* const kResFile = "resDir_rows.bin";

Spanish::Src* pieceAt(Spanish& sp, int i) {
    Spanish::Src* all[] = {&sp.text, &sp.statics, &sp.frontend, &sp.voiceDir, &sp.voices};
    return all[i];
}

FILE* openUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    const std::wstring wmode(mode, mode + strlen(mode));
    return _wfopen(p.c_str(), wmode.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool fromDisc(const std::string& path, Spanish& out, std::string& why) {
    std::string err;
    auto es = disc::open(path, err);
    if (!es) return why = "cannot open it: " + err, false;
    u8 id[8];
    if (!es->read(0, id, sizeof id) || memcmp(id, "GSWS64", 6)) return why = "it is not Rogue Leader for Spain (GSWS64)", false;
    Fst fst;
    if (!readFst(*es, fst)) return why = "cannot read its file system", false;
    if (!fst.files.count("Rogue2/data.dat") || !fst.files.count("Rogue2/r2voice.vdy"))
        return why = "Rogue2/data.dat or r2voice.vdy missing", false;
    const Fst::File dat = fst.files["Rogue2/data.dat"], v = fst.files["Rogue2/r2voice.vdy"];
    Dat d;
    if (!readDat(*es, dat.off, dat.size, d)) return why = "cannot read data.dat's directory", false;
    const char* rows[] = {"text/Spanish", "staticGame/sp", "frontendSP", "audio/voiceDir"};
    for (int i = 0; i < 4; i++) {
        const auto r = d.rows.find(rows[i]);
        if (r == d.rows.end() || r->second.folder) return why = std::string("no ") + rows[i], false;
        *pieceAt(out, i) = {es.get(), u64(dat.off) + r->second.off, r->second.size};
    }
    out.voices = {es.get(), v.off, v.size};
    const char* res[] = {"txtSpanish", "staticGame_sp", "feDataSP"};
    for (int i = 0; i < 3; i++) {
        const u8* r = resRow(d.resDir, res[i]);
        if (!r) return why = std::string("no resDir row ") + res[i], false;
        memcpy(out.res[i], r + 32, 12);
    }
    out.images.push_back(std::move(es));
    return true;
}

bool fromFolder(const std::string& dir, Spanish& out, std::string& why) {
    for (int i = 0; i < 5; i++) {
        FILE* f = openUtf8(dir + "/" + kPieceFiles[i], "rb");
        if (!f) return why = std::string("no ") + kPieceFiles[i] + " in it", false;
        auto img = std::make_unique<FileImage>(f);
        if (!img->size() || img->size() > 0xFFFFFFFFu) return why = std::string(kPieceFiles[i]) + " is empty or too large", false;
        *pieceAt(out, i) = {img.get(), 0, u32(img->size())};
        out.images.push_back(std::move(img));
    }
    FILE* f = openUtf8(dir + "/" + kResFile, "rb");
    if (!f) return why = std::string("no ") + kResFile + " in it", false;
    const bool ok = fread(out.res, 1, sizeof out.res, f) == sizeof out.res;
    fclose(f);
    if (!ok) return why = std::string(kResFile) + " is too short", false;
    return true;
}

bool loadSpanish(const std::string& path, Spanish& out, std::string& why) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(std::u8string(path.begin(), path.end())), ec))
        return fromFolder(path, out, why);
    return fromDisc(path, out, why);
}

class Localized final : public disc::Image {
public:
    struct Piece {
        u64 at, len;
        disc::Image* src;     // from there (when mem is empty)
        u64 srcOff;
        std::vector<u8> mem;  // or these bytes
    };
    Localized(std::unique_ptr<disc::Image> base, Spanish sp, std::vector<Piece> pieces, u64 end)
        : m_base(std::move(base)), m_sp(std::move(sp)), m_pieces(std::move(pieces)), m_end(end) {}

    bool read(uint64_t off, void* dst, uint32_t len) override {
        u8* out = static_cast<u8*>(dst);
        const u64 bsize = m_base->size();
        if (off < bsize) {
            const u32 n = u32(std::min<u64>(len, bsize - off));
            if (!m_base->read(off, out, n)) return false;
            if (n < len) memset(out + n, 0, len - n);
        } else {
            memset(out, 0, len);
        }
        for (const Piece& p : m_pieces) {
            const u64 a = std::max<u64>(off, p.at), b = std::min<u64>(off + len, p.at + p.len);
            if (a >= b) continue;
            if (!p.mem.empty()) memcpy(out + (a - off), p.mem.data() + (a - p.at), size_t(b - a));
            else if (!p.src->read(p.srcOff + (a - p.at), out + (a - off), u32(b - a))) return false;
        }
        return true;
    }
    uint64_t size() const override { return std::max<u64>(m_base->size(), m_end); }
    const char* kind() const override { return m_base->kind(); }

private:
    std::unique_ptr<disc::Image> m_base;
    Spanish m_sp;
    std::vector<Piece> m_pieces;
    u64 m_end;
};

constexpr u64 alignUp(u64 v, u64 a) { return (v + a - 1) / a * a; }

}  // namespace

// --extract-spanish: the pieces of the Spanish disc the game in Spanish reads, as files in dir
bool extractSpanish(const std::string& discPath, const std::string& dir, std::string& why) {
    Spanish sp;
    if (!fromDisc(discPath, sp, why)) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(std::u8string(dir.begin(), dir.end())), ec);
    std::vector<u8> buf(4u << 20);
    for (int i = 0; i < 5; i++) {
        const Spanish::Src& s = *pieceAt(sp, i);
        FILE* f = openUtf8(dir + "/" + kPieceFiles[i], "wb");
        if (!f) return why = std::string("cannot write ") + kPieceFiles[i] + " in " + dir, false;
        bool ok = true;
        for (u64 done = 0; ok && done < s.len;) {
            const u32 n = u32(std::min<u64>(buf.size(), s.len - done));
            ok = s.img->read(s.off + done, buf.data(), n) && fwrite(buf.data(), 1, n, f) == n;
            done += n;
        }
        if (fclose(f) || !ok) return why = std::string("cannot copy ") + kPieceFiles[i], false;
    }
    FILE* f = openUtf8(dir + "/" + kResFile, "wb");
    if (!f) return why = std::string("cannot write ") + kResFile, false;
    const bool ok = fwrite(sp.res, 1, sizeof sp.res, f) == sizeof sp.res;
    if (fclose(f) || !ok) return why = std::string("cannot write ") + kResFile, false;
    return true;
}

// The Spanish pieces at path (the disc, or the folder made from it): "" if usable, else why not
std::string spanishProblem(const std::string& path) {
    Spanish sp;
    std::string why;
    return loadSpanish(path, sp, why) ? std::string() : why;
}

std::unique_ptr<disc::Image> localizeDisc(std::unique_ptr<disc::Image> base, const std::string& spanishPath, std::string& why) {
    Spanish sp;
    if (!loadSpanish(spanishPath, sp, why)) return base;
    Fst usFst;
    if (!readFst(*base, usFst)) return why = "cannot read the USA disc's file system", std::move(base);
    const char* kDat = "Rogue2/data.dat";
    const char* kVoice = "Rogue2/r2voice.vdy";
    if (!usFst.files.count(kDat) || !usFst.files.count(kVoice))
        return why = "the USA disc has no data.dat or r2voice.vdy", std::move(base);
    const Fst::File usDat = usFst.files[kDat];
    Dat us;
    if (!readDat(*base, usDat.off, usDat.size, us)) return why = "cannot read data.dat's directory", std::move(base);

    std::vector<Localized::Piece> pieces;
    const u64 past = alignUp(base->size(), 1u << 20);  // past the end of the disc
    u64 cursor = past;
    // English piece <- Spanish piece (its folder too, when it sits in one: a folder is read whole)
    struct Swap {
        const char *row, *folder, *res;
        const Spanish::Src* from;
        int resIndex;
    };
    const Swap swaps[] = {
        {"text/English", "text", "txtEnglish", &sp.text, 0},
        {"staticGame/us", "staticGame", "staticGame_us", &sp.statics, 1},
        {"frontend", nullptr, "feData", &sp.frontend, 2},
        {"audio/voiceDir", nullptr, nullptr, &sp.voiceDir, -1},
    };
    for (const Swap& s : swaps) {
        const auto u = us.rows.find(s.row);
        if (u == us.rows.end() || u->second.folder) return why = std::string("the USA disc has no ") + s.row, std::move(base);
        const u32 len = s.from->len;
        Dat::Row* folder = nullptr;
        if (s.folder) {
            auto f = us.rows.find(s.folder);
            if (f == us.rows.end() || !f->second.folder || f->second.off != u->second.off)
                return why = std::string("unexpected layout around ") + s.row, std::move(base);
            folder = &f->second;
        }
        const u32 room = folder ? folder->size : u->second.size;
        u64 at;
        if (len <= room) {
            at = u64(usDat.off) + u->second.off;  // in its place
        } else {
            at = cursor;
            cursor = alignUp(cursor + len, 0x8000);
        }
        const u64 rel = at - usDat.off;
        if (rel > 0xFFFFFFF0u) return why = "the disc is too large", std::move(base);
        pieces.push_back({at, len, s.from->img, s.from->off, {}});
        u8* row = &us.dir[u->second.at];
        wr32be(row + 4, u32(rel));
        wr32be(row + 12, len);
        if (folder) {
            u8* frow = &us.dir[folder->at];
            wr32be(frow + 4, u32(rel));
            wr32be(frow + 12, u32(std::max<u64>(alignUp(len, 32), folder->size)));
        }
        if (s.res) {
            u8* to = resRow(us.resDir, s.res);
            if (!to) return why = std::string("no resDir row ") + s.res, std::move(base);
            memcpy(to + 32, sp.res[s.resIndex], 12);  // unpacked size, length, hash
        }
    }
    pieces.push_back({u64(usDat.off) + us.dirOff, us.dir.size(), nullptr, 0, us.dir});
    pieces.push_back({u64(usDat.off) + us.resDirOff, us.resDir.size(), nullptr, 0, us.resDir});

    // the voices: the whole file
    const Fst::File usV = usFst.files[kVoice];
    u64 vAt = usV.off;
    if (sp.voices.len > usV.size) {
        vAt = cursor;
        cursor = alignUp(cursor + sp.voices.len, 0x8000);
    }
    if (vAt > 0xFFFFFFF0u) return why = "the disc is too large", std::move(base);
    pieces.push_back({vAt, sp.voices.len, sp.voices.img, sp.voices.off, {}});
    wr32be(&usFst.bytes[usV.row + 4], u32(vAt));
    wr32be(&usFst.bytes[usV.row + 8], sp.voices.len);
    // data.dat reaches the pieces past the end
    if (cursor > past) wr32be(&usFst.bytes[usDat.row + 8], u32(cursor - usDat.off));
    pieces.push_back({usFst.off, usFst.bytes.size(), nullptr, 0, usFst.bytes});

    logf("disc", "Spanish: texts, menus and voices from %s (%zu pieces, %llu KiB past the disc's end)", spanishPath.c_str(),
         pieces.size(), (unsigned long long)((cursor - past) >> 10));
    return std::make_unique<Localized>(std::move(base), std::move(sp), std::move(pieces), cursor);
}

}  // namespace rt
