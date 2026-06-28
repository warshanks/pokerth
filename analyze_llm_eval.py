#!/usr/bin/env python3
"""Summarise a PokerTH LLM eval decision log (JSONL).

Each line is one decision written by LlmPlayer::logDecision(). This reports the
model's action mix, how often its answer had to be coerced/clamped or fell back to
a safe move, request latency, and the hero's chip-stack trajectory over the run.

Usage:
    python3 analyze_llm_eval.py [path-to.jsonl]   # default: ~/pokerth_llm_eval.jsonl
"""

import collections
import json
import os
import statistics
import sys


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                print(f"  (skipped malformed line {n})", file=sys.stderr)
    return rows


def pct(part, whole):
    return f"{100.0 * part / whole:.1f}%" if whole else "n/a"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/pokerth_llm_eval.jsonl")
    if not os.path.exists(path):
        sys.exit(f"No such log file: {path}")

    rows = load(path)
    if not rows:
        sys.exit("Log is empty.")

    total = len(rows)
    models = sorted({r.get("model", "?") for r in rows})
    actions = collections.Counter(r.get("action", "?") for r in rows)
    statuses = collections.Counter(r.get("status", "?") for r in rows)
    latencies = [r["latency_ms"] for r in rows if isinstance(r.get("latency_ms"), (int, float))]

    # A decision is "clean" if the model's own answer was applied verbatim.
    # Anything else was coerced/clamped/degraded or a network/parse fallback.
    clean = sum(1 for r in rows if r.get("status") == "ok")
    http_err = sum(1 for r in rows if str(r.get("status", "")).startswith("http_error"))
    parse_fail = sum(1 for r in rows if r.get("status") in
                     ("bad_response_json", "empty_content", "unparseable_decision"))

    print(f"file:        {path}")
    print(f"model(s):    {', '.join(models)}")
    print(f"decisions:   {total}")
    print()

    print("action mix:")
    for a, c in actions.most_common():
        print(f"  {a:<8} {c:>6}  {pct(c, total)}")
    print()

    print("answer quality:")
    print(f"  clean (applied as-is)   {clean:>6}  {pct(clean, total)}")
    print(f"  coerced/clamped/degraded{total - clean - http_err - parse_fail:>6}  "
          f"{pct(total - clean - http_err - parse_fail, total)}")
    print(f"  network errors          {http_err:>6}  {pct(http_err, total)}")
    print(f"  parse fallbacks         {parse_fail:>6}  {pct(parse_fail, total)}")
    if len(statuses) > 1:
        print("  status breakdown:")
        for s, c in statuses.most_common():
            print(f"    {s:<28} {c:>6}")
    print()

    if latencies:
        print("latency (ms):")
        print(f"  min {min(latencies):.0f}  median {statistics.median(latencies):.0f}  "
              f"mean {statistics.mean(latencies):.0f}  max {max(latencies):.0f}")
        print()

    # Hero chip-stack trajectory (captured in every observation).
    stacks = []
    for r in rows:
        hero = (r.get("observation") or {}).get("hero") or {}
        s = hero.get("stack")
        if isinstance(s, (int, float)):
            stacks.append(s)
    if stacks:
        print("hero stack (chips, at decision time):")
        print(f"  start {stacks[0]}  end {stacks[-1]}  min {min(stacks)}  max {max(stacks)}  "
              f"net {stacks[-1] - stacks[0]:+d}")
        # crude trajectory sparkline over the run
        lo, hi = min(stacks), max(stacks)
        ramp = "▁▂▃▄▅▆▇█"
        if hi > lo:
            spark = "".join(ramp[min(len(ramp) - 1, int((s - lo) / (hi - lo) * (len(ramp) - 1)))]
                            for s in stacks[:: max(1, len(stacks) // 80)])
            print(f"  {spark}")


if __name__ == "__main__":
    main()
