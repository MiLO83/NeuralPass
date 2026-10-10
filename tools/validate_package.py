#!/usr/bin/env python3
"""Validate a NeuralPass assembled package without executing it."""

from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
from datetime import datetime, timezone
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
    "Update NeuralPass.cmd",
    "Diagnose NeuralPass.cmd",
    "Install-NeuralPass.ps1",
    "Uninstall-NeuralPass.ps1",
    "Update-NeuralPass.ps1",
    "Diagnose-NeuralPass.ps1",
    "NeuralPassVulkanTest.exe",
    "Validate NeuralPass Vulkan.cmd",
    "Validate-NeuralPassVulkan.ps1",
    "PACKAGE.json",
    "SBOM.spdx.json",
    "SHA256SUMS.txt",
    "third-party/ReShade-LICENSE.txt",
    "third-party/Vulkan-Headers-LICENSE.txt",
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


def validate_provenance(metadata: dict) -> None:
    revision = metadata.get("source_revision")
    if not isinstance(revision, str) or re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise SystemExit("PACKAGE.json has an invalid source revision")
    epoch = metadata.get("source_date_epoch")
    if not isinstance(epoch, int) or isinstance(epoch, bool) or epoch < 0:
        raise SystemExit("PACKAGE.json has an invalid source epoch")
    if not isinstance(metadata.get("source_dirty"), bool):
        raise SystemExit("PACKAGE.json has an invalid source dirty flag")
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if metadata.get("created_utc") != created:
        raise SystemExit("PACKAGE.json timestamp does not match its source epoch")


def validate_sbom(root: Path, metadata: dict, mode: str) -> None:
    document = json.loads((root / "SBOM.spdx.json").read_text(encoding="utf-8"))
    if document.get("spdxVersion") != "SPDX-2.3" or document.get("dataLicense") != "CC0-1.0":
        raise SystemExit("SBOM is not an SPDX 2.3 JSON document")
    expected_namespace = (
        "https://github.com/MiLO83/NeuralPass/sbom/"
        f"{metadata['source_revision']}/{metadata['architecture']}/{mode}"
    )
    if document.get("documentNamespace") != expected_namespace:
        raise SystemExit("SBOM namespace does not match package provenance")
    if document.get("creationInfo", {}).get("created") != metadata.get("created_utc"):
        raise SystemExit("SBOM timestamp does not match package provenance")
    packages = {item.get("name"): item for item in document.get("packages", [])}
    required_packages = {"NeuralPass", "ReShade", "Vulkan-Headers"}
    if mode == "directml":
        required_packages.update({"ONNX Runtime", "DirectML"})
        manifest = json.loads((root / "models" / "manifest.json").read_text(encoding="utf-8"))
        for model in manifest.get("models", []):
            if (root / "models" / "downloads" / str(model["filename"])).is_file():
                required_packages.add(f"ONNX Model Zoo fast-neural-style {model['id']}")
    absent_packages = sorted(required_packages - packages.keys())
    if absent_packages:
        raise SystemExit("SBOM is missing packages: " + ", ".join(absent_packages))
    expected_files = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file() and path.name not in {"SBOM.spdx.json", "SHA256SUMS.txt"}
    }
    spdx_files: dict[str, str] = {}
    sha1_values: list[str] = []
    for item in document.get("files", []):
        relative = str(item.get("fileName", "")).removeprefix("./")
        checksums = {
            checksum.get("algorithm"): checksum.get("checksumValue")
            for checksum in item.get("checksums", [])
        }
        if not relative or relative.startswith("/") or ".." in Path(relative).parts:
            raise SystemExit(f"unsafe SBOM path: {relative}")
        if relative in spdx_files or "SHA256" not in checksums:
            raise SystemExit(f"invalid or duplicate SBOM file entry: {relative}")
        spdx_files[relative] = str(checksums["SHA256"])
    if set(spdx_files) != expected_files:
        raise SystemExit("SBOM does not exactly cover package payload files")
    for relative, checksum in spdx_files.items():
        path = root / relative
        if digest(path) != checksum:
            raise SystemExit(f"SBOM checksum mismatch: {relative}")
        sha1_values.append(hashlib.sha1(path.read_bytes()).hexdigest())
    neuralpass = packages.get("NeuralPass", {})
    verification = hashlib.sha1("".join(sorted(sha1_values)).encode("ascii")).hexdigest()
    actual_verification = neuralpass.get("packageVerificationCode", {}).get(
        "packageVerificationCodeValue")
    if actual_verification != verification:
        raise SystemExit("SBOM package verification code is invalid")


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
    validate_provenance(metadata)
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
    if pe_machine(root / "NeuralPassVulkanTest.exe") != expected_machine:
        raise SystemExit(f"Vulkan evidence runner PE architecture does not match {architecture}")
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
                   "runtime/onnxruntime.dll", "runtime/onnxruntime_providers_shared.dll",
                   "runtime/DirectML.dll", "runtime/NeuralPassWorker.exe",
                   "runtime/NeuralPassHardwareTest.exe", "Validate NeuralPass Hardware.cmd",
                   "Validate-NeuralPassHardware.ps1", "runtime/NeuralPassWorkerHealthTest.exe",
                   "Validate NeuralPass Worker.cmd", "Validate-NeuralPassWorker.ps1",
                   "Manage NeuralPass Models.cmd", "Manage-NeuralPassModels.ps1"}
        absent = sorted(name for name in runtime if not (root / name).is_file())
        if absent:
            raise SystemExit("DirectML package is missing: " + ", ".join(absent))
        unsafe_root_workers = sorted(
            name for name in ("NeuralPassWorker.exe", "NeuralPassHardwareTest.exe",
                              "NeuralPassWorkerHealthTest.exe")
            if (root / name).exists()
        )
        if unsafe_root_workers:
            raise SystemExit(
                "worker executables must be isolated below runtime/: " +
                ", ".join(unsafe_root_workers)
            )
        if pe_machine(root / "runtime" / "NeuralPassWorker.exe") != 0x8664:
            raise SystemExit("NeuralPassWorker.exe must be an x64 PE image")
        if pe_machine(root / "runtime" / "NeuralPassHardwareTest.exe") != 0x8664:
            raise SystemExit("NeuralPassHardwareTest.exe must be an x64 PE image")
        if pe_machine(root / "runtime" / "NeuralPassWorkerHealthTest.exe") != expected_machine:
            raise SystemExit("worker-health runner PE architecture does not match package")
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
    validate_sbom(root, metadata, mode)
    print(f"Package valid: {len(actual_files)} files, mode={mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
