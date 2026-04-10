# DFN3 OBS Noise Suppress Plugin

Native C++ OBS audio filter plugin for microphone noise suppression using DeepFilterNet3 through the libDF C API.

This build is configured to use the standard DFN3 model archive:
- DeepFilterNet3_onnx.tar.gz

## What It Does

- Registers a native OBS audio filter source.
- Processes audio on a worker thread with bounded queues.
- Uses stateful 480-sample hop processing (48 kHz model rate).
- Applies timestamp correction for internal buffering delay.
- Uses overflow-safe bypass and runtime reset behavior to protect stream stability.

## Prerequisites (Windows)

1. OBS Studio installed (default path: C:\Program Files\obs-studio).
2. Visual Studio 2022 build tools (x64 C++).
3. CMake 3.24+.
4. OBS source/build tree containing libobs headers and libraries (example: C:\dev\obs-studio).

## Quick Start (Windows)

Run PowerShell as Administrator if deploying into Program Files.

From this repository root:

- .\Build-And-Deploy.ps1 -ObsSdkRoot "C:\dev\obs-studio"

If OBS is installed elsewhere:

- .\Build-And-Deploy.ps1 -ObsSdkRoot "C:\dev\obs-studio" -ObsRoot "D:\Apps\obs-studio"

What the script does:
- resolves OBS SDK/build paths and configures CMake
- builds the plugin DLL
- bootstraps runtime assets (model and runtime library)
- downloads DeepFilterNet3_onnx.tar.gz
- ensures deepfilter.dll exists (copies local file or builds from DeepFilterNet via cargo-c)
- deploys plugin and assets into OBS plugin folders

## First Use in OBS

1. Restart OBS if it was open during deployment.
2. Open your microphone source.
3. Go to Filters -> Audio Filters -> add DeepFilterNet3 Noise Suppress.
4. Keep Custom Model Path and Custom DeepFilter Library empty unless overriding defaults.
5. Start with defaults and tune only if needed:
- Attenuation Limit (dB)
- Post Filter Beta
- Enable Adaptive Queue

## Manual Build + Deploy

If you prefer step-by-step commands:

1. Configure:
- .\scripts\Resolve-ObsSdk.ps1 -ObsRoot "C:\dev\obs-studio" -Configure

2. Build:
- cmake --build build --config Release

3. Deploy:
- .\scripts\Deploy-To-OBS.ps1 -ObsRoot "C:\Program Files\obs-studio"

## Runtime Asset Locations

After deployment, expected files are:

- C:\Program Files\obs-studio\obs-plugins\64bit\obs-dfn3-noise-suppress.dll
- C:\Program Files\obs-studio\data\obs-plugins\obs-dfn3-noise-suppress\deepfilter.dll
- C:\Program Files\obs-studio\data\obs-plugins\obs-dfn3-noise-suppress\models\DeepFilterNet3_onnx.tar.gz

## Troubleshooting

- Access denied writing into OBS folders:
  Run PowerShell as Administrator, or deploy to a writable OBS root path.

- deepfilter.dll build/bootstrap fails:
  Install Rust and cargo, then rerun. You can also pass -DeepFilterDllPath manually to deploy script.

- OBS SDK not found:
  Build libobs in OBS source tree first, then rerun Resolve-ObsSdk or Build-And-Deploy.

## Related Files

- scripts/Build-And-Deploy.ps1
- Build-And-Deploy.ps1
- scripts/Deploy-To-OBS.ps1
- scripts/Bootstrap-DeepFilterAssets.ps1
- scripts/Resolve-ObsSdk.ps1
