# Local Machine Configuration

RedClawDesktop keeps machine-specific paths outside tracked repository files.

## Configure a workstation

1. Copy `CMakeUserPresets.example.json` to `CMakeUserPresets.json`.
2. Set `VCPKG_ROOT` in the local file to the absolute path of your vcpkg checkout.
3. Keep `CMakeUserPresets.json` untracked; it is covered by `.gitignore`.
4. Configure and build with the local presets:

```powershell
cmake --preset vs2022-x64-local --fresh
.\build.ps1
```

Use `CMakeUserPresets.json`, the process environment, or IDE-local settings for other workstation-specific values. Do not add local absolute paths to shared documentation or presets.

## Build serialization

Only one configure, build, or test operation should run in a worktree at a time. If CMake reports another active operation or MSVC reports locked PDB, ILK, or output files, stop the competing operation and rerun the standard build entry point.

See `docs/setup/cmake-tools-api-failure-troubleshooting.md` for recovery steps. VS Code tasks use `scripts/service/run-cmake-guarded.ps1` to apply the same rule.
