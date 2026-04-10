# DFN3 OBS Noise Suppress Plugin

## 1. What This Project Is

This project is a native C++ audio filter plugin for OBS Studio.

It adds a filter named "DeepFilterNet3 Noise Suppress" that processes microphone audio using the DeepFilterNet runtime (`libDF` C API) and the standard DFN3 model archive (`DeepFilterNet3_onnx.tar.gz`).

This is not a standalone desktop app. It is a plugin that must be installed into an OBS Studio installation.

## 2. Supported Platforms

The codebase is intended for:

- Windows
- Linux
- macOS

Build and runtime success depend on having platform-matching OBS development files and runtime dependencies.

## 3. Prerequisites

Before building this plugin, you need:

1. CMake 3.24 or newer
2. A C++20-capable compiler toolchain
3. OBS development artifacts (headers and libraries for `libobs`)
4. OBS Studio installed (runtime target)
5. DeepFilter runtime library file
6. DeepFilterNet3 model archive (`DeepFilterNet3_onnx.tar.gz`)

## 4. How To Meet The Prerequisites

### 4.1 Install build tools

- Windows: Visual Studio 2022 Build Tools (Desktop C++) + CMake
- Linux: GCC or Clang + CMake + standard build tools
- macOS: Xcode Command Line Tools + CMake

### 4.2 Get OBS development artifacts (`libobs` headers and libraries)

If you installed OBS from a normal installer, you usually have runtime files only. For plugin development, you typically need OBS source/build artifacts.

Recommended path for all platforms:

```bash
git clone --recursive https://github.com/obsproject/obs-studio.git
cd obs-studio
cmake -S . -B build
cmake --build build --config RelWithDebInfo
```

After this, use your OBS source root as `OBS_ROOT_DIR` when configuring this plugin.

Linux alternative:

- You may use distro packages that provide `libobs` development files.
- In that case, make sure CMake can locate `libobsConfig.cmake` (directly or via `CMAKE_PREFIX_PATH`).

### 4.3 Quick validation that your OBS dev path is correct

Your OBS dev path should contain:

1. `obs-module.h` (usually under `libobs/` or `include/`)
2. `obsconfig.h` (usually under `build/config/`)
3. built `libobs` library output

### 4.4 Get DeepFilter runtime library and model

You must place these runtime assets into the OBS plugin data directory:

- Runtime library filename:
  - Windows: `deepfilter.dll`
  - Linux: `libdeepfilter.so`
  - macOS: `libdeepfilter.dylib`
- Model archive filename:
  - `DeepFilterNet3_onnx.tar.gz`

## 5. Path Explanation

This README uses these path placeholders:

- `<PROJECT_ROOT>`: this repository folder
- `<OBS_PREFIX>`: root of your OBS installation
- `<BUILD_DIR>`: your local CMake build directory (usually `<PROJECT_ROOT>/build`)

When you see:

`data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

that path is relative to `<OBS_PREFIX>`, so the full runtime path is:

`<OBS_PREFIX>/data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

Common `<OBS_PREFIX>` examples:

- Windows: `C:/Program Files/obs-studio`
- Linux: `/usr` (or a custom prefix)
- macOS: your OBS app/install prefix used for plugin loading

## 6. How To Build

Run commands from `<PROJECT_ROOT>`.

### 6.1 Configure

Windows (PowerShell):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOBS_ROOT_DIR=C:/path/to/obs-studio
```

Linux/macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOBS_ROOT_DIR=/path/to/obs-studio
```

If `libobsConfig.cmake` is already discoverable, you can omit `-DOBS_ROOT_DIR=...`.

### 6.2 Compile

Windows:

```powershell
cmake --build build --config Release
```

Linux/macOS:

```bash
cmake --build build
```

### 6.3 Install into OBS

Windows:

```powershell
cmake --install build --config Release --prefix "C:/Program Files/obs-studio"
```

Linux/macOS:

```bash
cmake --install build --prefix /path/to/obs-prefix
```

Install output behavior from this project:

- Plugin binary goes to `obs-plugins` (`obs-plugins/64bit` on Windows)
- Plugin data directory goes to `data/obs-plugins/obs-dfn3-noise-suppress`

### 6.4 Copy required runtime assets

After install, ensure these files exist under `<OBS_PREFIX>`:

1. Plugin binary:
  - Windows: `obs-plugins/64bit/obs-dfn3-noise-suppress.dll`
  - Linux: `obs-plugins/obs-dfn3-noise-suppress.so`
  - macOS: `obs-plugins/obs-dfn3-noise-suppress.dylib`
2. Runtime library:
  - `data/obs-plugins/obs-dfn3-noise-suppress/<platform runtime library>`
3. Model:
  - `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

## 7. How To Use The Plugin In OBS

1. Start or restart OBS.
2. Open the source you want to clean (usually microphone input).
3. Open Filters for that source.
4. Add an Audio Filter and select "DeepFilterNet3 Noise Suppress".
5. Leave "Custom Model Path" and "Custom DeepFilter Library" empty for default packaged runtime/model.
6. Speak and confirm noise suppression is active.
7. Tune only if needed:
  - Attenuation Limit (dB)
  - Post Filter Beta
  - Enable Adaptive Queue

If the filter appears but audio is not processed, verify section 6.4 file paths first.

## 8. License And Attributions

### 8.1 This project license

- This repository is licensed under the MIT License.
- See the `LICENSE` file at the repository root.

### 8.2 Upstream model/runtime project

- Project: https://github.com/rikorose/deepfilternet
- Upstream code license: dual-licensed under either MIT or Apache License 2.0 (at your option), per upstream license files.

### 8.3 Research paper
- Paper: DeepFilterNet: Perceptually Motivated Real-Time Speech Enhancement
- Authors: Hendrik Schroter, Tobias Rosenkranz, Alberto N. Escalante-B., Andreas Maier
- arXiv: https://arxiv.org/abs/2305.08227
- DOI: https://doi.org/10.48550/arXiv.2305.08227
