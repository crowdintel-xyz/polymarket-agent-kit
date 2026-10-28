#!/usr/bin/env python3
"""Reject gross CPU-path latency regressions without pretending CI is a NIC test."""
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
checks = {
    "consumable pool lookup+copy": 10_000,
    "decision+pool+mock-submit": 20_000,
    # All P1-P3 layers attached: housekeeping + brakes + adverse selection.
    "decision+pool+layers+mock-submit": 25_000,
    "decision+inline-sign+mock-submit": 500_000,
    "sign only (Keccak+ECDSA)": 500_000,
    # P4 brain: closed-form conjugate update/read must stay in tens of ns.
    "bayes posterior update": 1_000,
    "bayes posterior read": 1_000,
}
failed = False
for label, budget_ns in checks.items():
    match = re.search(
        rf"^{re.escape(label)}.*?p50=\s*([0-9]+)\b", text, re.MULTILINE
    )
    if not match:
        print(f"FAIL missing metric: {label}")
        failed = True
        continue
    actual = int(match.group(1))
    status = "PASS" if actual <= budget_ns else "FAIL"
    print(f"{status} {label}: p50={actual} ns, gross-regression budget={budget_ns} ns")
    failed |= actual > budget_ns
print("Budgets are CPU-only smoke limits, not production latency SLOs.")
raise SystemExit(1 if failed else 0)
