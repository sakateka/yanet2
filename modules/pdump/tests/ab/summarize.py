#!/usr/bin/env python3
"""Summarise modules/pdump/tests/ab/pdump-ab.sh's raw handler-matrix output.

Each line is one pdump_ab_bench launch, e.g.:
    AFTER  size=64 ring=1048576 filter=all reader=0 batch=8 ns/pkt
    min=35.67 med=35.87 max=38.63 rd_records=0 rd_resets=0

Launches of the same cell (size, ring, filter, reader, side) differ only in
run-to-run noise, so this takes the median of each launch's own median
ns/pkt, and reports that plus the [min..max] of the launch medians. AFTER's
two publish batches are columns of their own, each with its delta from
BEFORE.
"""
import collections as co
import re
import statistics as st
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <raw.txt>", file=sys.stderr)
        return 2

    by_cell = co.defaultdict(list)
    with open(sys.argv[1]) as f:
        for line in f:
            fields = dict(re.findall(r"(\w+)=(\S+)", line))
            if "size" not in fields:
                continue
            side = line.split()[0]
            if side != "BEFORE":
                side = f"AFTER-b{fields['batch']}"
            key = (
                int(fields["size"]),
                int(fields["ring"]),
                fields["filter"],
                int(fields["reader"]),
                side,
            )
            by_cell[key].append(float(fields["med"]))

    cells = sorted(
        {k[:4] for k in by_cell}, key=lambda k: (k[2], k[1], k[3], k[0])
    )

    def stats(key):
        vals = by_cell.get(key)
        if not vals:
            return None
        return st.median(vals), min(vals), max(vals)

    def render(cell):
        if cell is None:
            return " " * 20
        med, lo, hi = cell
        return f"{med:7.1f} [{lo:5.1f}..{hi:5.1f}]"

    def delta(cell, base):
        if cell is None or base is None:
            return ""
        return f"{(cell[0] / base[0] - 1) * 100:+6.1f}"

    header = (
        f"{'size':>5} {'ring':>4} {'filt':>4} {'rd':>2} | "
        f"{'BEFORE':>20} | {'AFTER b8':>20} {'d%':>6} | "
        f"{'AFTER b64':>20} {'d%':>6}"
    )
    print(header)
    for size, ring, filt, reader in cells:
        before = stats((size, ring, filt, reader, "BEFORE"))
        after8 = stats((size, ring, filt, reader, "AFTER-b8"))
        after64 = stats((size, ring, filt, reader, "AFTER-b64"))
        print(
            f"{size:>5} {ring >> 20:>3}M {filt:>4} {reader:>2} | "
            f"{render(before)} | {render(after8)} {delta(after8, before):>6} | "
            f"{render(after64)} {delta(after64, before):>6}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
