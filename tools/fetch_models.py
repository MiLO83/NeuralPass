#!/usr/bin/env python3
"""Download NeuralPass preset models and verify their immutable SHA-256 IDs."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "models" / "manifest.json"
DESTINATION = ROOT / "models" / "downloads"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("models", nargs="*", help="model IDs; default downloads all")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = {item["id"]: item for item in manifest["models"]}
    selected = args.models or list(entries)
    unknown = sorted(set(selected) - set(entries))
    if unknown:
        parser.error("unknown model(s): " + ", ".join(unknown))
    DESTINATION.mkdir(parents=True, exist_ok=True)
    for model_id in selected:
        item = entries[model_id]
        target = DESTINATION / item["filename"]
        if target.exists() and digest(target) == item["sha256"]:
            print(f"verified {model_id}: {target}")
            continue
        print(f"fetch {model_id}: {item['url']}")
        if args.dry_run:
            continue
        partial = target.with_suffix(target.suffix + ".partial")
        urllib.request.urlretrieve(item["url"], partial)
        actual = digest(partial)
        if actual != item["sha256"]:
            partial.unlink(missing_ok=True)
            raise RuntimeError(f"checksum mismatch for {model_id}: {actual}")
        partial.replace(target)
        print(f"verified {model_id}: {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
