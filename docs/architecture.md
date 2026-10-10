# Architecture

```text
game framebuffer
      |
      +-- capture before ReShade effects ----------+
      |                                            |
      |                                      worker thread
      |                               temporal confidence/rejection
      |                                  dilate + prioritize tiles
      |                                      ONNX/preview backend
      |                                            |
      +--> other ReShade effects --> NeuralPass compositor --> Present
                                         ^
                                         |
                              styled cache + validity texture
```

The worker owns the history state. The render thread exchanges only complete
captured frames and complete upload buffers under a short mutex. There is one
pending and one ready slot; newer capture work is dropped while either slot is
occupied, providing natural backpressure.

On Win32, model execution crosses into the packaged x64 `NeuralPassWorker.exe`
because the pinned ONNX Runtime DirectML distribution has no Win32 binaries. A
random per-process named pipe carries fixed-size versioned control records; one
private 8 MiB file mapping carries exactly one float RGBA tile at a time. The
add-on never passes handles or pointers across architectures. Width, height, and
payload bounds are validated at both ends, and the response must echo the request,
scene, style, and visible-binding generations. A disconnect causes one clean worker
restart and retry. The packaged worker-health runner performs inference, rolls the
generation tuple, deliberately terminates the child during a request, and requires
the restarted worker to complete the retry. Its x86 build talks to the same packaged
x64 worker used by Win32 games. The ordinary x64 add-on retains its in-process ONNX path.
Guided installation isolates the child and a private copy of its ONNX/DirectML DLLs
under `NeuralPass/runtime`; this prevents a game-root ReShade `dxgi.dll` or other
graphics proxy from entering the worker's dependency search. The add-on prefers the
isolated path and retains the package-root location only as a legacy/manual fallback.

Persistent storage is rooted below a game-build identity derived from the host
executable name, size, and modification timestamp. Scene manifests live beneath
that root; atlas paths then add the fixed-style identity, backend identity, and
model-content fingerprint. A preset transition saves the outgoing namespace,
invalidates its in-flight screen history and GPU replacements, increments the
style generation, and loads only the incoming namespace. Binding keys below that
point retain shader, descriptor/source, and immutable UV-topology identity.
The legacy external diffusion bridge is deliberately excluded from cross-launch
atlas persistence because its model is not package-versioned. Prompt identity is
still a live style generation: changing it clears queued frames/replacements and
rejects a late result from the preceding prompt.

The material-control plane is API-neutral. Each captured binding defaults to
100% style strength; a zero setting removes its correspondence from bake commits
and invalidates its screen-space output so the live game pixel survives. Partial
strength is blended exactly once into newly generated pixels before the
framebuffer-to-source transfer. Controls backed by restart-stable binding IDs are
stored in the per-game ReShade configuration. A sorted control-set fingerprint is
part of the fixed-style cache identity, and every edit increments the worker style
generation, clears queued replacements, and rejects older results. Session-only
bindings get the same runtime behavior without unsafe cross-launch persistence.
The all-default control set deliberately retains the original fixed-style identity,
so upgrading does not strand otherwise compatible pre-control atlases.

## Coordinate contracts

- Motion is **current pixel to previous pixel**, measured in pixels.
- Depth is normalized or linear but must remain consistent between adjacent
  frames. Validation uses both an absolute and a relative tolerance.
- A nonzero validity byte means the styled color belongs to the associated
  source/depth history sample.
- Tile cores form a non-overlapping grid. The padded rectangle adds inference
  context and is clipped to the framebuffer.

Future geometry-aware adapters should populate motion and depth first. A true
canonical adapter may additionally use an `R32_UINT` object ID and
`RGBA16_UNORM` bind-pose UVW/validity texture as a persistent cache key.

## Progressive framebuffer-to-texture painting

Material base-color atlases begin as a magenta debug sentinel with a separate
coverage byte per texel: 0 is unseen, 1 is synthetic UV-space inpaint, and 2 is
an observed restyled framebuffer sample.  The coverage byte, rather than the
filtered or compressed RGB value, determines whether a texel may be painted.

Each correspondence contains screen position, stable binding ID, mesh UV and
screen gradients, linear framebuffer depth, ray-hit depth, and confidence.
Samples are accepted only when the depths agree. Gradient-bearing samples use
a bounded elliptical footprint; adapters without gradients use a bilinear
fallback. An observed texel is immutable; an inpainted texel may be replaced
later by a real observation. Camera cuts clear screen-space history but retain
the current scene atlas; confirmed new scenes switch to an isolated namespace.

`MaterialTextureBaker` owns the scene's lazily-created atlas set. Capture
adapters submit only trustworthy sparse correspondences; missing materials,
invalid UVs, failed depth tests, and uncovered texels retain the live
screen-space result. The magenta atlas initialization is therefore diagnostic
state only and is never reconstructed into the presented framebuffer.

The D3D11 adapter ranks currently bound sampleable color SRVs as likely base
color sources. Because shader resource layouts are application-defined, the
overlay also stores an SRV-slot override for each pipeline-plus-descriptor
binding. Overrides backed by stable shader and texture fingerprints persist in
the game's ReShade configuration; handle-derived identities remain session-local
to avoid leaking a correction to an unrelated resource after restart. The replay samples only the forced
slot when an override is active and emits no source observation if that slot is
not a compatible texture/sampler pair. Fully transparent samples are discarded
from all capture targets so a common base-color-alpha cutout cannot become a
texture bake observation. A fourth target records `SV_Position.z` only for
fragments that pass the original depth surface with an equal-depth replay; this
provides matched device-depth evidence, while view-space linearization still
requires projection metadata from the adapter.

The D3D11 replacement assembler consumes the baker's conservative mip chain.
For a supported RGBA8/BGRA8 source it clones the complete GPU texture, then
updates contiguous covered runs only; unseen texels and incomplete coarse mip
footprints remain byte-for-byte source data. The adapter temporarily substitutes
that SRV for the application draw, restores the original binding, and performs
capture against the original source. Replacement objects are keyed by the same
pipeline-plus-descriptor identity as their atlas and are cleared on a confirmed
scene namespace change.

The local/world RGB8 planes produced by the StreamDiffusion bridge are optical-
flow coordinates into a prior screen image. They stabilize live generation but
are deliberately not treated as mesh UVs. A graphics adapter must provide a
stable material ID plus interpolated mesh UV and matching depth before a pixel
is eligible for the persistent material baker.

## Display color contract

Backbuffer format is not used as a proxy for transfer function. Swapchain events
record ReShade's declared color space, and capture starts only for a compatible
pair: RGBA/BGRA8 with SDR/unknown, FP16 with linear scRGB, or RGB10A2 with
HDR10/PQ. HLG and contradictory pairs remain bypassed.

The CPU readback decoder converts scRGB and PQ/BT.2020 pixels into bounded sRGB
for the existing inference backends. The final effect converts styled sRGB back
to the proven output encoding. For HDR, it transfers styled chroma and relative
contrast while scaling against the source scene luminance, preventing an SDR
model result from collapsing highlights to SDR white. The portable reference
implementation and shader share the same sRGB, ACES-fit, ST.2084, and gamut
matrices; neutral identity round trips are covered by core tests. Actual HDR
display/game validation is still required before promotion from experimental.

At the live effect boundary, the add-on serializes the observed graphics API,
process architecture, backbuffer dimensions/format, declared swapchain color
space, selected display encoding, and supported/bypassed classification to a
schema-v1 JSON report. The serializer is shared, deterministic, and exact-output
tested; the Windows publisher replaces the report atomically only after a complete
write. This evidence establishes which path executed, not the visual quality or
calibration of an HDR display.

## Graphics API portability

```text
D3D9/10/11 capture ----+
D3D12 capture ---------+--> SurfaceCaptureFrame --> sparse correspondences --> material baker
Vulkan capture --------+
```

Backend adapters own shader instrumentation, resource-state transitions, and
asynchronous GPU readback. They all decode into `SurfaceCaptureFrame`, whose
validation and compaction rules are shared. A backend may leave unsupported
pixels at material ID/confidence zero, allowing mixed supported and unsupported
draws in the same frame. Modern adapters do not stall presentation for readback;
late frames are dropped in favor of the newest complete capture. The legacy D3D9
preview is the explicit exception and reports that limitation because its portable
system-memory transfer is synchronous.

Swapchain teardown with the resize flag releases D3D9 capture resources before
`IDirect3DDevice9::Reset`, as required for default-pool objects. Modern adapters
retain their API attachment and atomically recreate size-dependent surfaces when
the next effect frame observes new backbuffer dimensions. The native D3D9 test
performs a real device `Reset`, recreates capture, and verifies both replacement
replay and fresh readback afterward. Native D3D10/11/12 tests exercise resize-style
reinitialization and verify that stale readback is not returned, then repeat their
complete capture/replacement workloads after destroying the first WARP devices.
Forced device removal remains a separate production gate.

The canonical planes map naturally to every target API: a two-component
unsigned target for the 64-bit stable material ID, a two-component float target
for mesh UV, float depth targets, and a confidence/validity target. The exact GPU
formats and shader-bytecode instrumentation are adapter details rather than
cache-policy details.

Material identity is collected through ReShade's descriptor callbacks rather
than fixed game-specific texture slots. Legacy D3D9/10/11 input layouts and
separately bound shaders are tracked independently; combined D3D12/Vulkan
pipelines use the same final draw state. Only sampled, non-render-target,
non-depth textures participate in the in-session material fingerprint, keeping
transient G-buffers and post-process attachments out of the persistent cache.
Shader bytecode plus bounded, whole-image texture-content fingerprints provide
restart-stable material keys when upload contents are observable. Descriptor or
resource handles are used only to avoid collisions inside the current process;
those keys are never admitted to the cross-launch atlas store.

The binding key also includes input-layout state and every bound vertex/index
buffer's content fingerprint, offset, and stride/index size. This separates
meshes that reuse the same material resources but carry different UV topology.
An immutable initial upload is restart-stable; observing a buffer update converts
that resource to a handle-backed session identity so deforming or streamed
geometry neither pollutes a cross-launch cache nor creates one identity per frame.

### D3D11 replay adapter

Direct and indirect, indexed and non-indexed instanced draws share the same replay
transaction. Indirect arguments remain GPU-resident: the application draw and the
capture replay consume the same argument buffer and offset without a CPU readback.

Replay snapshots the output-merger attachments plus UAVs, depth/stencil and blend
state, the selected source SRV, and the pixel shader including dynamic-linkage class
instances. All are restored after the capture draw; untouched input-assembler,
vertex-stage, rasterizer, viewport, scissor, sampler, and constant-buffer state is
left in place throughout.

The first live adapter uses draw replay rather than modifying the game's pixel
shader. D3D reflection selects a floating-point `TEXCOORD` from the vertex
shader's output signature. NeuralPass executes the original draw exactly once,
then temporarily binds a capture pixel shader, an `RGBA32_UINT` identity/UV
target, an `RGBA32_FLOAT` derivative target, and an `RGBA32_FLOAT` source-color
target while retaining vertex state, resources, viewport, rasterization, and
depth. The capture shader stores the 64-bit binding key, bit-exact interpolated
UV, screen-space UV derivatives, and an aggressively selected sampled 2D source
texel. Missing candidates are encoded as NaN and cannot enter source-transfer baking.
Blend, depth/stencil, render targets, UAVs, and the original pixel shader are
restored before control returns to the game.

### D3D10 replay adapter

The D3D10 adapter follows the D3D11 immediate-context contract with native
Shader Model 4 objects. It reflects the rasterized vertex-shader output, executes
the application draw once, replays direct or indexed geometry into the four
canonical MRT planes, and restores pixel shader, blend, depth-stencil, render
targets, and selected source binding. A three-slot event-query ring makes capture
readback nonblocking. For non-array RGBA8/BGRA8 sources it clones the original
resource and patches only covered atlas runs, keeping replacements isolated by
material identity. D3D10 has no general indirect draw entry point; device-loss
and real-game evidence remain promotion gates.

### D3D9 replay adapter

D3D9 uses a genuinely separate Shader Model 3 path. A bounded token parser accepts
only vertex-shader `dcl_texcoord` outputs, and a legacy pixel shader writes four
floating-point MRTs: four exact 16-bit material-ID chunks, UV/confidence, UV
gradients, and selected source color. The original draw executes once; a full
state block plus explicit render/depth attachment snapshots restore every touched
binding after capture. Native D3D9 topology is reconstructed from ReShade's
dynamic topology event. Indexed replay derives a conservative valid vertex range
from stream zero because D3D9's original min/max index range is absent from the
cross-API draw event.

Replacement is limited to lockable `A8R8G8B8`, `X8R8G8B8`, or `A8B8G8R8`
2D textures. The adapter copies every source mip through system memory, patches
only covered atlas texels, uploads a distinct default-pool texture per material,
and restores the original texture before capture. Portable D3D9 readback is
synchronous, and Shader Model 3 cannot expose post-raster fragment depth here;
both facts are reported rather than hidden behind an asynchronous/depth claim.

### D3D12 replay adapter

The D3D12 preview records complete graphics-pipeline creation metadata and builds
material-specific companion PSOs with the same root signature, vertex stages,
input layout, topology, rasterizer state, and depth format. Its pixel shader writes
the canonical identity/UV, derivative, source, and depth planes. Bounded pixel
descriptor tables are inspected for a sampleable 2D source and sampler, including
D3D12 register spaces; unavailable or unsafe candidates remain NaN. Capture forces
single-sample MRTs, disables blending and depth writes, and uses equal depth testing.

Direct, indexed, and their GPU argument-buffer variants execute the application draw
once and the capture draw once on the same direct command list. The adapter restores
the application's PSO and render/depth attachments. Four capture resources use
explicit render-target/copy-source transitions and a three-slot readback ring with
queue-ordered fences. WARP executes the allocation, barrier, copy, fence, and decode
path. Native render-pass draws are currently rejected rather than illegally changing
attachments inside an active pass. For bounded descriptor tables, RGBA8/BGRA8
replacement clones the complete table into add-on-owned storage, updates only the
selected SRV, binds it for the application draw, then restores the original table
before capture. Source-state tracking and covered-row uploads preserve untouched
texels. WARP verifies native descriptor isolation and restoration. An actual backend
replay test verifies application draw, companion draw, PSO
restoration, and render-target restoration. Production-created companion PSOs,
DXIL-specific reflection beyond the input-declaration fallback, device-loss stress,
and real-game validation remain required before D3D12 can be promoted.

### Vulkan replay adapter

The Vulkan scaffold retains application SPIR-V and fixed-function pipeline
metadata at pipeline creation. Before creating a companion pipeline, it locates
the plain 32-bit float2 vertex input at the inferred input-layout location. It
adds a dedicated output varying at an unused location guaranteed by Vulkan's
minimum vertex-output limit, registers that output with the selected entry point,
and stores the real UV on every return. The embedded capture fragment module has
one reserved input decoration that is patched to the new varying. Malformed or
ambiguous modules, interface blocks, missing entry points, and exhausted output
locations are rejected instead of receiving guessed UV data. Material identity
is supplied through specialization constants, while the application's original
vertex specialization constants are retained.

When the selected base-color candidate is scalar, embedded combined-sampler and
separate-image/sampler fragment variants patch their descriptor-set and binding
decorations to the application's existing layout. They sample with the captured
UV derivatives and reject fully transparent texels. The separate variant keeps
the image and sampler locations independent instead of inferring a relationship
between their bindings. Descriptor arrays currently emit NaN source color and
remain outside source-transfer baking.

Replacement is deliberately narrower than capture. For a non-array RGBA8/BGRA8
source created with transfer-source usage and held in a bounded descriptor table
without dynamic offsets, the adapter clones the complete source image, uploads
only covered atlas row runs, and clones the complete descriptor table before
changing its selected image view. The shadow table is bound only for the
application draw and the original is restored before capture replay. Images that
cannot legally be copied, unbounded/array descriptors, and unsupported formats
trip a per-material/source circuit breaker and retain screen-space output.

The current transformer targets vertex-to-fragment pipelines. Tessellation,
geometry, and mesh-shader pipelines stay on screen-space fallback because their
final pre-raster stage must be instrumented instead of assuming a vertex output
will propagate through intermediate stages.

For accepted direct, indexed, and single-command indirect draws, the adapter
executes the application draw once, binds four canonical capture targets, replays
with equal depth testing and writes disabled, then restores the application
pipeline and render/depth attachments. Capture targets are explicitly transitioned
into a three-slot buffer ring on ReShade's immediate command list; generic queue
fences expose only completed slots without a normal-path CPU wait. Native
render-pass draws, MSAA targets, and general source/replacement descriptor layouts,
forced device-loss recovery, SwiftShader coverage, and real-driver
game evidence remain promotion gates. The native x86/x64 runtime suite destroys
its first logical device after a complete capture/replacement/readback workload,
creates a fresh device, and repeats the workload to prove resource recreation.
The checked-in SPIR-V is generated from
`addon/shaders/vulkan_capture.frag`; a platform-neutral test instruments a real
compiled vertex fixture, checks failure cases and the fragment-location link, and
the transformed module passes SPIR-V Tools validation during development.

Capture uses equality depth testing with writes disabled, so replayed fragments
must match the surface written by the original draw. Three staging textures and
event queries provide readback without flushing or waiting. Each staging slot
carries the same monotonically increasing frame index as the framebuffer copy;
the worker only bakes matching pairs. Shader linkage registers are reflected
again after compiling the capture shader—if its UV register does not exactly
match the game's vertex output, the draw is rejected rather than mispainted.
The WARP integration test covers indexed and non-indexed draws, decoded IDs and
UVs, asynchronous readback, and restoration of the pixel shader, render target,
blend state, and depth/stencil state. Since replay replaces the application's
pixel shader, alpha-test and `discard` behavior cannot yet be mirrored; those
materials remain a known compatibility gap until shader instrumentation exists.

The atlas store serializes color, observation weight, and the three-state
coverage plane. Each payload is versioned and checksummed. Saves publish a new
uniquely named generation only after the temporary file is complete; startup
loads the newest valid generation per material and isolates corrupt or partial
files. Camera cuts only bump the in-flight epoch and do not touch this store.

## Reveal-only generation and cuts

The live material path is a two-phase operation. `plan` reprojects every
covered atlas sample into the current framebuffer and emits a reveal mask only
for visible, depth-verified UVs without real coverage. That boundary is split
into disjoint first-observation and newly-visible masks using the live visibility
classification. Neural work uses the masks plus a context halo, and later
disocclusion/front-face/off-screen reveals preempt bulk initial styling when the
tile budget is constrained. `commit` verifies that the two masks are disjoint and
their union exactly matches the original reveal boundary before accepting any
pixel, so ordinary camera motion never restyles established material texels and
unsupported pixels remain on the live screen-space fallback.

Tile admission is governed by an asymmetric frame-time controller in the worker.
The render callback only records cadence and submits bounded readbacks; it never
waits for inference. A missed target, expensive tile near the target, or dropped
capture backlog removes one admitted tile immediately, down to zero. Recovery is
deliberately slower: 30 consecutive observations below 90% of the target admit
one additional tile, and a suspended worker probes with one tile first. This is
a conservative GPU-headroom proxy rather than a vendor-specific utilization
query, so it works identically for x64 in-process and x86-to-x64 worker paths.
Newly-visible inpainting retains priority within whatever budget is admitted.

Every asynchronous plan carries both a baker epoch and scene key. An image-space
cut bumps the epoch immediately, resets optical-flow/temporal history, and
rejects older in-flight results. A bounded scene profile then classifies the
transition with hysteresis. Its canonical material IDs already encode pipeline,
descriptor binding, source content, and immutable geometry; weak identity overlap
must also agree in quantized UV occupancy, paired capture/hit depth, and sampled
source color. Conflicting evidence enters quarantine and may not read or mutate
either scene. Several confirming frames switch to a checksummed persistent scene
namespace. Missing optional UV/depth/color evidence is omitted and renormalized,
so older adapters remain conservative instead of inventing a mismatch.
