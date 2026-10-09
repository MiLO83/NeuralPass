# NeuralPass

NeuralPass is a persistent sparse neural post-process for ReShade. It keeps a
styled framebuffer alive across frames, rejects history that no longer matches
the scene, and sends only dirty or expired 256-pixel tiles through an inference
backend. The final `.fx` pass blends completed tiles immediately before the
framebuffer is presented.

This repository currently contains a working v0.1 pipeline:

- deterministic temporal rejection and dirty-tile scheduling;
- progressive two-tile-per-update bootstrap with 32-pixel inference halos;
- an asynchronous add-on worker that never waits in the presentation path;
- ONNX Runtime/DirectML inference when configured at build time;
- checksum-pinned Candy, Mosaic, Rain Princess, and Udnie model downloads;
- a bounded-residual Photo Detail trainer/exporter for local image corpora;
- a clearly labelled CPU preview backend when ONNX Runtime or a model is absent;
- a ReShade compositor with strength, validity debugging, and a manual HUD mask.

The generic v0.1 path uses framebuffer color confidence. The core API already
accepts depth and motion, but automatic game-resource discovery and canonical
UVW/object-ID capture are future adapters; NeuralPass does not claim to recover
those buffers generically.

## Build

Requirements for the add-on are Windows 10/11 x64, Visual Studio 2022, CMake,
a ReShade source checkout, and optionally the ONNX Runtime DirectML native
package.

### Command Prompt one-command build

From an ordinary Windows Command Prompt in the repository root:

```bat
build_windows.cmd
```

The script finds Visual Studio, downloads the pinned ReShade and DirectML SDKs,
fetches the checksum-verified Candy model, builds/tests Release x64, and creates
`dist\NeuralPass`. Alternative modes are:

```bat
build_windows.cmd directml all
build_windows.cmd preview
```

The first downloads every bundled art model. The second omits ONNX Runtime and
builds the temporal pipeline with its clearly-labelled preview backend.

### Manual build

```powershell
git clone https://github.com/crosire/reshade.git external/reshade
python tools/fetch_models.py candy

cmake -S . -B build -A x64 `
  -DRESHADE_SDK_DIR="$PWD/external/reshade" `
  -DONNXRUNTIME_ROOT="C:/sdk/onnxruntime"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Without ONNX Runtime, omit `ONNXRUNTIME_ROOT`; the DLL remains usable with the
preview backend so capture, caching, scheduling, and compositing can be tested.
For core-only builds on any C++20 platform:

```sh
cmake -S . -B build -DNEURALPASS_BUILD_ADDON=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Install and run

1. Copy `NeuralPass.addon64` beside the game's ReShade DLL.
2. Copy `shaders/NeuralPass.fx` into the game's `reshade-shaders/Shaders` folder.
3. Copy `models/downloads` beside the add-on, preserving that directory name.
4. Choose a preset in the NeuralPass add-on overlay. `NEURALPASS_PRESET` can
   optionally set the launch default.
5. Enable **NeuralPass (keep last)** and place it last in ReShade's technique order.

On first use the untouched game image remains visible while styled tiles fill
progressively. Press ReShade's reload button to clear effect textures. Use the
validity debug view to see which pixels currently have persistent styled data.

The add-on intentionally drops capture frames when inference is behind. It
never queues an unbounded amount of work or blocks the game waiting for a tile.

## Prompt-driven live restyling

The optional StreamDiffusion bridge performs semantic img2img restyling at
512x288 and sends completed frames back to the ReShade compositor. The default
prompt is `Santa's North Pole Workshop`. A CUDA RIFE 4.9 pass inserts one motion
compensated midpoint between generated frames when requested; it is disabled by
default because the continuous flow-warp path supersedes burst playback. Use
`--rife-factor 2` or `--rife-factor 4` to request extra generated midpoints.

The bridge builds separate local and world RGB8 UVW maps (exactly 3 bytes per
pixel each). Dominant affine motion provides the camera/world plane; residual
dense flow provides the object-local plane. Forward/backward flow consistency
produces a separate visibility byte, mirrored into the transport texture's
alpha channel so ReShade rejects occluded history. Holes are style-infilled
before live source detail is used as a fallback. The motion-aligned result is
also blended into the next diffusion input with `--morph-input-strength`
(default `0.45`). Style-biased UV-space infill preserves existing restyled
detail while the live framebuffer contributes structure only at disocclusions.
Histogram/luma scene-cut detection immediately invalidates the visible anchor,
discards old-scene inference still in flight, and prevents RIFE from tweening
across the cut. Tune it with `--scene-cut-threshold` (default `0.52`). These
planes are image-derived proxies, not engine geometry:
a future depth/object adapter can supply true XYZ and local transforms while
retaining the same screen flow -> {local UVW, world UVW} -> RGB contract.
Adjust the additional temporal anchor with `--temporal-strength` (default
`0.28`, maximum useful value `0.35`).

Start the service in WSL before launching the game:

```sh
cd /home/topnotch/github/MiLO83/NeuralPass
external/stream-venv/bin/python tools/stream_bridge.py
```

In ReShade's **Add-ons > NeuralPass** panel, keep **Prompt-driven
StreamDiffusion** enabled, edit the prompt, and click **Apply prompt**. The
bridge deliberately permits only one generated frame in flight, so latency is
bounded and stale camera views do not form a queue.

Applied prompts are kept in `NeuralPassBridge/prompt_history.txt` (newest
first, maximum 32). The editable prompt field has a **Prompt history** dropdown
that restores any prior prompt before it is applied again.

## Persistent material texture baker

The platform-neutral baker core accepts visible screen-pixel correspondences of
`{screen XY, material ID, texture UV, confidence}`. It bilinearly splats the
restyled framebuffer into a persistent per-material atlas, retains an observed
coverage mask, fills only unseen texels in texture space, and reconstructs the
visible framebuffer by sampling that same atlas. This keeps the wrapped mesh
view and the unwrapped texture representation consistent.

The ReShade add-on has an experimental D3D11 surface adapter. It reflects the
vertex shader's rasterized `TEXCOORD`, executes the game's draw once, replays it
with a capture pixel shader into `RGBA32_UINT`, and asynchronously decodes
`{material ID low/high, U bits, V bits}` through a three-slot staging ring. A
Windows WARP integration test renders a real triangle and verifies its decoded
64-bit material ID and interpolated UV. Capture frames and framebuffer frames
are paired by GPU sequence number before reveal-only atlas generation.

D3D9/10, D3D12, and Vulkan still require equivalent adapters, and the D3D11
path needs broad real-game compatibility testing. Unsupported APIs, shader
signatures, deferred contexts, render-target sizes, and MSAA draws retain the
screen-space path instead of receiving guessed UV data. The canonical capture
and multi-material baker remain API-neutral so every adapter emits the same
validated surface-frame contract.

| Graphics API | Geometry capture status |
| --- | --- |
| D3D11 | Experimental draw replay; automated WARP coverage |
| D3D9 / D3D10 | Adapter required |
| D3D12 | Adapter required |
| Vulkan | Adapter required |

Atlas snapshots use a versioned, checksummed `.npatlas` format. Writes create a
new generation and rename it only after the complete payload is flushed, so an
interrupted save cannot damage the preceding generation. Only material IDs
backed by shader bytecode and texture content fingerprints are eligible for a
cross-launch snapshot; handle-only identities deliberately remain session-local.

`tools/prompt_restyle.py` is the slower SDXL-Lightning quality reference for a
single screenshot. `tools/stream_restyle.py` benchmarks the persistent live
backend without starting the game.

## Important limitations

- Geometry-aware persistent baking is currently experimental on D3D11 only;
  this is not yet a cross-API production release.
- The D3D11 replay shader cannot reproduce application pixel-shader `discard`
  or alpha-test logic yet, so cutout/translucent materials may produce invalid
  correspondence around transparent texels.
- v0.1 supports SDR RGBA8 capture. HDR/scRGB is not processed correctly yet.
- The screen-space fallback cannot follow large camera motion as accurately as
  engine motion vectors; changed pixels are invalidated and restyled instead.
- Generic automatic HUD recognition is not yet reliable. A normalized manual
  exclusion rectangle is available in the shader UI.
- ONNX tile inference currently uses CPU tensor upload/readback around the
  DirectML session. It is asynchronous, but zero-copy GPU tensors remain future work.
- Fixed style-transfer networks are spatially local and may show seams. Halos
  and validity feathering reduce them but do not eliminate every model artifact.

## Design notes

`neuralpass_core` owns all policy: history validation, maximum age, mask
dilation, tile priority, and padded jobs. The add-on owns capture and lifetime;
inference backends only transform an input tile. That separation permits a
future UVW/object-ID cache or WinML backend without changing the scheduler.

The `DownToEarth` TinyUNet work informed the bounded residual `photo-detail`
preview, but NeuralPass does not import code or weights from another checkout.

## Credits

- Project direction and prompting: **MiLO83**
- Architecture and implementation: **OpenAI Codex**, working collaboratively
  with MiLO83
- ReShade and its add-on API: Patrick Mours and ReShade contributors
