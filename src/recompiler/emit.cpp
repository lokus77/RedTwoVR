// C++ emitter: one C++ function per guest entry point.
#include "emit.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>

#include "ppc.h"

using namespace ppc;

namespace {

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string R(u32 n) { return fmt("c.r[%u]", n); }
std::string F(u32 n) { return fmt("c.f[%u]", n); }
std::string P(u32 n) { return fmt("c.ps1[%u]", n); }
std::string hex(u32 v) { return fmt("0x%08Xu", v); }

// rA|0 + d
std::string eaD(u32 w) {
    const s32 d = simm(w);
    if (rA(w) == 0) return hex(u32(d));
    if (d == 0) return R(rA(w));
    return fmt("(c.r[%u] + %s)", rA(w), hex(u32(d)).c_str());
}
// rA|0 + rB
std::string eaX(u32 w) {
    if (rA(w) == 0) return R(rB(w));
    return fmt("(c.r[%u] + c.r[%u])", rA(w), rB(w));
}

}  // namespace

std::string Emitter::fn(u32 a) const { return fmt("f_%08X", a); }

std::string Emitter::callTo(u32 t, u32 pc, bool needJb) const {
    if (!an_.funcs.count(t)) return fmt("dispatch(c, %s);", hex(t).c_str());
    if (cfg_.saveCtx.count(t) && needJb) return fmt("RT_SAVECTX_CALL(c, %s, jb);", fn(t).c_str());
    (void)pc;
    return fn(t) + "(c);";
}

// Branch condition for BO/BI (CTR decrement emitted separately).
static std::string cond(u32 bo, u32 bi) {
    std::string ctr, crs;
    if (!(bo & 4)) ctr = (bo & 2) ? "c.ctr == 0" : "c.ctr != 0";
    if (!(bo & 0x10)) {
        const u32 m = 1u << (3 - (bi & 3));
        crs = fmt((bo & 8) ? "(c.cr[%u] & %u)" : "!(c.cr[%u] & %u)", bi >> 2, m);
    }
    if (ctr.empty() && crs.empty()) return "true";
    if (ctr.empty()) return crs;
    if (crs.empty()) return ctr;
    return "(" + ctr + ") && " + crs;
}

static std::string trapCond(u32 to, const std::string& a, const std::string& b) {
    std::vector<std::string> t;
    if (to & 16) t.push_back(fmt("s32(%s) < s32(%s)", a.c_str(), b.c_str()));
    if (to & 8) t.push_back(fmt("s32(%s) > s32(%s)", a.c_str(), b.c_str()));
    if (to & 4) t.push_back(fmt("%s == %s", a.c_str(), b.c_str()));
    if (to & 2) t.push_back(fmt("u32(%s) < u32(%s)", a.c_str(), b.c_str()));
    if (to & 1) t.push_back(fmt("u32(%s) > u32(%s)", a.c_str(), b.c_str()));
    if (t.empty()) return "false";
    std::string s;
    for (size_t i = 0; i < t.size(); i++) s += (i ? " || " : "") + t[i];
    return s;
}

bool Emitter::insn(const Function& f, u32 pc, u32 w, std::string& o, bool& usesJb) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto recIf = [&](const std::string& reg) {
        if (rc(w)) line("c.rec(" + reg + ");");
    };
    auto fpRec = [&]() {
        if (rc(w)) line("c.setcr1();");
    };
    auto branchTo = [&](u32 t, const std::string& c) {
        // intra-function goto or tail call into another entry
        const bool intra = f.labels.count(t) && std::binary_search(f.insns.begin(), f.insns.end(), t);
        std::string body;
        if (intra && t <= pc && cfg_.idleLoops.count(t)) body = fmt("idleWait(c, %s); goto L_%08X;", hex(t).c_str(), t);
        else if (intra) body = (t <= pc && !cfg_.noPollLoops.count(t) ? fmt("poll(c, %s); ", hex(t).c_str()) : std::string()) + fmt("goto L_%08X;", t);
        else body = callTo(t, pc, false) + " return;";
        if (c == "true") line(body);
        else line("if (" + c + ") { " + body + " }");
    };

    switch (op(w)) {
    case 3:  // twi
        line(fmt("if (%s) trap(c, %s);", trapCond(D, R(A), hex(u32(simm(w)))).c_str(), hex(pc).c_str()));
        return true;
    case 4:
        return ps(f, pc, w, o);
    case 7:
        line(fmt("%s = u32(s32(%s) * %d);", R(D).c_str(), R(A).c_str(), simm(w)));
        return true;
    case 8:
        line(fmt("{ u64 t = u64(~%s) + %s + 1; %s = u32(t); c.ca = u32(t >> 32); }", R(A).c_str(),
                 hex(u32(simm(w))).c_str(), R(D).c_str()));
        return true;
    case 10:
        line(fmt("c.cmpu(%u, %s, %s);", crfD(w), R(A).c_str(), hex(uimm(w)).c_str()));
        return true;
    case 11:
        line(fmt("c.cmps(%u, s32(%s), %d);", crfD(w), R(A).c_str(), simm(w)));
        return true;
    case 12:
    case 13:
        line(fmt("%s = addCarry(c, %s, %s);", R(D).c_str(), R(A).c_str(), hex(u32(simm(w))).c_str()));
        if (op(w) == 13) line("c.rec(" + R(D) + ");");
        return true;
    case 14:
        if (A == 0) line(fmt("%s = %s;", R(D).c_str(), hex(u32(simm(w))).c_str()));
        else line(fmt("%s = %s + %s;", R(D).c_str(), R(A).c_str(), hex(u32(simm(w))).c_str()));
        return true;
    case 15:
        if (A == 0) line(fmt("%s = %s;", R(D).c_str(), hex(uimm(w) << 16).c_str()));
        else line(fmt("%s = %s + %s;", R(D).c_str(), R(A).c_str(), hex(uimm(w) << 16).c_str()));
        return true;
    case 16: {
        const u32 t = bcTarget(w, pc);
        const u32 bo = BO(w), bi = BI(w);
        if (!(bo & 4)) line("c.ctr--;");
        const std::string cnd = cond(bo, bi);
        if (LK(w)) {
            line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
            if (t == pc + 4) return true;  // bcl $+4: reads the PC
            if (cnd == "true") line(callTo(t, pc, true));
            else line("if (" + cnd + ") { " + callTo(t, pc, false) + " }");
            if (cfg_.saveCtx.count(t)) usesJb = true;
            return true;
        }
        branchTo(t, cnd);
        return true;
    }
    case 17:
        line(fmt("sc(c, %s);", hex(pc).c_str()));
        return true;
    case 18: {
        const u32 t = bTarget(w, pc);
        if (LK(w)) {
            line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
            line(callTo(t, pc, true));
            if (cfg_.saveCtx.count(t)) usesJb = true;
            return true;
        }
        branchTo(t, "true");
        return true;
    }
    case 19:
        switch (xo10(w)) {
        case 0:  // mcrf
            line(fmt("c.cr[%u] = c.cr[%u];", crfD(w), crfS(w)));
            return true;
        case 16: {  // bclr
            const u32 bo = BO(w), bi = BI(w);
            if (!(bo & 4)) line("c.ctr--;");
            const std::string cnd = cond(bo, bi);
            if (LK(w)) {
                line(fmt("{ u32 t = c.lr; c.lr = %s; if (%s) dispatch(c, t); }", hex(pc + 4).c_str(), cnd.c_str()));
                return true;
            }
            if (cnd == "true") line("return;");
            else line("if (" + cnd + ") return;");
            return true;
        }
        case 528: {  // bcctr
            const u32 bo = BO(w), bi = BI(w);
            const std::string cnd = cond(bo | 4, bi);
            if (LK(w)) {
                line(fmt("c.lr = %s;", hex(pc + 4).c_str()));
                if (cnd == "true") line("dispatch(c, c.ctr);");
                else line("if (" + cnd + ") dispatch(c, c.ctr);");
                return true;
            }
            auto it = f.tables.find(pc);
            if (it != f.tables.end() && cnd == "true") {
                std::string s = "switch (c.ctr) {";
                std::set<u32> done;
                for (u32 t : it->second.targets) {
                    if (!done.insert(t).second) continue;
                    s += fmt(" case %s: goto L_%08X;", hex(t).c_str(), t);
                }
                s += " default: dispatch(c, c.ctr); return; }";
                line(s);
                return true;
            }
            if (cnd == "true") line("dispatch(c, c.ctr); return;");
            else line("if (" + cnd + ") { dispatch(c, c.ctr); return; }");
            return true;
        }
        case 50:
            line(fmt("rfi(c, %s);", hex(pc).c_str()));
            return true;
        case 150:  // isync
            return true;
        case 33: line(fmt("c.setcrbit(%u, !(c.crbit(%u) | c.crbit(%u)));", D, A, B)); return true;
        case 129: line(fmt("c.setcrbit(%u, c.crbit(%u) & !c.crbit(%u));", D, A, B)); return true;
        case 193: line(fmt("c.setcrbit(%u, c.crbit(%u) ^ c.crbit(%u));", D, A, B)); return true;
        case 225: line(fmt("c.setcrbit(%u, !(c.crbit(%u) & c.crbit(%u)));", D, A, B)); return true;
        case 257: line(fmt("c.setcrbit(%u, c.crbit(%u) & c.crbit(%u));", D, A, B)); return true;
        case 289: line(fmt("c.setcrbit(%u, c.crbit(%u) == c.crbit(%u));", D, A, B)); return true;
        case 417: line(fmt("c.setcrbit(%u, c.crbit(%u) | !c.crbit(%u));", D, A, B)); return true;
        case 449: line(fmt("c.setcrbit(%u, c.crbit(%u) | c.crbit(%u));", D, A, B)); return true;
        }
        return false;
    case 20: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        line(fmt("%s = (rotl(%s, %u) & %s) | (%s & %s);", R(A).c_str(), R(D).c_str(), B, hex(m).c_str(), R(A).c_str(),
                 hex(~m).c_str()));
        recIf(R(A));
        return true;
    }
    case 21: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        if (B == 0) line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(m).c_str()));
        else line(fmt("%s = rotl(%s, %u) & %s;", R(A).c_str(), R(D).c_str(), B, hex(m).c_str()));
        recIf(R(A));
        return true;
    }
    case 23: {
        const u32 m = rotMask(C, (w >> 1) & 31);
        line(fmt("%s = rotl(%s, %s) & %s;", R(A).c_str(), R(D).c_str(), R(B).c_str(), hex(m).c_str()));
        recIf(R(A));
        return true;
    }
    case 24:
        if (A == 0 && D == 0 && uimm(w) == 0) return true;  // nop
        line(fmt("%s = %s | %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str()));
        return true;
    case 25: line(fmt("%s = %s | %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str())); return true;
    case 26: line(fmt("%s = %s ^ %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str())); return true;
    case 27: line(fmt("%s = %s ^ %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str())); return true;
    case 28:
        line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w)).c_str()));
        line("c.rec(" + R(A) + ");");
        return true;
    case 29:
        line(fmt("%s = %s & %s;", R(A).c_str(), R(D).c_str(), hex(uimm(w) << 16).c_str()));
        line("c.rec(" + R(A) + ");");
        return true;
    case 31:
        return op31(pc, w, o);
    // --- integer loads/stores (D-form) ---
    case 32: line(fmt("%s = rd32(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 34: line(fmt("%s = rd8(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 40: line(fmt("%s = rd16(%s);", R(D).c_str(), eaD(w).c_str())); return true;
    case 42: line(fmt("%s = u32(s32(s16(rd16(%s))));", R(D).c_str(), eaD(w).c_str())); return true;
    case 33: case 35: case 41: case 43: {
        const char* ld = op(w) == 33 ? "rd32(ea)" : op(w) == 35 ? "rd8(ea)" : op(w) == 41 ? "rd16(ea)" : "u32(s32(s16(rd16(ea))))";
        line(fmt("{ u32 ea = %s + %s; %s = %s; %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(), R(D).c_str(), ld,
                 R(A).c_str()));
        return true;
    }
    case 36: line(fmt("wr32(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 38: line(fmt("wr8(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 44: line(fmt("wr16(%s, %s);", eaD(w).c_str(), R(D).c_str())); return true;
    case 37: case 39: case 45: {
        const char* st = op(w) == 37 ? "wr32" : op(w) == 39 ? "wr8" : "wr16";
        line(fmt("{ u32 ea = %s + %s; %s(ea, %s); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(), st, R(D).c_str(),
                 R(A).c_str()));
        return true;
    }
    case 46: {  // lmw
        std::string s = fmt("{ u32 ea = %s;", eaD(w).c_str());
        for (u32 r = D; r < 32; r++) s += fmt(" c.r[%u] = rd32(ea + %u);", r, (r - D) * 4);
        line(s + " }");
        return true;
    }
    case 47: {  // stmw
        std::string s = fmt("{ u32 ea = %s;", eaD(w).c_str());
        for (u32 r = D; r < 32; r++) s += fmt(" wr32(ea + %u, c.r[%u]);", (r - D) * 4, r);
        line(s + " }");
        return true;
    }
    // --- FP loads/stores ---
    case 48: line(fmt("%s = %s = s2d(rd32(%s));", F(D).c_str(), P(D).c_str(), eaD(w).c_str())); return true;
    case 49:
        line(fmt("{ u32 ea = %s + %s; %s = %s = s2d(rd32(ea)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), P(D).c_str(), R(A).c_str()));
        return true;
    case 50: line(fmt("%s = bdbl(rd64(%s));", F(D).c_str(), eaD(w).c_str())); return true;
    case 51:
        line(fmt("{ u32 ea = %s + %s; %s = bdbl(rd64(ea)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), R(A).c_str()));
        return true;
    case 52: line(fmt("wr32(%s, d2s(%s));", eaD(w).c_str(), F(D).c_str())); return true;
    case 53:
        line(fmt("{ u32 ea = %s + %s; wr32(ea, d2s(%s)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), R(A).c_str()));
        return true;
    case 54: line(fmt("wr64(%s, dbits(%s));", eaD(w).c_str(), F(D).c_str())); return true;
    case 55:
        line(fmt("{ u32 ea = %s + %s; wr64(ea, dbits(%s)); %s = ea; }", R(A).c_str(), hex(u32(simm(w))).c_str(),
                 F(D).c_str(), R(A).c_str()));
        return true;
    case 56: case 57: case 60: case 61: {  // psq_l / psq_lu / psq_st / psq_stu
        s32 d = s32(w & 0xFFF);
        if (d & 0x800) d -= 0x1000;
        const u32 W = (w >> 15) & 1, I = (w >> 12) & 7;
        const bool upd = op(w) == 57 || op(w) == 61;
        const bool st = op(w) >= 60;
        std::string ea = A == 0 && !upd ? hex(u32(d)) : fmt("%s + %s", R(A).c_str(), hex(u32(d)).c_str());
        line(fmt("{ u32 ea = %s; %s(c, %u, ea, %s, c.gqr[%u]);%s }", ea.c_str(), st ? "psqStore" : "psqLoad", D,
                 W ? "true" : "false", I, upd ? (" " + R(A) + " = ea;").c_str() : ""));
        return true;
    }
    case 59:
        return fp59(w, o);
    case 63:
        return fp63(w, o);
    }
    return false;
}

bool Emitter::op31(u32 pc, u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto rec = [&](const std::string& r) {
        if (rc(w)) line("c.rec(" + r + ");");
    };
    const std::string rd = R(D), ra = R(A), rb = R(B);
    // XO-form arithmetic (OE in bit 10)
    const u32 x9 = xo9(w);
    const bool ovf = oe(w);
    auto arith = [&](const std::string& expr, bool ovAdd, const std::string& oa, const std::string& ob) {
        if (ovf && ovAdd) {
            line(fmt("{ u32 a_ = %s, b_ = %s; u32 r_ = %s; addOv(c, a_, b_, r_); %s = r_; }", oa.c_str(), ob.c_str(),
                     expr.c_str(), rd.c_str()));
        } else {
            line(rd + " = " + expr + ";");
        }
        rec(rd);
    };
    switch (x9) {
    case 266: arith(ovf ? "a_ + b_" : ra + " + " + rb, true, ra, rb); return true;           // add
    case 10:  line(fmt("%s = addCarry(c, %s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;  // addc
    case 138: line(fmt("%s = addeCarry(c, %s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true; // adde
    case 234: line(fmt("%s = addeCarry(c, %s, 0xFFFFFFFFu);", rd.c_str(), ra.c_str())); rec(rd); return true;   // addme
    case 202: line(fmt("%s = addeCarry(c, %s, 0);", rd.c_str(), ra.c_str())); rec(rd); return true;             // addze
    case 40:  arith(ovf ? "a_ + b_ + 1u" : rb + " - " + ra, true, "~" + ra, rb); return true;  // subf
    case 8:   line(fmt("{ u64 t = u64(~%s) + %s + 1; %s = u32(t); c.ca = u32(t >> 32); }", ra.c_str(), rb.c_str(), rd.c_str())); rec(rd); return true;  // subfc
    case 136: line(fmt("%s = addeCarry(c, ~%s, %s);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;  // subfe
    case 232: line(fmt("%s = addeCarry(c, ~%s, 0xFFFFFFFFu);", rd.c_str(), ra.c_str())); rec(rd); return true;    // subfme
    case 200: line(fmt("%s = addeCarry(c, ~%s, 0);", rd.c_str(), ra.c_str())); rec(rd); return true;              // subfze
    case 104:  // neg
        if (ovf) line(fmt("c.ov = %s == 0x80000000u; c.so |= c.ov;", ra.c_str()));
        line(fmt("%s = 0u - %s;", rd.c_str(), ra.c_str()));
        rec(rd);
        return true;
    case 235:  // mullw
        if (ovf) line(fmt("{ s64 p = s64(s32(%s)) * s32(%s); c.ov = p != s32(p); c.so |= c.ov; }", ra.c_str(), rb.c_str()));
        line(fmt("%s = u32(s32(%s) * s32(%s));", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    case 75: line(fmt("%s = u32((s64(s32(%s)) * s32(%s)) >> 32);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;
    case 11: line(fmt("%s = u32((u64(%s) * %s) >> 32);", rd.c_str(), ra.c_str(), rb.c_str())); rec(rd); return true;
    case 491:
        if (ovf) line(fmt("c.ov = %s == 0 || (%s == 0x80000000u && %s == 0xFFFFFFFFu); c.so |= c.ov;", rb.c_str(), ra.c_str(), rb.c_str()));
        line(fmt("%s = divw(s32(%s), s32(%s));", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    case 459:
        if (ovf) line(fmt("c.ov = %s == 0; c.so |= c.ov;", rb.c_str()));
        line(fmt("%s = divwu(%s, %s);", rd.c_str(), ra.c_str(), rb.c_str()));
        rec(rd);
        return true;
    }
    // X-form: rS = bits 21-25, rA = destination for logical ops
    const std::string rs = R(D);
    switch (xo10(w)) {
    case 0: line(fmt("c.cmps(%u, s32(%s), s32(%s));", crfD(w), ra.c_str(), rb.c_str())); return true;
    case 32: line(fmt("c.cmpu(%u, %s, %s);", crfD(w), ra.c_str(), rb.c_str())); return true;
    case 4:
        if (D == 31) line(fmt("trap(c, %s);", hex(pc).c_str()));
        else line(fmt("if (%s) trap(c, %s);", trapCond(D, ra, rb).c_str(), hex(pc).c_str()));
        return true;
    case 19: line(rd + " = c.getcr();"); return true;
    case 144: line(fmt("c.mtcrf(0x%02X, %s);", (w >> 12) & 0xFF, rs.c_str())); return true;
    case 512: line(fmt("c.cr[%u] = u8((c.getxer() >> 28) & 15); c.so = c.ov = c.ca = 0;", crfD(w))); return true;
    case 20: line(fmt("{ u32 ea = %s; c.resvAddr = ea; %s = rd32(ea); }", eaX(w).c_str(), rd.c_str())); return true;
    case 150: line(fmt("{ u32 ea = %s; wr32(ea, %s); c.cr[0] = u8(2 | c.so); }", eaX(w).c_str(), rs.c_str())); return true;
    // indexed loads/stores
    case 23: line(fmt("%s = rd32(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 87: line(fmt("%s = rd8(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 279: line(fmt("%s = rd16(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 343: line(fmt("%s = u32(s32(s16(rd16(%s))));", rd.c_str(), eaX(w).c_str())); return true;
    case 534: line(fmt("%s = rd32r(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 790: line(fmt("%s = rd16r(%s);", rd.c_str(), eaX(w).c_str())); return true;
    case 55: case 119: case 311: case 375: {
        const char* ld = xo10(w) == 55 ? "rd32(ea)" : xo10(w) == 119 ? "rd8(ea)" : xo10(w) == 311 ? "rd16(ea)" : "u32(s32(s16(rd16(ea))))";
        line(fmt("{ u32 ea = %s + %s; %s = %s; %s = ea; }", ra.c_str(), rb.c_str(), rd.c_str(), ld, ra.c_str()));
        return true;
    }
    case 151: line(fmt("wr32(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 215: line(fmt("wr8(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 407: line(fmt("wr16(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 662: line(fmt("wr32r(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 918: line(fmt("wr16r(%s, %s);", eaX(w).c_str(), rs.c_str())); return true;
    case 183: case 247: case 439: {
        const char* st = xo10(w) == 183 ? "wr32" : xo10(w) == 247 ? "wr8" : "wr16";
        line(fmt("{ u32 ea = %s + %s; %s(ea, %s); %s = ea; }", ra.c_str(), rb.c_str(), st, rs.c_str(), ra.c_str()));
        return true;
    }
    case 535: line(fmt("%s = %s = s2d(rd32(%s));", F(D).c_str(), P(D).c_str(), eaX(w).c_str())); return true;
    case 567: line(fmt("{ u32 ea = %s + %s; %s = %s = s2d(rd32(ea)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), P(D).c_str(), ra.c_str())); return true;
    case 599: line(fmt("%s = bdbl(rd64(%s));", F(D).c_str(), eaX(w).c_str())); return true;
    case 631: line(fmt("{ u32 ea = %s + %s; %s = bdbl(rd64(ea)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), ra.c_str())); return true;
    case 663: line(fmt("wr32(%s, d2s(%s));", eaX(w).c_str(), F(D).c_str())); return true;
    case 695: line(fmt("{ u32 ea = %s + %s; wr32(ea, d2s(%s)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), ra.c_str())); return true;
    case 727: line(fmt("wr64(%s, dbits(%s));", eaX(w).c_str(), F(D).c_str())); return true;
    case 759: line(fmt("{ u32 ea = %s + %s; wr64(ea, dbits(%s)); %s = ea; }", ra.c_str(), rb.c_str(), F(D).c_str(), ra.c_str())); return true;
    case 983: line(fmt("wr32(%s, u32(dbits(%s)));", eaX(w).c_str(), F(D).c_str())); return true;
    case 597: line(fmt("lswi(c, %u, %s, %u);", D, A ? R(A).c_str() : "0u", B ? B : 32)); return true;
    case 725: line(fmt("stswi(c, %u, %s, %u);", D, A ? R(A).c_str() : "0u", B ? B : 32)); return true;
    case 533: line(fmt("lswi(c, %u, %s, c.xerBytes);", D, eaX(w).c_str())); return true;
    case 661: line(fmt("stswi(c, %u, %s, c.xerBytes);", D, eaX(w).c_str())); return true;
    // logical / shifts (dest rA)
    case 28: line(fmt("%s = %s & %s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 60: line(fmt("%s = %s & ~%s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 444:
        if (D == B) line(fmt("%s = %s;", ra.c_str(), rs.c_str()));
        else line(fmt("%s = %s | %s;", ra.c_str(), rs.c_str(), rb.c_str()));
        rec(ra);
        return true;
    case 412: line(fmt("%s = %s | ~%s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 316: line(fmt("%s = %s ^ %s;", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 476: line(fmt("%s = ~(%s & %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 124: line(fmt("%s = ~(%s | %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 284: line(fmt("%s = ~(%s ^ %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 24: line(fmt("%s = slw(%s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 536: line(fmt("%s = srw(%s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 792: line(fmt("%s = sraw(c, %s, %s);", ra.c_str(), rs.c_str(), rb.c_str())); rec(ra); return true;
    case 824: line(fmt("%s = srawi(c, %s, %u);", ra.c_str(), rs.c_str(), B)); rec(ra); return true;
    case 26: line(fmt("%s = cntlzw(%s);", ra.c_str(), rs.c_str())); rec(ra); return true;
    case 954: line(fmt("%s = u32(s32(s8(%s)));", ra.c_str(), rs.c_str())); rec(ra); return true;
    case 922: line(fmt("%s = u32(s32(s16(%s)));", ra.c_str(), rs.c_str())); rec(ra); return true;
    // system
    case 83: line(rd + " = c.msr;"); return true;
    case 146: line(fmt("mtmsr(c, %s, %s);", rs.c_str(), hex(pc).c_str())); return true;
    case 210: line(fmt("mtsr(c, %u, %s);", A & 15, rs.c_str())); return true;
    case 242: line(fmt("mtsr(c, %s >> 28, %s);", rb.c_str(), rs.c_str())); return true;
    case 595: line(fmt("%s = mfsr(c, %u);", rd.c_str(), A & 15)); return true;
    case 659: line(fmt("%s = mfsr(c, %s >> 28);", rd.c_str(), rb.c_str())); return true;
    case 339: {
        const u32 s = spr(w);
        if (s == 8) line(rd + " = c.lr;");
        else if (s == 9) line(rd + " = c.ctr;");
        else if (s == 1) line(rd + " = c.getxer();");
        else if (s >= 912 && s <= 919) line(fmt("%s = c.gqr[%u];", rd.c_str(), s - 912));
        else line(fmt("%s = mfspr(c, %u);", rd.c_str(), s));
        return true;
    }
    case 467: {
        const u32 s = spr(w);
        if (s == 8) line("c.lr = " + rs + ";");
        else if (s == 9) line("c.ctr = " + rs + ";");
        else if (s == 1) line("c.setxer(" + rs + ");");
        else if (s >= 912 && s <= 919) line(fmt("c.gqr[%u] = %s;", s - 912, rs.c_str()));
        else line(fmt("mtspr(c, %u, %s);", s, rs.c_str()));
        return true;
    }
    case 371: line(fmt("%s = mftb(c, %u);", rd.c_str(), spr(w))); return true;
    case 54: case 86: case 278: case 246: case 566: case 598: case 854:
        return true;  // dcbst dcbf dcbt dcbtst tlbsync sync eieio
    case 306: line(fmt("tlbie(c, %s);", rb.c_str())); return true;
    case 470: line(fmt("dcbi(c, %s);", eaX(w).c_str())); return true;
    case 982: line(fmt("icbi(c, %s);", eaX(w).c_str())); return true;
    case 1014: line(fmt("dcbz(c, %s);", eaX(w).c_str())); return true;
    }
    return false;
}

bool Emitter::fp59(u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    std::string e;
    switch (xo5(w)) {
    case 18: e = fmt("%s / %s", F(A).c_str(), F(B).c_str()); break;
    case 20: e = fmt("%s - %s", F(A).c_str(), F(B).c_str()); break;
    case 21: e = fmt("%s + %s", F(A).c_str(), F(B).c_str()); break;
    case 22: e = fmt("__builtin_sqrt(%s)", F(B).c_str()); break;
    case 24: line(fmt("%s = %s = fres(%s);", F(D).c_str(), P(D).c_str(), F(B).c_str())); goto done;
    case 25: e = fmt("%s * %s", F(A).c_str(), F(C).c_str()); break;
    case 28: e = fmt("fma_(%s, %s, -%s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 29: e = fmt("fma_(%s, %s, %s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 30: e = fmt("-fma_(%s, %s, -%s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    case 31: e = fmt("-fma_(%s, %s, %s)", F(A).c_str(), F(C).c_str(), F(B).c_str()); break;
    default: return false;
    }
    line(fmt("%s = %s = rsingle(%s);", F(D).c_str(), P(D).c_str(), e.c_str()));
done:
    if (rc(w)) line("c.setcr1();");
    return true;
}

bool Emitter::fp63(u32 w, std::string& o) const {
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto fin = [&]() {
        if (rc(w)) line("c.setcr1();");
        return true;
    };
    switch (xo5(w)) {
    case 18: line(fmt("%s = %s / %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 20: line(fmt("%s = %s - %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 21: line(fmt("%s = %s + %s;", F(D).c_str(), F(A).c_str(), F(B).c_str())); return fin();
    case 22: line(fmt("%s = __builtin_sqrt(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 23: line(fmt("%s = fsel(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 25: line(fmt("%s = %s * %s;", F(D).c_str(), F(A).c_str(), F(C).c_str())); return fin();
    case 26: line(fmt("%s = frsqrte(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 28: line(fmt("%s = fma_(%s, %s, -%s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 29: line(fmt("%s = fma_(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 30: line(fmt("%s = -fma_(%s, %s, -%s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    case 31: line(fmt("%s = -fma_(%s, %s, %s);", F(D).c_str(), F(A).c_str(), F(C).c_str(), F(B).c_str())); return fin();
    }
    switch (xo10(w)) {
    case 0: case 32: line(fmt("c.fcmp(%u, %s, %s);", crfD(w), F(A).c_str(), F(B).c_str())); return true;
    case 12: line(fmt("%s = %s = rsingle(%s);", F(D).c_str(), P(D).c_str(), F(B).c_str())); return fin();
    case 14: line(fmt("%s = ctiwBits(fctiw(c, %s, false));", F(D).c_str(), F(B).c_str())); return fin();
    case 15: line(fmt("%s = ctiwBits(fctiw(c, %s, true));", F(D).c_str(), F(B).c_str())); return fin();
    case 38: line(fmt("c.fpscr |= 0x%08Xu;", 0x80000000u >> D)); return fin();
    case 70: line(fmt("c.fpscr &= ~0x%08Xu;", 0x80000000u >> D)); return fin();
    case 134: {
        const u32 sh = 28 - 4 * crfD(w);
        line(fmt("c.fpscr = (c.fpscr & ~0x%08Xu) | 0x%08Xu;", 0xFu << sh, ((w >> 12) & 0xF) << sh));
        return fin();
    }
    case 711: {
        const u32 fm = (w >> 17) & 0xFF;
        u32 m = 0;
        for (int i = 0; i < 8; i++)
            if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
        line(fmt("c.fpscr = (c.fpscr & ~0x%08Xu) | (u32(dbits(%s)) & 0x%08Xu);", m, F(B).c_str(), m));
        return fin();
    }
    case 583: line(fmt("%s = bdbl(0xFFF8000000000000ull | c.fpscr);", F(D).c_str())); return fin();
    case 64: line(fmt("c.cr[%u] = u8((c.fpscr >> %u) & 15);", crfD(w), 28 - 4 * crfS(w))); return true;
    case 40: line(fmt("%s = fneg_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 72: line(fmt("%s = %s;", F(D).c_str(), F(B).c_str())); return fin();
    case 136: line(fmt("%s = fnabs_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    case 264: line(fmt("%s = fabs_(%s);", F(D).c_str(), F(B).c_str())); return fin();
    }
    return false;
}

bool Emitter::ps(const Function&, u32 pc, u32 w, std::string& o) const {
    (void)pc;
    const u32 D = rD(w), A = rA(w), B = rB(w), C = rC(w);
    auto line = [&](const std::string& s) { o += "    " + s + "\n"; };
    auto fin = [&]() {
        if (rc(w)) line("c.setcr1();");
        return true;
    };
    // two lanes computed from temporaries (operands may alias the destination)
    auto lanes = [&](const std::string& e0, const std::string& e1, bool round) {
        const char* rs = round ? "rsingle" : "";
        line(fmt("{ double a0 = %s, a1 = %s, b0 = %s, b1 = %s, c0 = %s, c1 = %s; (void)a0; (void)a1; (void)b0; (void)b1; (void)c0; (void)c1; "
                 "%s = %s(%s); %s = %s(%s); }",
                 F(A).c_str(), P(A).c_str(), F(B).c_str(), P(B).c_str(), F(C).c_str(), P(C).c_str(), F(D).c_str(), rs,
                 e0.c_str(), P(D).c_str(), rs, e1.c_str()));
        return fin();
    };
    switch (xo10(w)) {
    case 0: case 32: line(fmt("c.fcmp(%u, %s, %s);", crfD(w), F(A).c_str(), F(B).c_str())); return true;
    case 64: case 96: line(fmt("c.fcmp(%u, %s, %s);", crfD(w), P(A).c_str(), P(B).c_str())); return true;
    case 40: return lanes("fneg_(b0)", "fneg_(b1)", false);
    case 72: return lanes("b0", "b1", false);
    case 136: return lanes("fnabs_(b0)", "fnabs_(b1)", false);
    case 264: return lanes("fabs_(b0)", "fabs_(b1)", false);
    case 528: return lanes("a0", "b0", false);
    case 560: return lanes("a0", "b1", false);
    case 592: return lanes("a1", "b0", false);
    case 624: return lanes("a1", "b1", false);
    case 1014: line(fmt("dcbzl(c, %s);", eaX(w).c_str())); return true;
    }
    switch (xo5(w)) {
    case 10: return lanes("rsingle(a0 + b1)", "c1", false);
    case 11: return lanes("c0", "rsingle(a0 + b1)", false);
    case 12: return lanes("a0 * c0", "a1 * c0", true);
    case 13: return lanes("a0 * c1", "a1 * c1", true);
    case 14: return lanes("fma_(a0, c0, b0)", "fma_(a1, c0, b1)", true);
    case 15: return lanes("fma_(a0, c1, b0)", "fma_(a1, c1, b1)", true);
    case 18: return lanes("a0 / b0", "a1 / b1", true);
    case 20: return lanes("a0 - b0", "a1 - b1", true);
    case 21: return lanes("a0 + b0", "a1 + b1", true);
    case 23: return lanes("fsel(a0, c0, b0)", "fsel(a1, c1, b1)", false);
    case 24: return lanes("fres(b0)", "fres(b1)", false);
    case 25: return lanes("a0 * c0", "a1 * c1", true);
    case 26: return lanes("frsqrte(b0)", "frsqrte(b1)", true);
    case 28: return lanes("fma_(a0, c0, -b0)", "fma_(a1, c1, -b1)", true);
    case 29: return lanes("fma_(a0, c0, b0)", "fma_(a1, c1, b1)", true);
    case 30: return lanes("-fma_(a0, c0, -b0)", "-fma_(a1, c1, -b1)", true);
    case 31: return lanes("-fma_(a0, c0, b0)", "-fma_(a1, c1, b1)", true);
    }
    const u32 x6 = (w >> 1) & 0x3F;
    if (x6 == 6 || x6 == 7 || x6 == 38 || x6 == 39) {
        const bool st = x6 & 1, upd = x6 >= 38;
        const u32 W = (w >> 10) & 1, I = (w >> 7) & 7;
        std::string ea = upd ? fmt("%s + %s", R(A).c_str(), R(B).c_str()) : eaX(w);
        line(fmt("{ u32 ea = %s; %s(c, %u, ea, %s, c.gqr[%u]);%s }", ea.c_str(), st ? "psqStore" : "psqLoad", D,
                 W ? "true" : "false", I, upd ? (" " + R(A) + " = ea;").c_str() : ""));
        return true;
    }
    return false;
}

void Emitter::function(const Function& f, std::string& o, const std::string& name) const {
    std::string body;
    bool usesJb = false;
    const bool needEntryGoto = !f.insns.empty() && f.insns.front() != f.entry;
    for (size_t i = 0; i < f.insns.size(); i++) {
        const u32 pc = f.insns[i];
        const u32 w = img_.word(pc);
        if (f.labels.count(pc) || (needEntryGoto && pc == f.entry)) body += fmt("L_%08X:\n", pc);
        body += fmt("    // %08X: %08X\n", pc, w);
        if (!insn(f, pc, w, body, usesJb)) body += fmt("    illegal(c, %s, %s);\n", hex(pc).c_str(), hex(w).c_str());
        if (f.tailEntries.count(pc)) body += fmt("    %s return;\n", callTo(pc + 4, pc, false).c_str());
    }
    o += "void " + name + "(Ctx& c) {\n";
    o += fmt("    if (g_trace) [[unlikely]] traceEnter(c, %s);\n", hex(f.entry).c_str());
    if (usesJb) o += "    JmpBuf jb;\n";
    if (needEntryGoto) o += fmt("    goto L_%08X;\n", f.entry);
    o += body;
    o += "}\n\n";
}

bool Emitter::write(const std::string& outDir, int files) {
    std::vector<const Function*> list;
    for (auto& [e, f] : an_.funcs) list.push_back(&f);

    // header
    {
        std::string h = "// Generated by r2recomp. Do not edit.\n#pragma once\n#include \"ctx.h\"\n\nnamespace rt {\n";
        for (auto* f : list) {
            h += "void " + fn(f->entry) + "(Ctx& c);";
            if (auto it = cfg_.names.find(f->entry); it != cfg_.names.end()) h += "  // " + it->second;
            h += "\n";
            if (cfg_.hooks.count(f->entry)) h += "void " + fn(f->entry) + "_orig(Ctx& c);\n";
        }
        for (auto& [a, n] : cfg_.hooks) h += "void hle_" + n + "(Ctx& c);\n";
        h += "}  // namespace rt\n";
        if (!writeFile(outDir + "/funcs.h", h)) return false;
    }
    // bodies, split by instruction count
    size_t total = 0;
    for (auto* f : list) total += f->insns.size();
    const size_t per = total / size_t(files) + 1;
    int file = 0;
    size_t acc = 0;
    std::string o;
    auto flush = [&]() -> bool {
        if (o.empty()) return true;
        std::string full = "// Generated by r2recomp. Do not edit.\n#include \"funcs.h\"\n\nnamespace rt {\n\n" + o + "}  // namespace rt\n";
        o.clear();
        return writeFile(outDir + fmt("/code_%03d.cpp", file++), full);
    };
    for (auto* f : list) {
        if (auto it = cfg_.hooks.find(f->entry); it != cfg_.hooks.end()) {
            o += "void " + fn(f->entry) + "(Ctx& c) { hle_" + it->second + "(c); }\n";
            function(*f, o, fn(f->entry) + "_orig");
        } else {
            function(*f, o, fn(f->entry));
        }
        acc += f->insns.size();
        if (acc >= per) {
            if (!flush()) return false;
            acc = 0;
        }
    }
    if (!flush()) return false;
    for (; file < files; file++)  // keep the file set stable for the build system
        if (!writeFile(outDir + fmt("/code_%03d.cpp", file), "// empty\n")) return false;

    // function table + names
    {
        std::string t = "// Generated by r2recomp. Do not edit.\n#include \"funcs.h\"\n#include \"table.h\"\n\nnamespace rt {\n";
        t += "const FuncEntry g_funcTable[] = {\n";
        for (auto* f : list) t += fmt("    {%s, %s},\n", hex(f->entry).c_str(), fn(f->entry).c_str());
        t += "};\nconst u32 g_funcCount = sizeof(g_funcTable) / sizeof(g_funcTable[0]);\n";
        t += "const SymName g_symNames[] = {\n";
        for (auto& [a, n] : cfg_.names) t += fmt("    {%s, \"%s\"},\n", hex(a).c_str(), n.c_str());
        t += "    {0, nullptr},\n};\n";
        t += "const FuncEntry g_origTable[] = {\n";
        for (auto& [a, n] : cfg_.hooks)
            if (an_.funcs.count(a)) t += fmt("    {%s, %s_orig},\n", hex(a).c_str(), fn(a).c_str());
        t += "    {0, nullptr},\n};\n}  // namespace rt\n";
        if (!writeFile(outDir + "/table.cpp", t)) return false;
    }
    return true;
}

bool Emitter::writeFile(const std::string& path, const std::string& text) {
    // Only rewrite when the content changes (keeps incremental builds fast).
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (old == text) return true;
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out << text;
    return bool(out);
}
