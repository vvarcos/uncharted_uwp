#!/usr/bin/env python3
"""Find loops whose iteration count comes from data instead of a constant.

Why: a GPU submission that hangs for many seconds while the timeline is healthy is
almost always a shader loop whose bound was read from a buffer. This script reads
SPIR-V assembly (spirv-dis output, ".spvasm") and reports every loop whose
back-edge condition compares against something that is not a constant.

Usage:
    python tools/scan_loops.py <dir-or-file> [...]

For a directory, every *.spvasm below it is scanned (spirv-dis is not needed).
The report is one line per loop:

    <file>:<line>  loop<header-id>  bound=<id> (<kind>)  source=<how bound is defined>

Exit code is 0 even when candidates are found; 1 only on a usage error.
"""

import os
import re
import sys

DEF = re.compile(r"^\s*(?:%(\S+)\s*=\s*)?(\S+)\b(.*)$")
CMP_OPS = (
    "OpULessThan",
    "OpSLessThan",
    "OpUGreaterThan",
    "OpSLessThanEqual",
    "OpULessThanEqual",
    "OpFOrdLessThan",
    "OpFOrdGreaterThan",
    "OpIEqual",
    "OpINotEqual",
)

# Bounds that are effectively constant for our purposes.
CONST_OPS = ("OpConstant", "OpConstantNull", "OpSpecConstant")


def parse(path):
    """Return (lines, defs) where defs maps result id -> (opcode, operands, line)."""
    lines = []
    defs = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for number, line in enumerate(handle, 1):
            lines.append(line)
            m = DEF.match(line)
            if not m or not m.group(1):
                continue
            defs[m.group(1)] = (m.group(2), m.group(3).split(), number)
    return lines, defs


def bound_kind(defs, ident, depth=0, seen=None):
    """Follow an id back to the first instruction that is not an arithmetic op."""
    if seen is None:
        seen = set()
    if depth > 12 or ident in seen:
        return "cycle"
    seen = seen | {ident}
    entry = defs.get(ident)
    if entry is None:
        return "undefined"
    opcode, operands, _ = entry
    if opcode in CONST_OPS:
        return "constant"
    if opcode == "OpLoad":
        return "load"
    if opcode in ("OpBitcast", "OpCopyObject", "OpSelect", "OpPhi", "OpIAdd",
                  "OpISub", "OpIMul", "OpUDiv", "OpSDiv", "OpShl", "OpShr",
                  "OpShiftRightLogical", "OpShiftLeftLogical", "OpConvertUToF"):
        for operand in operands[1:]:
            if operand.startswith("%"):
                kind = bound_kind(defs, operand[1:], depth + 1, seen)
                if kind not in ("undefined", "constant"):
                    return kind
        return "constant"
    return "other:" + opcode


def loops(path):
    lines, defs = parse(path)
    # Pair each OpLoopMerge with the label defined just above it.
    current = None
    headers = []
    for number, line in enumerate(lines, 1):
        m = re.match(r"^\s*%(\S+)\s*=\s*OpLabel\s*$", line)
        if m:
            current = m.group(1)
            continue
        if line.strip().startswith("OpLoopMerge"):
            headers.append((current, number))
    findings = []
    for header, merge_line in headers:
        # The guard is the first OpBranchConditional after the merge instruction. The
        # compiler wraps it in OpLogicalNot when it tests "not in range".
        condition = None
        for offset in range(merge_line, min(merge_line + 24, len(lines))):
            m = re.match(r"^\s*OpBranchConditional\s+%(\S+)\s", lines[offset])
            if m:
                condition = m.group(1)
                break
        for _ in range(4):
            if condition is None:
                break
            entry = defs.get(condition)
            if entry is None:
                break
            opcode, operands, _ = entry
            if opcode in ("OpLogicalNot", "OpLogicalAnd", "OpLogicalOr"):
                condition = operands[1][1:] if len(operands) > 1 and operands[1].startswith("%") else None
                continue
            break
        if condition is None:
            continue
        entry = defs.get(condition, ("?", [], 0))
        opcode, operands, _ = entry
        if opcode not in CMP_OPS:
            continue
        # %cond = OpULessThan %bool %a %b ; one of the two is the induction variable,
        # the other the bound. Report the one that is not constant.
        candidates = []
        for operand in operands[1:]:
            if not operand.startswith("%"):
                continue
            kind = bound_kind(defs, operand[1:])
            if kind not in ("constant", "undefined"):
                candidates.append((operand, kind))
        if candidates:
            ident, kind = candidates[-1]
            findings.append((merge_line, header, ident, kind))
    return findings


def iter_files(targets):
    for target in targets:
        if os.path.isdir(target):
            for root, _dirs, names in os.walk(target):
                for name in sorted(names):
                    if name.endswith(".spvasm"):
                        yield os.path.join(root, name)
        elif os.path.isfile(target):
            yield target


def main(argv):
    if not argv:
        print(__doc__.strip())
        return 1
    total = 0
    for path in iter_files(argv):
        try:
            found = loops(path)
        except OSError as error:
            print(f"{path}: {error}")
            continue
        for line, header, ident, kind in found:
            total += 1
            print(f"{path}:{line}  loop%{header}  bound={ident} ({kind})")
    print(f"\n{total} data-dependent loop(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
