#!/usr/bin/env python3
"""Train/export the bounded residual Photo Detail preset.

The network learns restoration, not image synthesis: its result is clamped to a
small per-channel delta around the input. Training images remain local.
Requires PyTorch and Pillow; neither is needed to build or run NeuralPass.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import random

try:
    from PIL import Image
    import torch
    from torch import nn
    from torch.nn import functional as F
    from torch.utils.data import DataLoader, Dataset
except ImportError as error:
    raise SystemExit("Install training extras: pip install torch pillow") from error


class Block(nn.Module):
    def __init__(self, source: int, target: int):
        super().__init__()
        self.layers = nn.Sequential(
            nn.Conv2d(source, target, 3, padding=1),
            nn.GroupNorm(min(8, target), target), nn.SiLU(inplace=True),
            nn.Conv2d(target, target, 3, padding=1),
            nn.GroupNorm(min(8, target), target), nn.SiLU(inplace=True),
        )

    def forward(self, value: torch.Tensor) -> torch.Tensor:
        return self.layers(value)


class PhotoDetailNet(nn.Module):
    """Small fully-convolutional U-Net with a hard ±16/255 output bound."""

    def __init__(self, base: int = 24, maximum_delta: float = 16.0 / 255.0):
        super().__init__()
        self.maximum_delta = maximum_delta
        self.a = Block(3, base)
        self.b = Block(base, base * 2)
        self.c = Block(base * 2, base * 4)
        self.middle = Block(base * 4, base * 4)
        self.up_c = Block(base * 8, base * 2)
        self.up_b = Block(base * 4, base)
        self.up_a = Block(base * 2, base)
        self.out = nn.Conv2d(base, 3, 1)

    def forward(self, image: torch.Tensor) -> torch.Tensor:
        a = self.a(image)
        b = self.b(F.avg_pool2d(a, 2))
        c = self.c(F.avg_pool2d(b, 2))
        middle = self.middle(F.avg_pool2d(c, 2))
        up_c = self.up_c(torch.cat((F.interpolate(middle, size=c.shape[-2:], mode="bilinear",
                                                   align_corners=False), c), 1))
        up_b = self.up_b(torch.cat((F.interpolate(up_c, size=b.shape[-2:], mode="bilinear",
                                                   align_corners=False), b), 1))
        up_a = self.up_a(torch.cat((F.interpolate(up_b, size=a.shape[-2:], mode="bilinear",
                                                   align_corners=False), a), 1))
        delta = torch.tanh(self.out(up_a)) * self.maximum_delta
        return torch.clamp(image + delta, 0.0, 1.0)


class OnnxByteRange(nn.Module):
    """Keep the runtime contract identical to the ONNX model-zoo presets."""

    def __init__(self, model: PhotoDetailNet):
        super().__init__()
        self.model = model

    def forward(self, image_0_255: torch.Tensor) -> torch.Tensor:
        return self.model(image_0_255 / 255.0) * 255.0


class Images(Dataset):
    def __init__(self, root: Path, crop: int):
        self.files = sorted(path for path in root.rglob("*")
                            if path.suffix.lower() in {".jpg", ".jpeg", ".png", ".webp"})
        if not self.files:
            raise ValueError(f"no training images found beneath {root}")
        self.crop = crop

    def __len__(self) -> int:
        return len(self.files)

    def __getitem__(self, index: int) -> torch.Tensor:
        with Image.open(self.files[index]) as source:
            image = source.convert("RGB")
        scale = max(self.crop / image.width, self.crop / image.height, 1.0)
        if scale > 1.0:
            image = image.resize((round(image.width * scale), round(image.height * scale)),
                                 Image.Resampling.LANCZOS)
        x = random.randrange(image.width - self.crop + 1)
        y = random.randrange(image.height - self.crop + 1)
        image = image.crop((x, y, x + self.crop, y + self.crop))
        if random.random() < 0.5:
            image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
        data = torch.ByteTensor(torch.ByteStorage.from_buffer(image.tobytes()))
        return data.reshape(self.crop, self.crop, 3).permute(2, 0, 1).float() / 255.0


def degrade(clean: torch.Tensor) -> torch.Tensor:
    result = clean
    if random.random() < 0.7:
        factor = random.choice((2, 2, 3, 4))
        small = F.interpolate(result, scale_factor=1.0/factor, mode="bilinear", align_corners=False)
        result = F.interpolate(small, size=clean.shape[-2:], mode="bilinear", align_corners=False)
    if random.random() < 0.8:
        levels = (1 << random.randint(4, 7)) - 1
        result = torch.round(result * levels) / levels
    if random.random() < 0.5:
        result = F.avg_pool2d(F.pad(result, (1, 1, 1, 1), mode="reflect"), 3, stride=1)
    noise = torch.empty_like(result).uniform_(-2.0/255.0, 2.0/255.0)
    return torch.clamp(result + noise, 0.0, 1.0)


def edge_loss(output: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    ox = output[..., :, 1:] - output[..., :, :-1]
    tx = target[..., :, 1:] - target[..., :, :-1]
    oy = output[..., 1:, :] - output[..., :-1, :]
    ty = target[..., 1:, :] - target[..., :-1, :]
    return F.l1_loss(ox, tx) + F.l1_loss(oy, ty)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("models/downloads/photo-detail-9.onnx"))
    parser.add_argument("--checkpoint", type=Path, default=Path("out/photo-detail.pt"))
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--crop", type=int, default=256)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    args = parser.parse_args()

    loader = DataLoader(Images(args.data, args.crop), batch_size=args.batch, shuffle=True,
                        num_workers=2, drop_last=True, persistent_workers=True)
    model = PhotoDetailNet().to(args.device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-4)
    for epoch in range(1, args.epochs + 1):
        model.train()
        total = 0.0
        for clean in loader:
            clean = clean.to(args.device)
            damaged = degrade(clean)
            output = model(damaged)
            loss = F.l1_loss(output, clean) + 0.2 * edge_loss(output, clean)
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            optimizer.step()
            total += loss.item()
        print(f"epoch {epoch:03d}/{args.epochs} loss={total/max(1, len(loader)):.6f}")

    args.checkpoint.parent.mkdir(parents=True, exist_ok=True)
    torch.save({"model": model.state_dict(), "maximum_delta": model.maximum_delta}, args.checkpoint)
    model.eval().cpu()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(OnnxByteRange(model), torch.zeros(1, 3, args.crop, args.crop), args.out,
                      input_names=["input1"], output_names=["output1"], opset_version=17,
                      dynamic_axes={"input1": {2: "height", 3: "width"},
                                    "output1": {2: "height", 3: "width"}})
    print(f"wrote {args.checkpoint} and {args.out}")


if __name__ == "__main__":
    main()
