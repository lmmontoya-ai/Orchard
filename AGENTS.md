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

Run the inspect stub:

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

## M0 notes

- `orchard-inspect` is intentionally small and deterministic. It currently classifies a target path, probes for the `NXSB` APFS container magic in regular files, and emits structured JSON output.
- The fixture manifest is JSON and is validated with a CMake script so the repo does not take on a JSON library dependency during M0.
- `tests/fuzz/orchard_fuzz_smoke.cpp` is a scaffold target, not a full fuzzing setup. Real fuzzers should replace it in M1 and M5.
- CI baseline lives in `.github/workflows/ci.yml` and runs `cmake --preset default`, `cmake --build --preset default --parallel`, and `ctest --preset default` on `windows-latest`.
- Formatting is driven by `.clang-format` and `tools/dev/orchard-format.ps1`.
- Static analysis is driven by `.clang-tidy` and `tools/dev/orchard-lint.ps1`.
- The lint wrapper treats `clang-tidy` failures as hard failures; do not rely on its console output alone.
