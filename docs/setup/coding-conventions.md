# Coding Conventions

## Language Standard
- C++20 (`cxx_std_20`), compiled with MSVC via VS2022.
- Warnings: `/W4` on MSVC, `-Wall -Wextra -Wpedantic` on GCC/Clang.

## Code Formatting
- Use `.clang-format` at repo root (to be added under `X00-T03`).
- Until clang-format is configured, follow existing codebase style:
  - 4-space indentation, no tabs.
  - `snake_case` for variables, functions, namespaces.
  - `PascalCase` for types and classes.
  - `kPascalCase` for enum values (e.g. `kUserDesktop`, `kDenyAll`).
  - Prefix interfaces with `I` (e.g. `IInputInjectorBackend`).

## Testing
- **Target framework: GoogleTest** (already in `vcpkg.json`).
- Current codebase uses a custom `expect_true()` macro. Task `X00-T02` tracks migration to gtest.
- New test code must use `EXPECT_*` / `ASSERT_*` from GoogleTest.
- Use in-memory test backends for deterministic testing (no OS dependencies in unit tests).
- Test file naming: `<feature>_tests.cpp` under `tests/<module>/`.

## Security Constants
- Do not hardcode security-critical values inline. After `X00-T04`, use constants from `src/core/include/redclaw/core/security_constants.h`.
- Current known constants to centralize:

| Constant | Current Value | Location |
|----------|--------------|----------|
| PBKDF2 iteration count | 120,000 | protocol module |
| Grant token TTL | 120s | service module |
| Step-up proof TTL | 120s | service module |
| UAC action timeout | 30s | service module |
| Lockout threshold | 5 failures | service module |
| Lockout cooldown | 300s | service module |

## Module Structure
Every module follows this layout:
```
src/<module>/
  include/redclaw/<module>/<module>_module.h
  src/<module>_module.cpp
  CMakeLists.txt
```
- Each module builds as a `STATIC` library.
- Public headers go in `include/redclaw/<module>/`.
- Use `redclaw_apply_warnings(<target>)` macro from root CMake.

## Dependency Rules
- Modules may only depend on other modules explicitly listed in their CMake `target_link_libraries`.
- Cross-module interface changes must be noted in DEVLOG before implementation.
- Prefer dependency injection (interfaces) over direct instantiation for cross-module types.

## Platform Conditional Code
- Gate Windows-specific code with `#ifdef _WIN32`.
- Keep platform-agnostic logic in separate translation units from platform code where feasible.

## Commit Hygiene
- One logical change per commit.
- Commit message format: `[Mxx] brief description` (e.g. `[M01] Add ICE candidate parser skeleton`).
- Cross-cutting changes use `[X00]` prefix.
