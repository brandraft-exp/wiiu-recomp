"""rpxinfo: inspect a Wii U RPX/RPL executable.

Usage: rpxinfo.py <file.rpx> [--imports] [--json out.json]
"""
import argparse
import collections
import json
import struct
import zlib

SHF_EXECINSTR = 0x4
SHF_RPL_ZLIB = 0x08000000
SHT_NAMES = {0: "NULL", 1: "PROGBITS", 2: "SYMTAB", 3: "STRTAB", 4: "RELA",
             8: "NOBITS", 0x80000001: "RPL_EXPORTS", 0x80000002: "RPL_IMPORTS",
             0x80000003: "RPL_CRCS", 0x80000004: "RPL_FILEINFO"}


class Section:
    def __init__(self, idx, hdr, raw):
        (self.name_off, self.type, self.flags, self.addr, self.offset, self.size,
         self.link, self.info, self.addralign, self.entsize) = hdr
        self.idx = idx
        self.name = ""
        self.data = b""
        if self.type != 8 and self.size:
            blob = raw[self.offset:self.offset + self.size]
            if self.flags & SHF_RPL_ZLIB:
                self.data = zlib.decompress(blob[4:])
                assert len(self.data) == struct.unpack(">I", blob[:4])[0]
            else:
                self.data = blob

    @property
    def mem_size(self):
        return len(self.data) if self.type != 8 else self.size


class Rpx:
    def __init__(self, path):
        raw = open(path, "rb").read()
        assert raw[:4] == b"\x7fELF" and raw[4] == 1 and raw[5] == 2, "not ELF32 BE"
        assert raw[7] == 0xCA and raw[8] == 0xFE, "not a Wii U RPX/RPL"
        self.entry = struct.unpack(">I", raw[0x18:0x1C])[0]
        shoff = struct.unpack(">I", raw[0x20:0x24])[0]
        shentsize, shnum, shstrndx = struct.unpack(">HHH", raw[0x2E:0x34])
        self.sections = [
            Section(i, struct.unpack(">10I", raw[shoff + i * shentsize:shoff + i * shentsize + 40]), raw)
            for i in range(shnum)]
        strtab = self.sections[shstrndx].data
        for s in self.sections:
            s.name = strtab[s.name_off:strtab.index(b"\0", s.name_off)].decode()
        self.symbols = self._symbols()

    def _symbols(self):
        out = []
        for s in self.sections:
            if s.type != 2:
                continue
            strs = self.sections[s.link].data
            for o in range(0, len(s.data), 16):
                name, value, size, info, other, shndx = struct.unpack(">IIIBBH", s.data[o:o + 16])
                nm = strs[name:strs.index(b"\0", name)].decode("latin-1")
                out.append(dict(name=nm, value=value, size=size, type=info & 0xF,
                                bind=info >> 4, shndx=shndx))
        return out

    def imports(self):
        """{library: {"func": [...], "data": [...]}} from symbols in import sections."""
        res = collections.defaultdict(lambda: {"func": [], "data": []})
        for sym in self.symbols:
            if sym["shndx"] == 0 or sym["shndx"] >= len(self.sections):
                continue
            sec = self.sections[sym["shndx"]]
            if sec.type != 0x80000002 or not sym["name"]:
                continue
            kind = "func" if sec.name.startswith(".fimport_") else "data"
            res[sec.name.split("_", 1)[1]][kind].append(sym["name"])
        return {k: {kk: sorted(set(vv)) for kk, vv in v.items()} for k, v in sorted(res.items())}

    def code_sections(self):
        return [s for s in self.sections if s.flags & SHF_EXECINSTR and s.type == 1]


# --- instruction statistics -------------------------------------------------

def classify(insn):
    """Coarse category for a 32-bit PowerPC (750CL / Espresso) instruction."""
    op = insn >> 26
    if op == 4:
        xo10 = (insn >> 1) & 0x3FF
        xo5 = (insn >> 1) & 0x1F
        if xo10 == 1014:
            return "dcbz_l"
        if xo10 in (6, 7, 38, 39):
            return "paired_single_ldst_indexed"
        return "paired_single_arith" if xo5 >= 10 or xo10 in (0, 32, 64, 72, 136, 264, 528, 560, 592, 624) else "op4_other"
    if op in (56, 57, 60, 61):
        return "paired_single_ldst"
    if op in (59, 63):
        return "fpu_arith"
    if op in (48, 49, 50, 51, 52, 53, 54, 55):
        return "fpu_ldst"
    if op == 31:
        xo = (insn >> 1) & 0x3FF
        if xo in (339, 467):
            spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5)
            if 912 <= spr <= 919:
                return "gqr_access"
            return "spr_access"
        if xo in (535, 599, 663, 727, 567, 759, 631, 695):
            return "fpu_ldst"
        if xo in (20, 150):
            return "atomic_lwarx_stwcx"
        if xo in (598, 854, 982, 86, 54, 278, 246, 1014, 470):
            return "sync_cache"
        if xo in (83, 146, 210, 242, 595, 659, 4, 306, 370, 566):
            return "privileged_or_trap"
        return "integer"
    if op in (16, 18, 19):
        if op == 19 and ((insn >> 1) & 0x3FF) in (50, 150, 18):
            return "privileged_or_trap"
        return "branch"
    if op == 17:
        return "syscall"
    if op in (3, 2):
        return "privileged_or_trap"
    if op == 0:
        return "zero_or_invalid"
    return "integer"


def insn_stats(rpx):
    cats = collections.Counter()
    total = 0
    for s in rpx.code_sections():
        for o in range(0, len(s.data) - 3, 4):
            cats[classify(struct.unpack_from(">I", s.data, o)[0])] += 1
            total += 1
    return total, cats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpx")
    ap.add_argument("--imports", action="store_true", help="print every imported symbol")
    ap.add_argument("--json")
    a = ap.parse_args()
    r = Rpx(a.rpx)

    print("entry point: 0x%08x" % r.entry)
    print("\n%-3s %-28s %-13s %10s %10s %s" % ("#", "section", "type", "addr", "size", "flags"))
    for s in r.sections:
        print("%-3d %-28s %-13s 0x%08x %10d %s%s" % (
            s.idx, s.name[:28], SHT_NAMES.get(s.type, hex(s.type)), s.addr, s.mem_size,
            "X" if s.flags & SHF_EXECINSTR else "-", "z" if s.flags & SHF_RPL_ZLIB else "-"))

    funcs = [x for x in r.symbols if x["type"] == 2]
    local_funcs = [x for x in funcs if x["shndx"] < len(r.sections)
                   and r.sections[x["shndx"]].flags & SHF_EXECINSTR
                   and r.sections[x["shndx"]].type == 1]
    print("\nsymbols: %d total, %d functions, %d of them in the game's own code"
          % (len(r.symbols), len(funcs), len(local_funcs)))
    named = [x["name"] for x in local_funcs if x["name"]]
    print("sample local function names:", named[:15])

    imps = r.imports()
    nf = sum(len(v["func"]) for v in imps.values())
    nd = sum(len(v["data"]) for v in imps.values())
    print("\nimports: %d functions, %d data symbols from %d libraries" % (nf, nd, len(imps)))
    for lib, v in imps.items():
        print("  %-14s %4d funcs %3d data" % (lib, len(v["func"]), len(v["data"])))
        if a.imports:
            for f in v["func"] + v["data"]:
                print("      " + f)

    total, cats = insn_stats(r)
    print("\ncode: %d instructions (%.1f MB)" % (total, total * 4 / 1e6))
    for c, n in cats.most_common():
        print("  %-28s %9d  %5.2f%%" % (c, n, 100 * n / total))

    if a.json:
        json.dump(dict(entry=r.entry, imports=imps, insn_categories=dict(cats),
                       n_symbols=len(r.symbols), n_local_funcs=len(local_funcs)),
                  open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
