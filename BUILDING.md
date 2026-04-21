# Building MuteDiscordDevice

## Build from source

**Prerequisites:**
- Windows 10/11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- [CMake 3.20+](https://cmake.org/download/) (only needed for the CMake build)
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (only needed to build the installer)

### Option A — CMake (works from any shell)

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\MuteDiscordDevice_Config.exe`.

### Option B — MSBuild

Open **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu (a regular `cmd`/PowerShell/bash won't have `msbuild` on PATH), then from the repo root run:

```
msbuild MuteDiscordDevice_Config.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\MuteDiscordDevice_Config.exe`.

### Option C — Visual Studio GUI

Open `MuteDiscordDevice_Config.sln` in Visual Studio 2022, set the configuration to **Release / x64**, and Build Solution (Ctrl+Shift+B).

**Build the installer:**

The installer reads the app version from the built exe automatically. It packages the exe from the CMake output (`build-cmake\Release\`), so run Option A first, then:
```
iscc installer.iss
```

To package an MSBuild/Visual Studio build (`x64\Release\`) instead, override the build dir:
```
iscc /DBuildDir=x64\Release installer.iss
```

Output: `Output\MuteDiscordDevice-Setup-<app version>.exe`.

## Release process (maintainers)

1. Bump the version in `version.h` (three macros + `MDD_VERSION_STRING`).
2. Commit.
3. Tag: `git tag v1.2.3 && git push --tags`.
4. The `.github/workflows/release.yml` workflow builds the exe, compiles the installer, and attaches it to a GitHub release named after the tag.

The workflow fails fast if `version.h` doesn't match the tag, so bump first, tag second.
