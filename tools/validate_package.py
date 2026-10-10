#!/usr/bin/env python3
"""Validate a NeuralPass assembled package without executing it."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path


REQUIRED = {
    "reshade-shaders/Shaders/NeuralPass.fx",
    "README.md",
    "LICENSE.txt",
    "ARCHITECTURE.md",
    "COMPATIBILITY.md",
    "THIRD_PARTY_NOTICES.md",
    "Install NeuralPass.cmd",
    "Uninstall NeuralPass.cmd",
    "Diagnose NeuralPass.cmd",
    "Install-NeuralPass.ps1",
    "Uninstall-NeuralPass.ps1",
    "Diagnose-NeuralPass.ps1",
    "PACKAGE.json",
    "SHA256SUMS.txt",
}


def digest(path: Path) -> str:
    value = hashlib.sha256(path.read_bytes()).hexdigest()
    return value


def pe_machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise SystemExit(f"not a PE image: {path.name}")
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0":
        raise SystemExit(f"invalid PE header: {path.name}")
    return struct.unpack_from("<H", data, offset + 4)[0]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_package.py PACKAGE_DIRECTORY")
    root = Path(sys.argv[1]).resolve()
    missing = sorted(name for name in REQUIRED if not (root / name).is_file())
    if missing:
        raise SystemExit("missing package files: " + ", ".join(missing))
    metadata = json.loads((root / "PACKAGE.json").read_text(encoding="utf-8"))
    if metadata.get("format") != 1 or metadata.get("name") != "NeuralPass":
        raise SystemExit("invalid PACKAGE.json identity or format")
    architecture = metadata.get("architecture")
    addon = {"windows-x64": "NeuralPass.addon64",
             "windows-x86": "NeuralPass.addon32"}.get(architecture)
    if addon is None:
        raise SystemExit(f"unknown package architecture: {architecture}")
    if not (root / addon).is_file():
        raise SystemExit(f"package is missing architecture-matched add-on: {addon}")
    expected_machine = {"windows-x64": 0x8664, "windows-x86": 0x014C}[architecture]
    if pe_machine(root / addon) != expected_machine:
        raise SystemExit(f"add-on PE architecture does not match {architecture}")
    expected: dict[str, str] = {}
    for line in (root / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        checksum, marker = line.split(" ", 1)
        relative = marker.removeprefix("*")
        if relative.startswith("/") or ".." in Path(relative).parts:
            raise SystemExit(f"unsafe checksum path: {relative}")
        expected[relative] = checksum
    actual_files = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS.txt"
    }
    if set(expected) != actual_files:
        raise SystemExit("checksum manifest does not exactly cover package files")
    for relative, checksum in expected.items():
        if digest(root / relative) != checksum:
            raise SystemExit(f"checksum mismatch: {relative}")
    mode = metadata.get("mode")
    if mode == "directml":
        runtime = {"onnxruntime.dll", "onnxruntime_providers_shared.dll", "DirectML.dll",
                   "NeuralPassWorker.exe",
                   "NeuralPassHardwareTest.exe", "Validate NeuralPass Hardware.cmd",
                   "Validate-NeuralPassHardware.ps1"}
        absent = sorted(name for name in runtime if not (root / name).is_file())
        if absent:
            raise SystemExit("DirectML package is missing: " + ", ".join(absent))
        if pe_machine(root / "NeuralPassWorker.exe") != 0x8664:
            raise SystemExit("NeuralPassWorker.exe must be an x64 PE image")
        if not list((root / "models" / "downloads").glob("*.onnx")):
            raise SystemExit("DirectML package contains no ONNX model")
        notices = {
            "models/manifest.json",
            "third-party/ONNXRuntime-LICENSE.txt",
            "third-party/ONNXRuntime-NOTICES.txt",
            "third-party/DirectML-LICENSE.txt",
            "third-party/DirectML-NOTICES.txt",
        }
        missing_notices = sorted(name for name in notices if not (root / name).is_file())
        if missing_notices:
            raise SystemExit("DirectML package is missing notices: " + ", ".join(missing_notices))
    elif mode == "preview":
        stale = sorted(name for name in ("onnxruntime.dll", "DirectML.dll") if (root / name).exists())
        if stale:
            raise SystemExit("preview package contains stale DirectML files: " + ", ".join(stale))
    else:
        raise SystemExit(f"unknown package mode: {mode}")
    print(f"Package valid: {len(actual_files)} files, mode={mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
