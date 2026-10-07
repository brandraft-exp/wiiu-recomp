# AGENTS.md

Static recompiler for Wii U executables (RPX): PowerPC 750CL/Espresso machine code to C, plus a
standalone runtime that high-level emulates the Wii U OS.

## Hard rules
- **No game data in this repo.** No RPX/RPL files, assets, keys, decrypted output or generated C. Game
  data and build output live outside the repo (e.g. `../mh3u/`). `.gitignore` covers the obvious
  cases, but check `git status` before committing.
- **Correctness before speed.** Any change to instruction semantics in `recomp/insn.py` or
  `runtime/ppc.h` must pass the fuzzer. Don't weaken a comparison to make it pass; add a documented
  override in `compare()` only when the oracle is provably wrong or the ISA leaves the result undefined.
- Where Wii U OS behaviour is unclear, follow Cemu (`cemu-project/Cemu`, MPL-2.0) and cite the function.
- Commit as `Brandraft <338725959+Brandraft@users.noreply.github.com>` (already set in repo config).

## Layout
- `tools/rpxinfo.py` — RPX inspection (sections, symbols, imports, instruction stats).
- `recomp/insn.py` — decode one instruction and emit C. Branches are lowered by the caller.
- `recomp/program.py` — whole-program generation (functions, multi-entry helpers, branch tables,
  trampolines → imports, `table.c`, `imports.c`, `report.txt`).
- `runtime/ppc.h` — CPU context and helpers that the generated code targets.
- `runtime/standalone/` — loader, indirect calls, crash handler (`main.c`); OS HLE (`hle.c`).
- `tests/fuzz_insn.py` — differential fuzzing against Unicorn's 750CL model.

## Commands
```sh
python3 -m venv .venv && .venv/bin/pip install unicorn capstone ninja   # once
python3 tools/rpxinfo.py <rpx> [--imports]
.venv/bin/python tests/fuzz_insn.py <rpx> --per-form 8 --states 10       # quick, ~10 s
.venv/bin/python tests/fuzz_insn.py <rpx> --per-form 40 --states 150     # thorough, minutes
.venv/bin/python -m recomp.program <rpx> <out_dir>                      # ~2 min for 3.6M insns
```
Run the generator with `-m` from the repo root (it needs the package path; don't use `-I`).

## Gotchas
- Unicorn: the XER API ignores SO/OV/CA (use the `mtxer`/`mfxer` stub), translations are cached per
  address (one unique address per test, written up front), and `count=1` runs past the instruction
  (use the stop hook).
- RPX relocations are already applied for link addresses, so load at link addresses. `R_PPC_ADDR16_*`
  offsets point at the halfword, not the instruction. REL24 calls to imports branch to loader
  trampolines past `.text`, so resolve them through the relocation.
- RPX files from title updates are usually stripped of local symbols. Function boundaries must then
  come from relocations and analysis, not `.symtab`.
- Unimplemented imports log `STUB` and return 0. A crash shortly after a STUB usually means that
  import needs a real implementation.
