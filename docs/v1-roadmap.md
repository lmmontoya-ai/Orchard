# Orchard v1 Roadmap

## Purpose

Define a realistic v1 plan for Orchard:

- Transparent APFS access on Windows after installation.
- Volumes appear in File Explorer automatically when supported.
- Easy unmount through a small UI and CLI.
- Safe read support plus restricted user-data volume write support.

This roadmap assumes the findings in [apfs-windows-research.md](./apfs-windows-research.md): APFS is feasible, but only with strict feature gating, a parser-first architecture, and a conservative writable scope.

Execution of this roadmap is tracked in [task_tracker.md](../task_tracker.md).

## v1 product definition

### What a user should experience

1. The user runs the Orchard installer.
2. Orchard installs its service, filesystem runtime dependency, tray app, and command-line tool.
3. The user plugs in or boots with a supported APFS disk attached.
4. Orchard detects the APFS container, evaluates each volume, and mounts eligible ones.
5. The mounted volume appears in File Explorer with a stable drive letter and clear label.
6. The user can open, browse, copy, edit, create, rename, and delete files on supported writable user-data volumes.
7. The user can click the Orchard tray icon and unmount a volume cleanly.
8. If a volume is unsupported or only safe to mount read-only, Orchard says exactly why.

### v1 promise

Orchard v1 is not "all APFS on Windows."

Orchard v1 is:

- Automatic Explorer-visible mounting of supported APFS user-data volumes.
- Read support for a broader set of volumes than write support.
- Write support only for a tightly defined subset where the transaction engine is proven safe.
- A fail-closed product: uncertain volumes mount read-only or do not mount.

## v1 scope matrix

### Supported in v1 GA

- GPT-partitioned APFS containers on local disks and common removable storage.
- Non-encrypted, non-Fusion APFS user-data volumes.
- Case-insensitive user-data volumes with no unsupported incompatible features.
- Standard file and directory operations:
  - Read
  - Create
  - Overwrite
  - Append
  - Rename
  - Delete
  - Mkdir and rmdir
  - Timestamps
  - Basic attributes
  - Symlink read behavior
  - Hard-link preservation where already present
- Automatic mount to drive letter in Explorer.
- User-initiated clean unmount from tray UI and `orchardctl`.

### Read-only in v1 GA

- User-data volumes that Orchard can parse safely but not modify safely.
- Case-sensitive volumes.
- Volumes with snapshots, unless snapshot-aware write support reaches release quality during development.
- Volumes in macOS system/data volume groups, mounted as raw APFS volumes rather than a reconstructed macOS merged namespace.
- Helper volumes shown in advanced view only:
  - `System`
  - `Preboot`
  - `Recovery`
  - `VM`

### Explicitly out of scope for v1 GA

- FileVault-encrypted volumes.
- Hardware-encrypted or T2-style storage.
- Fusion drives.
- Writing to sealed volumes.
- Reconstructing macOS firmlink semantics into a merged root view.
- Boot support.
- Time Machine compatibility promises.
- Full Windows ACL round-tripping.
- Force-unmount as a default user path.

### Scope rule

If a volume's feature set is not fully understood and tested, Orchard must classify it as:

- `ReadOnly`, or
- `Unsupported`

Never `ReadWrite`.

## Product architecture for v1

### High-level design

Orchard v1 should ship as a user-mode filesystem product built around a shared APFS core.

Recommended v1 stack:

- APFS core and mount service: `C++20`
- Windows user-mode filesystem layer: `WinFsp`
- UI and control panel: `.NET 8` desktop app
- Build system: `CMake`
- Installer: `WiX`

This is the pragmatic route because it avoids a kernel-driver-first schedule and reduces signing and deployment risk.

### Major components

#### `orchard-apfs-core`

Responsibilities:

- Block-device and image abstraction.
- APFS container discovery.
- Checkpoint selection and validation.
- Object map parsing.
- B-tree walking.
- Volume feature detection and mount gating.
- Inode, dentry, extent, xattr, compression, and snapshot read logic.
- Write transaction planning and commit engine for supported writable volumes.

This library must be usable without Windows Explorer or WinFsp.

#### `orchard-mount-service`

Responsibilities:

- Windows service running at startup.
- Device arrival and removal detection.
- APFS scan and volume classification.
- Drive-letter assignment.
- Mount lifecycle management.
- Named-pipe control API for the UI and CLI.
- Logging and crash/report collection.

This is the product orchestrator.

#### `orchard-fs-winfsp`

Responsibilities:

- Translate Windows file operations into APFS core operations.
- Serve mounted volumes to Explorer and applications.
- Handle open/create/read/write/flush/query/set info/directory enumeration.
- Integrate with WinFsp notifications, caching, oplocks, and file identity behavior.

This is where Windows semantics meet Orchard's APFS core.

#### `orchard-ui`

Responsibilities:

- Tray icon with current mounted volumes.
- Mount status display:
  - `Mounted RW`
  - `Mounted RO`
  - `Busy`
  - `Unsupported`
  - `Error`
- Actions:
  - Open in Explorer
  - Unmount
  - Remount read-only
  - View volume details
  - Copy logs
- Settings:
  - Auto-mount on attach
  - Default drive-letter policy
  - Advanced volume visibility

This should be intentionally small. v1 needs a tray app, not a heavy management suite.

#### `orchardctl`

Responsibilities:

- Scriptable CLI for mount, unmount, status, and diagnostics.
- Essential for testing, CI automation, and power users.

Example commands:

- `orchardctl status`
- `orchardctl mount --volume <uuid>`
- `orchardctl unmount X:`
- `orchardctl inspect --disk \\.\PhysicalDrive3`

#### `orchard-testkit`

Responsibilities:

- Image-based test fixtures.
- Mutation tests on cloned disk images.
- Crash-simulation harness.
- Differential tests against macOS and Linux/APFS tools where appropriate.
- Fuzzers for structure parsers.

This is mandatory for write support.

## Volume classification and mount policy

### Orchard must classify every discovered volume

Each APFS volume gets a policy result:

- `Hide`
- `MountReadOnly`
- `MountReadWrite`
- `Reject`

Each result must carry a machine-readable reason, for example:

- `RoleSystem`
- `Encrypted`
- `Fusion`
- `Sealed`
- `SnapshotsPresent`
- `UnsupportedIncompatFeature`
- `CaseSensitive`
- `TransactionStateActive`
- `DirtyUnknown`

This policy engine is a first-class subsystem, not just a few `if` statements.

### Default mount behavior

- Auto-mount user-data volumes that are eligible.
- Hide helper volumes by default.
- Preserve prior drive-letter assignments by `container UUID + volume UUID`.
- Prefer the last assigned drive letter if still free.
- Otherwise choose from a configurable range, defaulting away from `A`, `B`, `C`.

### Default write eligibility rules for v1

A volume is writable only if all of the following are true:

- Volume role is user-data or no special role.
- Volume is not encrypted.
- Volume is not sealed.
- Container is not Fusion.
- Volume has no unsupported incompatible feature flags.
- Volume is not mid-encryption or decryption transition.
- Volume is not case-sensitive.
- Volume is not in a policy bucket Orchard has declared read-only, such as `SnapshotsPresent`, unless that capability is fully implemented and tested before release.

If any rule fails, the volume must fall back to read-only or unsupported.

## Filesystem semantics plan

### Path and case behavior

- Internal representation stays APFS-native.
- Windows-facing layer converts UTF-8 to UTF-16 carefully and deterministically.
- Case-insensitive APFS volumes map to Windows default behavior.
- Case-sensitive volumes remain read-only in v1 unless the full test matrix proves safe behavior.

### Metadata mapping

APFS does not map directly to Windows security semantics.

v1 approach:

- Preserve APFS mode bits, inode metadata, timestamps, xattrs, symlinks, and hard-link relationships where supported.
- Synthesize Windows security descriptors from a simple mount policy.
- Default to a single-owner mapping model for created files:
  - Configured APFS uid
  - Configured APFS gid
  - Default file mode
  - Default directory mode

This mirrors the practical strategy used by other cross-platform filesystem projects: stable synthetic ACLs for Windows usability, not a false promise of perfect ACL parity.

### Alternative data streams

v1 should not promise native APFS-backed ADS support.

Default behavior:

- Ignore Windows ADS writes or fail them with a documented unsupported status, depending on application compatibility testing.
- Do not silently invent cross-platform metadata formats on disk.

This needs an explicit spike early because Windows shell and security tools touch ADS frequently.

### Reparse points and symlinks

v1 should support reading APFS symlinks.

Writable symlink creation can be deferred until:

- Explorer behavior is understood.
- Windows permission requirements are clear.
- Target serialization is round-tripped correctly.

If this slips, keep symlink creation out of v1 GA while still reading existing symlinks.

## Write-engine plan

### Principle

Do not expose Windows write operations until Orchard has a real APFS transaction engine.

### Write-engine responsibilities

- Allocate new blocks safely.
- Update file extents.
- Mutate inode and dentry B-trees.
- Update object maps and volume counters.
- Create a crash-safe commit sequence.
- Preserve fields Apple says third-party implementations must preserve.
- Roll back or fail closed on partial errors.
- Downgrade mount to read-only on internal invariants failing.

### v1 write feature set

Mandatory:

- Create file
- Open existing file for write
- Truncate and extend
- Sequential and random writes
- Flush
- Rename within volume
- Delete file
- Create and remove directory
- Update basic times and size metadata

Defer unless proven safe:

- Clone-aware write optimization
- Snapshot creation or deletion
- Compression-on-write
- Symlink creation
- Extended-attribute mutation beyond the minimal set needed by Orchard itself

### Crash-safety bar

Before a writable mount is user-visible in Explorer, Orchard must pass:

- Forced process-kill during write tests
- Power-loss simulation on cloned images
- Repeated mount/replay/remount cycles
- Large-file overwrite tests
- Directory-heavy rename/delete storms

If recovery produces ambiguity, Orchard must remount read-only and ask for inspection.

## UX and control plane plan

### Tray UI

Required for v1:

- Show mounted volumes and mode (`RW` or `RO`).
- Show mount reason for `RO`.
- `Open`
- `Unmount`
- `View details`
- `Copy diagnostics`

Nice-to-have if schedule allows:

- Toast on successful auto-mount.
- One-click "mount advanced volumes" toggle.

### Unmount behavior

Unmount flow:

1. UI or CLI requests unmount.
2. Mount service blocks new opens.
3. Filesystem flushes pending state.
4. Service asks WinFsp to unmount.
5. If busy, return a clear error.

v1 should prefer safety over aggression:

- Do not default to force-unmount.
- If handles are still open, show `Volume busy; close applications and retry`.
- Force-unmount can be kept as an admin-only debug feature, not a normal UI button.

### Explorer integration

v1 should aim for:

- Drive label like `MacData (APFS)`
- Standard drive icon or a lightly branded APFS icon
- Context-menu entry at drive root:
  - `Unmount with Orchard`

If a shell extension risks schedule or stability, ship tray-only unmount for v1. The tray app is the minimum acceptable UX.

## Packaging and installation

### Installer behavior

The installer should:

- Install or upgrade WinFsp if missing.
- Install Orchard service.
- Install Orchard tray UI.
- Install `orchardctl`.
- Register startup behavior for the tray UI.
- Add uninstall support through Windows Apps and Features.

### Public release packaging

- Signed installer if possible.
- Signed Orchard binaries if possible.
- If custom kernel code is avoided in v1, Orchard inherits much less signing friction.

This is one of the strongest arguments for a user-mode-first release.

## Workstreams

### Workstream A: Core APFS read path

Deliverables:

- Container scan
- Checkpoint chooser
- Omap and B-tree parser
- Inode/path lookup
- File extent reading
- Compression support for the required corpus
- Read-only feature gating
- `orchard-inspect`

### Workstream B: Windows mount adapter

Deliverables:

- WinFsp-backed filesystem layer
- Path lookup
- Directory enumeration
- Open/read/query info
- File identity and caching model
- Initial Explorer compatibility

### Workstream C: Mount service and device discovery

Deliverables:

- Disk arrival notifications
- Raw-disk probing
- Volume policy engine
- Drive-letter persistence
- Mount/unmount lifecycle
- Control API

### Workstream D: Write engine

Deliverables:

- Writable eligibility classifier
- Allocation and extent mutation
- Inode/dentry mutation
- Commit path
- Flush semantics
- Read-only fallback on invariant failure

### Workstream E: UI and CLI

Deliverables:

- Tray UI
- Status view
- Unmount action
- Diagnostics export
- `orchardctl`

### Workstream F: Verification and release engineering

Deliverables:

- Corpus inventory
- Fuzzing
- Differential tests
- Crash simulation
- Soak tests
- Installer automation
- Release checklist

## Milestone plan

### M0: Foundations

Goals:

- Establish repo layout, build, coding standards, CI, test harness, and image corpus policy.

Deliverables:

- Workspace structure
- CMake presets
- Formatting and lint rules
- Unit-test framework
- Fixture manifest format
- `orchard-inspect` stub

Exit criteria:

- Fresh clone builds on supported developer machines.
- CI runs unit tests and parser smoke tests.

### M1: APFS reader core

Goals:

- Correctly discover and read supported APFS containers and volumes offline.

Deliverables:

- Block I/O abstraction
- Checkpoint and superblock parsing
- Omap and filesystem tree traversal
- Inode and directory enumeration
- File read path
- Feature detection and mount gating

Exit criteria:

- Orchard can list files from a representative corpus without mounting through Explorer.

### M2: Read-only Windows mount

Goals:

- Expose supported APFS volumes in Explorer through WinFsp.

Deliverables:

- Read-only WinFsp filesystem
- File and directory enumeration in Explorer
- Open/read/copy-out workflows
- Stable drive-letter mounts via service

Exit criteria:

- Supported user-data volumes appear in Explorer and can be browsed without shell crashes or metadata corruption.

### M3: Product control plane

Goals:

- Make Orchard feel like a product instead of a lab tool.

Deliverables:

- Windows service
- Device monitoring
- Auto-mount logic
- Tray UI
- `orchardctl`
- Unmount flow
- Diagnostic bundle generation

Exit criteria:

- Install, plug disk, open Explorer, unmount from tray works end to end.

### M4: Restricted write engine

Goals:

- Enable safe writes for the narrow v1 writable tier.

Deliverables:

- APFS transaction engine
- Create/write/truncate/rename/delete/mkdir/rmdir
- Flush and close semantics
- Single-owner metadata policy for created files
- Read-only downgrade on internal failure

Exit criteria:

- Mutation tests pass on cloned images for all supported write operations.

### M5: Hardening and beta

Goals:

- Eliminate silent corruption risk and stabilize Windows behavior.

Deliverables:

- Fuzzers and corpus expansion
- Crash-recovery tests
- Soak runs
- Performance profiling
- Beta installer and upgrade path
- Support matrix and release notes draft

Exit criteria:

- No known silent-corruption issues in supported configurations.
- Volumes flip to read-only instead of failing dangerously under injected faults.

### M6: v1 release

Goals:

- Ship a conservative, supportable public release.

Deliverables:

- Signed binaries where available
- Final installer
- User docs
- Troubleshooting guide
- Support matrix
- Backup warning and recovery guidance

Exit criteria:

- End-to-end install/mount/read/write/unmount workflow is stable on the release matrix.

## Test strategy

### Corpus categories

Minimum fixture categories:

- Plain case-insensitive external APFS data volume
- Multi-volume container
- Case-sensitive user volume
- Snapshot-bearing user volume
- macOS system/data volume group
- Compressed-file-heavy volume
- Large-directory volume
- Large-file volume
- Corrupt checkpoint samples

### Test layers

- Unit tests for parsers and data structures
- Property tests for invariants
- Fuzzing for all on-disk structure parsers
- Golden tests for path lookup and metadata output
- WinFsp integration tests
- UI smoke tests
- Image-clone mutation tests
- Long-running soak tests on Windows

### Differential testing

For supported read features, compare Orchard output against:

- macOS
- `apfs-fuse`
- `libfsapfs`
- `apfsprogs` where relevant

For write features, compare resulting disk images against:

- macOS visibility and mountability
- Internal Orchard invariants
- Offline inspection output

## Release gates for writable support

Writable support must not ship until all are true:

- Every write operation in the supported matrix has deterministic tests.
- Crash injection does not produce silent damage.
- Unsupported feature flags force `RO` or `Reject`.
- Busy unmount does not lose data.
- Repeated mount/unmount cycles do not leak resources or change on-disk state unexpectedly.
- Large copies from Explorer and common Windows applications complete reliably.

## Open design spikes to run early

These should be answered before the project is deep in implementation:

1. WinFsp mount model details:
   Do we mount each APFS volume as a drive letter directly, or through a service-managed namespace first?
2. Snapshot policy:
   Can v1 safely write to volumes with existing snapshots, or do they stay `RO`?
3. Firmlink handling:
   How should system/data volume-group `Data` volumes be presented in Explorer?
4. Metadata mapping:
   What is the exact uid/gid and mode-bit policy for files created from Windows?
5. ADS handling:
   Which Windows ADS operations can be rejected without breaking common applications?
6. Compression coverage:
   Which algorithms are mandatory for the release corpus?

## Recommended repo layout

```text
Orchard/
  docs/
  src/
    apfs-core/
    blockio/
    mount-service/
    fs-winfsp/
    orchardctl/
    ui/
  tests/
    unit/
    integration/
    corpus/
    mutation/
    fuzz/
  tools/
    inspect/
    image-tools/
  packaging/
    wix/
```

## Strategic note

The success condition for v1 is not "supports the most APFS features."

The success condition is:

- A user installs Orchard.
- A supported APFS user-data volume appears in Explorer.
- Reads are reliable.
- Writes are safe for the explicitly supported subset.
- Unmount is simple.
- Unsupported cases fail clearly and conservatively.

That is a credible v1. Anything broader should wait for v2.
