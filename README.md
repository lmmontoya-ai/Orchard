# Orchard

Orchard is an open-source APFS access project for Windows. It is being built as a conservative, user-mode filesystem stack: a shared C++ APFS core, a WinFsp mount layer, and a Windows service that can detect and mount eligible volumes.

The guiding rule is simple: if Orchard cannot classify a volume confidently, it should mount read-only or not mount at all.

Today, the practical user path is:

- inspect APFS images and disks from Windows
- mount supported APFS volumes in Explorer in read-only mode
- copy files out of supported volumes

## What Orchard Aims To Be

The v1 target is a Windows product that:

- mounts supported APFS user-data volumes automatically
- shows them in File Explorer with stable drive letters
- explains clearly why a volume is read-only, writable, hidden, or unsupported
- lets users unmount cleanly from a small UI or CLI
- enables write support only for a tightly restricted, crash-safe subset

The full product plan lives in `docs/v1-roadmap.md`. The execution status lives in `task_tracker.md`.

## Using Orchard

Orchard is source-first right now. There is not yet a packaged installer, tray app, or finished `orchardctl`, so the current way to use it is to build from source on Windows.

### Prerequisites

- Windows
- CMake 3.25+
- Ninja
- Clang/LLVM with C++20 support
- WinFsp runtime and SDK if you want Explorer mounts

### Build Orchard

If you only want offline inspection:

```powershell
cmake --preset default -DORCHARD_ENABLE_WINFSP=OFF
cmake --build --preset default --target orchard-inspect
```

If you want Explorer mounting too:

```powershell
$env:WINFSP_ROOT_DIR = "C:\path\to\WinFsp\SDK"
cmake --preset default -DORCHARD_ENABLE_WINFSP=ON
cmake --build --preset default --parallel
```

### Inspect An APFS Image Or Disk

Inspect a sample APFS image:

```powershell
.\build\default\tools\inspect\orchard-inspect.exe --target .\tests\corpus\samples\plain-user-data.img
```

List a path inside the inspected volume:

```powershell
.\build\default\tools\inspect\orchard-inspect.exe --target .\tests\corpus\samples\plain-user-data.img --list-path /docs
```

Inspect a raw disk and enrich the output with volume details:

```powershell
.\build\default\tools\inspect\orchard-inspect.exe --target \\.\PhysicalDrive2 --enrich-raw
```

If you already know the APFS volume object ID, you can target it directly:

```powershell
.\build\default\tools\inspect\orchard-inspect.exe --target \\.\PhysicalDrive2 --volume-oid 1026
```

### Mount A Supported Volume In Explorer

The simplest current mount flow is the console host. This keeps Orchard running in the foreground and unmounts cleanly when you stop it.

Mount a sample image to `R:`:

```powershell
.\build\default\src\mount-service\orchard-service-host.exe --console --target .\tests\corpus\samples\plain-user-data.img --mountpoint R:
```

Open `R:` in Explorer. Press `Ctrl+C` in the console to stop the host and unmount.

For a raw disk with multiple APFS volumes, inspect first to find the right `volume_object_id`, then mount that specific volume:

```powershell
.\build\default\tools\inspect\orchard-inspect.exe --target \\.\PhysicalDrive2 --enrich-raw
.\build\default\src\mount-service\orchard-service-host.exe --console --target \\.\PhysicalDrive2 --mountpoint R: --volume-oid 1026
```

## Current Boundaries

- Mounted access is read-only today.
- Orchard is currently aimed at supported, non-encrypted APFS volumes it can classify safely.
- Installer packaging, tray UI, `orchardctl`, and clean end-user unmount workflows are planned but not finished.
- Write support is planned, but it is intentionally gated behind later crash-safety and mutation-testing milestones.

## Minimal Roadmap

### Short Term

The next step is finishing the product control plane in `M3`:

- persistent drive-letter assignment
- named-pipe control API
- tray UI
- `orchardctl`
- clean unmount flow
- installer and uninstall baseline
- structured logging and diagnostics bundle

### Mid Term

After that, Orchard moves into restricted write support and hardening:

- `M4`: safe write eligibility gate, APFS mutation path, commit/flush engine, and read-only downgrade on failure
- `M5`: fuzzing expansion, writable crash campaigns, soak testing, performance baselines, upgrade validation, and beta docs/support matrix

## Repository Pointers

- `docs/v1-roadmap.md`: user-facing product direction and v1 scope
- `task_tracker.md`: milestone-by-milestone execution status
- `tools/inspect/`: offline inspection tool
- `src/mount-service/`: current service host and mount lifecycle
- `src/fs-winfsp/`: WinFsp-backed Explorer mount layer
