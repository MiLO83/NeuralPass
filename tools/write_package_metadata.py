#!/usr/bin/env python3
"""Write deterministic package provenance, SPDX SBOM, and SHA-256 checksums."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path


EXCLUDED_FROM_SBOM = {"SBOM.spdx.json", "SHA256SUMS.txt"}


def package_files(root: Path) -> list[Path]:
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS.txt"
    )


def digest(path: Path, algorithm: str = "sha256") -> str:
    value = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-c", f"safe.directory={root}", "-C", str(root), *args],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()


def source_provenance(repository: Path) -> tuple[str, int, bool]:
    revision = os.environ.get("GITHUB_SHA") or git(repository, "rev-parse", "HEAD")
    if len(revision) != 40 or any(ch not in "0123456789abcdefABCDEF" for ch in revision):
        raise SystemExit("could not determine a full source revision")
    epoch_text = os.environ.get("SOURCE_DATE_EPOCH")
    epoch = int(epoch_text) if epoch_text else int(
        git(repository, "show", "-s", "--format=%ct", revision)
    )
    dirty = bool(git(repository, "status", "--porcelain", "--untracked-files=no"))
    return revision.lower(), epoch, dirty


def spdx_id(prefix: str, value: str) -> str:
    safe = "".join(ch if ch.isalnum() or ch in ".-" else "-" for ch in value)
    return f"SPDXRef-{prefix}-{safe}"


def dependency_packages(mode: str, manifest: dict, root: Path) -> list[dict]:
    result = [{
        "SPDXID": "SPDXRef-Package-ReShade",
        "name": "ReShade",
        "versionInfo": "6.8.0",
        "downloadLocation": "https://github.com/crosire/reshade/tree/v6.8.0",
        "filesAnalyzed": False,
        "licenseConcluded": "BSD-3-Clause",
        "licenseDeclared": "BSD-3-Clause",
        "copyrightText": "Copyright 2014 Patrick Mours",
    }, {
        "SPDXID": "SPDXRef-Package-VulkanHeaders",
        "name": "Vulkan-Headers",
        "versionInfo": "vulkan-sdk-1.4.350.0",
        "downloadLocation": "https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.350.0",
        "filesAnalyzed": False,
        "licenseConcluded": "Apache-2.0 OR MIT",
        "licenseDeclared": "Apache-2.0 OR MIT",
        "copyrightText": "Copyright 2015-2023 The Khronos Group Inc.",
    }]
    if mode != "directml":
        return result
    result.extend([{
        "SPDXID": "SPDXRef-Package-ONNXRuntime",
        "name": "ONNX Runtime",
        "versionInfo": "1.24.4",
        "downloadLocation": "https://github.com/microsoft/onnxruntime",
        "filesAnalyzed": False,
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": "Copyright Microsoft Corporation",
    }, {
        "SPDXID": "SPDXRef-Package-DirectML",
        "name": "DirectML",
        "versionInfo": "1.15.4",
        "downloadLocation": "https://github.com/microsoft/DirectML",
        "filesAnalyzed": False,
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": "Copyright Microsoft Corporation",
    }])
    for model in manifest.get("models", []):
        model_id = str(model["id"])
        if not (root / "models" / "downloads" / str(model["filename"])).is_file():
            continue
        result.append({
            "SPDXID": spdx_id("Package-Model", model_id),
            "name": f"ONNX Model Zoo fast-neural-style {model_id}",
            "versionInfo": "opset-9",
            "downloadLocation": str(model["url"]),
            "checksums": [{"algorithm": "SHA256", "checksumValue": str(model["sha256"])}],
            "filesAnalyzed": False,
            "licenseConcluded": str(model["license"]),
            "licenseDeclared": str(model["license"]),
            "copyrightText": "NOASSERTION",
        })
    return result


def write_sbom(root: Path, repository: Path, metadata: dict, created: str) -> None:
    model_manifest = json.loads(
        (repository / "models" / "manifest.json").read_text(encoding="utf-8")
    )
    files = [path for path in package_files(root) if path.name not in EXCLUDED_FROM_SBOM]
    spdx_files = []
    sha1_values = []
    relationships = [{
        "spdxElementId": "SPDXRef-DOCUMENT",
        "relationshipType": "DESCRIBES",
        "relatedSpdxElement": "SPDXRef-Package-NeuralPass",
    }]
    for index, path in enumerate(files, 1):
        relative = path.relative_to(root).as_posix()
        sha1_values.append(digest(path, "sha1"))
        file_id = f"SPDXRef-File-{index}"
        spdx_files.append({
            "SPDXID": file_id,
            "fileName": f"./{relative}",
            "checksums": [{"algorithm": "SHA256", "checksumValue": digest(path)}],
            "licenseConcluded": "NOASSERTION",
            "copyrightText": "NOASSERTION",
        })
        relationships.append({
            "spdxElementId": "SPDXRef-Package-NeuralPass",
            "relationshipType": "CONTAINS",
            "relatedSpdxElement": file_id,
        })
    dependencies = dependency_packages(str(metadata["mode"]), model_manifest, root)
    for package in dependencies:
        relationships.append({
            "spdxElementId": "SPDXRef-Package-NeuralPass",
            "relationshipType": "DEPENDS_ON",
            "relatedSpdxElement": package["SPDXID"],
        })
    verification = hashlib.sha1("".join(sorted(sha1_values)).encode("ascii")).hexdigest()
    package = {
        "SPDXID": "SPDXRef-Package-NeuralPass",
        "name": "NeuralPass",
        "versionInfo": metadata["version"],
        "downloadLocation": "https://github.com/MiLO83/NeuralPass",
        "filesAnalyzed": True,
        "packageVerificationCode": {"packageVerificationCodeValue": verification},
        "licenseConcluded": "MIT",
        "licenseDeclared": "MIT",
        "copyrightText": "Copyright (c) 2026 MiLO",
        "externalRefs": [{
            "referenceCategory": "OTHER",
            "referenceType": "vcs",
            "referenceLocator": "git+https://github.com/MiLO83/NeuralPass@" +
                str(metadata["source_revision"]),
        }],
    }
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"NeuralPass-{metadata['version']}-{metadata['architecture']}",
        "documentNamespace": "https://github.com/MiLO83/NeuralPass/sbom/" +
            f"{metadata['source_revision']}/{metadata['architecture']}/{metadata['mode']}",
        "creationInfo": {
            "created": created,
            "creators": ["Organization: MiLO83", "Tool: NeuralPass write_package_metadata.py"],
        },
        "packages": [package, *dependencies],
        "files": spdx_files,
        "relationships": relationships,
    }
    (root / "SBOM.spdx.json").write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("preview", "directml"))
    parser.add_argument("--architecture", required=True, choices=("windows-x64", "windows-x86"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    root = args.package.resolve()
    repository = Path(__file__).resolve().parents[1]
    if not root.is_dir():
        raise SystemExit(f"package directory does not exist: {root}")

    revision, epoch, dirty = source_provenance(repository)
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    metadata = {
        "format": 1,
        "name": "NeuralPass",
        "version": args.version,
        "mode": args.mode,
        "architecture": args.architecture,
        "created_utc": created,
        "source_revision": revision,
        "source_date_epoch": epoch,
        "source_dirty": dirty,
        "signed": False,
        "support_tier": "research-preview",
    }
    (root / "PACKAGE.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_sbom(root, repository, metadata, created)
    lines = [
        f"{digest(path)} *{path.relative_to(root).as_posix()}"
        for path in package_files(root)
    ]
    (root / "SHA256SUMS.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote deterministic provenance, SPDX SBOM, and {len(lines)} checksums for {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
