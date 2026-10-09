#!/usr/bin/env python3
"""Write deterministic package metadata and SHA-256 checksums."""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path


def package_files(root: Path) -> list[Path]:
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS.txt"
    )


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("preview", "directml"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    root = args.package.resolve()
    if not root.is_dir():
        raise SystemExit(f"package directory does not exist: {root}")

    metadata = {
        "format": 1,
        "name": "NeuralPass",
        "version": args.version,
        "mode": args.mode,
        "architecture": "windows-x64",
        "created_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "signed": False,
        "support_tier": "research-preview",
    }
    (root / "PACKAGE.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        f"{digest(path)} *{path.relative_to(root).as_posix()}"
        for path in package_files(root)
    ]
    (root / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote metadata and {len(lines)} checksums for {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
