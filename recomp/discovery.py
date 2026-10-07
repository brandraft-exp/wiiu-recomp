"""Conservative code partitioning for RPXs without local function symbols.

Candidates are callable addresses, not a claim about original source functions.
Conditional branches must remain in one body; removed candidates are retained as
entry wrappers. Unconditional transfers between bodies are emitted as tail calls.
"""
import bisect
from . import insn


def discover(p):
    candidates = {p.lo, p.rpx.entry} | p.data_code_refs | p.code_addr_refs
    branches = []
    for a in range(p.lo, p.hi, 4):
        w = p.word(a)
        b = insn.branch_info(w, a)
        if b and b['kind'] == 'b':
            t = p.rel24.get(a, b['target'])
            if p.lo <= t < p.hi:
                branches.append((a, t, b))
                if b['lk']:
                    candidates.add(t)
        # GHS leaf/non-leaf stack-frame prologues. A false positive is safe
        # provided branches spanning it are reconciled below.
        if w & 0xFFFF0000 == 0x94210000 and w & 0x8000:
            candidates.add(a)
    candidates = {a for a in candidates if p.lo <= a < p.hi and a % 4 == 0}
    starts = sorted(candidates)
    # A jump to an interior helper entry can span the entire executable. Never
    # merge that span: create another callable entry for a cross-region jump.
    # Generated tail calls preserve the guest LR across these transfers.
    for a, t, b in branches:
        if not b['lk'] and insn.is_unconditional(b['bo']):
            if bisect.bisect_right(starts, a) != bisect.bisect_right(starts, t):
                candidates.add(t)
    starts = sorted(candidates)
    # Difference array removes every boundary crossed by a conditional branch.
    # Also keep ordinary jumps local unless the destination is itself a known
    # entry. This accommodates cold blocks without merging genuine tail calls.
    delta = [0] * (len(starts) + 1)
    for a, t, b in branches:
        if b['lk']:
            continue
        if insn.is_unconditional(b['bo']) and t in candidates:
            continue
        lo, hi = sorted((a, t))
        i, j = bisect.bisect_right(starts, lo), bisect.bisect_right(starts, hi)
        delta[i] += 1
        delta[j] -= 1
    kept, depth = [], 0
    for i, a in enumerate(starts):
        depth += delta[i]
        if not depth:
            kept.append(a)
    funcs = [(a, b - a) for a, b in zip(kept, kept[1:] + [p.hi])]
    return funcs, candidates
