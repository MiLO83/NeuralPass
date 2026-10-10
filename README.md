# NeuralPass

Created by Miles Cameron Johnston with OpenAI Codex — powered by the recurring
instruction, “please continue.”

[![CI](https://github.com/MiLO83/NeuralPass/actions/workflows/ci.yml/badge.svg)](https://github.com/MiLO83/NeuralPass/actions/workflows/ci.yml)

NeuralPass is a persistent neural restyling post-process for ReShade. It keeps a
styled framebuffer alive across frames, rejects history that no longer matches
the scene, and sends one coherent full-frame composition through an inference
backend. The final `.fx` pass blends the completed frame immediately before the
framebuffer is presented. Compatible geometry is also projected into persistent
material UV atlases so later viewpoints can reuse already restyled texels.

This repository currently contains a working **research-preview** pipeline:

- deterministic temporal rejection and coherent full-frame scheduling;
- prompt-driven SD-Turbo/StreamDiffusion with optical-flow persistence, plus
  whole-frame ONNX and preview fallbacks;
- an asynchronous add-on worker that never waits in the presentation path;
- ONNX Runtime/DirectML inference when configured at build time;
- checksum-pinned Candy, Mosaic, Rain Princess, and Udnie model downloads;
- a bounded-residual Photo Detail trainer/exporter for local image corpora;
- a clearly labelled CPU preview backend when ONNX Runtime or a model is absent;
- a ReShade compositor with strength, validity debugging, and a manual HUD mask;
- restart-stable pipeline-plus-descriptor binding keys and per-scene atlas namespaces;
- reveal provenance, UV-gradient elliptical splatting, scene-cut quarantine, and
  stale scene/request rejection.

The evidence-backed implementation status is tracked in
[`ROADMAP_CHECKLIST.txt`](ROADMAP_CHECKLIST.txt). An `[x]` requires a passing
test or release artifact; `[~]` means the production path is still incomplete.

The generic path uses framebuffer color confidence. The core accepts depth,
motion, mesh UV gradients, binding identity, and visibility classes. The
shared visibility-to-bake mapping separates first observations from disoccluded,
newly-front-facing, and off-screen-entry inpainting, so graphics adapters cannot
invent different persistent-coverage rules. Two-frame acceptance tests prove that
moving occluders and camera pans add only newly visible UV texels while established
and permanently unseen texture regions remain unchanged. The
experimental D3D9, D3D10, D3D11, and D3D12 adapters supply exact rasterized mesh UVs and
gradients; D3D12 now also samples a descriptor-tracked source texture when a
compatible texture/sampler pair is bound. Its bounded-table RGBA8/BGRA8
replacement path is experimental and still needs real-game validation. A new
Vulkan scaffold instruments a compatible vertex SPIR-V module to carry the real
UV input into the same surface contract and uses a generic three-slot fence/readback
ring. Combined image-sampler bindings and scalar separate image/sampler bindings
can provide source color through the application's existing descriptor sets.
Transfer-source RGBA8/BGRA8 images in
bounded descriptor tables also have an experimental coverage-safe replacement
path; driver/game validation remains.

## Build

Requirements for the add-on are Windows 10/11, Visual Studio 2022, CMake,
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
build_windows.cmd directml x86
build_windows.cmd preview x86
```

The first downloads every bundled art model. The second omits ONNX Runtime and
builds the x64 temporal pipeline with its clearly-labelled preview backend. The
third builds/tests a Win32 add-on and creates `dist\NeuralPass-x86` with an x64
`NeuralPassWorker.exe`. Microsoft's current ONNX Runtime DirectML package has no
Win32 runtime, so the add-on fits a coherent frame into the architecture-matched
host's private named pipe and 8 MiB shared-memory mailbox, then restores the output
to display size. Guided installs
place the worker and its private runtime DLLs under `NeuralPass/runtime`, away from
game-root graphics proxy DLLs. The fourth
command remains useful when a fully self-contained non-neural Win32 preview is wanted.

Every assembled package records the full source revision and its commit timestamp
in `PACKAGE.json`, inventories the payload and dependencies in an SPDX 2.3
`SBOM.spdx.json`, and covers the complete package (including the SBOM) with
`SHA256SUMS.txt`. MSVC links use reproducible PE/PDB settings; builds from the same
clean revision and inputs are expected to produce identical payload hashes.

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

Download the [Windows x64 DirectML preview package](https://github.com/MiLO83/NeuralPass/releases/download/v0.1.0-preview/NeuralPass-v0.1.0-preview-windows-x64-directml.zip),
then extract it to a separate directory.

For an assembled Windows package, run `Install NeuralPass.cmd` and give it the
game executable or directory. It refuses known anti-cheat markers, checks for a
graphics proxy, verifies every packaged file against `SHA256SUMS.txt`, displays the
unsigned-preview warning, and requires you to type `INSTALL` before copying anything.
The package also includes hash-aware manifest-scoped uninstallation and a diagnostics
report generator. Uninstall refuses to delete modified files unless you explicitly
pass `-ForceModified` to the PowerShell script.

To update, extract a newer architecture-matched package separately and run its
`Update NeuralPass.cmd`, selecting the existing game directory. The updater verifies
the complete new package, refuses modified/missing installed files by default,
requires `UPDATE`, backs up the owned installation, invokes the guided installer,
removes files owned only by the old release, and rolls back if replacement fails.
Use `-ForceModified` only when intentionally replacing locally changed NeuralPass
files. Verified optional models recognized by the new manifest are preserved.

For a manual installation:

1. Copy the architecture-matched `NeuralPass.addon64` or `NeuralPass.addon32`
   beside the game's ReShade DLL.
2. Copy `reshade-shaders/Shaders/NeuralPass.fx` into the corresponding game folder.
3. Copy `models/downloads` beside the add-on, preserving that directory name.
4. Choose a preset in the NeuralPass add-on overlay. `NEURALPASS_PRESET` can
   optionally set the launch default.
5. Enable **NeuralPass (keep last)** and place it last in ReShade's technique order.

On first use the untouched game image remains visible until the first coherent
styled frame is ready. Press ReShade's reload button to clear effect textures. Use
the validity debug view to see which pixels currently have persistent styled data.

The add-on intentionally drops capture frames when inference is behind. It
never queues an unbounded amount of work or blocks the game waiting for inference.
Full-resolution display and mesh-surface readback are paced to one capture per
eight effect frames by default while the compositor continuously presents the last
persistent result; the interval is adjustable in the overlay. This keeps GPU
copies and CPU color/surface decoding from dominating render cadence. One coherent
pass is admitted per worker update; the overlay reports capture cadence, frame time,
inference-pass time, and pressure observations.
In the DirectML build, D3D11 uses the DirectML execution provider; other graphics
APIs use ONNX Runtime's CPU provider until their device-loss stress gates pass.
The x86 add-on launches the packaged x64 worker for either provider and restarts it
once after a broken transport or provider failure. Every response must echo the
request, scene, style, and visible-binding generations before it can be committed.
The overlay reports `onnx/directml`, `onnx/cpu`, or `preview/...` so the active
backend is never ambiguous. On D3D11, the add-on resolves the game's DXGI adapter
LUID and passes that adapter index to DirectML instead of assuming adapter zero.
The x64 DirectML package also includes `Validate NeuralPass Hardware.cmd`; run it
outside the game to execute a finite-output model smoke test and write a shareable
JSON evidence report. Pass a DXGI device index when testing a non-default adapter.
Both DirectML packages include `Validate NeuralPass Worker.cmd`; it exercises the
packaged client/worker transport, generation rollover, and forced worker restart.
It defaults to DirectML and accepts `-Provider cpu` for transport-only validation.
After guided installation, `Manage NeuralPass Models.cmd` lists, verifies, installs,
repairs, and removes the four manifest-pinned style models. For example,
`Manage NeuralPass Models.cmd -Action Install -Model mosaic` downloads Mosaic and
accepts it only after its pinned SHA-256 matches. The manager updates the install
ownership manifest, refuses to mutate the checksummed package, and will not remove
the last verified model.
See [`docs/compatibility.md`](docs/compatibility.md) for the API support matrix,
safety policy, diagnostics, and troubleshooting sequence.

Both packages include `Validate NeuralPass Vulkan.cmd`. It creates a real Vulkan
device and graphics pipeline, executes NeuralPass's instrumented four-target capture,
then proves a cloned replacement can patch one covered texel without changing the
application source or uncovered texels. The resulting JSON records the process
tested executable architecture, display adapters, output, and pass/skip/failure status.

Once the add-on reaches ReShade's effects pass, it atomically writes
`NeuralPass-runtime-evidence.json` beside the game. This records the actual graphics
API, process architecture, backbuffer dimensions and format, declared swapchain color
space, and whether the exact format/color-space pair selected SDR, scRGB, HDR10/PQ,
or a safe bypass. It refreshes every 120 effect frames with generic draw, UV/material,
surface-capture, and inference counters. Diagnostics consumes this structured report;
proxy DLL names remain only pre-launch candidates, not proof of a live adapter path.

## Prompt-driven live restyling

The preferred prompt-driven path uses SD-Turbo through StreamDiffusion for one
coherent 512x288 img2img composition and sends completed frames back to the ReShade
compositor. The fixed-style ONNX models remain a package-local compatibility
fallback and are also evaluated as one complete composition, never as visible
independent tiles. The default
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

The development environment has been validated with PyTorch CUDA 12.8 on an
NVIDIA RTX 5060 Ti. It is still a separately managed Python environment rather
than part of the signed/checksummed Windows package:

```sh
cd /home/topnotch/github/MiLO83/NeuralPass
external/stream-venv/bin/python tools/stream_bridge.py \
  --bridge /mnt/d/path/to/your/game/NeuralPassBridge
```

The bridge location is per game. It may instead be supplied through the
`NEURALPASS_BRIDGE` environment variable; when neither is supplied, the helper
uses `NeuralPassBridge` below its current working directory. NeuralPass has no
built-in game path or 3DMark dependency.

After starting that external bridge, opt in via **Add-ons > NeuralPass >
Prompt-driven SD-Turbo**, edit the prompt, and click **Apply prompt**. The choice
is stored per game as `NeuralPassBridge/stream.enable`; disabling it removes that
marker. It is disabled by default on a new game so the packaged ONNX/preview backend
works immediately without waiting for an unbundled process. The
bridge deliberately permits only one generated frame in flight, so latency is
bounded and stale camera views do not form a queue.
Changing or enabling a stream prompt invalidates queued output and the current
material replacements. Because that external model is not versioned by the package,
its material atlases remain session-local and are never loaded into fixed-model caches.

Applied prompts are kept in `NeuralPassBridge/prompt_history.txt` (newest
first, maximum 32). The editable prompt field has a **Prompt history** dropdown
that restores any prior prompt before it is applied again.

The global prompt applies to the complete prompt-driven stream. For material
exceptions, select a captured binding in the add-on overlay and set **Binding
style strength** from 0 to 100. Zero bypasses neural generation and preserves
the live game pixels for that material; intermediate values blend the generated
appearance once before it enters the texture atlas. Restart-stable bindings save
their controls in the per-game ReShade configuration, while handle-derived
bindings remain session-only. Applying a change rejects in-flight results,
clears old GPU replacements, and switches to a cache namespace keyed by the
complete control set, so an atlas baked at one strength cannot leak into another.
These binding controls work with fixed presets and the stream path; free-text
prompting itself still requires the explicitly enabled external stream backend.

## Persistent material texture baker

The platform-neutral baker core accepts visible screen-pixel correspondences of
`{screen XY, binding ID, texture UV, UV gradients, depth, confidence}`. It uses
a bounded confidence-weighted elliptical footprint when gradients exist and a
bilinear fallback otherwise. Atlases distinguish unseen source, generated
inpaint, and direct observations; a later direct observation may replace an
inpainted texel. Source alpha is retained. Uncovered pixels always keep the live
framebuffer rather than sampling a debug sentinel.

The ReShade add-on has experimental D3D10 and D3D11 surface adapters. They reflect the
vertex shader's rasterized `TEXCOORD`, execute the game's draw once, then
replay it into an `RGBA32_UINT` identity/UV target plus gradient, source-color,
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

The D3D11 adapter handles indexed and non-indexed instanced draws through both
direct and indirect argument-buffer paths. The WARP suite executes every variant
through the same state-preserving application-draw and capture-replay path.
That transaction restores the selected source SRV, pixel shader and dynamic-linkage
instances, multiple render targets, depth view/state, blend state, stencil reference,
and output-merger UAV bindings before returning to the game.

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
uncovered-source preservation, SRV restoration, and isolation of distinct binding
replacements that share the same source texture.
Unsupported replacement layouts trip a per-binding/resource circuit breaker:
the adapter reports one rejection in the overlay and falls back without retrying
GPU allocation every draw. A new atlas snapshot or source resource retries it.

The live worker classifies each captured D3D11 pixel as known-visible,
disoccluded, newly front-facing, off-screen entry, first observation, or
unsupported and reports the current counts in the overlay. A camera cut passes
no previous surface to the classifier, so the new view becomes direct
first-observation evidence while the reveal-only atlas planner still schedules
its uncovered UVs. First-observation styling and later reveal inpainting use
disjoint commit-validated masks; newly visible samples receive atlas priority so a
small reveal is not starved by a larger initial screen restyle. Missing capture
frames break the comparison chain rather than reprojecting across an unknown gap.

D3D9 has a separate Shader Model 3 token/reflection and legacy draw path. It
captures direct/indexed UVs and replaces lockable 32-bit color textures, but its
readback is synchronous and cannot provide depth. D3D10 has native Shader Model 4
draw replay, asynchronous readback, and isolated RGBA8/BGRA8 replacement under WARP.
D3D12 has experimental PSO replay,
asynchronous capture, and descriptor-isolated RGBA8/BGRA8 replacement. Vulkan
has SPIR-V vertex instrumentation, pipeline replay, and asynchronous readback, but
accepts a draw only when it can safely identify and instrument a plain float2 UV
input. Both explicit APIs still need real-game evidence; Vulkan scalar combined
and separate-descriptor source sampling now passes native x86/x64 driver tests,
while descriptor arrays and broader replacement layouts remain. Unsupported APIs, shader signatures, native
render passes, render-target sizes, and MSAA draws retain the screen-space path
instead of receiving guessed UV data. The canonical capture and multi-material
baker remain API-neutral so every adapter emits the same validated surface-frame
contract.

| Graphics API | Geometry capture status |
| --- | --- |
| D3D9 | Experimental Shader Model 3 direct/indexed replay and lockable RGBA/BGRA replacement; native HAL reset/recreation test |
| D3D10 | Experimental direct/indexed replay and RGBA8/BGRA8 replacement; automated WARP recreation coverage |
| D3D11 | Experimental draw replay and RGBA8/BGRA8 replacement; automated WARP recreation coverage |
| D3D12 | Experimental direct/indirect PSO replay, readback, and bounded-table RGBA8/BGRA8 replacement; WARP recreation covered, game validation pending |
| Vulkan | Experimental SPIR-V UV/source capture, asynchronous readback, and bounded-table RGBA8/BGRA8 replacement; x86/x64 native NVIDIA capture/recreation evidence, game validation pending |

Atlas snapshots use a versioned, checksummed `.npatlas` format. Writes create a
new generation and rename it only after the complete payload is flushed, so an
interrupted save cannot damage the preceding generation. Only material IDs
backed by shader bytecode and texture content fingerprints are eligible for a
cross-launch snapshot; handle-only identities deliberately remain session-local.
Checksummed scene manifests accumulate restart-stable bindings from multiple
views. Confirmed scene changes switch namespaces; returning views match and
reload the prior namespace by binding overlap. The containing path also keys the
game executable build, selected style, inference backend, and exact model contents.
Changing presets saves the outgoing atlas generation, clears live replacements,
and loads only the matching style/model generation, so old-style texture pixels
cannot leak into the newly selected look.
Per-binding strength/bypass settings participate in the same style identity and
therefore receive the same stale-result rejection and cache isolation.

The add-on overlay also exposes armed cache deletion at three scopes. **Forget
selected binding** removes every saved generation for that material in the active
scene/style/model namespace. **Forget current scene** removes that scene and all
of its style/model atlases. **Forget all caches** removes the complete local
`NeuralPassCache`, including namespaces left by older game builds. Each command
runs on the worker, invalidates in-flight generations and GPU replacements, and
reports the number of entries and bytes removed. The destructive buttons appear
only while **Arm destructive cache controls** is checked; deletion is permanent.
Learning resumes from visible material immediately after deletion, so set a
binding to bypass if it should remain unstyled instead of being relearned.

When a game executable changes, NeuralPass keeps the new build in a separate
namespace and compares its restart-stable visible bindings with older scene
manifests. One uniquely compatible scene is copied transactionally into the new
build namespace; reuse remains keyed by exact material IDs and atlas coverage, so
changed bindings naturally begin uncovered. Equal best matches are quarantined
instead. The overlay lists their source build, scene, overlap, and binding count,
and requires **Use candidate** or **Start new scene** before persistent baking
continues. Existing destinations, symlinks, path escapes, and partial overwrites
are refused.

`tools/prompt_restyle.py` is the slower SDXL-Lightning quality reference for a
single screenshot. `tools/stream_restyle.py` benchmarks the persistent live
backend without starting the game.

## Important limitations

- Geometry-aware persistent baking has native D3D9 and D3D10/D3D11/D3D12 WARP
  tests plus x86/x64 Vulkan driver capture/replacement tests; the explicit APIs
  remain experimental and this is not a production release.
- The D3D11 replay shader rejects fully transparent texels from its selected
  source texture, but cannot reproduce application-specific `discard`, custom
  alpha thresholds, or opacity sourced from another texture yet.
- D3D11 base-color selection is heuristic. If an unusual material layout selects
  a normal/emissive texture, use the per-binding source-SRV override in the overlay;
  overrides persist in the game's ReShade configuration when the binding identity
  is restart-stable, while handle-derived bindings remain safely session-local.
- D3D11 GPU replacement currently supports non-array RGBA8/BGRA8 Texture2D SRVs.
  Compressed, HDR/float, array, and multisampled sources retain screen-space output.
- D3D9 capture requires four 32-bit floating-point MRTs, performs a synchronous
  readback, cannot recover fragment depth, and replaces only lockable 32-bit textures.
- The compositor now has format- and color-space-checked paths for linear FP16
  scRGB and RGB10A2 HDR10/PQ. Capture is tone-mapped to bounded sRGB for inference,
  then styled chroma/contrast is mapped back while retaining scene luminance.
  Mismatched or unknown HDR format/color-space pairs are bypassed. The math has
  automated round-trip coverage, but HDR monitor/game validation remains.
- DirectML model execution, including the x86-to-x64 worker transport, has an automated NVIDIA smoke test on the development
  machine; AMD and Intel execution still require hardware validation.
- The screen-space fallback cannot follow large camera motion as accurately as
  engine motion vectors. Default sticky history clears stale output at an
  effect-environment transition or the first visual cut, then preserves coverage
  across sustained motion so a paced progressive pass can converge. Disable it
  to invalidate on every detected cut instead.
- Generic automatic HUD recognition is not yet reliable. A normalized manual
  exclusion rectangle is available in the shader UI.
- ONNX full-frame inference currently uses CPU tensor upload/readback around the
  DirectML session. It is asynchronous, but zero-copy GPU tensors remain future work.
- Fixed style-transfer networks are lower-fidelity compatibility fallbacks. Their
  fixed input resolution can soften detail, but the output remains one coherent
  fullscreen composition without tile seams.
- Cache deletion is intentionally permanent and path-guarded. Export or copy the
  `NeuralPassCache` directory first if you may want to restore learned atlases.

## Design notes

`neuralpass_core` owns history, visibility, scene identity, and material-atlas
policy. The add-on owns capture and lifetime; inference backends transform one
coherent frame. That separation permits a
future UVW/object-ID cache or WinML backend without changing the scheduler.

The `DownToEarth` TinyUNet work informed the bounded residual `photo-detail`
preview, but NeuralPass does not import code or weights from another checkout.

## Credits

- Project direction and prompting: **MiLO83**
- Architecture and implementation: **OpenAI Codex**, working collaboratively
  with MiLO83
- ReShade and its add-on API: Patrick Mours and ReShade contributors
