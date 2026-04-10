# DFN3 OBS Noise Suppress Plugin

Native C++ OBS audio filter plugin for microphone noise suppression using DeepFilterNet3 through the libDF C API.

This plugin is configured for the standard model archive:
- DeepFilterNet3_onnx.tar.gz

## Read This First

To run this plugin, OBS needs three things:

1. The plugin binary (`obs-dfn3-noise-suppress`)
2. The DeepFilter runtime library (`deepfilter.dll`, `libdeepfilter.so`, or `libdeepfilter.dylib`)
3. The model archive (`DeepFilterNet3_onnx.tar.gz`)

If any of these are missing, the filter can appear in OBS but not process audio correctly.

## What "in the OBS install tree" Means

There are two different locations involved:

1. Project folder (this repository): where you build the plugin
2. OBS install folder: where OBS loads plugins and plugin data at runtime

When this README says:

"Model archive at `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz` in the OBS install tree"

it means the final path must be inside your OBS install folder, for example:

- `<OBS_PREFIX>/data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

Common OBS prefix examples:

- Windows: `C:/Program Files/obs-studio`
- Linux: `/usr` (system install) or custom prefix
- macOS app bundle installs: prefix path depends on your local OBS layout/build

## Platform Scope

- Windows
- Linux
- macOS

## Prerequisites (Build-Time)

1. CMake 3.24 or newer
2. C++20 compiler toolchain
3. OBS development artifacts (libobs headers and libraries), available by either:
- `libobsConfig.cmake` in your CMake search path, or
- passing `OBS_ROOT_DIR` to an OBS source/build root

## Build

Run from repository root.

### Windows (PowerShell)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOBS_ROOT_DIR=C:/path/to/obs-studio
cmake --build build --config Release
```

### Linux/macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOBS_ROOT_DIR=/path/to/obs-studio
cmake --build build
```

If your environment already finds `libobsConfig.cmake`, you can omit `-DOBS_ROOT_DIR=...`.

## Install Into OBS

Install to your OBS prefix:

### Windows

```powershell
cmake --install build --config Release --prefix "C:/Program Files/obs-studio"
```

### Linux/macOS

```bash
cmake --install build --prefix /path/to/obs-prefix
```

Install result:

- Plugin binary goes to `obs-plugins` (`obs-plugins/64bit` on Windows)
- Plugin data folder goes to `data/obs-plugins/obs-dfn3-noise-suppress`

## Add Runtime Assets (Required)

After install, place these files in OBS data path:

- Runtime library file in:
  `data/obs-plugins/obs-dfn3-noise-suppress/`
- Model archive file in:
  `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

Runtime library filename by platform:

- Windows: `deepfilter.dll`
- Linux: `libdeepfilter.so`
- macOS: `libdeepfilter.dylib`

## Verify Files Before Launching OBS

Check these exist under your OBS prefix:

1. Plugin binary
- Windows: `obs-plugins/64bit/obs-dfn3-noise-suppress.dll`
- Linux: `obs-plugins/obs-dfn3-noise-suppress.so`
- macOS: `obs-plugins/obs-dfn3-noise-suppress.dylib`

2. Runtime library
- `data/obs-plugins/obs-dfn3-noise-suppress/<platform runtime library>`

3. Model archive
- `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

## First Use In OBS

1. Restart OBS.
2. Open your microphone source.
3. Open Filters.
4. Add audio filter: DeepFilterNet3 Noise Suppress.
5. Leave Custom Model Path and Custom DeepFilter Library empty unless you are intentionally overriding defaults.
6. Start with default values, then tune:
- Attenuation Limit (dB)
- Post Filter Beta
- Enable Adaptive Queue

## Troubleshooting

- Configure fails with "Could not locate libobs":
  Pass `-DOBS_ROOT_DIR=/path/to/obs-studio` or set `CMAKE_PREFIX_PATH` so CMake can find `libobsConfig.cmake`.

- Filter is visible in OBS but has no denoise effect:
  Confirm both runtime library and model archive exist in the plugin data folder under OBS prefix.

- Wrong model/library loaded:
  Clear Custom Model Path and Custom DeepFilter Library in filter settings to use default packaged paths.

## Repository Files

- CMakeLists.txt
- src/plugin-main.cpp
- src/dfn3_filter.cpp
- src/df_runtime.cpp
- data/models/README.md
