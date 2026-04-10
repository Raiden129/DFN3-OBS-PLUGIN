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

## Platform Scope

- Windows
- Linux
- macOS

Build success depends on having a matching libobs SDK/build environment for your platform.

## Prerequisites

1. CMake 3.24 or newer.
2. A C++20 compiler toolchain for your platform.
3. OBS development artifacts (libobs headers and libraries) available either:
- via CMake package discovery (`libobsConfig.cmake`), or
- by passing `OBS_ROOT_DIR` to your local OBS source/build root.
4. DeepFilter runtime library placed with plugin data:
- Windows: `deepfilter.dll`
- Linux: `libdeepfilter.so`
- macOS: `libdeepfilter.dylib`
5. Model archive at `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz` in the OBS install tree.

## Build

From repository root:

```bash
cmake -S . -B build -DOBS_ROOT_DIR=/path/to/obs-studio
cmake --build build --config Release
```

If your system already exposes `libobsConfig.cmake`, `OBS_ROOT_DIR` can be omitted.

## Install

Install to your OBS root prefix:

```bash
cmake --install build --config Release --prefix /path/to/obs-studio
```

This installs:

- Plugin binary to `obs-plugins` (or `obs-plugins/64bit` on Windows)
- Plugin data to `data/obs-plugins/obs-dfn3-noise-suppress`

## First Use in OBS

1. Restart OBS if it was open during deployment.
2. Open your microphone source.
3. Go to Filters -> Audio Filters -> add DeepFilterNet3 Noise Suppress.
4. Keep Custom Model Path and Custom DeepFilter Library empty unless overriding defaults.
5. Start with defaults and tune only if needed:
- Attenuation Limit (dB)
- Post Filter Beta
- Enable Adaptive Queue

## Troubleshooting

- libobs not found during configure:
  Pass `-DOBS_ROOT_DIR=/path/to/obs-studio` or set `CMAKE_PREFIX_PATH` to where `libobsConfig.cmake` is located.

- Runtime model or library not found:
  Verify `DeepFilterNet3_onnx.tar.gz` and the platform runtime library are present under `data/obs-plugins/obs-dfn3-noise-suppress`.

## Related Files

- CMakeLists.txt
- src/plugin-main.cpp
- src/dfn3_filter.cpp
- src/df_runtime.cpp
- data/models/README.md
