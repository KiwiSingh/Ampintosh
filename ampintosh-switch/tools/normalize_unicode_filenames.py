#!/usr/bin/env python3
"""Normalize music filenames to NFC for Switch/libnx compatibility.

Usage:
  python3 tools/normalize_unicode_filenames.py /Volumes/SWITCH/ampintosh --dry-run
  python3 tools/normalize_unicode_filenames.py /Volumes/SWITCH/ampintosh

This is useful when files copied from macOS contain decomposed Japanese marks
such as キ + ゙ instead of the composed ギ. Some Switch filesystem layers can
fail to show those names even though macOS lists them fine.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import unicodedata


def unique_target(path: Path) -> Path:
    parent = path.parent
    stem = path.stem
    suffix = path.suffix
    candidate = path
    i = 1
    while candidate.exists() and candidate != path:
        candidate = parent / f"{stem} ({i}){suffix}"
        i += 1
    return candidate


def normalize_tree(root: Path, dry_run: bool) -> int:
    changes = 0
    # Bottom-up: rename children before parent dirs so paths remain valid.
    for dirpath, dirnames, filenames in os.walk(root, topdown=False):
        for name in filenames + dirnames:
            src = Path(dirpath) / name
            normalized = unicodedata.normalize("NFC", name)
            if normalized == name:
                continue
            dst = unique_target(Path(dirpath) / normalized)
            changes += 1
            print(f"{src} -> {dst}")
            if not dry_run:
                src.rename(dst)
    return changes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", help="Music root to normalize, e.g. /Volumes/SWITCH/ampintosh")
    parser.add_argument("--dry-run", action="store_true", help="Show changes without renaming")
    args = parser.parse_args()
    root = Path(args.root)
    if not root.exists() or not root.is_dir():
        raise SystemExit(f"Not a directory: {root}")
    changes = normalize_tree(root, args.dry_run)
    print(f"Done. {changes} name(s) {'would change' if args.dry_run else 'changed'}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
