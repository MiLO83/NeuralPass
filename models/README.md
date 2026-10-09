# Model files

Model binaries are intentionally not committed. Run:

```powershell
python tools/fetch_models.py candy mosaic rain-princess udnie
```

Every download is pinned by SHA-256 in `manifest.json`. The artistic models
come from the ONNX Model Zoo and retain its BSD-3-Clause license. Set the
`NEURALPASS_PRESET` environment variable to a manifest ID before starting a
game.

To create the bounded neural `photo-detail` model from a local image corpus:

```powershell
pip install torch pillow onnx
python tools/train_photodetail.py --data D:/training-images
```

The exporter writes `models/downloads/photo-detail-9.onnx`, which the add-on
loads automatically when the Photo Detail preset is selected. Until it exists,
that preset uses the built-in non-neural preview and labels it accordingly.
