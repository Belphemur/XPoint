#!/usr/bin/env python3
"""Generate a markdown changelog from conventional-commit titles.

Groups commits by type (feat/fix/chore/...), then orders entries within each
type by their module (the conventional-commit scope, e.g. `(x4pro)`, or the
first path component of a touched file when no scope is present). Designed to
feed the release notes for the crosspoint-x-reader fork.

Usage:
    scripts/generate_changelog.py                 # since last tag..HEAD
    scripts/generate_changelog.py v1.2.2..v1.2.3  # explicit range
    scripts/generate_changelog.py --base develop --head HEAD
    scripts/generate_changelog.py --out CHANGELOG.md

Only the first-parent subject lines are used; the parser is deliberately small
(stdlib only) so it runs on the CI runner and locally without extra deps.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

# Conventional-commit primary types, in the order they should appear.
TYPE_ORDER = [
    "feat",
    "fix",
    "perf",
    "refactor",
    "docs",
    "style",
    "test",
    "build",
    "ci",
    "chore",
    "release",
    "other",
]

TYPE_TITLE = {
    "feat": "Features",
    "fix": "Bug Fixes",
    "perf": "Performance",
    "refactor": "Refactors",
    "docs": "Documentation",
    "style": "Style",
    "test": "Tests",
    "build": "Build",
    "ci": "CI",
    "chore": "Chores",
    "release": "Release",
    "other": "Other",
}

# Pattern: type[(scope)][!]: description   (scope / breaking-mark optional)
SUBJECT_RE = re.compile(r"^(?P<type>[a-zA-Z]+)(?:\((?P<scope>[^)]*)\))?(?P<break>!)?:\s*(?P<desc>.+)$")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def resolve_range(base: str | None, head: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    if base:
        return f"{base}..{head}"
    # default: since the most recent tag
    try:
        last_tag = git("describe", "--tags", "--abbrev=0").strip()
        return f"{last_tag}..{head}"
    except subprocess.CalledProcessError:
        # no tags yet: compare against the first commit
        first = git("rev-list", "--max-parents=0", "HEAD").strip().splitlines()[0]
        return f"{first}..{head}"


def module_for(sha: str, scope: str | None) -> str:
    if scope:
        return scope
    # No conventional scope: infer the module from the first changed path.
    try:
        paths = git("diff-tree", "--no-commit-id", "--name-only", "-r", sha).splitlines()
    except subprocess.CalledProcessError:
        return "other"
    for p in paths:
        parts = p.split("/")
        # skip top-level meta files
        if len(parts) <= 1:
            continue
        cand = parts[0]
        if cand in ("lib", "src", "docs", "scripts", "freeink-sdk"):
            if len(parts) > 1 and cand in ("lib", "src"):
                return f"{cand}/{parts[1]}"
            return cand
    return "other"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("range", nargs="?", help="explicit git range, e.g. v1.2.2..v1.2.3")
    ap.add_argument("--base", help="base ref when no explicit range is given")
    ap.add_argument("--head", default="HEAD", help="head ref (default HEAD)")
    ap.add_argument("--out", help="write to this file instead of stdout")
    ap.add_argument("--pr-links", action="store_true", help="append (#N) PR references when present in subject")
    args = ap.parse_args()

    rng = resolve_range(args.base, args.head, args.range)
    try:
        log = git("log", "--pretty=format:%H%x1f%s", rng)
    except subprocess.CalledProcessError as e:
        print(f"git log failed for range '{rng}':\n{e.stderr}", file=sys.stderr)
        return 1

    # type -> module -> list[(desc, pr)]
    grouped: dict[str, dict[str, list[tuple[str, str]]]] = defaultdict(lambda: defaultdict(list))
    pr_re = re.compile(r"\(#(\d+)\)\s*$")

    for line in log.splitlines():
        if not line.strip():
            continue
        sha, _, subject = line.partition("\x1f")
        m = SUBJECT_RE.match(subject.strip())
        if not m:
            ctype, scope, desc = "other", None, subject.strip()
        else:
            ctype = m.group("type").lower()
            scope = m.group("scope")
            desc = m.group("desc").strip()
        if ctype not in TYPE_ORDER:
            ctype = "other"
        pr = ""
        pm = pr_re.search(desc)
        if pm:
            pr = pm.group(1)
            desc = desc[: pm.start()].strip()
        module = module_for(sha, scope)
        grouped[ctype][module].append((desc, pr))

    if not grouped:
        print(f"(no commits found in range {rng})", file=sys.stderr)
        return 0

    lines: list[str] = []
    lines.append("## Changelog")
    lines.append("")
    lines.append(f"_Range: `{rng}`_")
    lines.append("")
    for ctype in TYPE_ORDER:
        if ctype not in grouped:
            continue
        lines.append(f"### {TYPE_TITLE[ctype]}")
        lines.append("")
        for module in sorted(grouped[ctype].keys()):
            entries = grouped[ctype][module]
            if len(grouped[ctype]) > 1:
                lines.append(f"- **{module}**")
                indent = "  - "
            else:
                indent = "- "
            for desc, pr in entries:
                suffix = f" (#{pr})" if (pr and args.pr_links) else ""
                lines.append(f"{indent}{desc}{suffix}")
        lines.append("")

    text = "\n".join(lines).rstrip() + "\n"
    if args.out:
        Path(args.out).write_text(text)
        print(f"Wrote changelog to {args.out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
