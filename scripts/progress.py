#!/usr/bin/env python3
"""Recompute the README progress bar from ROADMAP.md.

Each roadmap line is `- [x|~| ] text (w N)`. Done counts N, partial counts N/2.
The bar is written between the <!-- progress --> markers in README.md.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ITEM = re.compile(r"^- \[([ x~])\] .*\(w (\d+)\)\s*$")
START, END = "<!-- progress:start -->", "<!-- progress:end -->"


def main() -> int:
    done = total = 0.0
    counts = {"x": 0, "~": 0, " ": 0}
    for line in (ROOT / "ROADMAP.md").read_text(encoding="utf-8").splitlines():
        m = ITEM.match(line)
        if not m:
            continue
        mark, weight = m.group(1), int(m.group(2))
        counts[mark] += 1
        total += weight
        done += weight if mark == "x" else weight / 2 if mark == "~" else 0
    if total == 0:
        print("no roadmap items found", file=sys.stderr)
        return 1

    pct = round(100 * done / total)
    filled = round(pct / 5)
    bar = "█" * filled + "░" * (20 - filled)
    block = (
        f"{START}\n"
        f"```\n{bar}  {pct}%\n```\n"
        f"**{counts['x']} done, {counts['~']} in progress, {counts[' ']} to go** "
        f"across {sum(counts.values())} milestones, weighted by effort "
        f"(see [ROADMAP.md](ROADMAP.md)).\n"
        f"{END}"
    )

    readme = ROOT / "README.md"
    text = readme.read_text(encoding="utf-8")
    new = re.sub(re.escape(START) + ".*?" + re.escape(END), block, text, flags=re.S)
    if new == text and START not in text:
        print("README.md has no progress markers", file=sys.stderr)
        return 1
    readme.write_text(new, encoding="utf-8", newline="\n")
    print(f"progress: {pct}% ({done:g}/{total:g} weighted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
