#!/usr/bin/env python3
"""Restyle one game frame through a local ComfyUI SDXL-Lightning server."""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import random
import time
import urllib.parse
import urllib.request
import uuid
from pathlib import Path


DEFAULT_PROMPT = (
    "Santa's North Pole Workshop, magical Christmas toy factory, warm golden "
    "lights, candy-cane machinery, snow and frost, red and green holiday "
    "decorations, busy elves, cinematic video game environment, preserve the "
    "original scene geometry and camera composition, highly detailed"
)
DEFAULT_NEGATIVE = (
    "text, watermark, logo, blurry, low detail, distorted geometry, duplicate "
    "objects, deformed, flat lighting"
)


def request_json(url: str, data: bytes | None = None, content_type: str | None = None) -> dict:
    headers = {"Content-Type": content_type} if content_type else {}
    request = urllib.request.Request(url, data=data, headers=headers)
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


def upload_image(server: str, image_path: Path) -> str:
    boundary = f"----NeuralPass{uuid.uuid4().hex}"
    mime = mimetypes.guess_type(image_path.name)[0] or "application/octet-stream"
    payload = bytearray()

    def field(name: str, value: str) -> None:
        payload.extend(f"--{boundary}\r\n".encode())
        payload.extend(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        payload.extend(value.encode())
        payload.extend(b"\r\n")

    payload.extend(f"--{boundary}\r\n".encode())
    payload.extend(
        f'Content-Disposition: form-data; name="image"; filename="{image_path.name}"\r\n'.encode()
    )
    payload.extend(f"Content-Type: {mime}\r\n\r\n".encode())
    payload.extend(image_path.read_bytes())
    payload.extend(b"\r\n")
    field("type", "input")
    field("overwrite", "true")
    payload.extend(f"--{boundary}--\r\n".encode())
    result = request_json(
        f"{server}/upload/image",
        bytes(payload),
        f"multipart/form-data; boundary={boundary}",
    )
    return result["name"]


def workflow(
    image_name: str,
    prompt: str,
    negative: str,
    seed: int,
    denoise: float,
    width: int,
    height: int,
) -> dict:
    return {
        "1": {
            "class_type": "CheckpointLoaderSimple",
            "inputs": {"ckpt_name": "sd_xl_base_1.0.safetensors"},
        },
        "2": {
            "class_type": "LoraLoaderModelOnly",
            "inputs": {
                "model": ["1", 0],
                "lora_name": "sdxl_lightning_4step_lora.safetensors",
                "strength_model": 1.0,
            },
        },
        "3": {"class_type": "LoadImage", "inputs": {"image": image_name}},
        "10": {
            "class_type": "ImageScale",
            "inputs": {
                "image": ["3", 0],
                "upscale_method": "lanczos",
                "width": width,
                "height": height,
                "crop": "disabled",
            },
        },
        "4": {
            "class_type": "VAEEncode",
            "inputs": {"pixels": ["10", 0], "vae": ["1", 2]},
        },
        "5": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": prompt, "clip": ["1", 1]},
        },
        "6": {
            "class_type": "CLIPTextEncode",
            "inputs": {"text": negative, "clip": ["1", 1]},
        },
        "7": {
            "class_type": "KSampler",
            "inputs": {
                "seed": seed,
                "steps": 4,
                "cfg": 1.0,
                "sampler_name": "euler",
                "scheduler": "sgm_uniform",
                "denoise": denoise,
                "model": ["2", 0],
                "positive": ["5", 0],
                "negative": ["6", 0],
                "latent_image": ["4", 0],
            },
        },
        "8": {
            "class_type": "VAEDecode",
            "inputs": {"samples": ["7", 0], "vae": ["1", 2]},
        },
        "9": {
            "class_type": "SaveImage",
            "inputs": {"filename_prefix": "NeuralPass/restyle", "images": ["8", 0]},
        },
    }


def run(args: argparse.Namespace) -> Path:
    server = args.server.rstrip("/")
    image_name = upload_image(server, args.input)
    graph = workflow(
        image_name,
        args.prompt,
        args.negative,
        args.seed,
        args.denoise,
        args.width,
        args.height,
    )
    queued = request_json(
        f"{server}/prompt",
        json.dumps({"prompt": graph, "client_id": uuid.uuid4().hex}).encode(),
        "application/json",
    )
    prompt_id = queued["prompt_id"]
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        history = request_json(f"{server}/history/{prompt_id}")
        if prompt_id in history:
            record = history[prompt_id]
            status = record.get("status", {})
            if status.get("status_str") == "error":
                raise RuntimeError(json.dumps(status, indent=2))
            images = record.get("outputs", {}).get("9", {}).get("images", [])
            if images:
                item = images[0]
                query = urllib.parse.urlencode(item)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(f"{server}/view?{query}", timeout=120) as response:
                    args.output.write_bytes(response.read())
                return args.output
        time.sleep(0.25)
    raise TimeoutError(f"ComfyUI did not finish within {args.timeout:.0f} seconds")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT)
    parser.add_argument("--negative", default=DEFAULT_NEGATIVE)
    parser.add_argument("--denoise", type=float, default=0.78)
    parser.add_argument("--seed", type=int, default=48201976)
    parser.add_argument("--width", type=int, default=768)
    parser.add_argument("--height", type=int, default=432)
    parser.add_argument("--server", default="http://127.0.0.1:8188")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    if not 0.0 <= args.denoise <= 1.0:
        parser.error("--denoise must be between 0 and 1")
    if args.width < 64 or args.height < 64 or args.width % 8 or args.height % 8:
        parser.error("--width and --height must be at least 64 and divisible by 8")
    args.input = args.input.resolve()
    args.output = args.output.resolve()
    return args


if __name__ == "__main__":
    started = time.perf_counter()
    options = parse_args()
    result = run(options)
    elapsed = time.perf_counter() - started
    print(f"Saved {result} in {elapsed:.2f}s")
