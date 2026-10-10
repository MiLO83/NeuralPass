# Compatibility and troubleshooting

NeuralPass 0.1 is an unsigned research preview for offline Windows games using a
full add-on build of ReShade. It is not suitable for protected multiplayer games.

## Support matrix

| Area | Current status |
| --- | --- |
| Windows / x64 | Automated DirectML build/package plus native D3D9/10/11/12 and Vulkan tests |
| Windows / x86 | Automated worker-backed DirectML build/package; 32-bit D3D9/10/11/12/Vulkan suites and x86-to-x64 ONNX transport pass |
| D3D9 | Experimental SM3 direct/indexed capture; synchronous readback, no fragment depth, lockable 32-bit replacement only |
| D3D10 | Experimental direct/indexed geometry capture and RGBA8/BGRA8 replacement; fresh-device WARP recreation passes |
| D3D11 | Experimental geometry capture and RGBA8/BGRA8 replacement; fresh-device WARP recreation passes |
| D3D12 | Experimental PSO replay, source sampling, bounded-table RGBA8/BGRA8 replacement, barriers, and fence/readback ring; fresh-device WARP recreation passes, real-game evidence pending |
| Vulkan | Experimental SPIR-V UV/source capture and transfer-source RGBA8/BGRA8 bounded-table replacement; x86/x64 native capture, replacement isolation, and logical-device recreation pass on NVIDIA, but ReShade/game evidence remains |
| SDR RGBA8 | Supported preview path |
| HDR / scRGB | Experimental FP16 scRGB and RGB10A2 HDR10/PQ paths with declared-color-space validation and luminance-preserving composition; monitor/game validation pending |
| DirectML | Game-device DXGI adapter is selected by LUID; model execution smoke-tested on NVIDIA; packaged evidence test supports explicit adapter indices; AMD and Intel evidence remains |
| CUDA | Legacy Python bridge only; not part of the managed package |
| x86 games | Experimental `addon32` package launches the x64 ONNX/DirectML worker over bounded shared memory; real-game validation pending |

Unsupported capture paths fall back to the screen-space compositor. They do not
guess mesh UVs or write guessed data into persistent material atlases.

HDR capture is enabled only when both pieces of evidence agree: FP16 with a
declared scRGB swapchain, or RGB10A2 with declared HDR10/PQ. NeuralPass converts
that signal to bounded sRGB before inference and converts the styled result back
in the final shader. HLG, unknown color spaces, and mismatched format/color-space
pairs are bypassed rather than treated as plausible HDR.

The legacy prompt-driven StreamDiffusion/WSL bridge is disabled by default and
is not required by either package. Enable it only after separately starting its
Python environment. This is distinct from the packaged `NeuralPassWorker.exe`,
which provides fixed-model ONNX inference to the 32-bit add-on without WSL.

## Safe installation

1. Install ReShade with full add-on support into an offline game.
2. Extract the NeuralPass package to a separate directory.
3. Run `Install NeuralPass.cmd` and select the game executable or directory.
4. Read the warning and type `INSTALL` to activate that one game.
5. In ReShade, enable **NeuralPass (keep last)** and keep it last in technique order.

The installer refuses common anti-cheat markers. That deny list is a safety net,
not a guarantee: never use NeuralPass when a game, launcher, server, or tournament
policy prohibits injection, graphics modifications, unsigned add-ons, or automation.

`Uninstall NeuralPass.cmd` reads `NeuralPass.install.json` and removes only the
files recorded by installation. Pass `-RemoveCache` to its PowerShell script if
you also want the local NeuralPass cache removed. It preserves modified installed
files by default; inspect them and pass `-ForceModified` only when deletion is
intentional. Package documentation and
notices are installed below the game's `NeuralPass` directory so generic game
files such as `README.md` and `LICENSE.txt` are never overwritten. Installation
also stops before copying if an unowned destination file would be replaced.

For an update, extract the new package outside the game and run its
`Update NeuralPass.cmd` against the existing game directory. The updater verifies
every package checksum, requires the installed and incoming architectures to match,
refuses missing or modified owned files unless `-ForceModified` is explicit, and
requires the exact `UPDATE` confirmation. It backs up the prior owned payload,
rolls back a failed replacement, removes files owned only by the old release, and
preserves optional models only when the new manifest recognizes their filename and
exact hash. It does not fetch releases or silently update a game.

## Diagnostics

Run `Diagnose NeuralPass.cmd` beside the game executable. It creates
`NeuralPass-diagnostics.txt` containing:

- Windows, process architecture, display adapter, and driver information;
- detected graphics proxy/runtime files with sizes and SHA-256 hashes;
- package provenance, SPDX SBOM, checksums, and installation-manifest presence;
- installed-file integrity against hashes captured during installation;
- proxy-derived API candidates and runtime API/HDR evidence from `ReShade.log`;
- the latest structured API/backbuffer/color-space classification emitted by the
  loaded add-on in `NeuralPass-runtime-evidence.json`;
- the latest packaged DirectML hardware/provider and worker-health results, when available;
- known safety markers at the game root; and
- the last 120 lines of `ReShade.log` when available.

Review the report before sharing it. It intentionally avoids environment variables,
user tokens, registry dumps, process lists, and arbitrary files.

`NeuralPass-runtime-evidence.json` is refreshed atomically whenever the loaded
ReShade runtime observes a changed API, backbuffer size/format, swapchain color
space, or supported/bypassed classification. A supported HDR entry therefore proves
that NeuralPass saw FP16 with declared scRGB or RGB10A2 with declared HDR10/PQ in
that game session. It does not prove visual quality, display calibration, or a soak
test; those remain hardware/game acceptance gates.

The x64 DirectML package additionally includes `Validate NeuralPass Hardware.cmd`.
It runs the packaged model through DirectML and writes
`NeuralPass-hardware-<device>.json` with adapter/driver metadata and the exact test
result. Before installation, evidence is written below
`%LOCALAPPDATA%\NeuralPass\Evidence` so the checksummed package remains immutable;
after installation, it is written beside the game for diagnostics. The PowerShell
validator also accepts `-OutputDirectory`. Use an optional numeric argument to select a non-default DXGI adapter,
for example `Validate NeuralPass Hardware.cmd 1`. A passing report proves model
execution on that machine; it does not substitute for a game soak test.

Both architectures include `Validate NeuralPass Vulkan.cmd`. It dynamically uses
the installed Vulkan loader, executes the instrumented four-MRT capture plus an
explicit-layout replacement-isolation test, and writes
`NeuralPass-vulkan-runtime.json`. Exit code 77 means the loader or a physical Vulkan
device is absent; a passing report is native driver evidence, not yet ReShade/game
evidence. It follows the same evidence-location rule and accepts `-OutputDirectory`.

Both DirectML packages include `Validate NeuralPass Worker.cmd`. It validates the
named-pipe/shared-memory inference path, generation rollover, forced child exit,
and automatic restart, then writes `NeuralPass-worker-health.json`. The x86 package
uses an x86 evidence client with the packaged x64 worker. DirectML is the default;
pass `-Provider cpu` to isolate transport and recovery from GPU-provider health.
Its report follows the same pre-install and installed evidence-location rule.

After guided installation, `Manage NeuralPass Models.cmd` lists the four pinned
style models and their local integrity state. Use, for example,
`Manage NeuralPass Models.cmd -Action Install -Model mosaic` to download a model,
or `-Action Verify -Model mosaic` to verify it. Downloads come only from the
package's checksummed manifest, are accepted only after their pinned SHA-256
matches, and are atomically placed into `models/downloads`. `-Repair` is required
to replace a corrupt file. Installed and removed models update the installer's
ownership manifest so diagnostics and uninstall remain accurate. The manager
refuses to mutate an uninstalled, checksummed package and refuses to remove the
last verified model.

## Troubleshooting order

1. Confirm the game uses a packaged architecture and supported graphics API, and is running
   without anti-cheat or protected multiplayer.
2. Confirm ReShade itself opens and its Add-ons tab lists NeuralPass.
3. Confirm the architecture-matched `NeuralPass.addon64` or
   `NeuralPass.addon32` is beside the active ReShade proxy DLL.
4. Confirm `reshade-shaders/Shaders/NeuralPass.fx` exists and compiles in ReShade.
5. Keep **NeuralPass (keep last)** last in technique order.
6. For a DirectML package, confirm `onnxruntime.dll`,
   `onnxruntime_providers_shared.dll`, `DirectML.dll`, the isolated
   `NeuralPass/runtime/NeuralPassWorker.exe` and runtime DLL copies, and the selected
   `.onnx` model are present.
7. Generate diagnostics and inspect `ReShade.log` for add-on load or shader errors.

If geometry capture reports unsupported draws, the screen-space path should continue
working. Do not treat unsupported counters as permission to enable the add-on in a
protected game.
