# Orchard Task Tracker

## Purpose

This file turns the roadmap into an execution system.

Use it to track:

- Product requirements
- Milestone tasks
- Dependencies
- Definition of Done
- Required verification
- Release blockers

This tracker is the source of truth for v1 execution. The roadmap in [docs/v1-roadmap.md](./docs/v1-roadmap.md) defines strategy; this file defines completion.

## Status Legend

- `Planned`: defined but not started
- `In Progress`: actively being worked
- `Blocked`: cannot proceed due to an unresolved dependency or decision
- `Done`: implemented and verified against the listed DoD
- `Deferred`: intentionally moved out of v1

## Global Rules

- No task may move to `Done` without the listed verification evidence.
- No writable feature may ship without crash-safety coverage.
- Any unsupported or uncertain APFS feature must result in `ReadOnly` or `Reject`, never `ReadWrite`.
- If Orchard detects an internal invariant failure on a writable mount, it must downgrade to read-only or fail the operation safely.
- Explorer-visible success is not enough. Tasks close only when offline validation and mount behavior both pass.

## Global Definition Of Done

A task is only `Done` when all apply:

- Code or document changes are committed in the repo.
- Relevant tests exist and pass locally or in CI.
- Logging and error handling are present where operationally required.
- The task's acceptance criteria are met.
- The tracker is updated with status and evidence location.

## Requirement Inventory

### Product and UX requirements

- `REQ-PROD-001`
  Status: `Planned`
  Requirement: Orchard installs as a Windows product with service, tray UI, CLI, and runtime dependency handling.
  Priority: `P0`
  Target: `M3`
  Verification: Clean install, repair, uninstall tests on Windows 10 and Windows 11 VMs.

- `REQ-PROD-002`
  Status: `Planned`
  Requirement: Supported APFS user-data volumes auto-mount and appear in File Explorer with a stable drive letter.
  Priority: `P0`
  Target: `M3`
  Verification: Plug-in and reboot scenarios; drive letter persistence tests.

- `REQ-PROD-003`
  Status: `Planned`
  Requirement: Users can unmount volumes cleanly from tray UI and `orchardctl`.
  Priority: `P0`
  Target: `M3`
  Verification: Busy and idle unmount tests; remount tests.

- `REQ-PROD-004`
  Status: `Planned`
  Requirement: Orchard explains why a volume is `RW`, `RO`, hidden, or unsupported.
  Priority: `P0`
  Target: `M3`
  Verification: Policy-engine unit tests and UI/CLI status snapshots.

- `REQ-PROD-005`
  Status: `Planned`
  Requirement: Helper volumes are hidden by default and visible only in advanced mode.
  Priority: `P1`
  Target: `M3`
  Verification: Advanced view tests in UI and CLI.

### APFS read-path requirements

- `REQ-READ-001`
  Status: `In Progress`
  Requirement: Orchard discovers APFS containers from supported disks and images.
  Priority: `P0`
  Target: `M1`
  Verification: Fixture scan tests across supported corpus.

- `REQ-READ-002`
  Status: `In Progress`
  Requirement: Orchard parses checkpoints, superblocks, object maps, and filesystem B-trees correctly.
  Priority: `P0`
  Target: `M1`
  Verification: Golden parser tests, fuzz smoke tests, offline inspect output.

- `REQ-READ-003`
  Status: `Done`
  Requirement: Orchard resolves paths, enumerates directories, and reads file contents on supported volumes.
  Priority: `P0`
  Target: `M1`
  Verification: Golden file-tree tests and read checks against fixture corpus.
  Evidence: `src/apfs-core/src/volume.cpp`, `path_lookup.cpp`, and `file_read.cpp` now resolve the volume omap and filesystem tree, support case-insensitive path lookup, directory enumeration, sparse-hole reads, and whole/range reads; `tests/unit/apfs_tests.cpp` validates root listing, nested lookup, not-found/non-directory failures, multi-extent reads, sparse reads, compressed reads, and zero-length files.

- `REQ-READ-004`
  Status: `Done`
  Requirement: Orchard supports the compression algorithms required by the release corpus.
  Priority: `P0`
  Target: `M1`
  Verification: Compression corpus tests and file hash comparisons.
  Evidence: `src/apfs-core/src/compression.cpp` adds the v1 baseline decoder for `decmpfs_uncompressed_attribute`; the synthetic corpus manifest now records that algorithm, regenerated fixtures contain inline compressed files, and `tests/unit/apfs_tests.cpp` plus `orchard_inspect_*` integration tests validate compressed reads and machine-readable output.

- `REQ-READ-005`
  Status: `Planned`
  Requirement: Orchard reads existing symlinks and preserves hard-link identity where supported.
  Priority: `P1`
  Target: `M2`
  Verification: Fixture-based metadata tests and Explorer smoke tests.

### Policy and safety requirements

- `REQ-POL-001`
  Status: `Done`
  Requirement: Every discovered volume is classified into `Hide`, `MountReadOnly`, `MountReadWrite`, or `Reject` with machine-readable reasons.
  Priority: `P0`
  Target: `M1`
  Verification: Policy matrix unit tests covering all supported and unsupported feature combinations.
  Evidence: `src/apfs-core/src/policy.cpp` is now the standalone policy engine; `src/apfs-core/src/discovery.cpp` assigns policy results during discovery; `tests/unit/apfs_tests.cpp` verifies `MountReadWrite`, `MountReadOnly`, and `Reject` cases, and new integration tests cover snapshot and sealed-volume inspect output.

- `REQ-POL-002`
  Status: `Planned`
  Requirement: v1 write eligibility is restricted to the explicitly supported tier only.
  Priority: `P0`
  Target: `M4`
  Verification: Eligibility tests using synthetic metadata and real fixture volumes.

- `REQ-POL-003`
  Status: `Planned`
  Requirement: Unsupported or uncertain feature flags never mount `RW`.
  Priority: `P0`
  Target: `M4`
  Verification: Negative tests on incompatible, encrypted, sealed, case-sensitive, and snapshot-bearing fixtures.

### APFS write-path requirements

- `REQ-WRITE-001`
  Status: `Planned`
  Requirement: Orchard supports create, overwrite, append, truncate, rename, delete, mkdir, and rmdir on the writable tier.
  Priority: `P0`
  Target: `M4`
  Verification: Mutation test suite on cloned images plus Explorer-based workflow tests.

- `REQ-WRITE-002`
  Status: `Planned`
  Requirement: Orchard commits APFS mutations crash-safely and preserves fields that third-party implementations must preserve.
  Priority: `P0`
  Target: `M4`
  Verification: Crash injection, remount, and offline inspection tests.

- `REQ-WRITE-003`
  Status: `Planned`
  Requirement: On invariant failure during writable operation, Orchard degrades the mount to `RO` or fails safely.
  Priority: `P0`
  Target: `M4`
  Verification: Fault-injection tests and service behavior tests.

- `REQ-WRITE-004`
  Status: `Planned`
  Requirement: File flush and close semantics are correct enough for common Windows applications.
  Priority: `P0`
  Target: `M4`
  Verification: Copy, save, replace, and large-file workflows from Explorer and common editors.

### Operations and diagnostics requirements

- `REQ-OPS-001`
  Status: `Planned`
  Requirement: Orchard logs mount decisions, mount failures, downgrade events, and unmount results.
  Priority: `P0`
  Target: `M3`
  Verification: Structured log snapshots from mount scenarios.

- `REQ-OPS-002`
  Status: `Planned`
  Requirement: Orchard can export a diagnostic bundle for support and debugging.
  Priority: `P1`
  Target: `M3`
  Verification: Manual bundle generation test and artifact review.

- `REQ-OPS-003`
  Status: `Planned`
  Requirement: Orchard persists drive-letter preferences by volume identity.
  Priority: `P1`
  Target: `M3`
  Verification: Reattach and reboot persistence tests.

### Release requirements

- `REQ-REL-001`
  Status: `Planned`
  Requirement: Orchard ships with a support matrix documenting what is `RW`, `RO`, and unsupported.
  Priority: `P0`
  Target: `M6`
  Verification: Release docs review.

- `REQ-REL-002`
  Status: `Planned`
  Requirement: Installer, upgrade path, and uninstall are stable on the release matrix.
  Priority: `P0`
  Target: `M6`
  Verification: VM install/upgrade/uninstall test runs.

- `REQ-REL-003`
  Status: `Planned`
  Requirement: No known silent-corruption issue remains in the supported writable tier.
  Priority: `P0`
  Target: `M6`
  Verification: Final crash, mutation, and soak test reports.

## Milestone Tracker

## M0 Foundations

### Exit Gate

- Repo layout exists.
- Build and test baseline exists.
- Fixture policy exists.
- First inspection tool skeleton exists.

### Review Snapshot

- Review date: `2026-04-12`
- Locally completed: `M0-T01`, `M0-T02`, `M0-T03`, `M0-T04`, `M0-T05`, `M0-T06`
- Latest local verification:
  - `cmake --preset default`
  - `cmake --build --preset default`
  - `ctest --preset default`
  - `cmake --build --preset default --target orchard_format_check orchard_lint`
  - Result: `5/5 tests passed`, `clang-format check passed`, `clang-tidy passed`
- Remote CI verification observed on `2026-04-12`:
  - `windows-baseline`: passed
  - `windows-lint`: passed
- `M0` is complete.

### Tasks

- [x] `M0-T01` Repository scaffolding
  Status: `Done`
  Depends on: none
  Done when: `src/`, `tests/`, `tools/`, `packaging/`, and `docs/` structure exists and matches roadmap.
  Verification: Repo tree review.
  Evidence: `src/`, `tests/`, `tools/`, `packaging/wix/`, `docs/`, and `AGENTS.md` are checked in with placeholder files where needed so Git tracks the scaffold.

- [x] `M0-T02` CMake and compiler baseline
  Status: `Done`
  Depends on: `M0-T01`
  Done when: Root CMake config builds core libraries, tools, and tests on supported developer machines.
  Verification: Clean configure and build on Windows dev environment.
  Evidence: `CMakeLists.txt`, `CMakePresets.json`, `cmake/OrchardProject.cmake`, and `cmake/OrchardTest.cmake` are in place; `cmake --preset default` and `cmake --build --preset default` passed with the local Conda LLVM toolchain documented in `AGENTS.md`.

- [x] `M0-T03` Formatting and linting
  Status: `Done`
  Depends on: `M0-T02`
  Done when: Formatting and static analysis commands exist and are documented.
  Verification: Local format and lint targets pass, and the remote CI lint job passes.
  Evidence: `.editorconfig`, `.clang-format`, `.clang-tidy`, `tools/dev/orchard-format.ps1`, and `tools/dev/orchard-lint.ps1` are checked in; `orchard_format_check` and `orchard_lint` both passed locally; `.github/workflows/ci.yml` includes a dedicated `windows-lint` job, and the first observed remote `windows-lint` run passed on `2026-04-12`.

- [x] `M0-T04` Test harness baseline
  Status: `Done`
  Depends on: `M0-T02`
  Done when: Unit-test framework, integration-test harness, and fuzz target scaffolding exist.
  Verification: CI runs sample unit and fuzz smoke targets.
  Evidence: Native unit tests, integration tests, and a fuzz-smoke target are wired through `CTest`; `ctest --preset default` passed with 5/5 tests green, and `.github/workflows/ci.yml` is checked in to run the same baseline on `windows-latest` once pushed.

- [x] `M0-T05` Fixture manifest and corpus policy
  Status: `Done`
  Depends on: `M0-T04`
  Done when: Fixture inventory format is defined with labels for role, flags, compression, snapshots, and expected mount policy.
  Verification: Sample manifest checked in and validated by test harness.
  Evidence: `docs/fixture-corpus-policy.md` defines the schema and conventions; `tests/corpus/manifests/sample-fixtures.json` is validated by `orchard_validate_fixture_manifest`.

- [x] `M0-T06` `orchard-inspect` stub
  Status: `Done`
  Depends on: `M0-T02`
  Done when: CLI tool exists and can open a disk or image target and emit placeholder structured output.
  Verification: Smoke run on sample image path.
  Evidence: `tools/inspect/src/main.cpp` builds to `orchard-inspect`, emits structured JSON, and the `orchard_inspect_stub_output` integration test passed against `tests/corpus/samples/plain-user-data.img`.

## M1 APFS Reader Core

### Exit Gate

- Orchard can inspect supported fixtures offline.
- Path lookup and file reading work for the supported read tier.
- Policy engine can classify volumes deterministically.

### Review Snapshot

- Review date: `2026-04-12`
- Locally completed: `M1-T01`, `M1-T02`, `M1-T03`, `M1-T04`, `M1-T05`, `M1-T06`, `M1-T07`, `M1-T08`, `M1-T09`
- Latest local verification:
  - `cmake --preset default`
  - `tests/corpus/generate-sample-fixtures.ps1`
  - `cmake --build --preset default --parallel`
  - `cmake --build --preset default --target orchard_format_check`
  - `cmake --build --preset default --target orchard_lint`
  - `ctest --preset default`
  - Result: `9/9 tests passed`, `clang-format check passed`, `clang-tidy passed`
- Key implementation note:
  - Volume superblocks are now resolved via the container object map; the bounded fallback block scan was removed from the primary discovery path.
  - Synthetic fixtures now include a volume omap, filesystem-tree records, plain files, sparse files, and inline compressed files.

### Tasks

- [x] `M1-T01` Block I/O abstraction
  Status: `Done`
  Depends on: `M0-T02`
  Done when: Core supports reading from files, images, and raw-disk handles behind one interface.
  Verification: Unit tests with mock device backend and integration tests with sample images.
  Evidence: `src/blockio/include/orchard/blockio/error.h`, `result.h`, and `reader.h` define the typed error/result model and synchronous reader interface; `src/blockio/src/reader.cpp` implements Windows handle-backed readers plus an in-memory reader for tests; `orchard_unit_blockio_reader` and `orchard_unit_blockio` passed locally.

- [x] `M1-T02` GPT and container discovery
  Status: `Done`
  Depends on: `M1-T01`
  Done when: Orchard finds APFS containers from supported partition layouts and direct images.
  Verification: Corpus scan tests.
  Evidence: `src/apfs-core/src/discovery.cpp` detects direct `NXSB` containers and GPT-partitioned APFS candidates; `tests/corpus/samples/plain-user-data.img` and `gpt-user-data.img` are deterministic synthetic fixtures regenerated by `tests/corpus/generate-sample-fixtures.ps1`; `orchard_inspect_direct_output` and `orchard_inspect_gpt_output` passed locally.

- [x] `M1-T03` Checkpoint and superblock parser
  Status: `Done`
  Depends on: `M1-T02`
  Done when: Orchard selects the correct live checkpoint and parses container and volume superblocks.
  Verification: Golden structure tests.
  Evidence: `src/apfs-core/src/discovery.cpp` parses container and volume superblocks, selects the highest-`xid` checkpoint candidate from the descriptor area, and extracts feature flags and roles; `tests/unit/apfs_tests.cpp` verifies direct-container parsing, GPT discovery, checkpoint selection, and volume metadata; `tools/inspect/src/main.cpp` emits structured JSON for the parsed result.

- [x] `M1-T04` Object map and B-tree traversal
  Status: `Done`
  Depends on: `M1-T03`
  Done when: Orchard resolves object IDs and walks required APFS tree structures.
  Verification: Golden tree traversal tests and fuzz smoke.
  Evidence: `src/apfs-core/include/orchard/apfs/format.h`, `object.h`, `btree.h`, and `omap.h` split low-level APFS parsing into dedicated modules; `src/apfs-core/src/object.cpp`, `btree.cpp`, and `omap.cpp` implement typed object parsing, physical block reads, generic B-tree traversal, and omap lookup by `(oid, xid)`; `src/apfs-core/src/discovery.cpp` now resolves `fs_oid` values through the container omap before parsing volume superblocks; `tests/unit/apfs_tests.cpp` covers object-header parsing, leaf/internal node parsing, exact/fallback/deleted omap lookups, and end-to-end discovery; `tests/fuzz/fuzz_smoke.cpp` exercises object-header and node parsing on representative inputs.

- [x] `M1-T05` Inode, dentry, and path lookup
  Status: `Done`
  Depends on: `M1-T04`
  Done when: Orchard resolves absolute and relative paths and enumerates directory entries.
  Verification: File-tree goldens and path lookup tests.
  Evidence: `src/apfs-core/include/orchard/apfs/volume.h`, `fs_keys.h`, `fs_records.h`, and `path_lookup.h` with matching `.cpp` implementations now resolve the volume omap, decode inode/dentry records, and walk paths against the filesystem tree; `tests/unit/apfs_tests.cpp` covers root enumeration, nested lookup, not-found, non-directory, and case-insensitive traversal.

- [x] `M1-T06` File extent and data read path
  Status: `Done`
  Depends on: `M1-T05`
  Done when: Orchard reads complete file contents correctly for the supported corpus.
  Verification: File hash comparisons.
  Evidence: `src/apfs-core/include/orchard/apfs/file_read.h` and `src/apfs-core/src/file_read.cpp` now resolve extents, zero-fill sparse ranges, and support bounded reads; `tests/unit/apfs_tests.cpp` validates whole-file, range, sparse, and zero-length file reads.

- [x] `M1-T07` Compression support baseline
  Status: `Done`
  Depends on: `M1-T06`
  Done when: Required compression formats for the release corpus are readable.
  Verification: Compressed fixture test set.
  Evidence: `src/apfs-core/include/orchard/apfs/compression.h` and `src/apfs-core/src/compression.cpp` implement the current corpus baseline for `decmpfs_uncompressed_attribute`; `tests/corpus/generate-sample-fixtures.ps1` regenerates compressed fixtures and `tests/unit/apfs_tests.cpp` verifies decode behavior and metadata reporting.

- [x] `M1-T08` Volume policy engine
  Status: `Done`
  Depends on: `M1-T03`
  Done when: Core classifies each volume into `Hide`, `MountReadOnly`, `MountReadWrite`, or `Reject` with reasons.
  Verification: Policy matrix tests.
  Evidence: `src/apfs-core/src/policy.cpp` encodes the v1 precedence rules and `src/apfs-core/src/discovery.cpp` attaches policy decisions to discovered volumes; `tests/unit/apfs_tests.cpp` and new inspect integration tests validate writable, snapshot read-only, and sealed reject outcomes.

- [x] `M1-T09` `orchard-inspect` real output
  Status: `Done`
  Depends on: `M1-T08`
  Done when: Inspect tool prints structured volume metadata, roles, features, and policy outcomes.
  Verification: Snapshot-based CLI tests.
  Evidence: `src/apfs-core/src/inspection.cpp` now enriches discovered volumes with root-directory samples and file probes, and `tools/inspect/src/main.cpp` prints policy, root entries, probe reads, and volume notes; integration tests cover direct, GPT, snapshot, and sealed inspect output.

## M2 Read-Only Windows Mount

### Exit Gate

- Supported read-only volumes can be mounted through WinFsp.
- Explorer can browse and copy files out reliably.
- No shell crashes or major compatibility regressions on core workflows.

### Review Snapshot

- Review date: `2026-04-13`
- Locally completed: `M2-T01`, `M2-T02`
- Latest local verification:
  - `cmake --preset default -DORCHARD_ENABLE_WINFSP=ON`
  - `tests/corpus/generate-sample-fixtures.ps1`
  - `cmake --build --preset default --parallel`
  - `cmake --build --preset default --target orchard_format_check`
  - `cmake --build --preset default --target orchard_lint`
  - `ctest --preset default --output-on-failure`
  - `orchard-mount-smoke --target tests/corpus/samples/plain-user-data.img --mountpoint R: --hold-ms 12000`
  - PowerShell smoke browse on the mounted drive:
    - repeated `Get-ChildItem R:\`
    - nested `Get-ChildItem R:\docs`
    - file-length checks for `alpha.txt` and `compressed.txt`
    - file read for `alpha.txt`
  - Result: `9/9 tests passed`, `clang-format check passed`, `clang-tidy passed`, WinFsp drive-letter smoke mount returned stable root listings and file metadata, and unmounted cleanly
- Implementation note:
  - WinFsp-linked executables now copy `winfsp-x64.dll` beside themselves after build so `ctest` and the smoke tool do not require a manually edited `PATH`.
  - The validated M2 smoke path uses a drive-letter mount (`R:`). A directory mountpoint attempt failed at `FspFileSystemSetMountPoint` and remains follow-up work outside `M2-T01`.
  - `M2-T02` hardened query mapping by expanding APFS-side metadata, centralizing APFS-to-Windows file-info translation, making path normalization reject unsupported Windows syntax explicitly, and extracting deterministic directory query filtering into a dedicated helper.
- `M2-T03` has automated shell-stress coverage in place, including a large-directory synthetic fixture, WinFsp-side directory pagination helpers, and a dedicated PowerShell smoke script. On `2026-04-13`, manual Explorer validation was also observed: the mounted volume appeared in File Explorer and `copy-source.bin` was copied successfully to `C:\Users\luism\OneDrive\Escritorio`.
- On `2026-04-14`, follow-up real-SSD Explorer validation exposed a remaining gap after the service-backed mount succeeded: right-click, second-window navigation, and copy-out on the external `LM` SSD were still visibly sluggish, and some copy attempts failed after long stalls. The current mitigation pass keeps the read-only scope intact while reducing repeated work in the WinFsp layer: `src/fs-winfsp/src/filesystem.cpp` now implements `GetDirInfoByName`, returns an immediate empty stream list, and uses longer metadata/dir/security/stream cache timeouts for the read-only mount; `src/fs-winfsp/src/mount.cpp` now primes child-path `FileNode` records from directory enumeration and caches per-inode extent/compression read plans for repeated `Read` callbacks; `src/apfs-core/src/path_lookup.cpp` now reuses embedded child inode records during path traversal. Focused verification is currently `orchard_unit_apfs` plus `orchard_format_check`; manual Explorer/copy-out revalidation on the real SSD is the remaining follow-up before this can be recorded as resolved evidence.
- On the same `2026-04-14` real-SSD copy follow-up, the remaining `0x8007045D` copy failures were narrowed to the raw read path rather than Explorer-only behavior. `src/apfs-core/src/fs_records.cpp` now masks packed APFS file-extent flags out of the extent length, and `src/blockio/src/reader.cpp` now retries partial `ReadAt` completions until the full request is satisfied before reporting `kShortRead`. Focused regression coverage is in `tests/unit/apfs_tests.cpp` and `tests/unit/reader_tests.cpp`; focused verification passed with `orchard_unit_apfs`, `orchard_unit_blockio_reader`, and `orchard_format_check`. The remaining follow-up is manual real-SSD copy-out revalidation through the installed service after rebuilding `orchard-service-host.exe`.
- The same failing PDF (`R:\Mac Meli\omscs\cs8903\_Luis_Montoya__CS_8903_OVM_Paper_2026_1.pdf`) was then reproduced directly with sequential PowerShell reads: reads succeeded up to offset `589824`, and the failure occurred on the final short tail read near EOF rather than on the destination side of `Copy-Item`. `src/apfs-core/src/volume.cpp` now aligns `ReadPhysicalBytes` to full APFS blocks and slices the requested bytes from memory, so raw-device reads no longer depend on short unaligned `ReadFile` calls for file tails. Focused regression coverage is `FileReadPathUsesAlignedPhysicalReadsForTailBytesOnRawDevices` in `tests/unit/apfs_tests.cpp`, and `orchard_unit_apfs` now passes with that case. The remaining follow-up is still manual real-SSD copy-out revalidation after the installed service is restarted on the rebuilt `orchard-service-host.exe`.
- After single-file copy-out succeeded, the remaining real-SSD folder-copy failure was isolated to `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E\.claude`. `orchard-inspect.exe --target '\\.\PhysicalDrive2' --volume-oid 1026 --list-path '/Mac Meli/omscs/cs8903/Reproducing-TTT-E2E/.claude'` showed one child name starting with raw bytes `C9 62`, which matches a false legacy decode of an APFS hashed directory key rather than a real file name. Apple’s APFS reference defines `j_drec_hashed_key_t` with a 32-bit `name_len_and_hash` field, so `src/apfs-core/src/fs_keys.cpp` now parses the hashed layout before falling back to the older 16-bit synthetic key shape. Focused regression coverage is `ParseDirectoryRecordKeyPrefersHashedLayoutWhenLegacyLengthAlsoFits` in `tests/unit/apfs_tests.cpp`. The remaining follow-up is manual real-SSD folder-copy revalidation after rebuilding and restarting the installed service.
- The next real-SSD folder-copy failure then narrowed to APFS symlink projection rather than raw reads or directory parsing: recursive copy and direct `Copy-Item` both failed on `.venv\bin\python`, `python3`, and `python3.12` because Windows followed those APFS symlinks and the final targets did not resolve within the mounted volume. Orchard now avoids mounted Windows reparse semantics for this v1 read-only path: `src/fs-winfsp/src/mount.cpp` dereferences in-volume symlink chains to their final APFS inode before WinFsp open/read/directory handling sees them, while `src/fs-winfsp/src/file_info.cpp` and `src/fs-winfsp/src/filesystem.cpp` surface unresolved/out-of-volume chains as opaque read-only files whose bytes are the stored POSIX symlink target. Focused regression coverage is `WinFspBrokenSymlinkFallsBackToOpaqueFileRead` in `tests/unit/apfs_tests.cpp`, and synthetic verification now shows relative/absolute symlink path reads plus recursive folder copy succeeding without aborting on broken links. The remaining follow-up is manual real-SSD service revalidation after rebuilding `orchard-service-host.exe`.
- The next recursive-copy follow-up exposed one more WinFsp-side mismatch: Orchard applied the mounted symlink policy during path open/read, but `src/fs-winfsp/src/directory_query.cpp` still surfaced raw APFS symlink metadata during enumeration. That let Explorer and PowerShell discover one object shape and then open another, which is consistent with the remaining folder-copy failures under `Reproducing-TTT-E2E`. The mounted view is now projected consistently in both places: `BuildDirectoryQueryEntry` resolves symlink children through `MountedVolume::ResolveFileNode`, and unresolved symlinks are downgraded to mounted regular files before directory query results are emitted. Focused verification passed with `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is manual real-SSD service revalidation after rebuilding `orchard-service-host.exe`.
- The remaining high-latency discovery symptom in that tree still pointed at symlink-heavy directory aliases. Orchard now keeps the mounted read-through behavior only for symlinks that resolve to regular files; if a symlink resolves to a directory, Orchard falls back to the same opaque read-only file surface used for broken or out-of-volume links. This avoids recursive copy descending into alias directories such as `latest-run` while keeping the v1 read-only scope conservative. Focused verification remains `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`; the remaining follow-up is manual real-SSD service revalidation after rebuilding `orchard-service-host.exe`.
- The next follow-up is now explicitly performance, not correctness: recursive copy of `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E` can complete, but discovery on that deep small-file/symlink-heavy tree is still materially slower than ordinary folders. The current mitigation pass attacks the repeated-work layer rather than changing v1 semantics again. `src/apfs-core/src/object.cpp` and `src/apfs-core/src/volume.cpp` now share a mount-lifetime APFS physical-block cache sized to roughly `32 MiB`, so repeated fs-tree and omap traversals stop issuing the same raw block reads on every lookup. `src/apfs-core/src/path_lookup.cpp` now records path-lookup counters (`path_lookup_calls`, walked components, and directory enumerations) through `VolumeContext::performance_stats()`, and `src/fs-winfsp/src/mount.cpp` no longer forces a full `GetFileMetadata` refresh when a regular file or directory was already primed from enumeration; only symlink nodes still run the mounted-view projection pass. Focused regression coverage is `VolumePathLookupTracksStatsAndReusesCachedBlocks` plus the updated `WinFspMountedVolumePrimesEnumeratedChildNodes` in `tests/unit/apfs_tests.cpp`. Focused verification passed with direct `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is manual real-SSD Explorer revalidation after stopping the installed Orchard service, rebuilding `orchard-service-host.exe`, and restarting the service on the new binary.
- A second pass now targets the remaining discovery stall inside that same tree by reducing repeated allocations and inode lookups rather than changing mounted behavior again. `src/apfs-core/src/volume.cpp` now memoizes inode records inside `VolumeContext`, `src/fs-winfsp/src/mount.cpp` now returns cached directory-entry vectors through shared handles instead of copying them on every hit, and `src/fs-winfsp/src/filesystem.cpp` now caches the derived WinFsp `DirectoryQueryEntry` vectors per directory inode inside the active mount session. Focused regression coverage is the updated `VolumePathLookupTracksStatsAndReusesCachedBlocks` plus `WinFspMountedVolumeReusesCachedDirectoryViews` in `tests/unit/apfs_tests.cpp`. Focused verification passed with `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is still manual real-SSD service revalidation against `Reproducing-TTT-E2E` after rebuilding and restarting `orchard-service-host.exe`.
- A third pass now targets the remaining “fast start, then sharp slowdown” shape more directly: `ReadDirectory` was still rebuilding a fully filtered `DirectoryQueryEntry` vector on every paged directory callback, which becomes expensive only after Explorer reaches a genuinely large directory. `src/fs-winfsp/src/directory_query.cpp` now exposes `PaginateFilteredDirectoryQueryEntries`, which resumes directly from the marker inside the cached query-entry vector and emits one page without materializing the whole filtered list. `src/fs-winfsp/src/filesystem.cpp` now uses that path for live `ReadDirectory`, and `WinFspLargeDirectoryPaginationResumesDeterministically` in `tests/unit/apfs_tests.cpp` now exercises the same pagination logic. Focused verification passed with `orchard_unit_apfs`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is still manual real-SSD service revalidation after rebuilding and restarting `orchard-service-host.exe`.
- The first explicit native-feel observability slice is now in place before any further deep optimization work. `src/fs-winfsp/include/orchard/fs_winfsp/mount.h` defines mount-session callback performance counters, `src/fs-winfsp/src/filesystem.cpp` records live per-callback count/total/max/slow-call stats for the main Explorer-facing WinFsp callbacks, `src/mount-service/src/mount_registry.cpp` now returns live performance snapshots through `ListMounts()` / `GetMount()`, and `src/mount-service/src/service_host.cpp` now accepts `--diagnose-perf` in console mode to print per-mount WinFsp callback stats beside the existing APFS path/block-cache counters and mounted-volume cache counters. Focused verification passed with `orchard_unit_mount_service`, `orchard_unit_apfs`, and `orchard_format_check`. The remaining follow-up is real-SSD usage of `--diagnose-perf` against the pathological tree before deciding the next optimization pass.
- Real-SSD `--diagnose-perf` is now recorded against `R:\Mac Meli\omscs\cs8903\Reproducing-TTT-E2E`. The callback mix showed `Read` dominating total callback time by a wide margin, with `GetSecurityByName` and `Open` already relatively cheap and `ReadDirectory` only a secondary contributor. The same run also exposed a WinFsp callback-semantics bug rather than another APFS parse failure: `src/fs-winfsp/src/filesystem.cpp` returned `STATUS_BUFFER_TOO_SMALL` whenever `PaginateFilteredDirectoryQueryEntries(...)` yielded an empty page inside a non-empty directory, even when the marker had already advanced past the last matching child. PowerShell recursive copy then surfaced that as `The data area passed to a system call is too small.` on `.git\objects\xx` directories. The fix is now in place, and `tests/unit/apfs_tests.cpp` covers both “marker past end” and “pattern matches nothing” through `WinFspDirectoryPaginationReturnsEmptySuccessPageWhenMarkerIsPastEnd`. The remaining follow-up is another real-SSD rebuild/retest to confirm that the `.git\objects` recursive-copy failure is gone and to measure what portion of the remaining latency is now pure read throughput.
- The follow-up real-SSD run confirmed the enumeration fix: the `.git\objects\xx` insufficient-buffer failures are gone, but recursive copy still took roughly five minutes and then ended in `OperationCanceledException`, which matches the `--hold-ms 300000` console-host timeout rather than another deterministic copy bug. The same perf dump showed `Read` still dominating callback time while `GetSecurityByName`, `Open`, and `ReadDirectory` stayed comparatively small. The next performance pass therefore moved one layer down into APFS-core: `src/apfs-core/src/object.cpp` now adds `PhysicalBlockCache::ReadBlocks(...)`, which coalesces contiguous cache misses into a single larger raw read and then seeds the per-block cache from that result, and `src/apfs-core/src/volume.cpp` now uses that range-read path for aligned physical reads. Focused regression coverage is `VolumeContextCoalescesContiguousPhysicalBlockMissesIntoOneDeviceRead` in `tests/unit/apfs_tests.cpp`. The remaining follow-up is a longer-hold real-SSD retest to see how much wall-clock copy time drops once cold sequential reads stop degenerating into one raw device I/O per APFS block.
- The longer-hold real-SSD retest confirmed the coalesced-read improvement but also showed it was not yet enough for native-feel recursive copy on `Reproducing-TTT-E2E`: `device_reads` fell from the earlier six-figure range to `53346`, and average `Read` callback latency improved materially, but `Read` still dominated total callback time and the copy again ended in `OperationCanceledException` after running into the console-host hold timeout. The next pass therefore moves up one layer into the mounted read cache rather than further into raw I/O: `src/fs-winfsp/src/mount.cpp` now adds per-inode sequential read-ahead over the existing extent cache, and `src/mount-service/src/service_host.cpp` now prints `read_ahead_hits` and `read_ahead_prefetches` through `--diagnose-perf` so the next real-SSD run can confirm whether Explorer/PowerShell copy is actually hitting the new cache. Focused regression coverage is `WinFspMountedVolumeReadAheadServesSequentialReadsFromCache` in `tests/unit/apfs_tests.cpp`. The remaining follow-up is another real-SSD retest with a hold timeout comfortably above the current copy duration so cancellation does not mask the next throughput result.
- The first real-SSD run after adding mounted read-ahead showed a bad policy interaction rather than a clean win: `read_ahead_prefetches` jumped to `19988`, which is far too high for a tree dominated by many small files and suggests Orchard was over-reading aggressively across the recursive copy workload. The mounted read-ahead gate is now deliberately narrow in `src/fs-winfsp/src/mount.cpp`: it only activates after three strictly sequential reads, only for files at least `512 KiB`, and only when the current request size is already at least `64 KiB`. That keeps the old extent cache benefits for ordinary repeated reads while avoiding speculative I/O on tiny git objects and other small-file churn. The remaining follow-up is another real-SSD retest to confirm that `read_ahead_prefetches` collapses and that wall-clock recursive copy time stops regressing.
- The next real-SSD retest confirmed that the narrower read-ahead gate eliminated the speculative-I/O regression (`read_ahead_hits=0`, `read_ahead_prefetches=0`), but recursive copy of `Reproducing-TTT-E2E` was still far too slow and `Read` still dominated the callback mix. That isolated a different fixed per-file cost in the WinFsp read path: for primed ordinary files, `src/fs-winfsp/src/mount.cpp` still fell back to full `GetFileMetadata()` on the first read, which reloaded the inode and scanned for the compression xattr even when the file was uncompressed. The mounted read path now reuses the inode-derived metadata already primed from directory enumeration and caches compression-xattr parsing per inode, while `src/mount-service/src/service_host.cpp` now prints `compression_info_hits` / `compression_info_misses` through `--diagnose-perf` so the next real-SSD run can confirm the remaining callback cost moved elsewhere. Focused regression coverage is `WinFspMountedVolumeCachesCompressionInfoForPrimedRegularFiles` in `tests/unit/apfs_tests.cpp`, and focused verification passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is another real-SSD rebuild/retest against the pathological tree.
- The follow-up real-SSD retest showed that the compression-info cache was behaving as intended (`compression_info_hits=170658`, `compression_info_misses=26410`) but recursive copy was still timing out after hours, with `Read` still dominating. That points at chunked small-file reads rather than per-file metadata reloads. The mounted read path in `src/fs-winfsp/src/mount.cpp` now caches the full payload of ordinary small files (currently up to `128 KiB`) for the lifetime of the open handle and drops that ephemeral payload cache on close, while `src/mount-service/src/service_host.cpp` now reports `whole_file_hits` / `whole_file_misses` through `--diagnose-perf` so the next real-SSD run can confirm whether repeated chunked reads on small files are being collapsed. Focused regression coverage is `WinFspMountedVolumeCachesWholeSmallFileAcrossChunkedReads` in `tests/unit/apfs_tests.cpp`, and focused verification passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, `orchard_format_check`, and `tools/dev/orchard-winfsp-link-smoke.ps1`. The remaining follow-up is another real-SSD rebuild/retest against `Reproducing-TTT-E2E`.
- The next diagnostics pass replaces that failed small-file optimization with the request-shape instrumentation Orchard actually needs. The real-SSD `.git\objects` retest showed the whole-file cache never produced hits and regressed wall-clock time, so `src/fs-winfsp/src/filesystem.cpp` now tracks read requests per open WinFsp file context and records per-open read-count buckets on close instead of trying to infer open behavior from inode caches. `src/fs-winfsp/src/mount.cpp` now records logical `read_requested_bytes` vs `read_fetched_bytes`, request-size buckets, sequential vs non-sequential requests, and large-file small-request counts. `src/apfs-core/src/object.cpp` and `src/apfs-core/include/orchard/apfs/object.h` now split physical block-cache stats into metadata vs file-data accounting, each with requested/fetched byte totals, so `--diagnose-perf` can finally show whether Orchard is slow because WinFsp is issuing too many small reads or because APFS-core is amplifying those reads into too much device traffic. `src/mount-service/src/service_host.cpp` now prints those new counters, and focused regression coverage now lives in `WinFspMountedVolumeTracksReadTelemetryBuckets` plus the updated block-cache assertions in `VolumePathLookupTracksStatsAndReusesCachedBlocks` and `VolumeContextCoalescesContiguousPhysicalBlockMissesIntoOneDeviceRead`. Focused verification passed with `orchard_unit_apfs`, `orchard_unit_mount_service`, and `orchard_format_check`. The remaining follow-up is a short real-SSD `--diagnose-perf` rerun against `.git\objects` and then the full pathological tree so the next optimization can target the measured request pattern rather than another heuristic.
- `M2-T04` now adds a dedicated link-behavior fixture, an APFS-core link reader API, Windows reparse translation utilities, and a mounted link smoke path. On `2026-04-14`, the mounted v1 read-only surface was tightened further: relative and absolute symlink paths are transparently dereferenced inside Orchard, while broken/out-of-volume symlinks fall back to opaque target-text files so Explorer copy-out remains reliable.

### Tasks

- [x] `M2-T01` WinFsp adapter skeleton
  Status: `Done`
  Depends on: `M1-T06`
  Done when: Orchard mounts a read-only filesystem stub through WinFsp.
  Verification: Manual mount smoke test.
  Evidence: `cmake/FindWinFsp.cmake` and `cmake/OrchardProject.cmake` now discover the WinFsp developer SDK/runtime and copy `winfsp-x64.dll` into WinFsp-linked output directories; `src/fs-winfsp/` now contains the real adapter split across `mount.cpp`, `filesystem.cpp`, `path_bridge.cpp`, and `file_info.cpp`; `tools/mount-smoke/src/main.cpp` mounts a selected APFS volume and supports timed clean shutdown with `--hold-ms`; `tests/unit/apfs_tests.cpp` covers path bridging and mounted-volume opening/policy gating; the local smoke run mounted `tests/corpus/samples/plain-user-data.img` at `R:`, browsed `docs`, `alpha.txt`, `compressed.txt`, and `holes.bin`, read `alpha.txt`, and observed clean unmount when the timed hold expired.

- [x] `M2-T02` File and directory query mapping
  Status: `Done`
  Depends on: `M2-T01`
  Done when: Windows file info, directory enumeration, and path resolution map correctly to APFS data.
  Verification: Integration tests and Explorer browsing tests.
  Evidence: `src/apfs-core/include/orchard/apfs/fs_records.h` and `file_read.h` now expose inode timestamps, allocated size, link count, mode, child count, and internal flags from the APFS reader core; `src/fs-winfsp/src/file_info.cpp` is now the single APFS-to-Windows metadata mapping layer; `src/fs-winfsp/src/path_bridge.cpp` normalizes repeated separators, resolves `.` and `..`, preserves matched case-insensitive names, and rejects stream syntax plus unsupported characters; `src/fs-winfsp/src/directory_query.cpp` centralizes deterministic directory-entry building, marker handling, and wildcard filtering; `tests/unit/apfs_tests.cpp` now covers metadata mapping, deterministic directory filtering, path normalization reject cases, and stable open-node identity reuse; local smoke verification against `plain-user-data.img` observed stable repeated root listings (`docs,alpha.txt,compressed.txt,holes.bin`), nested listing (`empty.txt,note.txt`), correct file lengths (`alpha.txt` = `14`, `compressed.txt` = `19`), successful file read (`Hello Orchard\n`), and clean timed unmount.

- [x] `M2-T03` Read-path Explorer compatibility
  Status: `Done`
  Depends on: `M2-T02`
  Done when: Open, preview, copy-out, and large-directory enumeration work in Explorer.
  Verification: Manual and automated shell smoke tests.
  Evidence: `tests/corpus/generate-sample-fixtures.ps1` now emits `tests/corpus/samples/explorer-large.img`, a synthetic stress volume with a multi-leaf filesystem tree, a 181-entry directory, nested subdirectory content, and a larger regular file for copy-out; `tests/corpus/manifests/sample-fixtures.json` and `tests/integration/CMakeLists.txt` now include the stress fixture and inspect validation; `src/fs-winfsp/src/directory_query.cpp` now exposes deterministic paging through `PaginateDirectoryQueryEntries`, and `src/fs-winfsp/src/filesystem.cpp` uses that paging helper plus explicit `STATUS_BUFFER_TOO_SMALL` handling for undersized directory buffers; `src/fs-winfsp/src/mount.cpp` now canonicalizes open-node cache keys after lookup so case-insensitive repeated opens reuse the same node identity; `tests/unit/apfs_tests.cpp` covers large-directory path lookup, large-file reads, case-variant node reuse, and marker/resume paging across the stress fixture; `tools/dev/orchard-winfsp-shell-smoke.ps1` mounts the stress fixture, validates repeated root and large-directory listings, nested preview reads, copy-out hash equality, and clean unmount; on `2026-04-13` the user confirmed the mounted volume appeared in File Explorer and copied `copy-source.bin` successfully to `C:\Users\luism\OneDrive\Escritorio`.

- [x] `M2-T04` Symlink and hard-link read behavior
  Status: `Done`
  Depends on: `M2-T02`
  Done when: Existing symlinks and hard links behave consistently with v1 support policy.
  Verification: Metadata integration tests and mounted link smoke tests.
  Evidence: `tests/corpus/generate-sample-fixtures.ps1` now emits `tests/corpus/samples/link-behavior.img`, a synthetic APFS-like volume containing relative, absolute, and broken symlinks plus same-directory and cross-directory hard-link aliases; `tests/corpus/manifests/sample-fixtures.json` and `tests/integration/CMakeLists.txt` now include the link fixture and inspect validation; `src/apfs-core/include/orchard/apfs/link_read.h` and `src/apfs-core/src/link_read.cpp` now expose a dedicated, Windows-agnostic symlink-target and link-info API; `src/apfs-core/src/inspection.cpp`, `src/apfs-core/include/orchard/apfs/discovery.h`, and `tools/inspect/src/main.cpp` now surface `link_count`, `symlink_target`, and visible alias paths in `orchard-inspect`; `src/fs-winfsp/include/orchard/fs_winfsp/reparse.h` and `src/fs-winfsp/src/reparse.cpp` now translate APFS symlink targets into Windows reparse data; `src/fs-winfsp/src/filesystem.cpp` now implements `ResolveReparsePoints` and `GetReparsePoint`, removes the blanket symlink-open rejection, and keeps reparse writes fail-closed; `tests/unit/apfs_tests.cpp` covers symlink target parsing, inspect link metadata, reparse translation, and hard-link identity mapping; local verification passed with `11/11` tests, `clang-format check passed for 58 file(s)`, `clang-tidy passed for 32 translation unit(s)`, and `tools/dev/orchard-winfsp-link-smoke.ps1`, which mounted `link-behavior.img` at `R:`, observed `ReadOnly, ReparsePoint` attributes on both symlink files, read `Nested note` through the relative symlink, read `Hello Orchard` through the absolute symlink, confirmed matching hard-link hashes, read the cross-directory alias, and observed clean failure on the broken symlink.

- [x] `M2-T05` Read-only mount stress baseline
  Status: `Done`
  Depends on: `M2-T03`
  Done when: Repeated mount/unmount and copy-out scenarios run without leaks or hangs.
  Verification: Soak smoke test.
  Evidence: `tools/mount-smoke/src/main.cpp` now accepts `--shutdown-event <name>` so local automation can request graceful unmount without killing the process; `tools/dev/OrchardWinFspTestCommon.ps1` now centralizes reusable drive-letter allocation, mount startup, process telemetry capture, and timed unmount verification for WinFsp smoke tooling; `tools/dev/orchard-winfsp-shell-smoke.ps1` and `tools/dev/orchard-winfsp-link-smoke.ps1` now reuse that shared helper instead of duplicating mount lifecycle logic; `tools/dev/orchard-winfsp-soak.ps1` now runs repeated mount/browse/copy-out cycles across `plain-user-data.img`, `explorer-large.img`, and `link-behavior.img`, captures per-run mount/unmount latency plus handle/private-byte telemetry, and fails on timeouts, stale drive letters, or obvious upward drift after warm-up; local verification passed with `cmake --preset default -DORCHARD_ENABLE_WINFSP=ON`, `cmake --build --preset default --parallel`, `cmake --build --preset default --target orchard_format_check`, `cmake --build --preset default --target orchard_lint`, `ctest --preset default --output-on-failure`, and `tools/dev/orchard-winfsp-soak.ps1 -Cycles 25`, which completed 25 cycles per scenario without hangs or stale mounts and observed max mount/unmount latencies of `378/268 ms` on `plain-user-data`, `324/268 ms` on `explorer-large`, and `316/266 ms` on `link-behavior`, with max handle counts of `148`, `148`, and `147` respectively.

## M3 Mount Service, UI, and Product Control Plane

### Exit Gate

- Orchard installs as a product.
- Supported volumes auto-mount.
- Users can inspect status and unmount from UI or CLI.

### Review Snapshot

- Review date: `2026-04-13`
- Locally completed: `M3-T01`
- Active implementation: `M3-T02`
- Latest local verification:
  - `cmake --build --preset default --parallel`
  - `cmake --build --preset default --target orchard_format_check`
  - `cmake --build --preset default --target orchard_lint`
  - `ctest --preset default --output-on-failure`
  - `tools/dev/orchard-service-console-smoke.ps1`
  - `tools/dev/orchard-service-smoke.ps1` from an elevated shell
  - Result: `12/12 tests passed`, `clang-format check passed`, `clang-tidy passed for 43 translation unit(s)`, the console-host smoke mounted `plain-user-data.img`, validated `alpha.txt` plus the nested note, and exited cleanly, the elevated SCM smoke installed a temporary service, observed `Running`, stopped it, and uninstalled it cleanly with final status `stopped_after_smoke`, and the new fake-driven `orchard_unit_mount_service` coverage exercised startup enumeration, removal cleanup, burst-event coalescing, and query-remove suppression for `M3-T02`

### Tasks

- [x] `M3-T01` Windows service host
  Status: `Done`
  Depends on: `M2-T05`
  Done when: Background service starts on boot and manages mount lifecycle.
  Verification: Service install and startup tests.
  Evidence: `src/mount-service/` is now a real service-host implementation rather than a placeholder, split across `service_host`, `runtime`, `mount_registry`, `service_state`, and `types`; `src/mount-service/src/main.cpp` now builds `orchard-service-host.exe` with `--console`, `--install`, and `--uninstall` entry points; `tests/unit/mount_service_tests.cpp` covers service-state transitions, duplicate mount rejection, runtime stop idempotence, and console command-line parsing; `tools/dev/orchard-service-console-smoke.ps1` validates the console-host runtime by mounting `tests/corpus/samples/plain-user-data.img`, reading the mounted contents, and requesting graceful shutdown through a named event; `src/CMakeLists.txt` now configures `fs-winfsp` before `mount-service`, and `src/mount-service/CMakeLists.txt` now copies `winfsp-x64.dll` beside `orchard-service-host.exe`, fixing the local loader failure shown by `winfsp-x64.dll was not found`; `src/mount-service/src/service_host.cpp` now persists explicit startup-mount arguments into the installed service `ImagePath`, honors them again in the SCM dispatcher path, and reuses an already-matching active mount during startup instead of failing if discovery reached the same volume first; `src/mount-service/src/device_discovery.cpp` now adopts matching mounts already present in the registry before allocating a new drive letter, so explicit startup mounts and the initial auto-discovery rescan do not double-mount the same APFS volume; `tools/dev/orchard-service-smoke.ps1` now uses `System.Diagnostics.Stopwatch` for portable timeout handling and validates that the installed service-mounted drive appears, serves the sample files, and disappears after stop; local verification passed with `cmake --build --preset default --parallel`, `cmake --build --preset default --target orchard_format_check`, `cmake --build --preset default --target orchard_lint`, `ctest --preset default --output-on-failure`, `tools/dev/orchard-service-console-smoke.ps1`, and an elevated `tools/dev/orchard-service-smoke.ps1` run that installed `OrchardSmoke-5d834718`, observed `Running`, stopped the service, and returned final status `stopped_after_smoke`.

- [x] `M3-T02` Device arrival and removal detection
  Status: `Done`
  Depends on: `M3-T01`
  Done when: Service detects supported disk attach/detach events and rescans appropriately.
  Verification: Fake-driven reconcile tests plus real plug/unplug scenario tests.
  Evidence: `src/mount-service/include/orchard/mount_service/device_monitor.h`, `device_enumerator.h`, `device_inventory.h`, `rescan_coordinator.h`, and `device_discovery.h` now define the device-discovery layer for the service runtime; the matching `.cpp` files under `src/mount-service/src/` now register `CM_Register_Notification` callbacks for `GUID_DEVINTERFACE_DISK`, enumerate present disks through `CM_Get_Device_Interface_ListW`, resolve those interfaces to `\\.\PhysicalDriveN` via `IOCTL_STORAGE_GET_DEVICE_NUMBER`, coalesce hotplug bursts onto the runtime worker queue, diff a persistent device inventory, probe candidates through the existing APFS discovery/policy path, mount auto-mountable volumes through the runtime registry callbacks, and unmount tracked sessions on removal/query-remove; `src/mount-service/src/runtime.cpp` now owns the `DeviceDiscoveryManager` lifecycle so discovery starts with the service and shuts down before worker teardown; `tests/unit/mount_service_tests.cpp` now covers inventory diffing, rescan coalescing, startup enumeration and auto-mount, device removal cleanup, duplicate burst handling, query-remove suppression until query-remove-failed clears it, and retained auto-mount failures on discovered volumes; during real-hardware validation, `src/apfs-core/src/btree.cpp`, `src/apfs-core/src/fs_search.cpp`, and `src/apfs-core/src/volume.cpp` were updated to replace full-tree filesystem scans with bounded lower-bound/range traversal, the synthetic fixture builders were corrected to sort APFS fs-tree keys by decoded key fields instead of raw little-endian bytes, `src/apfs-core/src/fs_records.cpp` was updated to accept the real APFS xattr value layout used by the external SSD, `orchard-inspect` gained on-demand raw-device enrichment through `--volume-oid`, and `orchard-service-host.exe --console --diagnose-discovery` now prints discovered devices, volumes, active mounts, and retained mount failures. Real-hardware validation on `\\.\PhysicalDrive2` now covers startup enumeration, auto-mount, unplug removal, and reattach remount: `orchard-mount-smoke` mounted `LM` at `R:` and served `README.md`, `orchard-service-host.exe --console --target '\\.\PhysicalDrive2' --mountpoint S: --volume-oid 1026 --hold-ms 300000` mounted the same volume and served `S:\README.md`, `orchard-service-host.exe --console --diagnose-discovery --hold-ms 5000` enumerated `\\.\PhysicalDrive2`, identified volume object ID `1026` (`LM`), and auto-mounted it at `R:`, unplugging the SSD removed `R:` immediately, and reattaching the SSD restored `R:` and `Get-Content R:\README.md -TotalCount 5` succeeded again.

- [ ] `M3-T03` Drive-letter persistence
  Status: `Planned`
  Depends on: `M3-T01`
  Done when: Service stores and reuses drive-letter assignments by volume identity.
  Verification: Reattach and reboot tests.

- [ ] `M3-T04` Named-pipe control API
  Status: `Planned`
  Depends on: `M3-T01`
  Done when: UI and CLI can query mounts and request mount/unmount through a stable control interface.
  Verification: API integration tests.

- [ ] `M3-T05` Tray UI
  Status: `Planned`
  Depends on: `M3-T04`
  Done when: Tray app shows mounted volumes, mode, and policy reason and exposes open/unmount/details/log actions.
  Verification: UI smoke tests on Windows 10 and 11.

- [ ] `M3-T06` `orchardctl`
  Status: `Planned`
  Depends on: `M3-T04`
  Done when: CLI supports `status`, `mount`, `unmount`, and `inspect`.
  Verification: CLI scenario tests.

- [ ] `M3-T07` Clean unmount flow
  Status: `Planned`
  Depends on: `M3-T05`
  Done when: Service blocks new opens, flushes, requests unmount, and returns clear `busy` states.
  Verification: Idle and busy unmount tests.

- [ ] `M3-T08` Installer and uninstall baseline
  Status: `Planned`
  Depends on: `M3-T01`
  Done when: Installer deploys service, tray app, CLI, and runtime dependency handling.
  Verification: Clean install and uninstall VM tests.

- [ ] `M3-T09` Structured logging and diagnostics bundle
  Status: `Planned`
  Depends on: `M3-T04`
  Done when: Service and CLI can emit logs and export a support bundle.
  Verification: Diagnostic bundle generation test.

## M4 Restricted Write Engine

### Exit Gate

- Writable mounts are enabled only for the supported tier.
- Supported file and directory mutations are correct and crash-safe.
- Faults downgrade to read-only or fail safely.

### Tasks

- [ ] `M4-T01` Writable eligibility gate
  Status: `Planned`
  Depends on: `M1-T08`
  Done when: A dedicated gate enforces the v1 writable tier and rejects all other configurations.
  Verification: Eligibility matrix tests.

- [ ] `M4-T02` APFS allocator and free-space updates
  Status: `Planned`
  Depends on: `M1-T04`
  Done when: Core can allocate and account for blocks needed by supported mutations.
  Verification: Allocation unit tests and offline inspect checks.

- [ ] `M4-T03` Inode and dentry mutation path
  Status: `Planned`
  Depends on: `M4-T02`
  Done when: Create, rename, delete, mkdir, and rmdir update APFS structures correctly.
  Verification: Mutation tests on cloned images.

- [ ] `M4-T04` File data write and truncate path
  Status: `Planned`
  Depends on: `M4-T02`
  Done when: Overwrite, append, truncate, and extend work for supported files.
  Verification: File-content and size mutation tests.

- [ ] `M4-T05` Commit and flush engine
  Status: `Planned`
  Depends on: `M4-T03`, `M4-T04`
  Done when: Orchard commits supported mutations in a crash-safe order and flushes consistently.
  Verification: Crash injection and remount tests.

- [ ] `M4-T06` Read-only downgrade path
  Status: `Planned`
  Depends on: `M4-T05`
  Done when: Internal invariant failures or fatal write errors downgrade the mount or fail safely.
  Verification: Fault injection tests.

- [ ] `M4-T07` Windows create/write/delete integration
  Status: `Planned`
  Depends on: `M4-T05`
  Done when: Explorer and common apps can create, save, replace, rename, and delete on supported writable mounts.
  Verification: Explorer workflow tests and editor save tests.

- [ ] `M4-T08` Writable unmount integrity
  Status: `Planned`
  Depends on: `M4-T07`
  Done when: Normal unmount flushes and remounts without replay ambiguity for supported operations.
  Verification: Write, unmount, remount, inspect test suite.

## M5 Hardening and Beta

### Exit Gate

- No known silent-corruption bugs remain in the supported tier.
- Release candidate is stable on the test matrix.

### Tasks

- [ ] `M5-T01` Parser fuzzing expansion
  Status: `Planned`
  Depends on: `M1-T09`
  Done when: Core parser coverage reaches all major on-disk structure parsers.
  Verification: Fuzz run reports and crash triage log.

- [ ] `M5-T02` Writable crash campaign
  Status: `Planned`
  Depends on: `M4-T08`
  Done when: Repeated crash injection across the writable matrix completes without silent corruption.
  Verification: Crash campaign report.

- [ ] `M5-T03` Soak and leak testing
  Status: `Planned`
  Depends on: `M4-T08`
  Done when: Long-running mount/read/write/unmount cycles show stable memory and handle behavior.
  Verification: Soak test logs.

- [ ] `M5-T04` Performance baseline
  Status: `Planned`
  Depends on: `M4-T07`
  Done when: Read and write throughput plus large-directory behavior are measured and tracked.
  Verification: Benchmark report.

- [ ] `M5-T05` Installer upgrade path
  Status: `Planned`
  Depends on: `M3-T08`
  Done when: Upgrade from prior beta to current beta works without orphaned services or stale mounts.
  Verification: VM upgrade tests.

- [ ] `M5-T06` Beta docs and support matrix
  Status: `Planned`
  Depends on: `M5-T02`
  Done when: Beta users get explicit support boundaries, backup guidance, and troubleshooting steps.
  Verification: Docs review.

## M6 Release

### Exit Gate

- Final installer passes release matrix.
- Writable tier passes final gates.
- Release documentation is complete.

### Tasks

- [ ] `M6-T01` Final support matrix
  Status: `Planned`
  Depends on: `M5-T06`
  Done when: Supported `RW`, `RO`, and unsupported configurations are documented without ambiguity.
  Verification: Release docs review.

- [ ] `M6-T02` Final installer and packaging
  Status: `Planned`
  Depends on: `M5-T05`
  Done when: GA installer, uninstall, and repair paths are stable.
  Verification: Final VM install matrix run.

- [ ] `M6-T03` Final diagnostics and troubleshooting docs
  Status: `Planned`
  Depends on: `M3-T09`
  Done when: Users can collect logs and understand common failure modes.
  Verification: Docs review.

- [ ] `M6-T04` Release candidate signoff
  Status: `Planned`
  Depends on: `M6-T01`, `M6-T02`, `M6-T03`
  Done when: All `P0` requirements are `Done`, all release blockers are cleared, and final test reports are attached.
  Verification: Release checklist signoff.

## Test Requirements By Milestone

### M0 required evidence

- CI builds project skeleton.
- Unit-test framework runs.
- Sample fuzz target runs.

### M1 required evidence

- Parser golden tests.
- Corpus scan tests.
- Path lookup tests.
- File hash comparison tests.
- Fuzz smoke on core parsers.

### M2 required evidence

- Explorer browse and copy-out tests.
- Large-directory enumeration tests.
- Repeated read-only mount/unmount tests.

### M3 required evidence

- Service boot/start tests.
- Plug/unplug tests.
- Drive-letter persistence tests.
- Tray UI smoke tests.
- Busy and idle unmount tests.
- Install/uninstall tests.

### M4 required evidence

- Mutation tests on cloned images.
- Crash injection tests.
- Remount and inspect validation after writes.
- Explorer save/replace/delete workflow tests.
- Fault-injection downgrade tests.

### M5 required evidence

- Extended fuzz reports.
- Soak and leak reports.
- Crash campaign report.
- Beta install and upgrade report.

### M6 required evidence

- Final release matrix report.
- Final support matrix.
- Final checklist with no open `P0` blocker.

## Release Blockers

These block v1 release until cleared:

- Any known silent-corruption issue in the supported writable tier.
- Any case where unsupported feature flags can mount `RW`.
- Any reproducible Explorer crash or system hang in common workflows.
- Any unmount path that can lose acknowledged writes.
- Any installer path that leaves the system in a broken state after uninstall or upgrade.
- Any mismatch between documented support matrix and real policy behavior.

## Open Design Spikes

- [x] `SPIKE-01` WinFsp mount shape and lifecycle details
  Status: `Done`
  Needed by: `M2-T01`
  Done when: Mount architecture document is checked in and adopted.
  Evidence: The adopted shape is now reflected in the implemented `src/fs-winfsp` split (`mount`, `filesystem`, `path_bridge`, `file_info`) and the `M2-T01` tracker evidence; the runtime lifecycle is handled by `MountedVolume` plus RAII `MountSession` ownership, and the smoke tool exercises that lifecycle directly.

- [ ] `SPIKE-02` Snapshot policy for v1 write eligibility
  Status: `Planned`
  Needed by: `M4-T01`
  Done when: Decision document says whether snapshot-bearing volumes remain `RO` in GA.

- [ ] `SPIKE-03` ADS handling policy
  Status: `Planned`
  Needed by: `M4-T07`
  Done when: Decision document defines reject, ignore, or limited support behavior and tests.

- [ ] `SPIKE-04` Metadata mapping policy for Windows-created files
  Status: `Planned`
  Needed by: `M4-T03`
  Done when: Policy document defines uid, gid, mode bits, timestamps, and synthetic ACL behavior.

- [ ] `SPIKE-05` Compression algorithms required for GA corpus
  Status: `Planned`
  Needed by: `M1-T07`
  Done when: Release corpus and mandatory decompression list are frozen.

## Immediate Next Queue

Start here once implementation begins:

- [x] `NOW-01` Complete `M0-T01` repository scaffolding
- [x] `NOW-02` Complete `M0-T02` CMake and compiler baseline
- [x] `NOW-03` Complete `M0-T04` test harness baseline
- [x] `NOW-04` Complete `M0-T05` fixture manifest and corpus policy
- [x] `NOW-05` Complete `M0-T06` `orchard-inspect` stub
- [x] `NOW-06` Complete `M0-T03` formatting and linting
- [x] `NOW-07` Observe first remote `windows-lint` GitHub Actions run
- [x] `NOW-08` Start `M1-T01` block I/O abstraction
- [x] `NOW-09` Start `M1-T02` GPT and container discovery on top of `M1-T01`
- [x] `NOW-10` Start `M1-T04` object map and B-tree traversal
- [x] `NOW-11` Start `M1-T05` inode, dentry, and path lookup
- [x] `NOW-12` Start `M2-T01` WinFsp read-only adapter skeleton on top of the offline reader core
- [x] `NOW-13` Start `M2-T02` file and directory query mapping hardening on top of the mounted adapter
- [x] `NOW-14` Finish `M2-T03` by recording manual Explorer browse/open/copy-out validation on top of the new shell-stress coverage
- [x] `NOW-15` Start `M2-T04` symlink and hard-link read behavior on top of the hardened read-only mount path
- [x] `NOW-16` Start `M2-T05` read-only mount stress baseline on top of the completed link-aware read path
- [x] `NOW-17` Finish `M3-T01` by running the SCM-backed service smoke from an elevated shell and recording the result
- [x] `NOW-18` Start `M3-T02` device arrival and removal detection on top of the completed service host
- [x] `NOW-19` Finish `M3-T02` by running real hardware unplug/query-remove/remount smoke and recording the result
