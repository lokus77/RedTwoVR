#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ppc.h"

using namespace ppc;

// ---------------------------------------------------------------------------------------
// Config: config/<GAMEID>/symbols.txt ("<addr> <name>") and hooks.txt ("<kind> <name|addr>")
// ---------------------------------------------------------------------------------------
static bool readLines(const std::string& path, std::vector<std::vector<std::string>>& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::vector<std::string> toks;
        std::string t;
        while (ss >> t) toks.push_back(t);
        if (!toks.empty()) out.push_back(toks);
    }
    return true;
}

bool Config::load(const std::string& dir) {
    std::vector<std::vector<std::string>> lines;
    if (readLines(dir + "/symbols.txt", lines)) {
        for (auto& l : lines)
            if (l.size() >= 2) names[uint32_t(std::stoul(l[0], nullptr, 16))] = l[1];
    }
    lines.clear();
    if (readLines(dir + "/layout.txt", lines)) {
        for (auto& l : lines)
            if (l.size() >= 2 && l[0] == "embedded") embedded.push_back(uint32_t(std::stoul(l[1], nullptr, 16)));
    }
    lines.clear();
    std::map<std::string, uint32_t> byName;
    for (auto& [a, n] : names) byName[n] = a;
    auto resolve = [&](const std::string& s, uint32_t& a) {
        if (auto it = byName.find(s); it != byName.end()) {
            a = it->second;
            return true;
        }
        try {
            size_t pos = 0;
            a = uint32_t(std::stoul(s, &pos, 16));
            return pos == s.size();
        } catch (...) {
            return false;
        }
    };
    lines.clear();
    if (readLines(dir + "/hooks.txt", lines)) {
        for (auto& l : lines) {
            if (l.size() < 2) continue;
            uint32_t a;
            if (!resolve(l[1], a)) {
                fprintf(stderr, "hooks.txt: unknown symbol %s\n", l[1].c_str());
                return false;
            }
            if (l[0] == "hle") hooks[a] = names.count(a) ? names[a] : l[1];
            else if (l[0] == "savectx") saveCtx.insert(a);
            else if (l[0] == "entry") extraEntries.insert(a);
            else if (l[0] == "noentry") notEntries.insert(a);
            else if (l[0] == "idle") idleLoops.insert(a);
            else if (l[0] == "nopoll") noPollLoops.insert(a);
            else {
                fprintf(stderr, "hooks.txt: unknown kind %s\n", l[0].c_str());
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------

bool Analysis::inText(uint32_t a) const { return img_.isText(a); }

bool Analysis::inCode(uint32_t a) const {
    const Section* s = img_.find(a);
    if (!s || !s->text) return false;
    auto it = codeLimit.find(s->addr);
    return it == codeLimit.end() || a < it->second;
}

static bool isPadding(uint32_t w) { return w == 0 || w == 0x60000000u; }  // zero or nop

static bool isTerminator(uint32_t w) {
    switch (flowOf(w)) {
    case Flow::Branch:
    case Flow::Return:
    case Flow::IndirectJump:
    case Flow::Rfi:
        return true;
    default:
        return false;
    }
}

bool Analysis::findJumpTable(uint32_t bctr, JumpTable& jt) const {
    // mtctr rX shortly before the bctr
    int ctrReg = -1;
    uint32_t pc = bctr;
    for (int i = 0; i < 4 && ctrReg < 0; i++) {
        pc -= 4;
        uint32_t w = img_.word(pc);
        if (op(w) == 31 && xo10(w) == 467 && spr(w) == 9) ctrReg = int(rS(w));
    }
    if (ctrReg < 0) return false;
    // lwzx rX, rA, rB
    int ra = -1, rb = -1;
    uint32_t lwzxPc = 0;
    for (int i = 0; i < 8; i++) {
        pc -= 4;
        uint32_t w = img_.word(pc);
        if (op(w) == 31 && xo10(w) == 23 && int(rD(w)) == ctrReg) {
            ra = int(rA(w));
            rb = int(rB(w));
            lwzxPc = pc;
            break;
        }
    }
    if (!lwzxPc) return false;
    // Table base: addi base, rT, lo  <- lis rT, hi
    auto findBase = [&](int base, uint32_t& table) {
        uint32_t p = lwzxPc;
        for (int i = 0; i < 16; i++) {
            p -= 4;
            uint32_t w = img_.word(p);
            if (op(w) == 14 && int(rD(w)) == base && rA(w) != 0) {
                const uint32_t t = rA(w);
                const int32_t lo = simm(w);
                uint32_t q = p;
                for (int k = 0; k < 16; k++) {
                    q -= 4;
                    uint32_t v = img_.word(q);
                    if (op(v) == 15 && rA(v) == 0 && rD(v) == t) {
                        table = (uimm(v) << 16) + uint32_t(lo);
                        return true;
                    }
                }
                return false;
            }
            if (op(w) == 15 && rA(w) == 0 && int(rD(w)) == base) {  // lis only (table at 64K boundary)
                table = uimm(w) << 16;
                return true;
            }
        }
        return false;
    };
    uint32_t table = 0;
    int idxReg = -1;
    if (findBase(ra, table)) idxReg = rb;
    else if (findBase(rb, table)) idxReg = ra;
    else return false;
    // The scaled index usually comes from slwi idx, rI, 2 (rlwinm idx, rI, 2, 0, 29).
    int rawIdx = idxReg;
    {
        uint32_t p = lwzxPc;
        for (int i = 0; i < 12; i++) {
            p -= 4;
            uint32_t w = img_.word(p);
            if (op(w) == 21 && int(rA(w)) == idxReg && rB(w) == 2) {
                rawIdx = int(rS(w));
                break;
            }
        }
    }
    // Bounds check: cmplwi crN, rawIdx, N
    int count = -1;
    {
        uint32_t p = lwzxPc;
        for (int i = 0; i < 32 && count < 0; i++) {
            p -= 4;
            uint32_t w = img_.word(p);
            if (op(w) == 10 && int(rA(w)) == rawIdx) count = int(uimm(w)) + 1;
        }
    }
    // Otherwise the nearest "cmplwi rX, N; bgt" before the load (index scaled elsewhere,
    // e.g. a loop counter and its *4 twin).
    if (count < 0) {
        uint32_t p = lwzxPc;
        for (int i = 0; i < 8 && count < 0; i++) {
            p -= 4;
            const uint32_t w = img_.word(p), n = img_.word(p + 4);
            if (op(w) == 10 && op(n) == 16 && BO(n) == 12 && BI(n) == crfD(w) * 4 + 1) count = int(uimm(w)) + 1;
        }
    }
    jt.table = table;
    const Section* bs = img_.find(bctr);
    if (count > 0 && count < 4096) {
        for (int i = 0; i < count; i++) {
            uint32_t t = img_.word(table + 4 * i);
            if (!inText(t) || (t & 3)) return false;
            jt.targets.push_back(t);
        }
        return true;
    }
    // No bounds check found: take entries while they look like nearby code.
    jt.guessed = true;
    for (int i = 0; i < 1024; i++) {
        uint32_t t = img_.word(table + 4 * i);
        const Section* ts = img_.find(t);
        if (!ts || ts != bs || (t & 3) || t + 0x10000 < bctr || t > bctr + 0x10000) break;
        if (flowOf(img_.word(t)) == Flow::Illegal) break;
        jt.targets.push_back(t);
    }
    return !jt.targets.empty();
}

bool Analysis::walk(uint32_t entry, Function* out, std::vector<uint32_t>* calls, bool limit) {
    Function f;
    f.entry = entry;
    std::vector<uint32_t> stack{entry};
    std::set<uint32_t> seen;
    auto otherEntry = [&](uint32_t a) { return a != entry && entries.count(a) != 0; };
    while (!stack.empty()) {
        const uint32_t pc = stack.back();
        stack.pop_back();
        if (seen.count(pc)) continue;
        if (pc & 3) return false;
        if (!(limit ? inCode(pc) : inText(pc))) return false;
        if (seen.size() > 400000) return false;
        const uint32_t w = img_.word(pc);
        const Flow fl = flowOf(w);
        if (fl == Flow::Illegal) return false;
        seen.insert(pc);
        auto fall = [&]() {
            if (otherEntry(pc + 4)) f.tailEntries.insert(pc);
            else stack.push_back(pc + 4);
        };
        auto jump = [&](uint32_t t) {
            if (otherEntry(t)) return;  // tail call
            f.labels.insert(t);
            stack.push_back(t);
        };
        switch (fl) {
        case Flow::Normal:
            fall();
            break;
        case Flow::Branch:
            jump(op(w) == 18 ? bTarget(w, pc) : bcTarget(w, pc));
            break;
        case Flow::CondBranch:
            jump(bcTarget(w, pc));
            fall();
            break;
        case Flow::Call: {
            const uint32_t t = op(w) == 18 ? bTarget(w, pc) : bcTarget(w, pc);
            if (!(op(w) == 16 && t == pc + 4) && calls) calls->push_back(t);
            fall();
            break;
        }
        case Flow::Return:
        case Flow::Rfi:
            break;
        case Flow::CondReturn:
        case Flow::CondIndirect:
        case Flow::IndirectCall:
            fall();
            break;
        case Flow::IndirectJump: {
            JumpTable jt;
            if (findJumpTable(pc, jt)) {
                for (uint32_t t : jt.targets) {
                    f.labels.insert(t);
                    stack.push_back(t);
                }
                f.tables[pc] = std::move(jt);
            }
            break;
        }
        case Flow::Illegal:
            return false;
        }
    }
    f.insns.assign(seen.begin(), seen.end());
    if (out) *out = std::move(f);
    return true;
}

void Analysis::scanConstants(std::set<uint32_t>& cands) const {
    for (auto& s : img_.sections) {
        if (!s.text) continue;
        uint32_t hiVal[32] = {};
        uint32_t hiAt[32] = {};
        for (uint32_t a = s.addr; a < s.end(); a += 4) {
            if (!inCode(a)) break;
            const uint32_t w = img_.word(a);
            if (op(w) == 15 && rA(w) == 0) {
                hiVal[rD(w)] = uimm(w) << 16;
                hiAt[rD(w)] = a;
            } else if ((op(w) == 14 || op(w) == 24) && rA(w) != 0) {
                const uint32_t r = rA(w);
                if (hiAt[r] && a - hiAt[r] < 0x80) {
                    const uint32_t v = op(w) == 14 ? hiVal[r] + uint32_t(simm(w)) : hiVal[r] | uimm(w);
                    if (!(v & 3) && inCode(v)) cands.insert(v);
                }
            }
        }
    }
}

void Analysis::scanData(std::set<uint32_t>& cands, const std::set<uint32_t>& tableWords) const {
    for (auto& s : img_.sections) {
        for (uint32_t a = s.addr; a + 4 <= s.end(); a += 4) {
            if (s.text && inCode(a)) continue;
            if (tableWords.count(a)) continue;
            const uint32_t v = img_.word(a);
            if (!(v & 3) && inCode(v)) cands.insert(v);
        }
    }
}

void Analysis::scanGaps(std::set<uint32_t>& cands) const {
    std::set<uint32_t> covered;
    for (auto& [e, f] : funcs) covered.insert(f.insns.begin(), f.insns.end());
    for (auto& s : img_.sections) {
        if (!s.text) continue;
        bool inGap = false;
        for (uint32_t a = s.addr; a < s.end() && inCode(a); a += 4) {
            if (covered.count(a)) {
                inGap = false;
                continue;
            }
            const uint32_t w = img_.word(a);
            if (isPadding(w)) continue;
            if (!inGap) {
                inGap = true;
                cands.insert(a);
            }
        }
    }
}

void Analysis::run() {
    std::set<uint32_t> bad;
    std::vector<uint32_t> work;
    auto add = [&](uint32_t a) {
        if (cfg_.notEntries.count(a) || entries.count(a) || bad.count(a)) return;
        entries.insert(a);
        work.push_back(a);
    };
    add(img_.entry);
    for (uint32_t a : cfg_.extraEntries) add(a);

    auto closure = [&](bool limit) {
        while (!work.empty()) {
            const uint32_t e = work.back();
            work.pop_back();
            Function f;
            std::vector<uint32_t> calls;
            if (!walk(e, &f, &calls, limit)) {
                entries.erase(e);
                bad.insert(e);
                char buf[96];
                snprintf(buf, sizeof buf, "entry %08x does not decode as code; dropped", e);
                notes.push_back(buf);
                continue;
            }
            funcs[e] = std::move(f);
            for (uint32_t t : calls)
                if (inText(t)) add(t);
        }
    };
    closure(false);

    // Code ends where the last reachable function ends, or where the bytes stop decoding as
    // code (text sections can end in data); whichever is later.
    for (auto& s : img_.sections) {
        if (!s.text) continue;
        uint32_t end = s.addr;
        for (auto& [e, f] : funcs)
            for (uint32_t a : f.insns)
                if (a >= s.addr && a < s.end()) end = std::max(end, a + 4);
        uint32_t heur = s.end();
        for (uint32_t blk = s.addr; blk < s.end(); blk += 1024) {
            int bad = 0, n = 0;
            for (uint32_t a = blk; a < std::min(blk + 1024, s.end()); a += 4, n++)
                if (img_.word(a) != 0 && flowOf(img_.word(a)) == Flow::Illegal) bad++;
            if (n && bad * 5 > n) {
                uint32_t p = blk;
                while (p < s.end() && !(img_.word(p) != 0 && flowOf(img_.word(p)) == Flow::Illegal)) p += 4;
                uint32_t q = p;
                while (q > s.addr && !isTerminator(img_.word(q - 4))) q -= 4;
                heur = q;
                break;
            }
        }
        codeLimit[s.addr] = std::max(end, heur);
    }

    for (int round = 0; round < 16; round++) {
        std::set<uint32_t> tableWords;
        std::set<uint32_t> covered;
        for (auto& [e, f] : funcs) {
            covered.insert(f.insns.begin(), f.insns.end());
            for (auto& [pc, jt] : f.tables)
                for (size_t i = 0; i < jt.targets.size(); i++) tableWords.insert(jt.table + uint32_t(4 * i));
        }
        std::set<uint32_t> consts, cands;
        scanConstants(consts);
        scanData(cands, tableWords);
        scanGaps(cands);
        cands.insert(consts.begin(), consts.end());
        std::set<uint32_t> tableTargets;
        for (auto& [e, f] : funcs)
            for (auto& [pc, jt] : f.tables) tableTargets.insert(jt.targets.begin(), jt.targets.end());
        const size_t before = entries.size();
        for (uint32_t c : cands) {
            if (entries.count(c) || bad.count(c) || cfg_.notEntries.count(c)) continue;
            // Only start functions where one can start: after a terminator or padding, or
            // where code takes the address (lis/addi), unless it is a switch case.
            const uint32_t prev = img_.word(c - 4);
            const Section* s = img_.find(c);
            const bool boundary = (s && c == s->addr) || isPadding(prev) || isTerminator(prev) ||
                                  (consts.count(c) && !tableTargets.count(c));
            if (!boundary && covered.count(c)) continue;
            if (!boundary && flowOf(prev) != Flow::Call) continue;
            if (!walk(c, nullptr, nullptr, true)) continue;
            add(c);
        }
        closure(false);
        if (entries.size() == before) break;
    }

    // Jump-table targets are labels, not functions: drop entries that only data pointed at
    // (the table itself) and that a table of another function lands on.
    {
        std::set<uint32_t> strong{img_.entry};
        strong.insert(cfg_.extraEntries.begin(), cfg_.extraEntries.end());
        std::set<uint32_t> consts;
        scanConstants(consts);
        strong.insert(consts.begin(), consts.end());
        std::set<uint32_t> tableTargets;
        for (auto& [e, f] : funcs) {
            for (auto& [pc, jt] : f.tables) tableTargets.insert(jt.targets.begin(), jt.targets.end());
            for (uint32_t a : f.insns) {
                const uint32_t w = img_.word(a);
                if (flowOf(w) == Flow::Call) strong.insert(op(w) == 18 ? bTarget(w, a) : bcTarget(w, a));
            }
        }
        size_t dropped = 0;
        for (uint32_t t : tableTargets)
            if (entries.count(t) && !strong.count(t)) {
                entries.erase(t);
                dropped++;
            }
        if (dropped) notes.push_back("dropped " + std::to_string(dropped) + " jump-table targets found as entries");
    }

    // Final bodies with the complete entry set (branches into other entries become tail calls).
    funcs.clear();
    for (uint32_t e : entries) {
        Function f;
        if (walk(e, &f, nullptr, false)) funcs[e] = std::move(f);
    }

    for (auto& [e, f] : funcs)
        for (auto& [pc, jt] : f.tables)
            if (jt.guessed) {

                char buf[128];
                snprintf(buf, sizeof buf, "jump table at %08x (fn %08x): no bounds check, %zu entries guessed", pc, e,
                         jt.targets.size());
                notes.push_back(buf);
            }
}
