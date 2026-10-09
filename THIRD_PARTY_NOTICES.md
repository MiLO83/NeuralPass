# Third-party notices

NeuralPass source and original assets are licensed under the repository's MIT
license. A DirectML binary package also contains the following components:

| Component | Version | License | Purpose |
| --- | --- | --- | --- |
| ONNX Runtime | 1.24.4 | MIT | ONNX model execution |
| DirectML | 1.15.4 | MIT | Windows GPU execution provider |
| ONNX Model Zoo fast-neural-style models | opset 9 files pinned in `models/manifest.json` | BSD-3-Clause | Bundled style presets |

The assembled package includes the exact ONNX Runtime and DirectML license and
third-party-notice files under `third-party/`. Model filenames, upstream URLs,
licenses, and SHA-256 digests are recorded in `models/manifest.json`.

ReShade headers are used to build the add-on. ReShade itself is not redistributed
by the NeuralPass package and must be installed separately with full add-on support.
