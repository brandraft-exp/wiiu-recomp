"""Differential fuzzing: recompiled C vs Unicorn (QEMU PPC 750CL) per instruction.

Usage: fuzz_insn.py <file.rpx> [--per-form N] [--states M] [--seed S] [--only MNEMONIC_SUBSTR]

Samples real instruction encodings from the RPX's code, grouped by form
(opcode/xo/Rc), emits each as a C test function, and compares the full CPU
state and a memory window after one step against Unicorn.
"""
import argparse
import collections
import ctypes
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import capstone  # noqa: E402
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UcError, UC_ERR_FETCH_UNMAPPED, UC_HOOK_CODE, UC_HOOK_INTR  # noqa: E402
from unicorn import ppc_const as P  # noqa: E402

import rpxinfo  # noqa: E402
from recomp import insn  # noqa: E402

TEST_PC = 0x01000000  # test i lives at TEST_PC + 4*i (Unicorn caches translations per address)
CODE_SIZE = 0x100000
XER_STUB = 0x00F00000  # mtxer r0 ; mfxer r0 (Unicorn API reads/writes of XER ignore SO/OV/CA)
UNMAPPED_OK = None
DATA = 0x10000000
DATA_SIZE = 0x20000


class Ctx(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 32), ("f", ctypes.c_uint64 * 32),
                ("ps1", ctypes.c_uint64 * 32), ("cr", ctypes.c_uint8 * 8),
                ("lr", ctypes.c_uint32), ("ctr", ctypes.c_uint32),
                ("xer_so", ctypes.c_uint8), ("xer_ov", ctypes.c_uint8),
                ("xer_ca", ctypes.c_uint8), ("xer_bc", ctypes.c_uint8),
                ("fpscr", ctypes.c_uint32), ("resv_addr", ctypes.c_uint32),
                ("resv_val", ctypes.c_uint32), ("resv", ctypes.c_uint8),
                ("npc", ctypes.c_uint32), ("host", ctypes.c_void_p)]


def form_key(w):
    op = w >> 26
    if op == 31:
        return (op, (w >> 1) & 0x3FF, w & 1)
    if op in (59,):
        return (op, (w >> 1) & 0x1F, w & 1)
    if op == 63:
        xo = (w >> 1) & 0x3FF
        return (op, xo if xo & 0x10 == 0 or (w >> 1) & 0x1F < 16 else (w >> 1) & 0x1F, w & 1)
    if op in (16, 18):
        return (op, (w >> 21) & 0x1F if op == 16 else (w & 2), w & 1)
    if op == 19:
        return (op, (w >> 1) & 0x3FF, (w >> 21) & 0x1F, w & 1)
    if op in (20, 21, 23, 4):
        return (op, w & 1)
    return (op,)


# Forms the oracle can't check (paired singles, traps, syscalls, timebase).
def oracle_skip(w):
    op = w >> 26
    if op in (4, 3, 17):
        return "no oracle (paired single / trap / sc)"
    if op == 31 and ((w >> 1) & 0x3FF) in (4, 371):
        return "no oracle (trap / timebase)"
    return None


# ---------------------------------------------------------------------------
# Random state

SPECIAL_D = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e30, -1e-30, float("inf"), float("-inf"),
             2147483647.5, -2147483648.0, 2147483648.0, 3.4028234663852886e38, 1e-40, 5e-324]


def rand_fpr(rng):
    k = rng.random()
    if k < 0.45:  # single-representable
        while True:
            b = rng.getrandbits(32)
            if (b & 0x7F800000) != 0x7F800000:
                break
        if rng.random() < 0.7:  # keep magnitudes sane most of the time
            b = (b & 0x807FFFFF) | ((rng.randint(100, 150)) << 23)
        return struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", struct.pack(">I", b))[0]))[0]
    if k < 0.75:
        return struct.unpack(">Q", struct.pack(">d", rng.uniform(-1e6, 1e6)))[0]
    if k < 0.9:
        return struct.unpack(">Q", struct.pack(">d", rng.choice(SPECIAL_D)))[0]
    if k < 0.95:  # NaNs, quiet and signalling, with payloads
        return (rng.getrandbits(1) << 63) | (0x7FF << 52) | rng.choice(
            [1 << 51, (1 << 51) | rng.getrandbits(29) << 29, rng.getrandbits(51) | 1])
    return rng.getrandbits(64)


INT_SPECIALS = [0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x8000, 31, 32, 63, 64]


def rand_gpr(rng):
    k = rng.random()
    if k < 0.25:
        return rng.choice(INT_SPECIALS)
    if k < 0.5:
        return rng.getrandbits(8) if rng.random() < 0.5 else (-rng.getrandbits(8)) & 0xFFFFFFFF
    return rng.getrandbits(32)


def mem_access(w):
    """(kind, size, ea_fn) if the instruction touches memory, else None."""
    op = w >> 26
    f = insn.F(w)
    d_sizes = {32: 4, 33: 4, 34: 1, 35: 1, 36: 4, 37: 4, 38: 1, 39: 1, 40: 2, 41: 2, 42: 2, 43: 2,
               44: 2, 45: 2, 48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8}
    if op in d_sizes:
        return ("d", d_sizes[op])
    if op in (46, 47):
        return ("d", 4 * (32 - f.rd))
    if op == 31:
        x_sizes = {23: 4, 55: 4, 87: 1, 119: 1, 279: 2, 311: 2, 343: 2, 375: 2, 151: 4, 183: 4,
                   215: 1, 247: 1, 407: 2, 439: 2, 534: 4, 662: 4, 790: 2, 918: 2, 20: 4, 150: 4,
                   535: 4, 567: 4, 599: 8, 631: 8, 663: 4, 695: 4, 727: 8, 759: 8, 983: 4,
                   1014: 32, 54: 4, 86: 4, 278: 4, 246: 4, 982: 4}
        if f.xo10 in x_sizes:
            return ("x", x_sizes[f.xo10])
        if f.xo10 in (597, 725):
            return ("n", f.rb or 32)
    return None


def make_state(rng, w):
    st = dict(r=[rand_gpr(rng) for _ in range(32)], f=[rand_fpr(rng) for _ in range(32)],
              cr=rng.getrandbits(32), lr=rng.getrandbits(32) & ~3, ctr=rng.getrandbits(32),
              so=rng.getrandbits(1), ov=rng.getrandbits(1), ca=rng.getrandbits(1),
              mem=bytes(rng.getrandbits(8) for _ in range(DATA_SIZE)) if False else None)
    if rng.random() < 0.3:
        st["ctr"] = rng.choice([0, 1, 2])
    acc = mem_access(w)
    if acc:
        kind, size = acc
        f = insn.F(w)
        align = 4 if size >= 4 else size
        if size == 8 or (w >> 26) == 31 and f.xo10 == 1014:
            align = 8
        ea = DATA + 0x1000 + rng.randrange(0, DATA_SIZE - 0x2000 - size) // align * align
        if kind == "d":
            if f.ra:
                st["r"][f.ra] = (ea - f.simm) & 0xFFFFFFFF
        elif kind == "n":
            if f.ra:
                st["r"][f.ra] = ea
        else:
            if f.ra == 0:
                st["r"][f.rb] = ea
            elif f.ra == f.rb:
                ea &= ~(2 * align - 1)
                st["r"][f.ra] = ea // 2
            else:
                off = rng.randrange(0, 0x100) // align * align
                st["r"][f.rb] = off
                st["r"][f.ra] = ea - off
        st["ea"] = ea
    return st


# ---------------------------------------------------------------------------
# C side

C_PRELUDE = r'''
#include "ppc.h"
uint8_t *g_mem;
int g_flags;
void ppc_call_indirect(ppc_ctx *c, uint32_t t) { g_flags |= 1; }
void ppc_branch_mid(ppc_ctx *c, uint32_t t, uint32_t f) { g_flags |= 2; }
void ppc_syscall(ppc_ctx *c, uint32_t a) { g_flags |= 4; }
void ppc_trap(ppc_ctx *c, uint32_t a) { g_flags |= 8; }
uint64_t ppc_timebase(ppc_ctx *c) { return 0; }
'''


def emit_test(w, pc):
    """C body for a single-instruction test at `pc`; sets c->npc."""
    b = insn.branch_info(w, pc)
    if b is None:
        return insn.emit(w, pc) + [" c->npc = 0x%Xu;" % (pc + 4)]
    pre, cond = insn.branch_cond(b["bo"], b["bi"])
    out = list(pre)
    if b["kind"] == "b":
        tgt = "0x%Xu" % b["target"]
    elif b["kind"] == "lr":
        out.append(" uint32_t tgt = c->lr & ~3u;")
        tgt = "tgt"
    else:
        out.append(" uint32_t tgt = c->ctr & ~3u;")
        tgt = "tgt"
    out.append(" c->npc = (%s) ? %s : 0x%Xu;" % (cond, tgt, pc + 4))
    if b["lk"]:
        out.append(" c->lr = 0x%Xu;" % (pc + 4))
    return out


def build_lib(tests, workdir):
    src = os.path.join(workdir, "tests.c")
    with open(src, "w") as fo:
        fo.write(C_PRELUDE)
        for i, (w, body) in enumerate(tests):
            fo.write("void t_%d(ppc_ctx *c) { /* %08x */\n%s\n}\n" % (i, w, "\n".join(body)))
        fo.write("typedef void (*tfn)(ppc_ctx *);\ntfn g_tests[] = {%s};\n"
                 % ", ".join("t_%d" % i for i in range(len(tests))))
    lib = os.path.join(workdir, "tests.so")
    subprocess.run(["clang", "-O2", "-shared", "-fPIC", "-ffp-contract=off", "-w",
                    "-I", os.path.join(ROOT, "runtime"), src, "-o", lib, "-lm"], check=True)
    return ctypes.CDLL(lib)


# ---------------------------------------------------------------------------
# Unicorn side

def new_uc():
    mu = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, cpu=P.UC_CPU_PPC32_750CL_V2_0)
    mu.mem_map(TEST_PC, CODE_SIZE)
    mu.mem_map(DATA, DATA_SIZE)
    mu.mem_map(XER_STUB, 0x1000)
    mu.mem_write(XER_STUB, struct.pack(">II", 0x7C0103A6, 0x7C0102A6))

    # Stop before a second instruction runs (count=1 alone lets Unicorn translate
    # and fault on whatever follows the test instruction).
    def on_code(uc, address, size, state):
        if address != state["pc"]:
            uc.emu_stop()
    mu.stop_state = {"pc": 0}
    mu.hook_add(UC_HOOK_CODE, on_code, mu.stop_state)

    def on_intr(uc, intno, state):
        state["intr"] = (intno, uc.reg_read(P.UC_PPC_REG_PC), uc.reg_read(P.UC_PPC_REG_MSR))
    mu.hook_add(UC_HOOK_INTR, on_intr, mu.stop_state)
    return mu


def uc_run(mu, w, pc, st, mem):
    for i in range(32):
        mu.reg_write(P.UC_PPC_REG_0 + i, st["r"][i])
        mu.reg_write(P.UC_PPC_REG_FPR0 + i, st["f"][i])
    mu.reg_write(P.UC_PPC_REG_CR, st["cr"])
    mu.reg_write(P.UC_PPC_REG_LR, st["lr"])
    mu.reg_write(P.UC_PPC_REG_CTR, st["ctr"])
    mu.reg_write(P.UC_PPC_REG_0, (st["so"] << 31) | (st["ov"] << 30) | (st["ca"] << 29))
    mu.stop_state["pc"] = XER_STUB
    mu.emu_start(XER_STUB, XER_STUB + 4, count=1)
    mu.reg_write(P.UC_PPC_REG_0, st["r"][0])
    mu.reg_write(P.UC_PPC_REG_FPSCR, 0)
    mu.reg_write(P.UC_PPC_REG_MSR, mu.reg_read(P.UC_PPC_REG_MSR) | 0x2000)
    mu.ctl_remove_cache(pc, pc + 4)
    mu.mem_write(DATA, mem)
    mu.stop_state["pc"] = pc
    try:
        mu.emu_start(pc, 0xFFFFFFFF, count=2)
    except UcError as e:  # a taken branch to an unmapped target: PC is still valid
        if e.errno != UC_ERR_FETCH_UNMAPPED:
            raise
    regs = [mu.reg_read(P.UC_PPC_REG_0 + i) & 0xFFFFFFFF for i in range(32)]
    npc = mu.reg_read(P.UC_PPC_REG_PC) & 0xFFFFFFFF
    mu.stop_state["pc"] = XER_STUB + 4
    mu.emu_start(XER_STUB + 4, XER_STUB + 8, count=1)
    xer = mu.reg_read(P.UC_PPC_REG_0)
    return dict(r=regs,
                f=[mu.reg_read(P.UC_PPC_REG_FPR0 + i) for i in range(32)],
                cr=mu.reg_read(P.UC_PPC_REG_CR) & 0xFFFFFFFF, lr=mu.reg_read(P.UC_PPC_REG_LR) & 0xFFFFFFFF,
                ctr=mu.reg_read(P.UC_PPC_REG_CTR) & 0xFFFFFFFF, so=(xer >> 31) & 1, ov=(xer >> 30) & 1,
                ca=(xer >> 29) & 1, pc=npc,
                mem=bytes(mu.mem_read(DATA, DATA_SIZE)))


def c_run(lib, idx, st, mem, membuf):
    c = Ctx()
    for i in range(32):
        c.r[i] = st["r"][i]
        c.f[i] = st["f"][i]
    for i in range(8):
        c.cr[i] = (st["cr"] >> (28 - 4 * i)) & 0xF
    c.lr, c.ctr = st["lr"], st["ctr"]
    c.xer_so, c.xer_ov, c.xer_ca = st["so"], st["ov"], st["ca"]
    ctypes.memmove(membuf, mem, DATA_SIZE)
    lib.g_tests_fn[idx](ctypes.byref(c))
    cr = 0
    for i in range(8):
        cr |= c.cr[i] << (28 - 4 * i)
    return dict(r=list(c.r), f=list(c.f), cr=cr, lr=c.lr, ctr=c.ctr, so=c.xer_so, ov=c.xer_ov,
                ca=c.xer_ca, pc=c.npc, mem=bytes(membuf))


KNOWN_ORACLE_BUGS = collections.Counter()


def compare(w, a, b):
    diffs = []
    op, f = w >> 26, insn.F(w)
    fmask = 0xFFFFFFFFFFFFFFFF
    if op == 63 and f.xo10 in (14, 15):
        fmask = 0xFFFFFFFF  # fctiw[z]: upper word undefined
        # QEMU returns 0x80000000 on positive overflow; the ISA (and hardware) give 0x7FFFFFFF.
        if a["f"][f.frd] & fmask == 0x80000000 and b["f"][f.frd] & fmask == 0x7FFFFFFF:
            KNOWN_ORACLE_BUGS["fctiw positive overflow"] += 1
            a = dict(a, f=list(a["f"]))
            a["f"][f.frd] = b["f"][f.frd]
    if op == 31 and f.xo9 in (491, 459) and a["r"][f.rd] != b["r"][f.rd]:
        # Divide by zero / overflow is undefined in the ISA; we follow Cemu (and Dolphin).
        divisor, dividend = b["r"][f.rb], b["r"][f.ra]
        if divisor == 0 or (dividend == 0x80000000 and divisor == 0xFFFFFFFF):
            KNOWN_ORACLE_BUGS["divide undefined case (follows Cemu)"] += 1
            a = dict(a, r=list(a["r"]))
            a["r"][f.rd] = b["r"][f.rd]
            if f.rc:
                a["cr"] = (a["cr"] & 0x0FFFFFFF) | (b["cr"] & 0xF0000000)
    for i in range(32):
        if a["r"][i] != b["r"][i]:
            diffs.append("r%d uc=%08x c=%08x" % (i, a["r"][i], b["r"][i]))
        if (a["f"][i] ^ b["f"][i]) & fmask:
            diffs.append("f%d uc=%016x c=%016x" % (i, a["f"][i], b["f"][i]))
    for k in ("cr", "lr", "ctr", "so", "ov", "ca", "pc"):
        if a[k] != b[k]:
            diffs.append("%s uc=%x c=%x" % (k, a[k], b[k]))
    if a["mem"] != b["mem"]:
        i = next(i for i in range(DATA_SIZE) if a["mem"][i] != b["mem"][i])
        diffs.append("mem @%08x uc=%s c=%s" % (DATA + i, a["mem"][i:i + 8].hex(), b["mem"][i:i + 8].hex()))
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpx")
    ap.add_argument("--per-form", type=int, default=24)
    ap.add_argument("--states", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only")
    ap.add_argument("--show", type=int, default=3, help="failures to print per form")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)

    def dis(w):
        i = list(md.disasm(struct.pack(">I", w), TEST_PC))
        return ("%s %s" % (i[0].mnemonic, i[0].op_str)) if i else "?"

    r = rpxinfo.Rpx(a.rpx)
    words = set()
    for s in r.code_sections():
        words.update(struct.unpack(">%dI" % (len(s.data) // 4), s.data))
    forms = collections.defaultdict(list)
    for w in sorted(words):
        forms[form_key(w)].append(w)

    tests, meta, unsupported, skipped = [], [], collections.Counter(), collections.Counter()
    for key, ws in sorted(forms.items()):
        sample = ws if len(ws) <= a.per_form else rng.sample(ws, a.per_form)
        for w in sample:
            name = dis(w).split()[0] if dis(w) != "?" else "?"
            if a.only and a.only not in name:
                continue
            try:
                body = emit_test(w, TEST_PC + 4 * len(tests))
            except insn.Unsupported as e:
                unsupported["%s (%s)" % (name, e)] += 1
                continue
            why = oracle_skip(w)
            if why:
                skipped[name] += 1
                continue
            tests.append((w, body))
            meta.append((w, name))
    print("forms: %d, tests: %d, oracle-skipped: %s" % (len(forms), len(tests), dict(skipped)))
    if unsupported:
        print("UNSUPPORTED:", dict(unsupported))

    work = tempfile.mkdtemp(prefix="fuzz_insn_")
    lib = build_lib(tests, work)
    tfn = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx))
    lib.g_tests_fn = (tfn * len(tests)).in_dll(lib, "g_tests")
    membuf = ctypes.create_string_buffer(DATA_SIZE)
    ctypes.c_void_p.in_dll(lib, "g_mem").value = ctypes.addressof(membuf) - DATA

    mu = new_uc()
    # All test instructions are written up front: a branch test may make Unicorn
    # translate (and cache) code at another test's address before it runs.
    mu.mem_write(TEST_PC, b"".join(struct.pack(">I", w) for w, _ in meta))
    fails = collections.defaultdict(list)
    runs = 0
    for idx, (w, name) in enumerate(meta):
        for _ in range(a.states):
            st = make_state(rng, w)
            mem = rng.randbytes(DATA_SIZE) if runs % 8 == 0 or not runs else mem
            try:
                ua = uc_run(mu, w, TEST_PC + 4 * idx, st, mem)
            except UcError as e:
                fails[name].append((w, "unicorn error %s intr=%s prev=%08x" % (e, mu.stop_state.get("intr"), meta[idx - 1][0] if idx else 0)))
                break
            ca = c_run(lib, idx, st, mem, membuf)
            runs += 1
            d = compare(w, ua, ca)
            if d:
                fails[name].append((w, "; ".join(d[:4]) + " | in: " + inputs_str(w, st)))
    total_fail = sum(len(v) for v in fails.values())
    if KNOWN_ORACLE_BUGS:
        print("known oracle bugs overridden:", dict(KNOWN_ORACLE_BUGS))
    print("ran %d states over %d encodings: %d mismatches in %d mnemonics"
          % (runs, len(meta), total_fail, len(fails)))
    for name, lst in sorted(fails.items(), key=lambda x: -len(x[1])):
        print("  %-10s %5d  e.g. %s" % (name, len(lst), dis(lst[0][0])))
        for w, msg in lst[:a.show]:
            print("       %08x %-28s %s" % (w, dis(w), msg))
    return 1 if total_fail else 0


def inputs_str(w, st):
    f = insn.F(w)
    parts = []
    for n in sorted({f.ra, f.rb, f.rd}):
        parts.append("r%d=%08x" % (n, st["r"][n]))
    for n in sorted({f.fra, f.frb, f.frc, f.frd}):
        parts.append("f%d=%016x" % (n, st["f"][n]))
    parts.append("ca=%d so=%d" % (st["ca"], st["so"]))
    return " ".join(parts)


if __name__ == "__main__":
    sys.exit(main())
