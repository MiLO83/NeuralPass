# Third-party notices

NeuralPass source and original assets are licensed under the repository's MIT
license. Assembled packages contain or compile against the following components:

| Component | Version | License | Purpose |
| --- | --- | --- | --- |
| ReShade | 6.8.0 | BSD-3-Clause | Add-on API headers compiled into NeuralPass |
| ONNX Runtime | 1.24.4 | MIT | ONNX model execution |
| DirectML | 1.15.4 | MIT | Windows GPU execution provider |
| ONNX Model Zoo fast-neural-style models | opset 9 files pinned in `models/manifest.json` | BSD-3-Clause | Bundled style presets |

The assembled package includes the exact ReShade license and, for DirectML builds,
the exact ONNX Runtime and DirectML license and third-party-notice files under
`third-party/`. Model filenames, upstream URLs, licenses, and SHA-256 digests are
recorded in `models/manifest.json`. ReShade itself is not redistributed and must be
installed separately with full add-on support.
