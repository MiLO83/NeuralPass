# NeuralPass

NeuralPass is a persistent sparse neural post-process for ReShade. It keeps a
styled framebuffer alive across frames, rejects history that no longer matches
the scene, and sends only dirty or expired 256-pixel tiles through an inference
backend. The final `.fx` pass blends completed tiles immediately before the
framebuffer is presented.

This repository currently contains a working **research-preview** pipeline:

- deterministic temporal rejection and dirty-tile scheduling;
- progressive two-tile-per-update bootstrap with 32-pixel inference halos;
- an asynchronous add-on worker that never waits in the presentation path;
- ONNX Runtime/DirectML inference when configured at build time;
- checksum-pinned Candy, Mosaic, Rain Princess, and Udnie model downloads;
- a bounded-residual Photo Detail trainer/exporter for local image corpora;
- a clearly labelled CPU preview backend when ONNX Runtime or a model is absent;
- a ReShade compositor with strength, validity debugging, and a manual HUD mask.
- restart-stable pipeline-plus-descriptor binding keys and per-scene atlas namespaces;
- reveal provenance, UV-gradient elliptical splatting, scene-cut quarantine, and
  stale scene/request rejection.

The evidence-backed implementation status is tracked in
[`ROADMAP_CHECKLIST.txt`](ROADMAP_CHECKLIST.txt). An `[x]` requires a passing
test or release artifact; `[~]` means the production path is still incomplete.

The generic path uses framebuffer color confidence. The core accepts depth,
motion, mesh UV gradients, binding identity, and visibility classes, but only
the experimental D3D11 adapter currently supplies exact rasterized mesh UVs and
gradients. NeuralPass does not claim generic geometry recovery on other APIs.

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

This is not a signed v1.0 release or a universal installer. Use it only with an
offline game and a ReShade build with full add-on support. Do not inject it into
anti-cheat or protected multiplayer software.

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

The optional legacy StreamDiffusion bridge performs semantic img2img restyling at
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

The moved WSL environment in the original development machine currently lacks
PyTorch and ONNX Runtime. The following command is valid only after those
dependencies have been restored in `external/stream-venv`; it is not the final
managed-worker experience:

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
`{screen XY, binding ID, texture UV, UV gradients, depth, confidence}`. It uses
a bounded confidence-weighted elliptical footprint when gradients exist and a
bilinear fallback otherwise. Atlases distinguish unseen source, generated
inpaint, and direct observations; a later direct observation may replace an
inpainted texel. Source alpha is retained. Uncovered pixels always keep the live
framebuffer rather than sampling a debug sentinel.

The ReShade add-on has an experimental D3D11 surface adapter. It reflects the
vertex shader's rasterized `TEXCOORD`, executes the game's draw once, then
replays it into an `RGBA32_UINT` identity/UV target plus gradient, source-color,
and device-depth targets. The adapter ranks currently bound sampleable
2D color SRVs, samples its aggressive base-color candidate with the mesh UV,
and emits NaN when no trustworthy texture/sampler pair exists. The overlay can
override that choice per pipeline-plus-descriptor binding by forcing a D3D11
pixel-shader SRV slot, or return the binding to automatic selection. A three-slot
staging ring asynchronously decodes the 64-bit pipeline-plus-descriptor binding
ID, exact UV, `ddx`/`ddy` footprint, source texel, and equal-tested raster depth. A
Windows WARP test covers indexed and non-indexed replay, derivative capture,
and state restoration. Capture frames and framebuffer frames are paired by GPU
sequence number before reveal-only atlas generation.

Binding identity includes shader bytecode, pixel-resource slot/content
fingerprints, input-layout semantics, and bound vertex/index-buffer topology.
Immutable geometry uploads are restart-stable. Once a geometry buffer is
updated, its identity becomes handle-backed and session-only, preventing
animated buffers from creating a new persistent atlas on every update while
also preventing unsafe cross-launch reuse.

For D3D11 RGBA8/BGRA8 Texture2D sources, completed atlas snapshots are assembled
into real per-binding replacement textures. Each replacement begins as a GPU
copy of the selected source; only covered atlas runs are patched, using the
coverage-safe mip chain. The replacement SRV is bound only for the application
draw and the original SRV is restored before the capture replay, so unseen
texels and transfer inputs remain original. Scene changes discard the live
replacement set before the new namespace can draw. WARP verifies substitution,
uncovered-source preservation, and SRV restoration.
Unsupported replacement layouts trip a per-binding/resource circuit breaker:
the adapter reports one rejection in the overlay and falls back without retrying
GPU allocation every draw. A new atlas snapshot or source resource retries it.

The live worker classifies each captured D3D11 pixel as known-visible,
disoccluded, newly front-facing, off-screen entry, first observation, or
unsupported and reports the current counts in the overlay. A camera cut passes
no previous surface to the classifier, so the new view becomes direct
first-observation evidence while the reveal-only atlas planner still schedules
its uncovered UVs. Missing capture frames break the comparison chain rather
than reprojecting across an unknown gap.

D3D9/10, D3D12, and Vulkan still require equivalent adapters, and the D3D11
path needs broad real-game compatibility testing. Unsupported APIs, shader
signatures, deferred contexts, render-target sizes, and MSAA draws retain the
screen-space path instead of receiving guessed UV data. The canonical capture
and multi-material baker remain API-neutral so every adapter emits the same
validated surface-frame contract.

| Graphics API | Geometry capture status |
| --- | --- |
| D3D11 | Experimental draw replay and RGBA8/BGRA8 replacement; automated WARP coverage |
| D3D9 / D3D10 | Adapter required |
| D3D12 | Adapter required |
| Vulkan | Adapter required |

Atlas snapshots use a versioned, checksummed `.npatlas` format. Writes create a
new generation and rename it only after the complete payload is flushed, so an
interrupted save cannot damage the preceding generation. Only material IDs
backed by shader bytecode and texture content fingerprints are eligible for a
cross-launch snapshot; handle-only identities deliberately remain session-local.
Checksummed scene manifests accumulate restart-stable bindings from multiple
views. Confirmed scene changes switch namespaces; returning views match and
reload the prior namespace by binding overlap.

`tools/prompt_restyle.py` is the slower SDXL-Lightning quality reference for a
single screenshot. `tools/stream_restyle.py` benchmarks the persistent live
backend without starting the game.

## Important limitations

- Geometry-aware persistent baking is currently experimental on D3D11 only;
  this is not yet a cross-API production release.
- The D3D11 replay shader rejects fully transparent texels from its selected
  source texture, but cannot reproduce application-specific `discard`, custom
  alpha thresholds, or opacity sourced from another texture yet.
- D3D11 base-color selection is heuristic. If an unusual material layout selects
  a normal/emissive texture, use the per-binding source-SRV override in the overlay;
  overrides persist in the game's ReShade configuration when the binding identity
  is restart-stable, while handle-derived bindings remain safely session-local.
- D3D11 GPU replacement currently supports non-array RGBA8/BGRA8 Texture2D SRVs.
  Compressed, HDR/float, array, and multisampled sources retain screen-space output.
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
