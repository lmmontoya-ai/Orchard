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
- `tools/dev/orchard-lint.ps1` now follows `compile_commands.json` rather than `git ls-files`, so optional backends like WinFsp are linted only when the active CMake configuration actually includes them.

## M1 notes

- `src/blockio` now exposes a typed, synchronous reader interface with Windows handle-backed file/raw-device support and an in-memory backend for tests.
- `src/apfs-core/src/discovery.cpp` currently supports:
  - direct APFS container detection via `NXSB`
  - GPT header parsing and APFS partition discovery
  - checkpoint selection by scanning the main superblock plus the checkpoint descriptor area for the highest `xid`
  - object-header parsing, generic B-tree node parsing, container omap traversal, and omap-backed volume superblock resolution
- `src/apfs-core` is now split so `discovery.cpp` orchestrates layout detection while low-level parsing lives in `format.cpp`, `object.cpp`, `btree.cpp`, and `omap.cpp`.
- Synthetic APFS fixtures now include a real omap object plus root/leaf omap B-tree nodes; regenerate them with `tests/corpus/generate-sample-fixtures.ps1` after fixture-layout changes.
- The synthetic fixture generator now uses an inline Python builder inside `tests/corpus/generate-sample-fixtures.ps1`; keep the Python and `tests/unit/apfs_tests.cpp` fixture layouts in sync when record layouts change.
- The current synthetic filesystem corpus exercises:
  - volume omap resolution
  - filesystem-tree inode and directory records
  - multi-extent file reads
  - sparse-hole reads
  - inline `decmpfs_uncompressed_attribute` reads
  - policy outcomes for writable, snapshot read-only, and sealed reject cases
- `orchard-inspect` now enriches each discovered volume with root-directory samples and up to two root-file probes. The current probes are intended for offline inspection and test observability, not for final UX design.
- `src/apfs-core/src/btree.cpp` now exposes `BtreeWalker::LowerBound` and `VisitRange` in addition to full-tree traversal. Use those bounded APIs for filesystem-tree lookups that need one key or one key range; do not reintroduce full-tree scans for inode, directory, extent, or xattr queries.
- `src/apfs-core/src/volume.cpp` now drives `GetInode`, `ListDirectoryEntries`, `ListFileExtents`, and `FindXattr` through typed lower-bound/range helpers in `src/apfs-core/src/fs_search.cpp`. This is the fix for the real-media shell stall found while mounting the external APFS SSD during `M3-T02` validation.
- Synthetic filesystem-tree fixtures must be sorted by decoded APFS key fields, not raw little-endian key bytes. Both `tests/unit/apfs_tests.cpp` and `tests/corpus/generate-sample-fixtures.ps1` now do this explicitly; keep them in sync if fixture key layouts change.
- `orchard_lint` now runs across 43 translation units and can take a long time locally; prefer running it separately from `ctest`.
- When collecting verification evidence, do not run `cmake --build` and `ctest` in parallel. Running them concurrently can produce misleading failures against stale executables.

## M2 notes

- `ORCHARD_ENABLE_WINFSP` now accepts `AUTO`, `ON`, or `OFF`. Use `ON` when validating the WinFsp adapter locally.
- The `WinFsp` runtime installed via `winget install WinFsp.WinFsp` was not sufficient for local developer builds on this machine because it did not expose headers and import libraries under `C:\Program Files (x86)\WinFsp`. The working local developer SDK came from extracting the official MSI into:
  - `C:\Users\luism\AppData\Local\Temp\winfsp-sdk\DYNAMIC`
- For WinFsp-enabled configure/build/test runs on this machine, set:

```powershell
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
cmake --preset default -DORCHARD_ENABLE_WINFSP=ON
cmake --build --preset default --parallel
ctest --preset default --output-on-failure
```

- `FindWinFsp.cmake` now discovers the WinFsp runtime DLL in addition to headers/import libraries, and WinFsp-linked executables copy `winfsp-x64.dll` beside themselves after build. This fixed the local `0xc0000135` / `winfsp-x64.dll was not found` test failure.
- The validated local smoke path for `M2-T01` is a drive-letter mount, not a directory mountpoint. A mount to `R:` worked; a mount to `%TEMP%\orchard-m2-smoke` failed at `FspFileSystemSetMountPoint` and needs follow-up in a later M2 task.
- `tools/mount-smoke/src/main.cpp` now supports `--hold-ms <milliseconds>` so a local smoke run can mount, stay alive long enough for inspection, and then unmount cleanly without killing the process.
- `M2-T02` extended the APFS metadata surface used by WinFsp query mapping. `FileMetadata` and `InodeRecord` now carry allocated size, timestamps, link count, child count, mode, and inode flags, and `BuildBasicFileInfo` in `src/fs-winfsp/src/file_info.cpp` is the single APFS-to-Windows file-info translation path.
- `src/fs-winfsp/src/path_bridge.cpp` now treats Windows query paths more deliberately:
  - collapses repeated separators
  - resolves `.` and `..`
  - rejects stream syntax using `:`
  - rejects unsupported wildcard/control/path characters in normalized lookup paths
- `src/fs-winfsp/src/directory_query.cpp` now owns WinFsp-facing directory query shaping:
  - per-entry metadata fetch and `BasicFileInfo` construction
  - marker/resume filtering
  - simple wildcard pattern filtering
  - deterministic output order inherited from the APFS directory listing
- `M2-T03` added a larger synthetic shell-stress volume at `tests/corpus/samples/explorer-large.img`. It uses a multi-leaf filesystem tree and includes:
  - a 181-entry directory at `/bulk items`
  - a nested file at `/bulk items/Nested Folder/deep-note.txt`
  - a larger regular file at `/copy-source.bin` for copy-out validation
- `src/fs-winfsp/src/directory_query.cpp` now also exposes `PaginateDirectoryQueryEntries`, which is used by the WinFsp callback layer to make buffer-bounded directory enumeration deterministic under larger listings.
- `src/fs-winfsp/src/mount.cpp` now caches open nodes by the canonical path returned from APFS lookup, not only by the caller-supplied path spelling. This matters for repeated case-insensitive opens such as `/alpha.txt` followed by `/ALPHA.TXT`.
- Local `M2-T01` smoke verification used:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\build\default\tools\mount-smoke\orchard-mount-smoke.exe --target .\tests\corpus\samples\plain-user-data.img --mountpoint R: --hold-ms 30000
```

- The successful smoke browse observed these root entries on `R:\`:
  - `docs`
  - `alpha.txt`
  - `compressed.txt`
  - `holes.bin`
- `alpha.txt` read successfully during the smoke run with bytes:
  - `48-65-6C-6C-6F-20-4F-72-63-68-61-72-64-0A`
- Local `M2-T02` shell-side smoke verification used the same drive-letter mount shape with `--hold-ms 12000`, then checked:
  - repeated `Get-ChildItem R:\` returned `docs, alpha.txt, compressed.txt, holes.bin`
  - `Get-ChildItem R:\docs` returned `empty.txt, note.txt`
  - `(Get-Item R:\alpha.txt).Length` returned `14`
  - `(Get-Item R:\compressed.txt).Length` returned `19`
  - `[System.IO.File]::ReadAllText("R:\alpha.txt")` returned `Hello Orchard` plus trailing newline
- Local `M2-T03` automated shell smoke is now in `tools/dev/orchard-winfsp-shell-smoke.ps1`. On this machine it passed with:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\tools\dev\orchard-winfsp-shell-smoke.ps1
```

- The automated `M2-T03` smoke observed:
  - repeated root listings on the mounted drive
  - repeated listing of `/bulk items` with `181` entries
  - preview reads for `preview.txt` and `bulk items\Nested Folder\deep-note.txt`
  - `Copy-Item` + hash equality for `copy-source.bin`
  - clean timed unmount after validation
- The PowerShell shell view listed `bulk items` before `alpha.txt` on the mounted root. That is a shell-ordering observation, not an APFS-directory sort guarantee; do not use shell-visible ordering as the only correctness oracle.
- On `2026-04-13`, manual Explorer validation was observed on this machine: the mounted volume appeared in File Explorer and `copy-source.bin` was copied successfully to `C:\Users\luism\OneDrive\Escritorio`. That closes the remaining manual-evidence gap for `M2-T03`.
- `M2-T04` adds a dedicated synthetic link corpus image at `tests/corpus/samples/link-behavior.img` with:
  - a relative file symlink at `/a-relative-note-link.txt`
  - an absolute file symlink at `/absolute-alpha-link.txt`
  - a broken symlink at `/broken-link.txt`
  - same-directory hard-link aliases at `/hard-a.txt` and `/hard-b.txt`
  - a cross-directory hard-link alias at `/note-link.txt` for `/docs/note.txt`
- `src/apfs-core/include/orchard/apfs/link_read.h` is now the dedicated APFS-core API for link semantics. Keep WinFsp-specific reparse handling in `src/fs-winfsp/src/reparse.cpp`; do not push Windows reparse structures back into `apfs-core`.
- Local `M2-T04` mounted link smoke is now in `tools/dev/orchard-winfsp-link-smoke.ps1`. On this machine it passed with:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\tools\dev\orchard-winfsp-link-smoke.ps1
```

- The automated `M2-T04` link smoke now observes Orchard's v1 mounted-view symlink policy:
  - mounted relative and absolute symlink paths read through to target file contents
  - mounted symlink paths are surfaced as plain read-only files rather than Windows reparse points
  - broken symlink paths fall back to opaque text containing the stored POSIX target
  - matching hashes for `/hard-a.txt` and `/hard-b.txt`
  - `Nested note` read successfully through the cross-directory alias `/note-link.txt`
- `tools/mount-smoke/src/main.cpp` now accepts `--shutdown-event <name>` in addition to `--hold-ms`. Use the named-event path for automated stress runs so the caller can request graceful unmount and measure teardown time directly instead of waiting for the fixed hold timeout.
- `tools/dev/OrchardWinFspTestCommon.ps1` is now the shared helper for WinFsp smoke tooling. Keep drive-letter selection, mount startup, telemetry capture, and unmount verification there; keep scenario-specific filesystem assertions in the individual smoke scripts.
- Local `M2-T05` soak verification is now in `tools/dev/orchard-winfsp-soak.ps1`. On this machine it passed with:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\tools\dev\orchard-winfsp-soak.ps1 -Cycles 25
```

- The automated `M2-T05` soak observed:
  - 25 successful mount/browse/unmount cycles on each of `plain-user-data.img`, `explorer-large.img`, and `link-behavior.img`
  - max mount/unmount latencies of `378/268 ms` on `plain-user-data`
  - max mount/unmount latencies of `324/268 ms` on `explorer-large`
  - max mount/unmount latencies of `316/266 ms` on `link-behavior`
  - max handle counts of `148`, `148`, and `147` respectively
  - max private bytes of `3100672`, `2854912`, and `2809856` respectively
  - no stale drive letters after unmount, no timeout-triggered hangs, and no copy-out/hash mismatches during the soak run

## M3 notes

- `src/mount-service` is no longer a placeholder. The current `M3-T01` service-host foundation is split across:
  - `service_host.*` for SCM and console-mode entry points
  - `runtime.*` for the owned worker/stop lifecycle
  - `mount_registry.*` for active mounted-session ownership
  - `service_state.*` for service-state transitions
  - `types.*` for mount/service request records
- `src/CMakeLists.txt` must keep `add_subdirectory(fs-winfsp)` before `add_subdirectory(mount-service)`. `mount-service` depends on WinFsp target availability during configure time for its runtime-DLL deployment rule.
- `src/mount-service/CMakeLists.txt` now adds an explicit post-build copy of `$<TARGET_FILE:WinFsp::WinFsp>` for `orchard-service-host.exe`. `TARGET_RUNTIME_DLLS` alone was not sufficient through the static link chain.
- The local loader failure `winfsp-x64.dll was not found` for `orchard-service-host.exe` is fixed by the two points above. After a rebuild, `build/default/src/mount-service` should contain:
  - `orchard-service-host.exe`
  - `winfsp-x64.dll`
- Local console-host smoke verification now passes with:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\tools\dev\orchard-service-console-smoke.ps1
```

- That smoke run mounted `plain-user-data.img`, validated `alpha.txt` and the nested note, then shut the console host down through its named event and observed clean mount disappearance.
- The SCM-backed smoke script is:

```powershell
$env:PATH = "C:\Users\luism\AppData\Roaming\Python\Python311\Scripts;C:\Users\luism\miniconda3\Library\bin;C:\Users\luism\miniconda3\Library\x86_64-w64-mingw32\bin;$env:PATH"
$env:WINFSP_ROOT_DIR = "$env:TEMP\winfsp-sdk\DYNAMIC"
.\tools\dev\orchard-service-smoke.ps1
```

- On this machine, `orchard-service-smoke.ps1` currently requires an elevated PowerShell session. A non-elevated run failed while opening the Windows Service Control Manager for install.
- `tools/dev/orchard-service-smoke.ps1` should use `System.Diagnostics.Stopwatch` for timeouts, not `[Environment]::TickCount64`. The latter was not available in the Windows PowerShell/.NET combination used during local `M3-T01` validation.
- Elevated local `M3-T01` SCM smoke now passes. The successful run installed a temporary service named `OrchardSmoke-5d834718`, observed the service reach `Running`, stopped it, and returned JSON with final status `stopped_after_smoke` before uninstalling it.
- `src/mount-service/src/service_host.cpp` now persists explicit `--target`, `--mountpoint`, `--volume-name`, and `--volume-oid` startup-mount arguments into the installed service `ImagePath`, and the SCM dispatcher path now honors the same startup mount as the console host. Use this when validating Explorer-visible mounts from the installed service rather than only the interactive `--console` path.
- `tools/dev/orchard-service-smoke.ps1` now installs the service with an explicit startup mount against `tests/corpus/samples/plain-user-data.img`, waits for the drive letter to appear, validates `alpha.txt` plus the nested note through the mounted drive, and then waits for the drive letter to disappear again after `Stop-Service`.
- Startup mounts and device auto-discovery now cooperate instead of racing: `src/mount-service/src/service_host.cpp` reuses an already-active matching mount during startup, and `src/mount-service/src/device_discovery.cpp` adopts matching mounts already present in the registry before allocating a new drive letter. This avoids SCM startup failures when a configured startup mount and the initial discovery rescan target the same APFS volume.
- Explorer directory enumeration was still visibly sluggish on the real SSD after SCM-backed mounting succeeded. The first mitigation is now in place: `src/apfs-core/src/volume.cpp` stores each child inode record on the corresponding `DirectoryEntryRecord` during `ListDirectoryEntries`, and `src/fs-winfsp/src/directory_query.cpp` reuses that embedded inode metadata instead of immediately calling `GetFileMetadata` again for the same child. This removes one full per-entry metadata walk from Explorer folder listings.
- A second Explorer-read mitigation is now in place in `src/fs-winfsp/src/mount.cpp`: `MountedVolume` caches directory listings by inode ID and keeps resolved `FileNode` records cached by canonical path even after handles close. On read-only mounts this is safe and reduces repeat work during Explorer back/forward navigation, repeated folder opens, and path-based metadata probes.
- A third Explorer-read mitigation is now in place across `src/fs-winfsp/src/filesystem.cpp`, `src/fs-winfsp/src/mount.cpp`, and `src/apfs-core/src/path_lookup.cpp`: WinFsp now serves `GetDirInfoByName`, returns an immediate empty result for stream enumeration, uses longer metadata/dir/security/stream cache timeouts for the read-only mount, primes child-path `FileNode` cache entries from directory enumeration, caches per-inode extent/compression read plans for repeated `Read` calls, and reuses embedded child inode records during APFS path traversal. This targets the remaining real-SSD symptoms where right-click, second-window navigation, and copy-out were still sluggish or intermittently failing after the earlier directory-listing fixes.
- Follow-up real-SSD copy failures with Explorer error `0x8007045D` were traced one layer deeper than the WinFsp caching work. `src/apfs-core/src/fs_records.cpp` now parses `j_file_extent_val_t` as packed `length_and_flags` instead of treating the high-byte flags as part of the length, and `src/blockio/src/reader.cpp` now retries partial `ReadAt` completions until the full request is satisfied or a true short read occurs. This targets selective copy/open failures on large or fragmented real-media files while keeping the mount read-only. Focused regression coverage is in `tests/unit/apfs_tests.cpp` (`ParseFileExtentRecordMasksPackedLengthFlagsFromRealApfsLayout`) and `tests/unit/reader_tests.cpp` (`ReadExactRetriesPartialReadsUntilTheRequestIsSatisfied`).
- The remaining real-SSD copy failure on `R:\Mac Meli\omscs\cs8903\_Luis_Montoya__CS_8903_OVM_Paper_2026_1.pdf` was reproducible outside Explorer: sequential reads succeeded until offset `589824`, and the failure was the final short tail read near EOF. `src/apfs-core/src/volume.cpp` now serves `ReadPhysicalBytes` by reading full APFS blocks from the underlying raw device and slicing the requested bytes in memory instead of issuing unaligned short raw reads directly. This keeps the fix in APFS-core rather than teaching the generic Windows reader about APFS block layout. Focused regression coverage is `FileReadPathUsesAlignedPhysicalReadsForTailBytesOnRawDevices` in `tests/unit/apfs_tests.cpp`.
- The remaining real-SSD folder-copy failure under `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E\.claude` narrowed to APFS directory-key decoding, not Explorer copy itself. Apple’s APFS reference defines `j_drec_hashed_key_t` with a 32-bit `name_len_and_hash` field; Orchard had still tried the older 16-bit name-length interpretation first, so hashed keys whose low hash bits were zero could be misread as legacy keys and surface two hash bytes as part of the file name. `src/apfs-core/src/fs_keys.cpp` now prefers the hashed layout before falling back to the legacy synthetic layout, and `tests/unit/apfs_tests.cpp` now covers the exact false-positive shape with `ParseDirectoryRecordKeyPrefersHashedLayoutWhenLegacyLengthAlsoFits`.
- The next real-SSD folder-copy failure was not another generic Explorer regression: it narrowed to Unix-style virtualenv symlinks under `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E\.venv\bin\python*`. On Windows, `Copy-Item` followed those APFS symlinks and failed because the final targets did not resolve within the mounted volume. Orchard now stops projecting mounted APFS symlinks as Windows reparse points for the v1 read-only surface. Instead, in-volume symlink chains are dereferenced inside `src/fs-winfsp/src/mount.cpp` so open/read/copy work against the final target inode, and unresolved/out-of-volume chains fall back to opaque read-only files whose bytes are the stored POSIX symlink target. The WinFsp-facing shape change is in `src/fs-winfsp/src/file_info.cpp` and `src/fs-winfsp/src/filesystem.cpp`, and the updated mounted-link smoke lives in `tools/dev/OrchardWinFspTestCommon.ps1`. Focused regression coverage is `WinFspBrokenSymlinkFallsBackToOpaqueFileRead` in `tests/unit/apfs_tests.cpp`. This is the current v1 tradeoff for reliable Explorer copy-out on mixed macOS user-data volumes.
- A follow-up recursive-copy failure showed one more mounted-view inconsistency: Orchard had updated symlink behavior in `ResolveFileNode`, but `src/fs-winfsp/src/directory_query.cpp` still shaped Explorer enumeration from the raw APFS symlink inode. That let recursive copy discover a file-like placeholder and then open a different effective object later. The mounted view is now projected consistently in directory enumeration too: `BuildDirectoryQueryEntry` resolves symlink children through `MountedVolume::ResolveFileNode`, and unresolved symlinks are downgraded to mounted regular files before WinFsp sees them. Focused verification passed with `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`.
- The mounted symlink policy is now narrower on purpose: Orchard only read-through dereferences symlinks when the final in-volume target is a regular file. If the resolved target is a directory, Orchard falls back to the same opaque read-only file surface used for broken or out-of-volume links. This avoids recursive copy walking alias directories such as `latest-run` in large real-world trees.
- A new read-only performance pass is now in place for the remaining deep-tree Explorer workload. `src/apfs-core/src/object.cpp` and `src/apfs-core/src/volume.cpp` now share a mount-lifetime APFS physical-block cache sized to roughly `32 MiB` (by block count) so repeated filesystem-tree and omap walks stop re-reading the same object blocks from the device. `src/apfs-core/src/path_lookup.cpp` now records bounded lookup counters (`path_lookup_calls`, walked components, and directory enumerations) through `VolumeContext::performance_stats()` for future diagnosis. On the WinFsp side, `src/fs-winfsp/src/mount.cpp` no longer forces a full `GetFileMetadata` refresh when an ordinary regular file or directory was already primed from directory enumeration; only symlink nodes still force the mounted-view projection pass. This keeps Explorer's open/get-info probes on deep small-file trees from redoing APFS inode+xattr walks before any real file read occurs.
- Focused verification for that performance pass is currently `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. If the installed SCM service is running, `orchard-service-host.exe` must be stopped before the rebuilt executable can pick up the new static WinFsp/APFS libraries; a locked `orchard-service-host.exe` will fail to relink with `permission denied`.
- A follow-up performance pass now targets the remaining recursive-copy discovery stall on `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E` without changing v1 read-only semantics. `src/apfs-core/src/volume.cpp` now memoizes inode records by inode ID inside `VolumeContext`, so large first-pass directory enumerations stop repeating bounded inode lookups for the same children. On the WinFsp side, `src/fs-winfsp/src/mount.cpp` now exposes cached directory-entry vectors through shared handles instead of copying them back out on every cache hit, and `src/fs-winfsp/src/filesystem.cpp` now caches the derived `DirectoryQueryEntry` vectors per directory inode inside the active mount session. This removes one more layer of repeated APFS-to-Windows query shaping during Explorer's recursive discovery walk on deep small-file trees.
- Focused verification for that second performance pass is `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. Real-SSD validation still requires stopping the installed Orchard service, rebuilding `orchard-service-host.exe`, restarting the service, and then re-running the pathological folder copy in Explorer.
- The next recursive-copy hotspot was in WinFsp directory paging itself: `ReadDirectory` still rebuilt a fully filtered `DirectoryQueryEntry` vector on every page before truncating it to the caller's buffer. `src/fs-winfsp/src/directory_query.cpp` now exposes `PaginateFilteredDirectoryQueryEntries`, which seeks directly from the marker inside the cached query-entry list and emits one page without materializing the whole filtered vector first. `src/fs-winfsp/src/filesystem.cpp` now uses that direct paging path for `ReadDirectory`, and `WinFspLargeDirectoryPaginationResumesDeterministically` in `tests/unit/apfs_tests.cpp` now exercises the same code path the live mount uses. This specifically targets the “fast at first, then suddenly stalls in discovery” behavior once Explorer hits a genuinely large directory inside a deep tree.
- The first explicit native-feel observability slice is now in place. `src/fs-winfsp/include/orchard/fs_winfsp/mount.h` defines mount-session callback performance counters, `src/fs-winfsp/src/filesystem.cpp` records per-callback count/total/max/slow-call stats for `GetSecurityByName`, `Create`, `Open`, `GetFileInfo`, `GetDirInfoByName`, `ReadDirectory`, `Read`, `ResolveReparsePoints`, `GetReparsePoint`, `Close`, `GetSecurity`, and `GetStreamInfo`, and `src/mount-service/src/service_host.cpp` now accepts `--diagnose-perf` in console mode to print live per-mount WinFsp callback stats alongside the existing APFS path/block-cache counters and mounted-volume cache counters. `src/mount-service/src/mount_registry.cpp` now returns live performance snapshots through `ListMounts()` / `GetMount()` instead of stale mount-time records. Focused verification passed with `orchard_unit_mount_service`, `orchard_unit_apfs`, and `orchard_format_check`.
- Real-SSD `--diagnose-perf` on `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E` showed that the remaining native-feel gap is no longer directory-open churn first: `GetSecurityByName` and `Open` stayed cheap, `ReadDirectory` was noticeable but limited, and `Read` dominated total callback time during recursive copy. The same run also exposed a WinFsp callback-semantics bug: `src/fs-winfsp/src/filesystem.cpp` was returning `STATUS_BUFFER_TOO_SMALL` whenever paged directory enumeration produced an empty page inside a non-empty directory, even when the marker had simply advanced past the last matching entry. That surfaced in PowerShell/Explorer recursive copy as `The data area passed to a system call is too small.` on `.git\objects\xx` directories inside `Reproducing-TTT-E2E`. The fix is now in place and covered by `WinFspDirectoryPaginationReturnsEmptySuccessPageWhenMarkerIsPastEnd` in `tests/unit/apfs_tests.cpp`.
- The next measured hotspot after fixing recursive-copy enumeration semantics is the raw read path itself. Real-SSD `--diagnose-perf` now shows `Read` dominating callback time, and `VolumeContext::ReadPhysicalBytes` was still walking aligned requests one APFS block at a time through `PhysicalBlockCache::ReadBlock`. That is efficient only for hot metadata blocks; on cold sequential copy-out it degenerates into very large numbers of 4 KiB device reads. `src/apfs-core/src/object.cpp` now adds `PhysicalBlockCache::ReadBlocks(...)`, which coalesces contiguous cache misses into one larger raw read and seeds the per-block cache from that range, and `src/apfs-core/src/volume.cpp` now uses that range-read path for aligned physical reads. Focused regression coverage is `VolumeContextCoalescesContiguousPhysicalBlockMissesIntoOneDeviceRead` in `tests/unit/apfs_tests.cpp`.
- The next follow-up after coalesced raw reads still showed recursive copy timing out under the console-host hold timeout rather than failing on correctness: `device_reads` dropped sharply and average `Read` callback latency improved, but `Read` still dominated the callback mix. `src/fs-winfsp/src/mount.cpp` now adds mounted per-inode sequential read-ahead on top of the existing extent cache, and `src/mount-service/src/service_host.cpp` now prints `read_ahead_hits` and `read_ahead_prefetches` in `--diagnose-perf` output. Focused regression coverage is `WinFspMountedVolumeReadAheadServesSequentialReadsFromCache` in `tests/unit/apfs_tests.cpp`.
- The first mounted read-ahead policy turned out to be too eager for the pathological real-SSD tree: a `--diagnose-perf` run showed `read_ahead_prefetches=19988`, which is consistent with over-reading across many small files and making recursive copy worse instead of better. The mounted read-ahead gate in `src/fs-winfsp/src/mount.cpp` is now intentionally narrow: it only arms after three sequential reads, only for files at least `512 KiB`, and only when the current request is already at least `64 KiB`. Small-file sequential reads should now stay on the existing extent cache without speculative prefetch.
- The next measured gap after narrowing read-ahead was not another path-lookup issue: real-SSD `--diagnose-perf` still showed `Read` dominating even with `read_ahead_hits=0` and `read_ahead_prefetches=0`, which means Orchard was still doing too much fixed work on the first read of each ordinary file. The hot path in `src/fs-winfsp/src/mount.cpp` now resolves read metadata differently for primed regular files: it reuses the inode-derived metadata already cached from directory enumeration and caches compression-xattr parsing per inode instead of calling full `GetFileMetadata()` on every first read. `src/mount-service/src/service_host.cpp` now prints `compression_info_hits` / `compression_info_misses` in `--diagnose-perf`, and `tests/unit/apfs_tests.cpp` covers the new behavior with `WinFspMountedVolumeCachesCompressionInfoForPrimedRegularFiles`. Focused verification passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`.
- The next real-SSD retest showed that compression-info caching was working (`compression_info_hits=170658`, `compression_info_misses=26410`) but recursive copy was still taking hours, which points at chunked small-file reads rather than metadata reloads. `src/fs-winfsp/src/mount.cpp` now caches the full payload of ordinary small files (currently up to `128 KiB`) for the lifetime of the open handle, resets that ephemeral payload cache on close, and reports `whole_file_hits` / `whole_file_misses` through `src/mount-service/src/service_host.cpp`. This is intended to collapse repeated chunked `Read` callbacks for git objects, logs, and similar small files into one source fetch per open file instead of several. Focused regression coverage is `WinFspMountedVolumeCachesWholeSmallFileAcrossChunkedReads` in `tests/unit/apfs_tests.cpp`, and focused verification again passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`.
- The real-SSD `.git\objects` and full-tree retests showed that the small-file whole-payload cache was the wrong optimization: it never produced hits on the pathological workload and made wall-clock time worse. Orchard now exposes request-shape telemetry instead of guessing. `src/fs-winfsp/src/filesystem.cpp` tracks read requests per open WinFsp file context and records per-open read-count buckets on close; `src/fs-winfsp/src/mount.cpp` now reports logical `read_requested_bytes` vs `read_fetched_bytes`, request-size buckets, sequential vs non-sequential reads, and large-file small-request counts; `src/apfs-core/src/object.cpp` now splits physical block-cache accounting into metadata vs file-data counters with requested/fetched byte totals; and `src/mount-service/src/service_host.cpp` prints all of that through `--diagnose-perf`. Focused regression coverage is now `WinFspMountedVolumeTracksReadTelemetryBuckets`, plus the updated block-cache assertions in `VolumePathLookupTracksStatsAndReusesCachedBlocks` and `VolumeContextCoalescesContiguousPhysicalBlockMissesIntoOneDeviceRead`. Focused verification passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, and `orchard_format_check`.
- The first request-shape telemetry run on the real SSD narrowed the remaining gap further: `.git\objects` is a roughly `9.3 GiB` mostly-large sequential streaming workload, not a tiny-file-only case. The live counters showed `mounted.read_requests.large=9591`, `mounted.read_requests.sequential=9017`, and `read_ahead_hits/prefetches=0`, which means the existing read-ahead window never expanded beyond the current request. `src/fs-winfsp/src/mount.cpp` now keeps sequential state advancing even when a request is served from the read-ahead cache, and large streaming files can now grow their prefetch window past the current request size (up to `4 MiB`) instead of stalling at the old `1 MiB` cap. Focused regression coverage is `WinFspMountedVolumeReadAheadPrefetchesLargeStreamingReads`, which mounts a synthetic multi-megabyte APFS fixture and verifies that large sequential reads now produce chained prefetches plus at least one cache hit without reopening the small-file regression.
- `M3-T02` now adds a device-discovery layer under `src/mount-service`:
  - `device_monitor.*` registers ConfigMgr notifications through `CM_Register_Notification`
  - `device_enumerator.*` enumerates `GUID_DEVINTERFACE_DISK` interfaces and resolves them to `\\.\PhysicalDriveN` paths with `IOCTL_STORAGE_GET_DEVICE_NUMBER`
  - `device_inventory.*` keeps the normalized discovered-device and mounted-volume view
  - `rescan_coordinator.*` coalesces burst notifications and posts reconciles onto the service runtime worker queue
  - `device_discovery.*` ties monitor, enumerator, prober, and mount callbacks together
- Keep the notification callback path non-blocking. `DeviceMonitor` only emits hint events; the real enumerate/probe/mount/unmount work must stay on the runtime queue through `RescanCoordinator`.
- `DeviceDiscoveryManager` currently treats ConfigMgr events as hints, not truth. It always re-enumerates present devices, diffs against `DeviceInventory`, and only then mounts or unmounts.
- Mounted-device remove/query-remove notifications are tracked separately from global disk-interface notifications. Suppressed-remount state is intentionally kept until `kMountedDeviceQueryRemoveFailed` clears it, so Orchard does not immediately remount a device during the same removal flow.
- The current `M3-T02` verification is fake-driven through `orchard_unit_mount_service`; it covers startup enumeration, removal unmount, burst-event coalescing, and query-remove suppression. Real hardware plug/unplug smoke is still pending and should be recorded before closing `M3-T02`.
- Real-hardware `M3-T02` validation against the external APFS SSD named `LM` exposed one more prerequisite: mounted shell queries on real media were stalling because `VolumeContext` still used full-tree scans on the filesystem B-tree. The bounded lookup/range fix is now in place, but the actual hardware attach/mount/unplug smoke still needs to be rerun and recorded before `M3-T02` can move to `Done`.
- On `2026-04-14`, real-hardware revalidation on `\\.\PhysicalDrive2` moved the issue from APFS traversal to service/discovery evidence:
  - `orchard-mount-smoke.exe --target '\\.\PhysicalDrive2' --mountpoint R: --volume-oid 1026 --hold-ms 300000` mounted volume `LM`, `Get-Item R:\README.md` reported length `8153`, and `Get-Content R:\README.md -TotalCount 5` returned the real file contents.
  - `orchard-service-host.exe --console --target '\\.\PhysicalDrive2' --mountpoint S: --volume-oid 1026 --hold-ms 300000` also mounted successfully and served the same `README.md` contents through `S:`.
  - `orchard-service-host.exe --console --diagnose-discovery --hold-ms 5000` enumerated `\\.\PhysicalDrive2`, probed APFS volume object ID `1026` named `LM`, and auto-mounted it at `R:` inside the console-host runtime.
  - In the same console-host auto-discovery run, a real hot-unplug removed `R:` immediately (`Get-PSDrive -Name R` and `Get-Item R:\README.md` both went empty after unplug), and reattaching the SSD brought `R:` back automatically; `Get-Content R:\README.md -TotalCount 5` then succeeded again. This is the real-hardware attach/remove/remount evidence for `M3-T02`.
- `src/mount-service/src/service_host.cpp` now accepts `--diagnose-discovery` in console mode and prints discovered devices, volumes, active mounts, and any retained auto-mount failures. Use that before changing discovery logic again.
- `src/mount-service/src/device_inventory.cpp` now retains per-volume auto-mount errors. Discovery failures should be debugged from the recorded `mount_error` on the discovered volume instead of inferred only from missing drive letters.
- `orchard-inspect.exe --target '\\.\PhysicalDrive2' --volume-oid 1026` now supports on-demand raw-device enrichment and can list the selected volume's root entries without going through the WinFsp mount path. During the same `2026-04-14` validation it still reported `ReadAt failed.` in `root_file_probes` even though the mounted read path worked, so the inspect preview path remains a separate follow-up item.
