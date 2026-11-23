# 12 — Driver and toolchain integration

## 1. Command-line syntax

`ncc.exe` accepts `cl`-style options as its primary syntax. Everything around it in this toolchain (`link.exe`, `lib.exe`, MSBuild, the VS project system, CMake's MSVC generator logic, vcpkg triplets) speaks `cl`, and a `cl`-compatible driver lets `ncc` be dropped into existing builds by changing the compiler path. Options are accepted with `/` or `-` prefixes. Options that `cl` has and we do not implement are rejected with a clear diagnostic rather than silently ignored, except for a documented list of harmless no-ops (`/nologo`, `/FS`, `/Gm-`, `/errorReport:*`, `/diagnostics:*`).

| Option | Meaning |
|--------|---------|
| `/c`, `/Fo<path>`, `/Fe<path>`, `/Fa<path>`, `/FA[s]`, `/E`, `/EP`, `/P` | Compile only, output paths, assembly listing, preprocess |
| `/I`, `/D`, `/U`, `/FI`, `/X`, `/external:I`, `/external:W0` | Preprocessor |
| `/std:c23` (default), `/std:c17`, `/std:c11` | Language version. Older modes switch keywords and semantics (e.g. `int f()` unprototyped under C17) |
| `/Od`, `/O1`, `/O2`, `/Ox`, `/Ob0-2`, `/Oy-` | Optimisation (`/Oy` is accepted and ignored: the frame chain is always kept) |
| `/Z7`, `/Zi`, `/ZI` | CodeView in the object ([10-codeview.md](10-codeview.md)) |
| `/MD`, `/MDd`, `/MT`, `/MTd`, `/Zl` | CRT selection and default-library emission |
| `/W0`–`/W4`, `/Wall`, `/WX`, `/wd`, `/we`, `/wo` | Warnings |
| `/J` | Unsigned `char` |
| `/fp:precise\|strict\|fast\|contract` | FP model |
| `/volatile:iso\|ms` | `volatile` semantics |
| `/GS[-]`, `/guard:cf[-]`, `/Gy` (always on), `/Gw[-]`, `/bigobj` (automatic) | Codegen/security |
| `/utf-8`, `/source-charset:utf-8` | Accepted; UTF-8 is the only source charset |
| `/Brepro`, `/pathmap:` | Reproducibility |
| `/MP[n]` | Parallel compilation of multiple sources (process-level) |
| `/link ...` | Pass the rest to `link.exe` |
| `/permissive-` | Pedantic warnings for MSVC extensions in user code |
| `/arch:`, `/feature:` | Target features ([06-arm64-backend.md](06-arm64-backend.md) §8) |
| `/Q...` | `ncc`-specific options: `/Qstrict-aliasing`, `/Qwrapv`, `/Qcommon`, `/Qmsc-version:`, `/Qpasses:`, `/Qdump-ir`, `/Qverify-unwind`, `/Qtime-report` |

## 2. Environment discovery

The supported environment is a VS 2026 Developer Command Prompt (or `vcvarsall.bat arm64` / `x64_arm64`), where `INCLUDE`, `LIB`, `LIBPATH` and `PATH` are already set; this is how `cl` itself works and what MSBuild provides. Outside such an environment, `ncc` locates the newest VS 2026 installation with `vswhere.exe` (from its fixed location under `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`) and the Windows SDK via the registry (`HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots`), selecting the newest SDK unless `/Qwinsdk-version:` says otherwise, and constructs the same paths `vcvarsall` would. This fallback exists for convenience; builds that care about reproducibility should run inside the developer environment.

On Linux (CI and cross-development), `ncc` compiles normally but needs the headers and libraries from a Windows SDK and MSVC installation copied or mounted locally, supplied via `INCLUDE`/`LIB` or `/winsysroot:`, as `clang-cl` does. We do not download Microsoft components automatically; licence acceptance remains the user's responsibility.

## 3. Include order

Both owned headers and overlays must be found before the UCRT's same-named headers, so they live in a single directory placed ahead of `INCLUDE`. The normative search order is:

1. Quote-include directories (MSVC include-stack rule, [03-frontend.md](03-frontend.md) §3.2), for `"..."` only
2. `/I` paths, then `/external:I` paths
3. `include/ncc` — owned headers and overlays; overlays reach the UCRT original with `#include_next`
4. `INCLUDE` environment paths in order (VC tools, UCRT, `shared`, `um`, `winrt`)

A consequence is that a user `/I` directory containing its own `stdint.h` (common in older portable code) shadows ours, exactly as it would shadow the UCRT's under `cl`. That is the expected behaviour and is not worked around.

## 4. Linking

`ncc` never links by itself; when invoked without `/c` it runs `link.exe` exactly as `cl` does: passing the objects, `/OUT:` from `/Fe`, `/DEBUG` when `/Z7` is in effect, `/MACHINE:ARM64`, and everything after `/link`. Default libraries come from the objects' `.drectve` sections, so a manual `link.exe` invocation on `ncc` objects needs nothing extra beyond `LIB` containing the `libncrt` directory (installed next to `ncc.exe`, and added to the `/LIBPATH:` the driver passes). `link.exe` is located via `PATH`, or next to the discovered VC tools; `/Qlinker:` overrides it (e.g. to `lld-link` for testing).

## 5. Build-system integration

**MSBuild / Visual Studio.** An MSBuild toolset definition (`ncc.props`/`ncc.targets`, installed per-user or per-solution via a `Directory.Build.props`) swaps `ClCompile`'s `ToolExe` and `ToolPath` to `ncc.exe` and filters a small number of `ClCompile` metadata items that would produce unsupported switches. Because the driver is `cl`-compatible and the diagnostics are MSVC-formatted, IntelliSense, the error list and incremental builds work without a custom task. `/sourceDependencies` JSON output (what MSBuild's tracker-free mode uses) is implemented so incremental builds know header dependencies.

**CMake.** CMake identifies compilers by probing predefined macros. Because `ncc` defines `_MSC_VER`, CMake would identify it as MSVC and use MSVC flags, which mostly works. The cleaner path is a CMake toolchain file that sets `CMAKE_C_COMPILER` to `ncc.exe` and `CMAKE_C_COMPILER_ID` to `MSVC` with `CMAKE_C_COMPILER_FRONTEND_VARIANT` = `MSVC`, documented in the repository, until upstream CMake knows about `ncc` (Q-24).

**Dependency output.** `/showIncludes` (MSVC format, for Ninja's `deps = msvc`) and `/sourceDependencies` are both implemented.

## 6. Diagnostics format

`file(line,col): error NC1234: message` and `warning NC4xxx`, plus caret snippets when the console supports it (not when `MSBUILD_*` environment indicates a build log). Error numbers are stable across releases.

## Open items from this document

CMake upstream identification (Q-24).
