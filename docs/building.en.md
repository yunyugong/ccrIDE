# Building on Windows

ccrIDE currently targets Windows x64 with MinGW-w64. Other platforms and MSVC are not supported by the current CMake configuration.

Requirements: CMake 3.24+, MinGW-w64 GCC, matching static zlib, and these dependencies:

- Dear ImGui **1.92.9b docking**, including `backends/`.
- GLFW **3.4**, headers and a matching static `libglfw3.a` (or a CMake GLFW package).
- nlohmann/json headers.
- Python 3 when running tests, generating catalogs, or packaging.

Put dependencies under `dependencies/imgui`, `dependencies/glfw`, and `dependencies/nlohmann`, or specify `CCRIDE_DEPS_DIR`. Individual overrides are `IMGUI_DIR`, `GLFW_DIR` and `NLOHMANN_DIR`. GLFW also supports `find_package(glfw3 CONFIG)` and `CMAKE_PREFIX_PATH`. zlib uses `find_package(ZLIB)` with optional `ZLIB_ROOT` or explicit library/include overrides. The project does not download dependencies automatically. Match architectures and compiler runtimes, and use the same ImGui version for the host and UI plugins.

With CMake and the compiler on your shell PATH:

```text
cmake -S . -B build/cmake -G "MinGW Makefiles" -DBUILD_TESTING=ON
cmake --build build/cmake -j 4
ctest --test-dir build/cmake --output-on-failure
```

Set `CMAKE_CXX_COMPILER` to choose a compiler and use a fresh build directory when changing toolchains. Set `Python3_EXECUTABLE` if needed. `build.ps1` wraps this same configuration; it accepts compiler/dependency/build-directory options and `-Test`. Disable `BUILD_TESTING` to build without the test dependency on Python.

The internal application target is still named `MiniCPP`; release executables are named **ccrIDE** and written to `build/releases/`. Existing artifacts are preserved with a serial suffix on rebuild. Version metadata includes build time; executable filenames do not. The native `ccride_installer.exe` is only a stub until packaging appends its payload.

For a self-contained clangd plugin, set `CCRIDE_CLANGD_BUNDLE` to an official clangd distribution root containing `bin/clangd.exe`, resource files and licenses. The whole distribution is copied into the plugin's `toolchain/`. No clangd download happens during a normal build. Tests use `CCRIDE_TEST_CLANGD` or discovery; a missing clangd skips its suite rather than passing it.

Generate translations before rebuilding if the JSON changed:

```text
python tools/generate-locales.py
python tools/generate-locales.py --check
```

Offline packaging uses `packaging/package-portable.py`. Supply a fresh `--stage`, `--output`, explicit MinGW root with `--compiler`, a settings file with `--settings`, and dependency sources/licenses with `--dependencies`. Packaging runs a relocation check using the staged compiler with development PATH entries removed. It removes validation task logs before writing the archive manifest. Plugin packaging and GitHub catalog generation are described in [Community hosting](community.md).
