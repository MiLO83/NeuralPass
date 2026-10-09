#!/usr/bin/env python3
"""Serve prompt-driven StreamDiffusion frames to the NeuralPass ReShade add-on."""

from __future__ import annotations

import argparse
import os
import statistics
import struct
import threading
import time
import warnings
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
import torch
from PIL import Image
from streamdiffusion import StreamDiffusionWrapper


INPUT_HEADER = struct.Struct("<4sQII")
OUTPUT_HEADER = struct.Struct("<4sQIII")
DEFAULT_PROMPT = (
    "Santa's North Pole Workshop, the same scene redesigned as a magical Christmas toy factory, "
    "red velvet, green enamel, brass toy machinery, candy-cane accents, snow, frost, warm golden "
    "fairy lights, cinematic realistic video game graphics, preserve silhouettes and composition"
)
DEFAULT_BRIDGE = Path(
    "/mnt/d/SteamLibrary/steamapps/common/3DMark Demo/dlc/steel-nomad-test/windows/bin/x64/"
    "NeuralPassBridge"
)


def parse_args() -> argparse.Namespace:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, default=DEFAULT_BRIDGE)
    parser.add_argument("--model", type=Path, default=project / "models/downloads/sd-turbo")
    parser.add_argument("--vae", type=Path, default=project / "models/downloads/taesd")
    parser.add_argument(
        "--rife-model",
        type=Path,
        default=project / "models/downloads/rife/rife49_ensemble_True_scale_1_sim.onnx",
    )
    parser.add_argument("--rife-factor", type=int, choices=(1, 2, 4), default=1)
    parser.add_argument("--temporal-strength", type=float, default=0.28)
    parser.add_argument("--morph-input-strength", type=float, default=0.45)
    parser.add_argument("--scene-cut-threshold", type=float, default=0.52)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=288)
    parser.add_argument("--t-index", type=int, default=32)
    parser.add_argument("--seed", type=int, default=48201976)
    return parser.parse_args()


def read_frame(path: Path) -> tuple[int, Image.Image]:
    data = path.read_bytes()
    if len(data) < INPUT_HEADER.size:
        raise ValueError(f"short bridge frame: {path}")
    magic, sequence, width, height = INPUT_HEADER.unpack_from(data)
    expected = INPUT_HEADER.size + width * height * 4
    if magic != b"NPF1" or width < 1 or height < 1 or len(data) != expected:
        raise ValueError(f"invalid bridge frame: {path}")
    image = Image.frombytes("RGBA", (width, height), data[INPUT_HEADER.size:]).convert("RGB")
    return sequence, image


def write_frames(bridge: Path, sequence: int, images: list[Image.Image]) -> None:
    converted = [image.convert("RGBA") for image in images]
    first = converted[0]
    payload = OUTPUT_HEADER.pack(b"NPF2", sequence, first.width, first.height, len(converted))
    payload += b"".join(image.tobytes() for image in converted)
    temp = bridge / f"output_{sequence}.tmp"
    target = bridge / f"output_{sequence}.rgba"
    temp.write_bytes(payload)
    os.replace(temp, target)


def write_live_frame(bridge: Path, sequence: int, image: Image.Image) -> None:
    image = image.convert("RGBA")
    payload = INPUT_HEADER.pack(b"NPL2", sequence, image.width, image.height) + image.tobytes()
    temp = bridge / "live_output.tmp"
    target = bridge / "live_output.rgba"
    temp.write_bytes(payload)
    os.replace(temp, target)


def read_live_frame(path: Path) -> tuple[int, Image.Image]:
    data = path.read_bytes()
    if len(data) < INPUT_HEADER.size:
        raise ValueError("short live bridge frame")
    magic, sequence, width, height = INPUT_HEADER.unpack_from(data)
    expected = INPUT_HEADER.size + width * height * 4
    if magic != b"NPL1" or width < 1 or height < 1 or len(data) != expected:
        raise ValueError("invalid live bridge frame")
    return sequence, Image.frombytes("RGBA", (width, height), data[INPUT_HEADER.size:]).convert("RGB")


def rife_array(image: Image.Image) -> np.ndarray:
    array = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    return np.transpose(array, (2, 0, 1))[None]


def rife_image(array: np.ndarray) -> Image.Image:
    array = np.clip(array[0].transpose(1, 2, 0) * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(array, "RGB")


def tween_frames(
    session: ort.InferenceSession | None,
    previous: Image.Image | None,
    current: Image.Image,
    factor: int,
) -> list[Image.Image]:
    if session is None or previous is None:
        return [current]
    first = rife_array(previous)
    second = rife_array(current)
    frames = []
    for timestep in (np.arange(1, factor, dtype=np.float32) / factor):
        output = session.run(
            None,
            {"img0": first, "img1": second, "timestep": np.array([timestep], dtype=np.float32)},
        )[0]
        frames.append(rife_image(output))
    frames.append(current)
    return frames


def scene_cut_score(previous: Image.Image | None, current: Image.Image) -> float:
    """Return a motion-tolerant 0..1 cut score from color histograms and luma change."""
    if previous is None:
        return 0.0
    size = (96, 54)
    first = np.asarray(previous.convert("RGB").resize(size, Image.Resampling.BILINEAR))
    second = np.asarray(current.convert("RGB").resize(size, Image.Resampling.BILINEAR))
    first_hsv = cv2.cvtColor(first, cv2.COLOR_RGB2HSV)
    second_hsv = cv2.cvtColor(second, cv2.COLOR_RGB2HSV)
    first_hist = cv2.calcHist([first_hsv], [0, 1], None, [32, 16], [0, 180, 0, 256])
    second_hist = cv2.calcHist([second_hsv], [0, 1], None, [32, 16], [0, 180, 0, 256])
    cv2.normalize(first_hist, first_hist, alpha=1.0, norm_type=cv2.NORM_L1)
    cv2.normalize(second_hist, second_hist, alpha=1.0, norm_type=cv2.NORM_L1)
    histogram_distance = float(
        cv2.compareHist(first_hist, second_hist, cv2.HISTCMP_BHATTACHARYYA)
    )
    first_luma = cv2.cvtColor(first, cv2.COLOR_RGB2GRAY).astype(np.float32)
    second_luma = cv2.cvtColor(second, cv2.COLOR_RGB2GRAY).astype(np.float32)
    luma_change = float(np.mean(np.abs(first_luma - second_luma)) / 255.0)
    return float(np.clip(histogram_distance * 0.72 + min(luma_change * 2.5, 1.0) * 0.28, 0.0, 1.0))


def is_scene_cut(previous: Image.Image | None, current: Image.Image, threshold: float) -> bool:
    return scene_cut_score(previous, current) >= np.clip(threshold, 0.20, 0.95)


def temporal_anchor(
    flow_estimator,
    previous_input: Image.Image | None,
    previous_output: Image.Image | None,
    current_input: Image.Image,
    current_output: Image.Image,
    strength: float,
) -> tuple[Image.Image, np.ndarray]:
    width, height = current_output.size
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    gray = np.asarray(current_input.convert("L").resize((width, height)), dtype=np.uint8)
    if previous_input is None or previous_output is None or strength <= 0:
        guide = np.stack(
            (x / max(width - 1, 1), y / max(height - 1, 1), gray / 255.0), axis=-1
        )
        return current_output, np.clip(guide * 255.0, 0, 255).astype(np.uint8)

    previous_gray = np.asarray(previous_input.convert("L").resize((width, height)), dtype=np.uint8)
    flow_to_previous = flow_estimator.calc(gray, previous_gray, None)
    map_x = x + flow_to_previous[..., 0]
    map_y = y + flow_to_previous[..., 1]

    previous_rgb = np.asarray(previous_output.convert("RGB"), dtype=np.float32)
    warped_output = cv2.remap(
        previous_rgb, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    warped_gray = cv2.remap(
        previous_gray, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    valid = (map_x >= 0) & (map_x <= width - 1) & (map_y >= 0) & (map_y <= height - 1)
    confidence = np.exp(-np.abs(gray.astype(np.float32) - warped_gray) / 24.0) * valid
    alpha = (np.clip(strength, 0.0, 0.35) * confidence)[..., None]
    current_rgb = np.asarray(current_output.convert("RGB"), dtype=np.float32)
    anchored = current_rgb * (1.0 - alpha) + warped_output * alpha

    guide = np.stack(
        (
            np.clip(map_x / max(width - 1, 1), 0.0, 1.0),
            np.clip(map_y / max(height - 1, 1), 0.0, 1.0),
            gray.astype(np.float32) / 255.0,
        ),
        axis=-1,
    )
    guide = np.clip(guide * 255.0, 0, 255).astype(np.uint8)
    return Image.fromarray(np.clip(anchored, 0, 255).astype(np.uint8), "RGB"), guide


def warp_style_to_current(
    flow_estimator,
    anchor_input: Image.Image,
    anchor_output: Image.Image,
    current_input: Image.Image,
) -> tuple[Image.Image, np.ndarray, np.ndarray, np.ndarray]:
    """Reproject style through local and world RGB8 UVW planes."""
    width, height = anchor_output.size
    current = current_input.convert("RGB").resize((width, height), Image.Resampling.BILINEAR)
    anchor = anchor_input.convert("RGB").resize((width, height), Image.Resampling.BILINEAR)
    current_gray = np.asarray(current.convert("L"), dtype=np.uint8)
    anchor_gray = np.asarray(anchor.convert("L"), dtype=np.uint8)
    # Backward flow gives each current pixel its coordinate in the styled anchor.
    flow = flow_estimator.calc(current_gray, anchor_gray, None)
    forward_flow = flow_estimator.calc(anchor_gray, current_gray, None)
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    map_x = x + flow[..., 0]
    map_y = y + flow[..., 1]

    # Estimate dominant screen motion (mostly camera/world motion). The dense
    # residual around it becomes the object-local motion plane.
    stride = 8
    sample_x = x[::stride, ::stride].reshape(-1)
    sample_y = y[::stride, ::stride].reshape(-1)
    source_points = np.stack((sample_x, sample_y), axis=-1)
    destination_points = source_points + flow[::stride, ::stride].reshape(-1, 2)
    world_matrix, _ = cv2.estimateAffinePartial2D(
        source_points, destination_points, method=cv2.RANSAC, ransacReprojThreshold=2.5
    )
    if world_matrix is None:
        world_matrix = np.array(
            [[1.0, 0.0, float(np.median(flow[..., 0]))],
             [0.0, 1.0, float(np.median(flow[..., 1]))]],
            dtype=np.float32,
        )
    world_x = world_matrix[0, 0] * x + world_matrix[0, 1] * y + world_matrix[0, 2]
    world_y = world_matrix[1, 0] * x + world_matrix[1, 1] * y + world_matrix[1, 2]
    world_flow_x = world_x - x
    world_flow_y = world_y - y
    residual = np.sqrt(
        np.square(flow[..., 0] - world_flow_x) + np.square(flow[..., 1] - world_flow_y)
    )
    local_motion = 1.0 - np.exp(-residual / 2.5)

    styled = np.asarray(anchor_output.convert("RGB"), dtype=np.float32)
    local_warped = cv2.remap(
        styled, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    world_warped = cv2.remap(
        styled, world_x, world_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    warped_anchor_gray = cv2.remap(
        anchor_gray, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    world_anchor_gray = cv2.remap(
        anchor_gray, world_x, world_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT101
    )
    sampled_forward_x = cv2.remap(
        forward_flow[..., 0], map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT
    )
    sampled_forward_y = cv2.remap(
        forward_flow[..., 1], map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT
    )
    local_valid = (map_x >= 0) & (map_x <= width - 1) & (map_y >= 0) & (map_y <= height - 1)
    world_valid = (
        (world_x >= 0) & (world_x <= width - 1) & (world_y >= 0) & (world_y <= height - 1)
    )
    local_photometric = np.exp(
        -np.abs(current_gray.astype(np.float32) - warped_anchor_gray.astype(np.float32)) / 28.0
    )
    world_photometric = np.exp(
        -np.abs(current_gray.astype(np.float32) - world_anchor_gray.astype(np.float32)) / 28.0
    )
    # A backward vector followed by the sampled forward vector should return to
    # the same pixel. Failure marks a disocclusion or unreliable correspondence.
    cycle_error = np.sqrt(
        np.square(flow[..., 0] + sampled_forward_x) +
        np.square(flow[..., 1] + sampled_forward_y)
    )
    consistency = np.exp(-cycle_error / 1.75)
    local_confidence = np.clip(local_photometric * consistency * local_valid, 0.0, 1.0)
    world_confidence = np.clip(
        world_photometric * np.exp(-residual / 3.0) * world_valid, 0.0, 1.0
    )

    # Static scene regions prefer the dominant world plane. Independently moving
    # regions prefer the dense local plane, while retaining a weak local fallback.
    local_weight = local_confidence * (0.15 + 0.85 * local_motion)
    world_weight = world_confidence * (1.0 - 0.70 * local_motion)
    weight_sum = np.maximum(local_weight + world_weight, 1e-5)[..., None]
    warped = (
        local_warped * local_weight[..., None] + world_warped * world_weight[..., None]
    ) / weight_sum
    confidence = np.clip(np.maximum(local_confidence, world_confidence), 0.0, 1.0)

    # Infill disocclusions from nearby persistent style first. Blend the live
    # source back only where no previous-frame coordinate can be trusted.
    occluded = (confidence < 0.16).astype(np.uint8) * 255
    warped_u8 = np.clip(warped, 0, 255).astype(np.uint8)
    filled = cv2.inpaint(warped_u8, occluded, 3.0, cv2.INPAINT_TELEA).astype(np.float32)
    source = np.asarray(current, dtype=np.float32)
    # Prefer persistent styled texels; the live framebuffer contributes only
    # enough structure to stabilize genuinely new/disoccluded regions.
    fallback = filled * 0.82 + source * 0.18
    alpha = np.clip(confidence * 1.20, 0.0, 1.0)[..., None]
    motion_frame = warped * alpha + fallback * (1.0 - alpha)
    # Each plane is an independent RGB8 map: exactly 3 bytes per screen pixel.
    # The third coordinate is currently a luminance-derived depth proxy and can
    # be replaced by reconstructed depth without changing the contract.
    local_uvw = np.stack(
        (
            np.clip(map_x / max(width - 1, 1), 0.0, 1.0),
            np.clip(map_y / max(height - 1, 1), 0.0, 1.0),
            warped_anchor_gray.astype(np.float32) / 255.0,
        ),
        axis=-1,
    )
    world_uvw = np.stack(
        (
            np.clip(world_x / max(width - 1, 1), 0.0, 1.0),
            np.clip(world_y / max(height - 1, 1), 0.0, 1.0),
            world_anchor_gray.astype(np.float32) / 255.0,
        ),
        axis=-1,
    )
    local_uvw = np.clip(local_uvw * 255.0, 0, 255).astype(np.uint8)
    world_uvw = np.clip(world_uvw * 255.0, 0, 255).astype(np.uint8)
    visibility = np.clip(confidence * 255.0, 0, 255).astype(np.uint8)
    return (
        Image.fromarray(np.clip(motion_frame, 0, 255).astype(np.uint8), "RGB"),
        local_uvw,
        world_uvw,
        visibility,
    )


class MotionAnchors:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.source: Image.Image | None = None
        self.styled: Image.Image | None = None
        self.epoch = 0

    def set(self, source: Image.Image, styled: Image.Image, expected_epoch: int | None = None) -> bool:
        with self.lock:
            if expected_epoch is not None and expected_epoch != self.epoch:
                return False
            self.source = source.copy()
            self.styled = styled.copy()
            return True

    def get(self) -> tuple[Image.Image | None, Image.Image | None, int]:
        with self.lock:
            return self.source, self.styled, self.epoch

    def clear(self) -> int:
        with self.lock:
            if self.source is not None or self.styled is not None:
                self.epoch += 1
            self.source = None
            self.styled = None
            return self.epoch


def motion_loop(
    bridge: Path, anchors: MotionAnchors, stop: threading.Event, cut_threshold: float
) -> None:
    flow = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_ULTRAFAST)
    path = bridge / "live_input.rgba"
    last_sequence = 0
    previous_live: Image.Image | None = None
    while not stop.is_set():
        try:
            sequence, current = read_live_frame(path)
        except (OSError, ValueError):
            time.sleep(0.001)
            continue
        if sequence <= last_sequence:
            time.sleep(0.001)
            continue
        if is_scene_cut(previous_live, current, cut_threshold):
            anchors.clear()
            # Alpha zero immediately reveals the new live framebuffer instead
            # of holding or morphing the previous scene while diffusion catches up.
            clear_rgba = np.dstack(
                (np.asarray(current.convert("RGB"), dtype=np.uint8),
                 np.zeros((current.height, current.width), dtype=np.uint8))
            )
            write_live_frame(bridge, sequence, Image.fromarray(clear_rgba, "RGBA"))
            previous_live = current
            last_sequence = sequence
            continue
        previous_live = current
        source, styled, _ = anchors.get()
        if source is not None and styled is not None:
            motion_frame, local_uvw, world_uvw, visibility = warp_style_to_current(
                flow, source, styled, current
            )
            rgba = np.dstack((np.asarray(motion_frame, dtype=np.uint8), visibility))
            # Both coordinate planes are 3 B/px. Alpha transports their combined
            # validity byte to ReShade; the planes remain available to the morph pass.
            write_live_frame(bridge, sequence, Image.fromarray(rgba, "RGBA"))
        last_sequence = sequence


def load_prompt(path: Path) -> str:
    if not path.exists():
        path.write_text(DEFAULT_PROMPT, encoding="utf-8")
    return path.read_text(encoding="utf-8").strip() or DEFAULT_PROMPT


def next_input(bridge: Path) -> Path | None:
    candidates = list(bridge.glob("input_*.rgba"))
    if not candidates:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def main() -> None:
    args = parse_args()
    warnings.filterwarnings("ignore", message="Passing `image` as torch tensor.*", category=FutureWarning)
    args.bridge.mkdir(parents=True, exist_ok=True)
    prompt_path = args.bridge / "prompt.txt"
    prompt = load_prompt(prompt_path)

    print("Loading SD-Turbo onto the GPU...")
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
    stream.prepare(prompt=prompt, num_inference_steps=50, guidance_scale=1.0)
    warmup = Image.new("RGB", (args.width, args.height), (96, 96, 96))
    for _ in range(3):
        stream(image=warmup)
    torch.cuda.synchronize()
    rife = None
    if args.rife_factor > 1:
        ort.preload_dlls()
        options = ort.SessionOptions()
        options.log_severity_level = 3
        rife = ort.InferenceSession(
            str(args.rife_model.resolve()),
            sess_options=options,
            providers=["CUDAExecutionProvider", "CPUExecutionProvider"],
        )
    print(f"NeuralPass stream ready: {args.bridge}")
    print(f"Prompt: {prompt}")
    print(f"RIFE: {args.rife_factor}x ({rife.get_providers()[0] if rife else 'disabled'})")
    print(
        "Temporal guide: dual RGB8 local/world UVW planes "
        f"(3 B/px each) + DIS flow, strength {args.temporal_strength:.2f}"
    )
    print(f"Live morph: DIS optical flow; restyler feedback {args.morph_input_strength:.2f}")

    durations: list[float] = []
    previous: Image.Image | None = None
    previous_input: Image.Image | None = None
    flow_estimator = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_ULTRAFAST)
    anchors = MotionAnchors()
    stop_motion = threading.Event()
    motion_thread = threading.Thread(
        target=motion_loop,
        args=(args.bridge, anchors, stop_motion, args.scene_cut_threshold),
        name="neuralpass-motion",
        daemon=True,
    )
    motion_thread.start()
    try:
        while True:
            input_path = next_input(args.bridge)
            if input_path is None:
                time.sleep(0.002)
                continue
            try:
                sequence, image = read_frame(input_path)
            except (OSError, ValueError):
                time.sleep(0.002)
                continue

            updated_prompt = load_prompt(prompt_path)
            changed_prompt = updated_prompt != prompt
            cut_score = scene_cut_score(previous_input, image)
            cut = cut_score >= np.clip(args.scene_cut_threshold, 0.20, 0.95)
            if cut:
                previous = None
                previous_input = None
                anchors.clear()
                print(f"Scene cut: score {cut_score:.3f}; temporal and RIFE history reset")
            started = time.perf_counter()
            restyler_input = image
            anchor_source, anchor_styled, anchor_epoch = anchors.get()
            if anchor_source is not None and anchor_styled is not None and args.morph_input_strength > 0:
                morphed, _, _, _ = warp_style_to_current(
                    flow_estimator, anchor_source, anchor_styled, image
                )
                restyler_input = Image.blend(
                    image.convert("RGB").resize(morphed.size, Image.Resampling.BILINEAR),
                    morphed,
                    np.clip(args.morph_input_strength, 0.0, 0.65),
                )
            result = stream(image=restyler_input, prompt=updated_prompt if changed_prompt else None)
            result, guide = temporal_anchor(
                flow_estimator,
                previous_input,
                previous,
                image,
                result,
                args.temporal_strength,
            )
            frames = tween_frames(rife, previous, result, args.rife_factor)
            torch.cuda.synchronize()
            duration = time.perf_counter() - started
            if not anchors.set(image, result, expected_epoch=anchor_epoch):
                # A newer live frame crossed a cut while diffusion was running.
                # Discard this old-scene result rather than showing or tweening it.
                input_path.unlink(missing_ok=True)
                print("Discarded in-flight restyle across scene cut")
                continue
            write_frames(args.bridge, sequence, frames)
            previous = result
            previous_input = image
            input_path.unlink(missing_ok=True)
            if changed_prompt:
                prompt = updated_prompt
                print(f"Prompt updated: {prompt}")
            durations.append(duration)
            if len(durations) >= 30:
                median = statistics.median(durations)
                print(f"stream: {1 / median:.2f} FPS median ({median * 1000:.1f} ms)")
                durations.clear()
    finally:
        stop_motion.set()
        motion_thread.join(timeout=1.0)


if __name__ == "__main__":
    main()
