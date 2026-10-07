"""Whole-program recompilation of an RPX into C.

Usage: python -m recomp.program <file.rpx> <out_dir> [--per-file N]

Output (all game-derived; never commit it):
  funcs.h          prototypes
  funcs_NNN.c      recompiled functions
  table.c          guest address -> host function table
  imports.c        import descriptors (library, symbol, stub address)
  report.txt       diagnostics (mid-function branches, unresolved targets, ...)
"""
import argparse
import bisect
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import rpxinfo  # noqa: E402

from recomp import insn  # noqa: E402

R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HA, R_PPC_REL24 = 1, 4, 6, 10


class Program:
    def __init__(self, path):
        self.rpx = rpxinfo.Rpx(path)
        secs = self.rpx.sections
        self.text = next(s for s in secs if s.name == ".text")
        self.lo, self.hi = self.text.addr, self.text.addr + len(self.text.data)
        self.syms = self.rpx.symbols
        self._imports()
        self._functions()
        self._relocs()
        self._entries()

    def _entries(self):
        """Direct call/jump targets that land inside another function (multi-entry helpers)."""
        self.entries = collections.defaultdict(set)  # function start -> extra entry addresses
        for start, size in self.funcs:
            for a in range(start, start + size, 4):
                b = insn.branch_info(self.word(a), a)
                if not b or b["kind"] != "b":
                    continue
                tgt = self.rel24.get(a, b["target"])
                if start <= tgt < start + size and not b["lk"]:
                    continue
                f = self.func_containing(tgt)
                if f and tgt != f[0]:
                    self.entries[f[0]].add(tgt)
        self.entry_set = {e for es in self.entries.values() for e in es}

    def callable(self, addr):
        return addr in self.func_set or addr in self.entry_set

    def word(self, addr):
        return struct.unpack_from(">I", self.text.data, addr - self.lo)[0]

    def _imports(self):
        """Map import-section addresses to (library, name, kind)."""
        self.imports = {}
        for s in self.syms:
            if s["shndx"] >= len(self.rpx.sections) or not s["name"]:
                continue
            sec = self.rpx.sections[s["shndx"]]
            if sec.type != 0x80000002:
                continue
            kind = "func" if sec.name.startswith(".fimport_") else "data"
            lib = sec.name.split("_", 1)[1]
            if lib.endswith(".rpl"):
                lib = lib[:-4]
            self.imports[s["value"]] = (lib, s["name"], kind)
        self.import_ids = {a: i for i, a in enumerate(sorted(self.imports))}

    def _functions(self):
        sized = {}
        for s in self.syms:
            if s["type"] == 2 and s["shndx"] < len(self.rpx.sections) and \
                    self.rpx.sections[s["shndx"]] is self.text and s["size"]:
                sized[s["value"]] = max(sized.get(s["value"], 0), s["size"])
        starts = sorted(sized)
        funcs = []
        prev_end = self.lo
        for a in starts:
            if a > prev_end:  # uncovered gap: treat as its own function
                funcs.append((prev_end, a - prev_end))
            end = a + sized[a]
            funcs.append((a, sized[a]))
            prev_end = max(prev_end, end)
        if prev_end < self.hi:
            funcs.append((prev_end, self.hi - prev_end))
        self.funcs = funcs
        self.func_starts = [a for a, _ in funcs]
        self.func_set = set(self.func_starts)

    def func_containing(self, addr):
        i = bisect.bisect_right(self.func_starts, addr) - 1
        if i < 0:
            return None
        a, n = self.funcs[i]
        return (a, n) if addr < a + n else None

    def _relocs(self):
        """REL24 targets per instruction, and code addresses referenced from data."""
        secs = self.rpx.sections
        symtab = next(s for s in secs if s.type == 2)

        def target(info, addend):
            v = struct.unpack_from(">I", symtab.data, (info >> 8) * 16 + 4)[0]
            return (v + addend) & 0xFFFFFFFF

        self.rel24 = {}
        self.tramps = {}              # loader trampoline address -> import address
        self.data_code_refs = set()   # code addresses stored in data (vtables, jump tables, fn pointers)
        self.code_addr_refs = set()   # code addresses materialised in code via lis/addi
        for s in secs:
            if s.type != 4:  # RELA
                continue
            target_sec = secs[s.info]
            d = s.data
            for o in range(0, len(d), 12):
                off, info, addend = struct.unpack_from(">IIi", d, o)
                t = info & 0xFF
                tgt = target(info, addend)
                if target_sec is self.text and t == R_PPC_REL24:
                    self.rel24[off] = tgt
                    if tgt in self.imports:
                        w = self.word(off)
                        li = w & 0x03FFFFFC
                        li = li - 0x04000000 if li & 0x02000000 else li
                        self.tramps[(off + li) & 0xFFFFFFFF] = tgt
                elif target_sec is self.text and t == R_PPC_ADDR16_LO:
                    if self.lo <= tgt < self.hi:
                        self.code_addr_refs.add(tgt)
                elif t == R_PPC_ADDR32 and self.lo <= tgt < self.hi:
                    self.data_code_refs.add(tgt)
                elif t == R_PPC_ADDR32 and tgt in self.imports and target_sec.type == 1:
                    stored = struct.unpack_from(">I", target_sec.data, off - target_sec.addr)[0]
                    self.tramps[stored] = tgt


def fn_name(a):
    return "fn_%08X" % a


class FuncEmitter:
    def __init__(self, prog, start, size, report):
        self.p, self.start, self.size, self.report = prog, start, size, report
        self.end = start + size

    def labels(self):
        labs = set()
        for a in range(self.start, self.end, 4):
            b = insn.branch_info(self.p.word(a), a)
            if b and b["kind"] == "b" and a not in self.p.rel24 and self.start <= b["target"] < self.end:
                labs.add(b["target"])
        # In-function code addresses referenced from data or code: jump-table targets.
        jt = {t for t in (self.p.data_code_refs | self.p.code_addr_refs) if self.start < t < self.end}
        # Branch tables: `switch` lowered to bctr into a run of unconditional `b` instructions,
        # where only the table base is ever referenced. Every entry is a potential target.
        run = []
        for a in range(self.start, self.end + 4, 4):
            w = self.p.word(a) if a < self.end else 0
            if (w >> 26) == 18 and not (w & 3):
                run.append(a)
                continue
            if len(run) >= 3:
                jt.update(run)
            run = []
        self.jt = sorted(jt)
        labs.update(self.jt)
        return labs

    def call(self, tgt, ret):
        """C statements for a call (with link) to an absolute target."""
        p = self.p
        if tgt in p.imports:
            lib, name, _ = p.imports[tgt]
            return [" c->lr = 0x%Xu; ppc_import(c, %d); /* %s.%s */" % (ret, p.import_ids[tgt], lib, name)]
        if p.callable(tgt):
            return [" c->lr = 0x%Xu; %s(c);" % (ret, fn_name(tgt))]
        self.report["call to non-function"].append("%08x -> %08x" % (ret - 4, tgt))
        return [" c->lr = 0x%Xu; ppc_call_indirect(c, 0x%Xu);" % (ret, tgt)]

    def jump(self, tgt, frm):
        """C statements for an unconditional jump (no link) to an absolute target."""
        p = self.p
        if self.start <= tgt < self.end:
            return [" goto L_%08X;" % tgt]
        if tgt in p.imports:
            lib, name, _ = p.imports[tgt]
            return [" ppc_import(c, %d); return; /* tail: %s.%s */" % (p.import_ids[tgt], lib, name)]
        if p.callable(tgt):
            return [" %s(c); return;" % fn_name(tgt)]
        self.report["jump into other function"].append("%08x -> %08x" % (frm, tgt))
        return [" ppc_branch_mid(c, 0x%Xu, 0x%Xu); return;" % (tgt, frm)]

    def emit(self):
        p = self.p
        labs = self.labels()
        extra = sorted(p.entries.get(self.start, ()))
        labs.update(extra)
        if extra:
            body = "body_%08X" % self.start
            out = ["static void %s(ppc_ctx *c, uint32_t entry);" % body]
            for e in [self.start] + extra:
                out.append("void %s(ppc_ctx *c) { %s(c, 0x%Xu); }" % (fn_name(e), body, e))
            out.append("static void %s(ppc_ctx *c, uint32_t entry) {" % body)
            out.append(" switch (entry) {")
            out += ["  case 0x%Xu: goto L_%08X;" % (e, e) for e in extra]
            out.append(" }")
        else:
            out = ["void %s(ppc_ctx *c) {" % fn_name(self.start)]
        falls = True
        for a in range(self.start, self.end, 4):
            w = p.word(a)
            if a in labs:
                out.append("L_%08X:;" % a)
            b = insn.branch_info(w, a)
            if b is None:
                try:
                    out += insn.emit(w, a)
                except insn.Unsupported as e:
                    self.report["unsupported"].append("%08x %08x %s" % (a, w, e))
                    out.append(" ppc_trap(c, 0x%Xu); /* unsupported %08x */" % (a, w))
                falls = True
                continue
            out += self.branch(b, a)
            falls = not (is_uncond(b) and not b["lk"])
        if falls:
            nxt = self.end
            if nxt in p.func_set:
                out.append(" %s(c); return; /* falls through */" % fn_name(nxt))
            else:
                out.append(" ppc_branch_mid(c, 0x%Xu, 0x%Xu); return;" % (nxt, self.end - 4))
        out.append("}")
        return out

    def branch(self, b, a):
        ret = a + 4
        tgt = self.p.rel24.get(a, b["target"]) if b["kind"] == "b" else None
        pre, cond = insn.branch_cond(b["bo"], b["bi"])
        out = list(pre)
        if b["kind"] == "b":
            body = self.call(tgt, ret) if b["lk"] else self.jump(tgt, a)
        elif b["kind"] == "lr":
            if b["lk"]:
                body = [" { uint32_t t = c->lr & ~3u; c->lr = 0x%Xu; ppc_call_indirect(c, t); }" % ret]
            else:
                body = [" return;"]
        else:  # ctr
            if b["lk"]:
                body = [" c->lr = 0x%Xu; ppc_call_indirect(c, c->ctr & ~3u);" % ret]
            else:
                body = self.ctr_jump(a)
        if cond == "1":
            return out + body
        if len(body) == 1:
            return out + [" if (%s)%s" % (cond, body[0])]
        return out + [" if (%s) {" % cond] + body + [" }"]

    def ctr_jump(self, a):
        """bctr: a switch over this function's jump-table targets, else an indirect tail call."""
        out = []
        if self.jt:
            out.append(" switch (c->ctr & ~3u) {")
            for t in self.jt:
                out.append("  case 0x%Xu: goto L_%08X;" % (t, t))
            out.append(" }")
        out.append(" ppc_call_indirect(c, c->ctr & ~3u); return;")
        return out


def is_uncond(b):
    return insn.is_unconditional(b["bo"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpx")
    ap.add_argument("out")
    ap.add_argument("--per-file", type=int, default=1500)
    a = ap.parse_args()
    p = Program(a.rpx)
    os.makedirs(a.out, exist_ok=True)
    report = collections.defaultdict(list)

    with open(os.path.join(a.out, "funcs.h"), "w") as fo:
        fo.write('#pragma once\n#include "ppc.h"\nvoid ppc_import(ppc_ctx *c, int id);\n')
        for s in sorted(p.func_set | p.entry_set):
            fo.write("void %s(ppc_ctx *c);\n" % fn_name(s))

    nfiles = 0
    for i in range(0, len(p.funcs), a.per_file):
        with open(os.path.join(a.out, "funcs_%03d.c" % nfiles), "w") as fo:
            fo.write('#include "funcs.h"\n\n')
            for s, n in p.funcs[i:i + a.per_file]:
                fo.write("\n".join(FuncEmitter(p, s, n, report).emit()) + "\n\n")
        nfiles += 1

    with open(os.path.join(a.out, "table.c"), "w") as fo:
        fo.write('#include "funcs.h"\n#include <stddef.h>\n')
        fo.write("const struct { uint32_t addr; ppc_fn fn; } g_fn_table[] = {\n")
        allfns = sorted(p.func_set | p.entry_set)
        for s in allfns:
            fo.write(" {0x%Xu, %s},\n" % (s, fn_name(s)))
        fo.write("};\nconst size_t g_fn_count = %d;\n" % len(allfns))
        fo.write("const uint32_t g_text_lo = 0x%Xu, g_text_hi = 0x%Xu;\n" % (p.lo, p.hi))

    with open(os.path.join(a.out, "imports.c"), "w") as fo:
        fo.write("#include <stdint.h>\n#include <stddef.h>\n")
        fo.write("const struct { uint32_t addr; const char *lib, *name; int is_data; } g_imports[] = {\n")
        for addr in sorted(p.imports):
            lib, name, kind = p.imports[addr]
            fo.write(' {0x%Xu, "%s", "%s", %d},\n' % (addr, lib, name, kind == "data"))
        fo.write("};\nconst size_t g_import_count = %d;\n" % len(p.imports))
        fo.write("/* loader trampolines (pre-relocated pointers to imports) -> import index */\n")
        fo.write("const struct { uint32_t addr; int id; } g_tramps[] = {\n")
        for t in sorted(p.tramps):
            fo.write(" {0x%Xu, %d},\n" % (t, p.import_ids[p.tramps[t]]))
        fo.write("};\nconst size_t g_tramp_count = %d;\n" % len(p.tramps))

    with open(os.path.join(a.out, "report.txt"), "w") as fo:
        fo.write("functions: %d in %d files\n" % (len(p.funcs), nfiles))
        for k, v in sorted(report.items()):
            fo.write("\n%s: %d\n" % (k, len(v)))
            for line in v[:200]:
                fo.write("  %s\n" % line)
    print("functions: %d (+%d extra entry points), files: %d" % (len(p.funcs), len(p.entry_set), nfiles))
    for k, v in sorted(report.items()):
        print("  %s: %d" % (k, len(v)))


if __name__ == "__main__":
    main()
