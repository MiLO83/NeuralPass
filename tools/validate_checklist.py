#!/usr/bin/env python3
"""Validate evidence discipline in ROADMAP_CHECKLIST.txt."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CHECKLIST = ROOT / "ROADMAP_CHECKLIST.txt"
ITEM = re.compile(r"^\[(?P<status>[ x~!])\] (?P<title>.+)$")


def main() -> int:
    lines = CHECKLIST.read_text(encoding="utf-8").splitlines()
    failures: list[str] = []
    counts = {" ": 0, "~": 0, "x": 0, "!": 0}

    entries: list[tuple[int, str, str]] = []
    for index, line in enumerate(lines):
        match = ITEM.match(line)
        if match:
            status = match.group("status")
            counts[status] += 1
            entries.append((index, status, match.group("title")))

    for entry_index, (line_index, status, title) in enumerate(entries):
        next_index = entries[entry_index + 1][0] if entry_index + 1 < len(entries) else len(lines)
        body = lines[line_index + 1:next_index]
        if status == "x" and not any(line.startswith("    Evidence:") for line in body):
            failures.append(f"line {line_index + 1}: verified item lacks Evidence: {title}")
        if status == "!" and not any(line.startswith("    Blocker:") for line in body):
            failures.append(f"line {line_index + 1}: blocked item lacks Blocker: {title}")

    if failures:
        print("Checklist validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1

    print(
        "Checklist valid: "
        f"{counts['x']} verified, {counts['~']} in progress, "
        f"{counts[' ']} not started, {counts['!']} externally blocked."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
