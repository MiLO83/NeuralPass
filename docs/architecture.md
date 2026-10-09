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
draws in the same frame. No adapter is allowed to stall presentation for a
readback; late frames are dropped in favor of the newest complete capture.

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
for visible, depth-verified UVs without real coverage. Neural inpainting uses
that mask plus a context halo. `commit` accepts pixels only from the original
reveal mask, so ordinary camera motion never restyles established material
texels and unsupported pixels remain on the live screen-space fallback.

Every asynchronous plan carries both a baker epoch and scene key. An image-space
cut bumps the epoch immediately, resets optical-flow/temporal history, and
rejects older in-flight results. Binding overlap then classifies the transition
with hysteresis: shared identities mean a camera cut; foreign identities enter
quarantine and may not read or mutate either scene. Several confirming frames
switch to a checksummed persistent scene namespace. If geometry capture is
missing, the conservative behavior is to reset screen history and preserve the
current texture namespace.
