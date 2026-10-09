# Compatibility and troubleshooting

NeuralPass 0.1 is an unsigned research preview for offline Windows games using a
full add-on build of ReShade. It is not suitable for protected multiplayer games.

## Support matrix

| Area | Current status |
| --- | --- |
| Windows / x64 | Automated build, package, core/SPIR-V tests, and D3D11/D3D12 WARP tests |
| D3D11 | Experimental geometry capture and RGBA8/BGRA8 replacement |
| D3D9 / D3D10 | Screen-space effect only; geometry adapter not implemented |
| D3D12 | Experimental PSO replay, source sampling, bounded-table RGBA8/BGRA8 replacement, barriers, and fence/readback ring; real-game evidence pending |
| Vulkan | Experimental SPIR-V UV/source capture and transfer-source RGBA8/BGRA8 bounded-table replacement; no driver/game evidence |
| SDR RGBA8 | Supported preview path |
| HDR / scRGB | Not supported correctly; leave NeuralPass disabled |
| DirectML | Model execution smoke-tested on NVIDIA; AMD and Intel hardware validation remains |
| CUDA | Legacy Python bridge only; not part of the managed package |
| x86 games | Not packaged or validated |

Unsupported capture paths fall back to the screen-space compositor. They do not
guess mesh UVs or write guessed data into persistent material atlases.

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
you also want the local NeuralPass cache removed. Package documentation and
notices are installed below the game's `NeuralPass` directory so generic game
files such as `README.md` and `LICENSE.txt` are never overwritten. Installation
also stops before copying if an unowned destination file would be replaced.

## Diagnostics

Run `Diagnose NeuralPass.cmd` beside the game executable. It creates
`NeuralPass-diagnostics.txt` containing:

- Windows, process architecture, display adapter, and driver information;
- detected graphics proxy/runtime files with sizes and SHA-256 hashes;
- package and installation-manifest presence;
- known safety markers at the game root; and
- the last 120 lines of `ReShade.log` when available.

Review the report before sharing it. It intentionally avoids environment variables,
user tokens, registry dumps, process lists, and arbitrary files.

## Troubleshooting order

1. Confirm the game is x64 D3D11 (or an explicitly tested D3D12/Vulkan title) and running
   without anti-cheat or protected multiplayer.
2. Confirm ReShade itself opens and its Add-ons tab lists NeuralPass.
3. Confirm `NeuralPass.addon64` is beside the active ReShade proxy DLL.
4. Confirm `reshade-shaders/Shaders/NeuralPass.fx` exists and compiles in ReShade.
5. Keep **NeuralPass (keep last)** last in technique order.
6. For a DirectML package, confirm `onnxruntime.dll`,
   `onnxruntime_providers_shared.dll`, `DirectML.dll`, and the selected `.onnx` model
   are present.
7. Generate diagnostics and inspect `ReShade.log` for add-on load or shader errors.

If geometry capture reports unsupported draws, the screen-space path should continue
working. Do not treat unsupported counters as permission to enable the add-on in a
protected game.
