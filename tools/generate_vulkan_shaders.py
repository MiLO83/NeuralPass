#!/usr/bin/env python3
"""Regenerate NeuralPass's checked-in SPIR-V byte-array headers."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def write_header(binary: bytes, symbol: str, destination: pathlib.Path, source: pathlib.Path) -> None:
    lines = [
        "#pragma once",
        "",
        f"// Generated from {source.relative_to(ROOT).as_posix()} with glslangValidator -V.",
        f"inline constexpr unsigned char {symbol}[] = {{",
    ]
    for offset in range(0, len(binary), 12):
        chunk = ", ".join(f"0x{value:02x}" for value in binary[offset : offset + 12])
        lines.append(f"  {chunk},")
    lines.extend(
        [
            "};",
            f"inline constexpr unsigned int {symbol}_len = {len(binary)};",
            "",
        ]
    )
    destination.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def compile_shader(compiler: str, source: pathlib.Path, symbol: str,
                   destination: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="neuralpass-spirv-") as temporary:
        output = pathlib.Path(temporary) / "shader.spv"
        subprocess.run([compiler, "-V", str(source), "-o", str(output)], check=True)
        write_header(output.read_bytes(), symbol, destination, source)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", help="Path to glslangValidator or glslang")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("glslangValidator") or shutil.which("glslang")
    if compiler is None:
        parser.error("glslangValidator/glslang was not found; pass --compiler")
    compile_shader(
        compiler,
        ROOT / "addon/shaders/vulkan_capture.frag",
        "neuralpass_vulkan_capture_spv",
        ROOT / "addon/vulkan_capture_shader_spv.hpp",
    )
    compile_shader(
        compiler,
        ROOT / "addon/shaders/vulkan_capture_combined.frag",
        "neuralpass_vulkan_capture_combined_spv",
        ROOT / "addon/vulkan_capture_combined_spv.hpp",
    )
    compile_shader(
        compiler,
        ROOT / "addon/shaders/vulkan_capture_separate.frag",
        "neuralpass_vulkan_capture_separate_spv",
        ROOT / "addon/vulkan_capture_separate_spv.hpp",
    )
    compile_shader(
        compiler,
        ROOT / "tests/shaders/vulkan_instrument_test.vert",
        "neuralpass_vulkan_instrument_test_spv",
        ROOT / "tests/vulkan_instrument_test_spv.hpp",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
