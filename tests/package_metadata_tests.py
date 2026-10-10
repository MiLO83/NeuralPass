#!/usr/bin/env python3
"""Regression checks for deterministic package provenance and SPDX output."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
WRITER = REPOSITORY / "tools" / "write_package_metadata.py"


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="neuralpass-metadata-") as directory:
        package = Path(directory)
        (package / "payload.bin").write_bytes(b"deterministic payload\n")
        command = [
            sys.executable, str(WRITER), "--package", str(package),
            "--mode", "preview", "--architecture", "windows-x64",
            "--version", "test",
        ]
        subprocess.run(command, check=True)
        generated = ("PACKAGE.json", "SBOM.spdx.json", "SHA256SUMS.txt")
        first = {name: file_digest(package / name) for name in generated}
        subprocess.run(command, check=True)
        second = {name: file_digest(package / name) for name in generated}
        if first != second:
            raise SystemExit("metadata output changed across identical runs")
        metadata = json.loads((package / "PACKAGE.json").read_text(encoding="utf-8"))
        sbom = json.loads((package / "SBOM.spdx.json").read_text(encoding="utf-8"))
        if sbom["creationInfo"]["created"] != metadata["created_utc"]:
            raise SystemExit("SBOM and package timestamps differ")
        names = {item["name"] for item in sbom["packages"]}
        if names != {"NeuralPass", "ReShade", "Vulkan-Headers"}:
            raise SystemExit(f"unexpected preview dependencies: {sorted(names)}")
    print("package metadata determinism tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
