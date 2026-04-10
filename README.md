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

## 3. How To Use Prebuilt Binaries

If you do not want to build from source, install a release package from this repository:

1. Open the GitHub Releases page for this project.
2. Download the archive for your platform:
  - Windows: `*-windows-x64.zip`
  - Linux: `*-linux-x64.tar.xz`
  - macOS: `*-macos.tar.xz`
3. Extract the archive.
4. Copy the extracted `obs-plugins/...` and `data/...` folders into your OBS installation prefix.
5. Restart OBS.
6. Add the filter named "DeepFilterNet3 Noise Suppress" to your microphone source.

The release package already includes the plugin binary, runtime library, and `DeepFilterNet3_onnx.tar.gz` model in the expected layout.

## 4. Prerequisites

Before building this plugin, you need:

1. CMake 3.24 or newer
2. A C++20-capable compiler toolchain
3. OBS development artifacts (headers and libraries for `libobs`)
4. OBS Studio installed (runtime target)
5. DeepFilter runtime library file
6. DeepFilterNet3 model archive (`DeepFilterNet3_onnx.tar.gz`)

## 5. How To Meet The Prerequisites

### 5.1 Install build tools

- Windows: Visual Studio 2022 Build Tools (Desktop C++) + CMake
- Linux: GCC or Clang + CMake + standard build tools
- macOS: Xcode Command Line Tools + CMake

### 5.2 Get OBS development artifacts (`libobs` headers and libraries)

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

### 5.3 Quick validation that your OBS dev path is correct

Your OBS dev path should contain:

1. `obs-module.h` (usually under `libobs/` or `include/`)
2. `obsconfig.h` (usually under `build/config/`)
3. built `libobs` library output

### 5.4 Get DeepFilter runtime library and model

You must place these runtime assets into the OBS plugin data directory:

- Runtime library filename:
  - Windows: `deepfilter.dll`
  - Linux: `libdeepfilter.so`
  - macOS: `libdeepfilter.dylib`
- Model archive filename:
  - `DeepFilterNet3_onnx.tar.gz`

## 6. Path Explanation

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

## 7. How To Build

Run commands from `<PROJECT_ROOT>`.

### 7.0 Recommended: CMake Presets

Set `OBS_ROOT_DIR` in your shell first:

Windows (PowerShell):

```powershell
$env:OBS_ROOT_DIR="C:/path/to/obs-studio"
```

Linux/macOS:

```bash
export OBS_ROOT_DIR=/path/to/obs-studio
```

List available presets:

```bash
cmake --list-presets
```

Configure/build with presets:

- Windows (Visual Studio):

```powershell
cmake --preset vs2022-x64-obs
cmake --build --preset vs2022-x64-release
```

- Linux:

```bash
cmake --preset linux-make-obs
cmake --build --preset linux-release
```

- macOS:

```bash
cmake --preset macos-make-obs
cmake --build --preset macos-release
```

- Generic cross-platform (requires Ninja):

```bash
cmake --preset ninja-obs-env
cmake --build --preset ninja-release
```

### 7.1 Configure

Windows (PowerShell):

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOBS_ROOT_DIR=C:/path/to/obs-studio
```

Linux/macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOBS_ROOT_DIR=/path/to/obs-studio
```

If `libobsConfig.cmake` is already discoverable, you can omit `-DOBS_ROOT_DIR=...`.

### 7.2 Compile

Windows:

```powershell
cmake --build build --config Release
```

Linux/macOS:

```bash
cmake --build build
```

### 7.3 Install into OBS

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

### 7.4 Copy required runtime assets

After install, ensure these files exist under `<OBS_PREFIX>`:

1. Plugin binary:
  - Windows: `obs-plugins/64bit/obs-dfn3-noise-suppress.dll`
  - Linux: `obs-plugins/obs-dfn3-noise-suppress.so`
  - macOS: `obs-plugins/obs-dfn3-noise-suppress.dylib`
2. Runtime library:
  - `data/obs-plugins/obs-dfn3-noise-suppress/<platform runtime library>`
3. Model:
  - `data/obs-plugins/obs-dfn3-noise-suppress/models/DeepFilterNet3_onnx.tar.gz`

## 8. How To Use The Plugin In OBS

1. Start or restart OBS.
2. Open the source you want to clean (usually microphone input).
3. Open Filters for that source.
4. Add an Audio Filter and select "DeepFilterNet3 Noise Suppress".
5. The plugin uses the packaged standard DFN3 model/runtime automatically.
6. Speak and confirm noise suppression is active.
7. Tune only if needed:
  - Attenuation Limit (dB)
  - Post Filter Beta
  - Enable Adaptive Queue

The filter properties panel includes a runtime status block showing readiness, queue target, counters, and last error.

If the filter appears but audio is not processed, verify section 7.4 file paths first.

## 9. License And Attributions

### 9.1 This project license

- This repository is licensed under the MIT License.
- See the `LICENSE` file at the repository root.

### 9.2 Upstream model/runtime project

- Project: https://github.com/rikorose/deepfilternet
- Upstream code license: dual-licensed under either MIT or Apache License 2.0 (at your option), per upstream license files.

### 9.3 Research paper
- Paper: DeepFilterNet: Perceptually Motivated Real-Time Speech Enhancement
- Authors: Hendrik Schroter, Tobias Rosenkranz, Alberto N. Escalante-B., Andreas Maier
- arXiv: https://arxiv.org/abs/2305.08227
- DOI: https://doi.org/10.48550/arXiv.2305.08227

## 10. GitHub Release Binaries (Windows/Linux/macOS)

This repository includes a release workflow at `.github/workflows/release.yml` that:

1. Builds plugin binaries for Windows, Linux, and macOS.
2. Builds the DeepFilter runtime library from DeepFilterNet source for each platform.
3. Copies `DeepFilterNet3_onnx.tar.gz` from DeepFilterNet source into the package.
4. Packages OBS-ready archives.
5. Attaches archives and checksums to a GitHub Release when a version tag is pushed.

### 10.1 Repository variables

No runtime/model URL variables are required.

Optional repository variables:

1. `OBS_STUDIO_REF` (default in workflow: `31.0.2`)
2. `DEEPFILTERNET_REF` (default in workflow: `v0.5.6`)

### 10.2 Triggering a release build

Tag-based release (recommended):

```bash
git tag v0.1.0
git push origin v0.1.0
```

This triggers the workflow, then publishes packaged binaries to the release for that tag.

Manual run:

1. Open GitHub Actions.
2. Run `Build And Release Binaries` via workflow dispatch.

### 10.3 Artifact layout

Each packaged archive is OBS-ready and contains:

1. Plugin binary in `obs-plugins/...`
2. Plugin data in `data/obs-plugins/obs-dfn3-noise-suppress/...`
3. DeepFilter runtime library with expected platform filename
4. `DeepFilterNet3_onnx.tar.gz` model archive
