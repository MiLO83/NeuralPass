#!/usr/bin/env python3
"""Persistent low-latency img2img benchmark for the NeuralPass live backend."""

from __future__ import annotations

import argparse
import statistics
import time
from pathlib import Path

import torch
from streamdiffusion import StreamDiffusionWrapper


DEFAULT_PROMPT = (
    "Santa's North Pole Workshop, the same scene redesigned as a magical "
    "Christmas toy factory, red velvet, green enamel, brass toy machinery, "
    "candy-cane accents, snow, frost, warm golden fairy lights, cinematic "
    "realistic video game graphics, preserve silhouettes and composition"
)


def parse_args() -> argparse.Namespace:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prompt", default=DEFAULT_PROMPT)
    parser.add_argument("--model", type=Path, default=project / "models/downloads/sd-turbo")
    parser.add_argument("--vae", type=Path, default=project / "models/downloads/taesd")
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=288)
    parser.add_argument("--t-index", type=int, default=32)
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--seed", type=int, default=48201976)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    started = time.perf_counter()
    stream = StreamDiffusionWrapper(
        model_id_or_path=str(args.model.resolve()),
        vae_id=str(args.vae.resolve()),
        t_index_list=[args.t_index],
        frame_buffer_size=1,
        width=args.width,
        height=args.height,
        warmup=4,
        acceleration="none",
        mode="img2img",
        use_denoising_batch=True,
        cfg_type="none",
        seed=args.seed,
    )
    stream.prepare(prompt=args.prompt, num_inference_steps=50, guidance_scale=1.0)
    image = stream.preprocess_image(str(args.input.resolve()))

    for _ in range(3):
        stream(image=image)
    torch.cuda.synchronize()

    durations = []
    result = None
    for _ in range(args.frames):
        frame_started = time.perf_counter()
        result = stream(image=image)
        torch.cuda.synchronize()
        durations.append(time.perf_counter() - frame_started)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.save(args.output)
    median = statistics.median(durations)
    print(f"model startup: {time.perf_counter() - started - sum(durations):.2f}s")
    print(f"median frame: {median * 1000:.1f}ms ({1 / median:.2f} FPS)")
    print(f"range: {min(durations) * 1000:.1f}-{max(durations) * 1000:.1f}ms")
    print(f"saved: {args.output.resolve()}")


if __name__ == "__main__":
    main()
