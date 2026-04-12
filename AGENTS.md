# Orchard Engineering Notes

## Purpose

This file is the local engineering diary for Orchard. Keep operational details here that are easy to forget and not obvious from the source tree.

## Current baseline

- Language baseline: `C++20`
- Build system baseline: `CMake` + `Ninja`
- Test baseline: `CTest` with native unit, integration, and fuzz-smoke executables

## Local toolchain notes

- `cmake` and `ninja` were installed with:
  - `python -m pip install --user cmake ninja`
- On this machine, the script directory is:
  - `C:\Users\luism\AppData\Roaming\Python\Python311\Scripts`
- If that directory is not on `PATH`, use:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;$env:PATH"
```

## Baseline commands

Configure:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;$env:PATH"
cmake --preset default
```

Build:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;$env:PATH"
cmake --build --preset default
```

Run tests:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;$env:PATH"
ctest --preset default
```

Run the inspect stub:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;$env:PATH"
.\build\default\tools\inspect\orchard-inspect.exe --target .\tests\corpus\samples\placeholder.img
```

## M0 notes

- `orchard-inspect` is intentionally small and deterministic. It currently classifies a target path, probes for the `NXSB` APFS container magic in regular files, and emits structured JSON output.
- The fixture manifest is JSON and is validated with a CMake script so the repo does not take on a JSON library dependency during M0.
- `tests/fuzz/orchard_fuzz_smoke.cpp` is a scaffold target, not a full fuzzing setup. Real fuzzers should replace it in M1 and M5.
- CI baseline lives in `.github/workflows/ci.yml` and runs `cmake --preset default`, `cmake --build --preset default --parallel`, and `ctest --preset default` on `windows-latest`.
