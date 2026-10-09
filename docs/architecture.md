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

Each correspondence contains screen position, stable material ID, mesh UV,
linear framebuffer depth, and ray-hit depth.  Samples are accepted only when
the depths agree.  An observed texel is immutable; an inpainted texel may be
replaced later by a real observation.  Scene cuts clear screen-space temporal
history but never discard persistent material atlases.

`MaterialTextureBaker` owns the scene's lazily-created atlas set. Capture
adapters submit only trustworthy sparse correspondences; missing materials,
invalid UVs, failed depth tests, and uncovered texels retain the live
screen-space result. The magenta atlas initialization is therefore diagnostic
state only and is never reconstructed into the presented framebuffer.

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

Every asynchronous plan carries a baker epoch. An image-space camera cut bumps
the epoch immediately, resets optical-flow/temporal history, and rejects older
in-flight results while retaining material atlases. Material-ID overlap then
classifies the transition with hysteresis: shared identities mean a camera cut;
several consecutive frames containing only foreign identities confirm a true
scene change and reset the active atlas cache. If geometry capture is missing,
the conservative behavior is to reset screen history but preserve textures.
