"""PowerPC (750CL/Espresso) instruction decoding and C emission.

emit(word, addr) returns C statements for a non-branch instruction, operating on
`ppc_ctx *c`. Branches are decoded by `branch_info` and lowered by the caller,
because their translation depends on the surrounding function.
"""


class Unsupported(Exception):
    pass


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def mask32(mb, me):
    """PowerPC MASK(mb, me), wrapping when mb > me."""
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


class F:
    """Instruction fields."""

    def __init__(self, w):
        self.w = w
        self.op = w >> 26
        self.rd = self.rs = self.frd = self.frs = (w >> 21) & 31
        self.ra = self.fra = (w >> 16) & 31
        self.rb = self.frb = self.sh = (w >> 11) & 31
        self.frc = self.mb = (w >> 6) & 31
        self.me = (w >> 1) & 31
        self.rc = w & 1
        self.oe = (w >> 10) & 1
        self.xo10 = (w >> 1) & 0x3FF
        self.xo9 = (w >> 1) & 0x1FF
        self.xo5 = (w >> 1) & 0x1F
        self.uimm = w & 0xFFFF
        self.simm = sext16(self.uimm)
        self.crfd = self.rd >> 2
        self.crfs = self.ra >> 2


def R(n):
    return "c->r[%d]" % n


def RA0(n):
    """(rA|0) operand."""
    return "0u" if n == 0 else R(n)


def FR(n):
    return "c->f[%d]" % n


def hx(v):
    return "0x%Xu" % (v & 0xFFFFFFFF)


def rc0(f, dst):
    return [" RC0(%s);" % dst] if f.rc else []


# ---------------------------------------------------------------------------
# Branches

def branch_info(w, addr):
    """Return a dict describing a branch, or None if `w` is not a branch."""
    op = w >> 26
    lk = w & 1
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        tgt = (li if w & 2 else addr + li) & 0xFFFFFFFF
        return dict(kind="b", target=tgt, lk=lk, bo=20, bi=0)
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        tgt = (bd if w & 2 else addr + bd) & 0xFFFFFFFF
        return dict(kind="b", target=tgt, lk=lk, bo=(w >> 21) & 31, bi=(w >> 16) & 31)
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return dict(kind="lr", target=None, lk=lk, bo=(w >> 21) & 31, bi=(w >> 16) & 31)
        if xo == 528:
            return dict(kind="ctr", target=None, lk=lk, bo=(w >> 21) & 31, bi=(w >> 16) & 31)
    return None


def is_unconditional(bo):
    return (bo & 0x14) == 0x14


def branch_cond(bo, bi):
    """C statements (side effects) and a condition expression for BO/BI."""
    pre = []
    conds = []
    if not bo & 4:
        pre.append(" c->ctr--;")
        conds.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
    if not bo & 16:
        conds.append("%scr_bit(c, %d)" % ("" if bo & 8 else "!", bi))
    return pre, " && ".join(conds) if conds else "1"


# ---------------------------------------------------------------------------
# Integer

def _arith31(f):
    d, a, b = R(f.rd), R(f.ra), R(f.rb)
    x = f.xo9
    if x not in (266, 40, 10, 8, 138, 136, 202, 200, 234, 232, 104, 235, 75, 11, 491, 459):
        return None
    if f.oe:
        raise Unsupported("OE=1 arithmetic")
    if x == 266:   # add
        return [" %s = %s + %s;" % (d, a, b)] + rc0(f, d)
    if x == 40:    # subf
        return [" %s = %s - %s;" % (d, b, a)] + rc0(f, d)
    if x == 10:    # addc
        return [" %s = add_ca(c, %s, %s, 0);" % (d, a, b)] + rc0(f, d)
    if x == 8:     # subfc
        return [" %s = add_ca(c, ~%s, %s, 1);" % (d, a, b)] + rc0(f, d)
    if x == 138:   # adde
        return [" %s = add_ca(c, %s, %s, c->xer_ca);" % (d, a, b)] + rc0(f, d)
    if x == 136:   # subfe
        return [" %s = add_ca(c, ~%s, %s, c->xer_ca);" % (d, a, b)] + rc0(f, d)
    if x == 202:   # addze
        return [" %s = add_ca(c, %s, 0, c->xer_ca);" % (d, a)] + rc0(f, d)
    if x == 200:   # subfze
        return [" %s = add_ca(c, ~%s, 0, c->xer_ca);" % (d, a)] + rc0(f, d)
    if x == 234:   # addme
        return [" %s = add_ca(c, %s, 0xFFFFFFFFu, c->xer_ca);" % (d, a)] + rc0(f, d)
    if x == 232:   # subfme
        return [" %s = add_ca(c, ~%s, 0xFFFFFFFFu, c->xer_ca);" % (d, a)] + rc0(f, d)
    if x == 104:   # neg
        return [" %s = 0u - %s;" % (d, a)] + rc0(f, d)
    if x == 235:   # mullw
        return [" %s = (uint32_t)((int32_t)%s * (int64_t)(int32_t)%s);" % (d, a, b)] + rc0(f, d)
    if x == 75:    # mulhw
        return [" %s = (uint32_t)(((int64_t)(int32_t)%s * (int32_t)%s) >> 32);" % (d, a, b)] + rc0(f, d)
    if x == 11:    # mulhwu
        return [" %s = (uint32_t)(((uint64_t)%s * %s) >> 32);" % (d, a, b)] + rc0(f, d)
    if x == 491:   # divw
        return [" %s = divw(%s, %s);" % (d, a, b)] + rc0(f, d)
    if x == 459:   # divwu
        return [" %s = divwu(%s, %s);" % (d, a, b)] + rc0(f, d)
    return None


LOGIC31 = {  # xo10: C expression template over s, b
    28: "{s} & {b}", 60: "{s} & ~{b}", 444: "{s} | {b}", 412: "{s} | ~{b}",
    316: "{s} ^ {b}", 124: "~({s} | {b})", 476: "~({s} & {b})", 284: "~({s} ^ {b})",
}

# xo10 -> (load fn, size, signed, update)  /  (store fn, update)
LOADX = {23: ("ld32", 0, 0), 55: ("ld32", 0, 1), 87: ("ld8", 0, 0), 119: ("ld8", 0, 1),
         279: ("ld16", 0, 0), 311: ("ld16", 0, 1), 343: ("ld16", 1, 0), 375: ("ld16", 1, 1)}
STOREX = {151: ("st32", 0), 183: ("st32", 1), 215: ("st8", 0), 247: ("st8", 1),
          407: ("st16", 0), 439: ("st16", 1)}
LOADD = {32: ("ld32", 0, 0), 33: ("ld32", 0, 1), 34: ("ld8", 0, 0), 35: ("ld8", 0, 1),
         40: ("ld16", 0, 0), 41: ("ld16", 0, 1), 42: ("ld16", 1, 0), 43: ("ld16", 1, 1)}
STORED = {36: ("st32", 0), 37: ("st32", 1), 38: ("st8", 0), 39: ("st8", 1),
          44: ("st16", 0), 45: ("st16", 1)}


def _load(f, fn, signed, update, ea):
    v = "%s(ea)" % fn
    if signed:
        v = "(uint32_t)(int32_t)(int16_t)" + v
    out = [" { uint32_t ea = %s;" % ea, "   %s = %s;" % (R(f.rd), v)]
    if update:
        out.append("   %s = ea;" % R(f.ra))
    return out + [" }"]


def _store(f, fn, update, ea):
    cast = {"st8": "(uint8_t)", "st16": "(uint16_t)", "st32": ""}[fn]
    out = [" { uint32_t ea = %s;" % ea, "   %s(ea, %s%s);" % (fn, cast, R(f.rs))]
    if update:
        out.append("   %s = ea;" % R(f.ra))
    return out + [" }"]


def _op31(f):
    x = f.xo10
    r = _arith31(f)
    if r is not None:
        return r
    s, a, b = R(f.rs), R(f.ra), R(f.rb)
    if x in LOGIC31:
        return [" %s = %s;" % (a, LOGIC31[x].format(s=s, b=b))] + rc0(f, a)
    if x == 0 or x == 32:  # cmp / cmpl
        if f.rd & 1:
            raise Unsupported("64-bit compare")
        fn = "cr_cmp_s" if x == 0 else "cr_cmp_u"
        cast = "(int32_t)" if x == 0 else ""
        return [" c->cr[%d] = %s(c, %s%s, %s%s);" % (f.crfd, fn, cast, a, cast, b)]
    if x == 24:   # slw
        return [" %s = (%s & 0x20) ? 0 : %s << (%s & 31);" % (a, b, s, b)] + rc0(f, a)
    if x == 536:  # srw
        return [" %s = (%s & 0x20) ? 0 : %s >> (%s & 31);" % (a, b, s, b)] + rc0(f, a)
    if x == 792:  # sraw
        return [" %s = sraw(c, %s, %s & 0x3F);" % (a, s, b)] + rc0(f, a)
    if x == 824:  # srawi
        return [" %s = sraw(c, %s, %d);" % (a, s, f.sh)] + rc0(f, a)
    if x == 26:   # cntlzw
        return [" %s = %s ? (uint32_t)__builtin_clz(%s) : 32u;" % (a, s, s)] + rc0(f, a)
    if x == 954:  # extsb
        return [" %s = (uint32_t)(int32_t)(int8_t)%s;" % (a, s)] + rc0(f, a)
    if x == 922:  # extsh
        return [" %s = (uint32_t)(int32_t)(int16_t)%s;" % (a, s)] + rc0(f, a)
    if x in LOADX:
        fn, sg, up = LOADX[x]
        return _load(f, fn, sg, up, "%s + %s" % (RA0(f.ra), b))
    if x in STOREX:
        fn, up = STOREX[x]
        return _store(f, fn, up, "%s + %s" % (RA0(f.ra), b))
    if x == 534:  # lwbrx
        return [" %s = __builtin_bswap32(ld32(%s + %s));" % (R(f.rd), RA0(f.ra), b)]
    if x == 662:  # stwbrx
        return [" st32(%s + %s, __builtin_bswap32(%s));" % (RA0(f.ra), b, s)]
    if x == 790:  # lhbrx
        return [" %s = __builtin_bswap16(ld16(%s + %s));" % (R(f.rd), RA0(f.ra), b)]
    if x == 918:  # sthbrx
        return [" st16(%s + %s, __builtin_bswap16((uint16_t)%s));" % (RA0(f.ra), b, s)]
    if x == 20:   # lwarx
        return [" { uint32_t ea = %s + %s; c->resv = 1; c->resv_addr = ea;" % (RA0(f.ra), b),
                "   c->resv_val = %s = ld32(ea); }" % R(f.rd)]
    if x == 150 and f.rc:  # stwcx.
        return [" { uint32_t ea = %s + %s; int ok = 0;" % (RA0(f.ra), b),
                "   if (c->resv && c->resv_addr == ea) {",
                "     uint32_t exp = __builtin_bswap32(c->resv_val), nv = __builtin_bswap32(%s);" % s,
                "     ok = __atomic_compare_exchange_n((uint32_t *)(g_mem + ea), &exp, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);",
                "   }",
                "   c->resv = 0; c->cr[0] = (ok ? 2 : 0) | c->xer_so; }"]
    if x == 339 or x == 467:  # mfspr / mtspr
        spr = ((f.w >> 16) & 31) | (((f.w >> 11) & 31) << 5)
        names = {8: "c->lr", 9: "c->ctr"}
        if spr == 1:
            if x == 339:
                return [" %s = ((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc;" % R(f.rd)]
            return [" c->xer_so = %s >> 31; c->xer_ov = (%s >> 30) & 1; c->xer_ca = (%s >> 29) & 1; c->xer_bc = %s & 0x7F;" % (s, s, s, s)]
        if spr not in names:
            raise Unsupported("spr %d" % spr)
        return [" %s = %s;" % (R(f.rd), names[spr])] if x == 339 else [" %s = %s;" % (names[spr], s)]
    if x == 371:  # mftb
        tbr = ((f.w >> 16) & 31) | (((f.w >> 11) & 31) << 5)
        sh = {268: 0, 269: 32}[tbr]
        return [" %s = (uint32_t)(ppc_timebase(c) >> %d);" % (R(f.rd), sh)]
    if x == 19:   # mfcr
        return [" %s = cr_get(c);" % R(f.rd)]
    if x == 144:  # mtcrf
        return [" cr_put(c, %s, 0x%X);" % (s, (f.w >> 12) & 0xFF)]
    if x == 597:  # lswi
        nb = f.rb or 32
        return [" { uint32_t ea = %s; int r = %d;" % (RA0(f.ra), f.rd),
                "   for (int i = 0; i < %d; i++) {" % nb,
                "     if ((i & 3) == 0) { c->r[r] = 0; }",
                "     c->r[r] |= (uint32_t)ld8(ea + i) << (24 - 8 * (i & 3));",
                "     if ((i & 3) == 3) r = (r + 1) & 31;",
                "   } }"]
    if x == 725:  # stswi
        nb = f.rb or 32
        return [" { uint32_t ea = %s; int r = %d;" % (RA0(f.ra), f.rs),
                "   for (int i = 0; i < %d; i++) {" % nb,
                "     st8(ea + i, (uint8_t)(c->r[r] >> (24 - 8 * (i & 3))));",
                "     if ((i & 3) == 3) r = (r + 1) & 31;",
                "   } }"]
    if x == 1014:  # dcbz
        return [" memset(g_mem + ((%s + %s) & ~31u), 0, 32);" % (RA0(f.ra), b)]
    if x in (54, 86, 278, 246, 982, 598, 854, 470):  # dcbst dcbf dcbt dcbtst icbi sync eieio dcbi
        return [" /* cache/sync: no-op */"]
    if x == 4:  # tw
        return [" if (%s) ppc_trap(c, ADDR);" % _trap_cond(f.rd, R(f.ra), R(f.rb))]
    # FP indexed loads/stores
    fx = {535: ("lfs", 0), 567: ("lfs", 1), 599: ("lfd", 0), 631: ("lfd", 1),
          663: ("stfs", 0), 695: ("stfs", 1), 727: ("stfd", 0), 759: ("stfd", 1), 983: ("stfiwx", 0)}
    if x in fx:
        kind, up = fx[x]
        return _fls(f, kind, up, "%s + %s" % (RA0(f.ra), b))
    raise Unsupported("op31 xo %d" % x)


def _trap_cond(to, a, b):
    parts = []
    if to & 16: parts.append("(int32_t)%s < (int32_t)%s" % (a, b))
    if to & 8: parts.append("(int32_t)%s > (int32_t)%s" % (a, b))
    if to & 4: parts.append("%s == %s" % (a, b))
    if to & 2: parts.append("%s < %s" % (a, b))
    if to & 1: parts.append("%s > %s" % (a, b))
    return " || ".join(parts) or "0"


# ---------------------------------------------------------------------------
# Floating point

def _fls(f, kind, update, ea):
    n = f.frd
    out = [" { uint32_t ea = %s;" % ea]
    if kind == "lfs":
        out.append("   c->f[%d] = c->ps1[%d] = to_double(ld32(ea));" % (n, n))
    elif kind == "lfd":
        out.append("   c->f[%d] = ld64(ea);" % n)
    elif kind == "stfs":
        out.append("   st32(ea, to_single(c->f[%d]));" % n)
    elif kind == "stfd":
        out.append("   st64(ea, c->f[%d]);" % n)
    elif kind == "stfiwx":
        out.append("   st32(ea, (uint32_t)c->f[%d]);" % n)
    if update:
        out.append("   %s = ea;" % R(f.ra))
    return out + [" }"]


def _single(dst, expr):
    return [" c->f[%d] = c->ps1[%d] = rnd_single(%s);" % (dst, dst, expr)]


def _op59(f):
    a, b, cc = FR(f.fra), FR(f.frb), FR(f.frc)
    x = f.xo5
    if f.rc:
        raise Unsupported("FP Rc=1")
    e = {18: "f_div(%s, %s)" % (a, b), 20: "f_sub(%s, %s)" % (a, b), 21: "f_add(%s, %s)" % (a, b),
         25: "f_mul(%s, %s)" % (a, cc), 28: "f_msub(%s, %s, %s)" % (a, cc, b),
         29: "f_madd(%s, %s, %s)" % (a, cc, b),
         30: "f_neg_unless_nan(f_msub(%s, %s, %s))" % (a, cc, b),
         31: "f_neg_unless_nan(f_madd(%s, %s, %s))" % (a, cc, b)}.get(x)
    if e is None:
        raise Unsupported("op59 xo %d" % x)
    if x in (30, 31):  # fnmsubs / fnmadds: round, then negate
        inner = ("f_msub" if x == 30 else "f_madd") + "(%s, %s, %s)" % (a, cc, b)
        return [" { uint64_t t = f_neg_unless_nan(rnd_single(%s));" % inner,
                "   c->f[%d] = c->ps1[%d] = t; }" % (f.frd, f.frd)]
    return _single(f.frd, e)


def _op63(f):
    d, a, b, cc = FR(f.frd), FR(f.fra), FR(f.frb), FR(f.frc)
    if f.rc:
        raise Unsupported("FP Rc=1")
    x5 = f.xo5
    if x5 >= 16:
        e = {18: "f_div(%s, %s)" % (a, b), 20: "f_sub(%s, %s)" % (a, b), 21: "f_add(%s, %s)" % (a, b),
             25: "f_mul(%s, %s)" % (a, cc), 28: "f_msub(%s, %s, %s)" % (a, cc, b),
             29: "f_madd(%s, %s, %s)" % (a, cc, b),
             30: "f_neg_unless_nan(f_msub(%s, %s, %s))" % (a, cc, b),
             31: "f_neg_unless_nan(f_madd(%s, %s, %s))" % (a, cc, b)}.get(x5)
        if x5 == 23:  # fsel
            return [" %s = (fd(%s) >= 0.0) ? %s : %s;" % (d, a, cc, b)]
        if e is None:
            raise Unsupported("op63 xo5 %d" % x5)
        return [" %s = %s;" % (d, e)]
    x = f.xo10
    if x == 0 or x == 32:  # fcmpu / fcmpo (exception differences are not modelled)
        return [" c->cr[%d] = fcmp(%s, %s);" % (f.crfd, a, b)]
    if x == 12:   # frsp
        return [" c->f[%d] = c->ps1[%d] = rnd_single(%s);" % (f.frd, f.frd, b)]
    if x == 15:
        return [" %s = fctiwz(%s);" % (d, b)]
    if x == 14:
        return [" %s = fctiw(%s);" % (d, b)]
    if x == 72:
        return [" %s = %s;" % (d, b)]
    if x == 40:
        return [" %s = %s ^ 0x8000000000000000ull;" % (d, b)]
    if x == 264:
        return [" %s = %s & 0x7FFFFFFFFFFFFFFFull;" % (d, b)]
    if x == 136:
        return [" %s = %s | 0x8000000000000000ull;" % (d, b)]
    raise Unsupported("op63 xo %d" % x)


def _op4(f):
    x = f.xo10
    d, a, b = f.frd, f.fra, f.frb
    merges = {528: ("f", "f"), 560: ("f", "ps1"), 592: ("ps1", "f"), 624: ("ps1", "ps1")}
    if x in merges and not f.rc:
        sa, sb = merges[x]
        return [" { uint64_t p0 = c->%s[%d], p1 = c->%s[%d];" % (sa, a, sb, b),
                "   c->f[%d] = p0; c->ps1[%d] = p1; }" % (d, d)]
    raise Unsupported("op4 xo %d" % x)


def _op19(f):
    x = f.xo10
    if x == 150:  # isync
        return [" /* isync */"]
    if x == 0:    # mcrf
        return [" c->cr[%d] = c->cr[%d];" % (f.crfd, f.crfs)]
    ops = {257: "a & b", 129: "a & !b", 289: "!(a ^ b)", 225: "!(a & b)",
           33: "!(a | b)", 449: "a | b", 417: "a | !b", 193: "a ^ b"}
    if x in ops:
        bt, ba, bb = f.rd, f.ra, f.rb
        return [" { int a = cr_bit(c, %d), b = cr_bit(c, %d); cr_setbit(c, %d, %s); }"
                % (ba, bb, bt, ops[x])]
    raise Unsupported("op19 xo %d" % x)


def emit(w, addr):
    """C statements for a non-branch instruction at `addr`."""
    return [l.replace("ADDR", hx(addr)) for l in _emit(w)]


def _emit(w):
    f = F(w)
    op = f.op
    d, a, s = R(f.rd), R(f.ra), R(f.rs)
    if op == 14:  # addi
        return [" %s = %s + %s;" % (d, RA0(f.ra), hx(f.simm))]
    if op == 15:  # addis
        return [" %s = %s + %s;" % (d, RA0(f.ra), hx(f.simm << 16))]
    if op == 24:
        return [" %s = %s | %s;" % (a, s, hx(f.uimm))]
    if op == 25:
        return [" %s = %s | %s;" % (a, s, hx(f.uimm << 16))]
    if op == 26:
        return [" %s = %s ^ %s;" % (a, s, hx(f.uimm))]
    if op == 27:
        return [" %s = %s ^ %s;" % (a, s, hx(f.uimm << 16))]
    if op == 28:
        return [" %s = %s & %s;" % (a, s, hx(f.uimm)), " RC0(%s);" % a]
    if op == 29:
        return [" %s = %s & %s;" % (a, s, hx(f.uimm << 16)), " RC0(%s);" % a]
    if op == 7:   # mulli
        return [" %s = (uint32_t)((int32_t)%s * (int64_t)%d);" % (d, a, f.simm)]
    if op == 8:   # subfic
        return [" %s = add_ca(c, ~%s, %s, 1);" % (d, a, hx(f.simm))]
    if op == 12 or op == 13:  # addic / addic.
        return [" %s = add_ca(c, %s, %s, 0);" % (d, a, hx(f.simm))] + ([" RC0(%s);" % d] if op == 13 else [])
    if op == 10 or op == 11:  # cmpli / cmpi
        if f.rd & 1:
            raise Unsupported("64-bit compare")
        if op == 11:
            return [" c->cr[%d] = cr_cmp_s(c, (int32_t)%s, %d);" % (f.crfd, a, f.simm)]
        return [" c->cr[%d] = cr_cmp_u(c, %s, %s);" % (f.crfd, a, hx(f.uimm))]
    if op in (20, 21, 23):  # rlwimi / rlwinm / rlwnm
        m = mask32(f.mb, f.me)
        rot = "rotl32(%s, %s)" % (s, ("%d" % f.sh) if op != 23 else R(f.rb) + " & 31")
        if op == 21 and f.sh == 0:
            rot = s
        if op == 20:
            out = [" %s = (%s & %s) | (%s & %s);" % (a, rot, hx(m), a, hx(~m))]
        else:
            out = [" %s = %s & %s;" % (a, rot, hx(m))]
        return out + rc0(f, a)
    if op in LOADD:
        fn, sg, up = LOADD[op]
        return _load(f, fn, sg, up, "%s + %s" % (RA0(f.ra), hx(f.simm)))
    if op in STORED:
        fn, up = STORED[op]
        return _store(f, fn, up, "%s + %s" % (RA0(f.ra), hx(f.simm)))
    if op == 46:  # lmw
        return [" { uint32_t ea = %s + %s;" % (RA0(f.ra), hx(f.simm)),
                "   for (int i = %d; i < 32; i++, ea += 4) c->r[i] = ld32(ea); }" % f.rd]
    if op == 47:  # stmw
        return [" { uint32_t ea = %s + %s;" % (RA0(f.ra), hx(f.simm)),
                "   for (int i = %d; i < 32; i++, ea += 4) st32(ea, c->r[i]); }" % f.rs]
    fl = {48: ("lfs", 0), 49: ("lfs", 1), 50: ("lfd", 0), 51: ("lfd", 1),
          52: ("stfs", 0), 53: ("stfs", 1), 54: ("stfd", 0), 55: ("stfd", 1)}
    if op in fl:
        kind, up = fl[op]
        return _fls(f, kind, up, "%s + %s" % (RA0(f.ra), hx(f.simm)))
    if op == 31:
        return _op31(f)
    if op == 59:
        return _op59(f)
    if op == 63:
        return _op63(f)
    if op == 4:
        return _op4(f)
    if op == 19:
        return _op19(f)
    if op == 3:   # twi
        return [" if (%s) ppc_trap(c, ADDR);" % _trap_cond(f.rd, a, hx(f.simm))]
    if op == 17:
        return [" ppc_syscall(c, ADDR);"]
    raise Unsupported("opcode %d" % op)
