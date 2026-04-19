# Building MuteDiscordDevice

## Build from source

**Prerequisites:**
- Windows 10/11 x64
- Visual Studio 2022 with the **Desktop development with C++** workload
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (only needed to build the installer)

**Build the exe:**
```
msbuild MuteDiscordDevice_Config.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\MuteDiscordDevice_Config.exe`.

**Build the installer:**
```
iscc /DMyAppVersion=1.0.0 installer.iss
```

Output: `Output\MuteDiscordDevice-Setup-1.0.0.exe`.

## Release process (maintainers)

1. Bump the version in `version.h` (three macros + `MDD_VERSION_STRING`).
2. Commit.
3. Tag: `git tag v1.2.3 && git push --tags`.
4. The `.github/workflows/release.yml` workflow builds the exe, compiles the installer, and attaches it to a GitHub release named after the tag.

The workflow fails fast if `version.h` doesn't match the tag, so bump first, tag second.
