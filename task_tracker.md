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
  Status: `Planned`
  Requirement: Orchard resolves paths, enumerates directories, and reads file contents on supported volumes.
  Priority: `P0`
  Target: `M1`
  Verification: Golden file-tree tests and read checks against fixture corpus.

- `REQ-READ-004`
  Status: `Planned`
  Requirement: Orchard supports the compression algorithms required by the release corpus.
  Priority: `P0`
  Target: `M1`
  Verification: Compression corpus tests and file hash comparisons.

- `REQ-READ-005`
  Status: `Planned`
  Requirement: Orchard reads existing symlinks and preserves hard-link identity where supported.
  Priority: `P1`
  Target: `M2`
  Verification: Fixture-based metadata tests and Explorer smoke tests.

### Policy and safety requirements

- `REQ-POL-001`
  Status: `Planned`
  Requirement: Every discovered volume is classified into `Hide`, `MountReadOnly`, `MountReadWrite`, or `Reject` with machine-readable reasons.
  Priority: `P0`
  Target: `M1`
  Verification: Policy matrix unit tests covering all supported and unsupported feature combinations.

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
- Locally completed: `M1-T01`, `M1-T02`, `M1-T03`, `M1-T04`
- Latest local verification:
  - `cmake --preset default`
  - `tests/corpus/generate-sample-fixtures.ps1`
  - `cmake --build --preset default --parallel`
  - `cmake --build --preset default --target orchard_format_check orchard_lint`
  - `ctest --preset default`
  - Result: `7/7 tests passed`, `clang-format check passed`, `clang-tidy passed`
- Key implementation note:
  - Volume superblocks are now resolved via the container object map; the bounded fallback block scan was removed from the primary discovery path.

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

- [ ] `M1-T05` Inode, dentry, and path lookup
  Status: `Planned`
  Depends on: `M1-T04`
  Done when: Orchard resolves absolute and relative paths and enumerates directory entries.
  Verification: File-tree goldens and path lookup tests.

- [ ] `M1-T06` File extent and data read path
  Status: `Planned`
  Depends on: `M1-T05`
  Done when: Orchard reads complete file contents correctly for the supported corpus.
  Verification: File hash comparisons.

- [ ] `M1-T07` Compression support baseline
  Status: `Planned`
  Depends on: `M1-T06`
  Done when: Required compression formats for the release corpus are readable.
  Verification: Compressed fixture test set.

- [ ] `M1-T08` Volume policy engine
  Status: `Planned`
  Depends on: `M1-T03`
  Done when: Core classifies each volume into `Hide`, `MountReadOnly`, `MountReadWrite`, or `Reject` with reasons.
  Verification: Policy matrix tests.

- [ ] `M1-T09` `orchard-inspect` real output
  Status: `Planned`
  Depends on: `M1-T08`
  Done when: Inspect tool prints structured volume metadata, roles, features, and policy outcomes.
  Verification: Snapshot-based CLI tests.

## M2 Read-Only Windows Mount

### Exit Gate

- Supported read-only volumes can be mounted through WinFsp.
- Explorer can browse and copy files out reliably.
- No shell crashes or major compatibility regressions on core workflows.

### Tasks

- [ ] `M2-T01` WinFsp adapter skeleton
  Status: `Planned`
  Depends on: `M1-T06`
  Done when: Orchard mounts a read-only filesystem stub through WinFsp.
  Verification: Manual mount smoke test.

- [ ] `M2-T02` File and directory query mapping
  Status: `Planned`
  Depends on: `M2-T01`
  Done when: Windows file info, directory enumeration, and path resolution map correctly to APFS data.
  Verification: Integration tests and Explorer browsing tests.

- [ ] `M2-T03` Read-path Explorer compatibility
  Status: `Planned`
  Depends on: `M2-T02`
  Done when: Open, preview, copy-out, and large-directory enumeration work in Explorer.
  Verification: Manual and automated shell smoke tests.

- [ ] `M2-T04` Symlink and hard-link read behavior
  Status: `Planned`
  Depends on: `M2-T02`
  Done when: Existing symlinks and hard links behave consistently with v1 support policy.
  Verification: Metadata integration tests.

- [ ] `M2-T05` Read-only mount stress baseline
  Status: `Planned`
  Depends on: `M2-T03`
  Done when: Repeated mount/unmount and copy-out scenarios run without leaks or hangs.
  Verification: Soak smoke test.

## M3 Mount Service, UI, and Product Control Plane

### Exit Gate

- Orchard installs as a product.
- Supported volumes auto-mount.
- Users can inspect status and unmount from UI or CLI.

### Tasks

- [ ] `M3-T01` Windows service host
  Status: `Planned`
  Depends on: `M2-T05`
  Done when: Background service starts on boot and manages mount lifecycle.
  Verification: Service install and startup tests.

- [ ] `M3-T02` Device arrival and removal detection
  Status: `Planned`
  Depends on: `M3-T01`
  Done when: Service detects supported disk attach/detach events and rescans appropriately.
  Verification: Plug/unplug scenario tests.

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

- [ ] `SPIKE-01` WinFsp mount shape and lifecycle details
  Status: `Planned`
  Needed by: `M2-T01`
  Done when: Mount architecture document is checked in and adopted.

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
- [ ] `NOW-11` Start `M1-T05` inode, dentry, and path lookup
