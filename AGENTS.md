# Orchard Engineering Notes

## Purpose

This file is the local engineering diary for Orchard. Keep operational details here that are easy to forget and not obvious from the source tree.

## Current baseline

- Language baseline: `C++20`
- Build system baseline: `CMake` + `Ninja`
- Test baseline: `CTest` with native unit, integration, and fuzz-smoke executables
- Maintenance baseline: `clang-format` + `clang-tidy`

## Local toolchain notes

- `cmake` and `ninja` were installed with:
  - `python -m pip install --user cmake ninja`
- LLVM toolchain and analysis tools were installed into Conda with:
  - `conda install -y --solver classic -c conda-forge clangxx binutils lld clang-tools`
- On this machine, the script directory is:
  - `C:\Users\luism\AppData\Roaming\Python\Python311\Scripts`
- LLVM binaries used by Orchard live under:
  - `C:\Users\luism\miniconda3\Library\bin`
- MinGW binutils used for `windres` live under:
  - `C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin`
- If that directory is not on `PATH`, use:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
```

## Baseline commands

Configure:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:CC = "C:\Users\luism\miniconda3\Library\bin\clang.exe"
$env:CXX = "C:\Users\luism\miniconda3\Library\bin\clang++.exe"
$env:RC = "C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin\windres.exe"
cmake --preset default
```

Build:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
cmake --build --preset default
```

Run tests:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
ctest --preset default
```

Run `orchard-inspect`:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
.\build\default\tools\inspect\orchard-inspect.exe --target .\tests\corpus\samples\plain-user-data.img
```

Run formatting:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
cmake --build --preset default --target orchard_format
```

Run formatting check:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
cmake --build --preset default --target orchard_format_check
```

Run static analysis:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
cmake --build --preset default --target orchard_lint
```

Regenerate synthetic sample fixtures:

```powershell
.\tests\corpus\generate-sample-fixtures.ps1
```

## M0 notes

- `orchard-inspect` started as an M0 stub, but it is now a real offline inspector for direct-container and GPT-wrapped synthetic APFS fixtures.
- The fixture manifest is JSON and is validated with a CMake script so the repo does not take on a JSON library dependency during M0.
- `tests/fuzz/orchard_fuzz_smoke.cpp` is a scaffold target, not a full fuzzing setup. Real fuzzers should replace it in M1 and M5.
- CI baseline lives in `.github/workflows/ci.yml` and runs `cmake --preset default`, `cmake --build --preset default --parallel`, and `ctest --preset default` on `windows-latest`.
- Formatting is driven by `.clang-format` and `tools/dev/orchard-format.ps1`.
- Static analysis is driven by `.clang-tidy` and `tools/dev/orchard-lint.ps1`.
- The maintenance scripts skip tracked files that have been deleted from the worktree, which matters after test-file renames.
- The lint wrapper treats `clang-tidy` failures as hard failures; do not rely on its console output alone.

## M1 notes

- `src/blockio` now exposes a typed, synchronous reader interface with Windows handle-backed file/raw-device support and an in-memory backend for tests.
- `src/apfs-core/src/discovery.cpp` currently supports:
  - direct APFS container detection via `NXSB`
  - GPT header parsing and APFS partition discovery
  - checkpoint selection by scanning the main superblock plus the checkpoint descriptor area for the highest `xid`
  - object-header parsing, generic B-tree node parsing, container omap traversal, and omap-backed volume superblock resolution
- `src/apfs-core` is now split so `discovery.cpp` orchestrates layout detection while low-level parsing lives in `format.cpp`, `object.cpp`, `btree.cpp`, and `omap.cpp`.
- Synthetic APFS fixtures now include a real omap object plus root/leaf omap B-tree nodes; regenerate them with `tests/corpus/generate-sample-fixtures.ps1` after fixture-layout changes.
- When collecting verification evidence, do not run `cmake --build` and `ctest` in parallel. Running them concurrently can produce misleading failures against stale executables.
