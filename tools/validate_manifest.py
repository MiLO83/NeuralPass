#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import re

path = Path(__file__).resolve().parents[1] / "models" / "manifest.json"
data = json.loads(path.read_text(encoding="utf-8"))
assert data["schema_version"] == 1
ids: set[str] = set()
for model in data["models"]:
    assert model["id"] not in ids
    ids.add(model["id"])
    assert re.fullmatch(r"[0-9a-f]{64}", model["sha256"])
    assert model["url"].startswith("https://")
    assert model["layout"] == "NCHW"
    assert model["range"] == [0, 255]
print(f"validated {len(ids)} model entries")
