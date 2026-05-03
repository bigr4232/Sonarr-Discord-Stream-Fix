# Building MuteDiscordDevice

## Build from source

**Prerequisites:**
- Windows 10/11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- [CMake 3.20+](https://cmake.org/download/) (only needed for the CMake build)
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (only needed to build the installer)

### Option A — CMake (works from any shell)

```
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
```

Output: `build-cmake\Release\MuteDiscordDevice_Config.exe`. (This is the layout the installer and CI release workflow expect.)

### Option B — MSBuild

Open **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu (a regular `cmd`/PowerShell/bash won't have `msbuild` on PATH), then from the repo root run:

```
msbuild MuteDiscordDevice_Config.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\MuteDiscordDevice_Config.exe`.

### Option C — Visual Studio GUI

Open `MuteDiscordDevice_Config.sln` in Visual Studio 2022, set the configuration to **Release / x64**, and Build Solution (Ctrl+Shift+B).

**Build the installer:**

The installer reads the app version from the built exe's PE metadata, which is generated from `version.h` at compile time. **You must rebuild after bumping `version.h`** — otherwise iscc will package a stale exe and name the installer after the old version.

It packages the exe from the CMake output (`build-cmake\Release\`), so run Option A first, then:
```
iscc installer.iss
```

To package an MSBuild/Visual Studio build (`x64\Release\`) instead, override the build dir — but note that a stale `x64\Release\` exe from a previous build will produce an installer named after the old version:
```
iscc /DBuildDir=x64\Release installer.iss
```

Output: `Output\MuteDiscordDevice-Setup-<app version>.exe`.

## Running tests

The test suite covers all pure (side-effect-free) functions extracted from `main.cpp` into
`mdd_pure.cpp`. It requires no special setup beyond the normal build prerequisites.

```
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
cd build-cmake && ctest -C Release --output-on-failure
```

Or run the test binary directly:
```
build-cmake\tests\Release\MuteDiscordDeviceTests.exe
```

Tests cover:
- **Session fingerprint extraction** - stability across Discord app version updates
- **Config serialization round-trips** - parse/serialize for all filter modes
- **Device name matching** - default mute targets (Sonar mic, Arctis Nova)
- **Utility functions** - HRESULT formatting, GUID formatting, device ID packing
- **Filter display strings** - human-readable summaries for each filter mode
- **DeviceConfig defaults** - struct initialization and copy semantics
- **File I/O round-trips** - route target and config file persistence

## Release process (maintainers)

1. Bump the version in `version.h` (three macros + `MDD_VERSION_STRING`).
2. Commit.
3. Tag: `git tag v1.2.3 && git push --tags`.
4. The `.github/workflows/release.yml` workflow builds the exe, compiles the installer, and attaches it to a GitHub release named after the tag.

The workflow fails fast if `version.h` doesn't match the tag, so bump first, tag second.
