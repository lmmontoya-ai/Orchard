# Orchard Research Brief

## Goal

Build open-source APFS support for Windows, eventually including read/write access.

## Bottom line

This is feasible, but "APFS read/write on Windows" is not a single problem. It is at least four problems:

1. Correctly parsing modern APFS containers and volumes.
2. Correctly projecting APFS semantics into the Windows file-system model.
3. Performing crash-safe APFS copy-on-write updates.
4. Deciding which APFS features are in scope for the first writable release.

The hard part is not basic file reads. The hard part is safe mutation of a modern APFS volume while preserving APFS transaction rules, checkpoints, object maps, snapshots, encryption state, and Windows compatibility expectations.

## What APFS requires

Apple's official on-disk reference is detailed enough to build against. The 2020 Apple File System Reference documents:

- Container superblocks, checkpoints, checkpoint maps, and object maps.
- Volume superblocks and volume roles.
- File-system B-tree objects, inodes, directory records, extents, extended attributes, snapshots, and data streams.
- Encryption metadata, keybag structures, and encryption state transitions.
- Sealed volume integrity metadata.

The practical implication is that Orchard needs a real APFS implementation, not a metadata shim.

### Core APFS concepts that matter for Orchard

- APFS is container-based. A single container can hold multiple volumes that share free space.
- APFS uses copy-on-write metadata. Writes create new metadata objects and then atomically advance the checkpointed state.
- APFS uses object identifiers plus object maps rather than simple fixed pointers everywhere.
- Volumes can be case-sensitive or case-insensitive.
- Volumes can have roles such as `System`, `Data`, `Preboot`, `Recovery`, and `VM`.
- Snapshots are first-class and participate in retention of shared blocks.
- Clones, compression, xattrs, symlinks, and hard links are normal features, not edge cases.
- Encryption is built into the format. The volume superblock stores metadata-crypto information and a live encryption/decryption state object when a transition is in progress.
- Modern macOS also uses sealed volumes, where integrity metadata stores a root hash for a hashed tree over the file system.

### APFS mount rules are strict

Apple's reference explicitly says:

- If a read-only compatible feature is present but unsupported, the implementation should mount read-only.
- If a backward-incompatible feature is present but unsupported, the implementation must not mount the volume.

That rule should drive Orchard's architecture from day one. Feature gating is not optional.

## What current software and projects actually achieve

### Official Apple material

- Apple File System Reference: the primary on-disk format source. This is the single most important document for the project.
- Apple Platform Security: useful for understanding APFS encryption and FileVault context.

### Open and commercial implementations

#### `apfs-fuse`

Official repo: <https://github.com/sgan81/apfs-fuse>

What it demonstrates:

- Read-only mounting.
- Support for software-encrypted volumes and Fusion drives.
- Support for snapshots and sealed volumes.
- Support for case-sensitive and case-insensitive volumes, symlinks, hard links, xattrs, and some compression modes.

What it does not solve:

- Writing.
- Full compression coverage.
- Hardware-encrypted/T2-style volumes.

This is strong evidence that modern APFS read support is achievable in an open project, but it also shows how long the tail is even for read-only compatibility.

#### `libfsapfs`

Official repo: <https://github.com/libyal/libfsapfs>

What it demonstrates:

- Experimental read-only library approach.
- Support for APFS version 2 with ZLIB, LZVN, encryption, and xattrs.

Explicit limitations in its README:

- No Fusion drive support.
- No snapshot support.
- No LZFSE support.
- No T2 encryption support.

This is valuable as evidence that a reusable parsing/inspection core is a good architecture, but also that many real-world APFS features stay unsupported for a long time in read-only tooling.

#### `linux-apfs-rw` and `apfsprogs`

Official repos:

- <https://github.com/linux-apfs/linux-apfs-rw>
- <https://github.com/linux-apfs/apfsprogs>

What they demonstrate:

- There is an active attempt at writable APFS on Linux.
- `apfsprogs` exists mainly to help test the Linux kernel module and includes `mkapfs`, `apfs-snap`, `apfs-label`, and `apfsck`.

What matters for Orchard:

- Writable APFS requires dedicated tooling, consistency checking, and test fixtures alongside the driver.
- A writable implementation is a multi-repo, multi-tool effort, not just a mount layer.

#### Paragon APFS SDK Community Edition

Official repo: <https://github.com/Paragon-Software-Group/paragon_apfs_sdk_ce>

What it demonstrates:

- Cross-platform APFS access is commercially viable.
- Even Paragon's community edition is read-only.
- The README lists support for cloned files, compressed files, xattrs, sub-volumes, and encrypted volumes.
- The same README states that write operations require a separate full read-write version not included in the community edition.

This is a strong industry signal: even a mature storage vendor publicly ships the open/community SDK as read-only.

#### ApfsAware

Official site: <https://www.apfsaware.com/>

What it demonstrates:

- There is at least one currently marketed Windows-native APFS driver focused on read-only integration.

This reinforces the pattern: Windows APFS access exists in production, but public claims are conservative and centered on safe read access.

### Relevant Windows precedent: `WinBtrfs`

Official repo: <https://github.com/maharmstone/btrfs>

This is not an APFS implementation, but it is an important precedent:

- It is a from-scratch Windows filesystem driver for a complex copy-on-write filesystem.
- Its README shows years of bug fixes, testing work, Windows integration issues, signing concerns, oplock/caching issues, and behavior fixes under real workloads.

Inference: the best open-source comparison for Orchard's engineering effort is not a parser library. It is a project like WinBtrfs, because it shows the size of the Windows integration surface.

## Why Windows support is harder than "port the APFS code"

### A filter driver is not enough

To mount a new on-disk filesystem format on Windows, Orchard needs an installable filesystem implementation, not a minifilter. Minifilters sit above existing filesystems; they do not define a new disk format.

### Windows imposes its own semantics

A usable Windows filesystem implementation needs correct behavior for at least:

- File and directory enumeration.
- Share modes and byte-range locking.
- Oplocks.
- Cached I/O and memory-mapped I/O.
- Reparse points and symlink projection.
- Change notifications.
- Security descriptors and access checks.
- Volume information queries and mount behavior.
- Case-sensitivity behavior that is consistent enough for Windows applications.

The APFS side and the Windows side both have strong semantics. Orchard has to preserve both.

### Write support is the real cliff

Read-only support mainly needs:

- Correct object parsing.
- Checksum validation.
- Snapshot-aware tree walking.
- Decompression.
- Decryption for supported encrypted volumes.

Write support additionally needs:

- Block allocator and free-space manager integration.
- Object-map updates.
- B-tree mutation logic for inodes, dentries, extents, xattrs, and snapshots.
- Checkpoint creation and crash-safe commit ordering.
- Snapshot/clone reference safety.
- Rollback behavior on partial failure.
- Strict feature gating for volumes the implementation must not modify.

Inference: Orchard should treat the writable path as a transaction engine project, not as "adding create/write callbacks" to a reader.

### Low-level write obligations from Apple's format reference

Even before higher-level Windows behavior, a writable implementation needs to respect specific APFS bookkeeping rules documented by Apple. Examples:

- The volume's modification history fields must be updated when the volume is changed.
- Some fields, such as the container's recorded "newest mounted version," are explicitly documented as fields that non-Apple implementations must preserve rather than rewrite.
- Volume and container counters, object identifiers, and checkpoint-related structures need deterministic updates on every committed mutation.

This matters because silent "mostly works" writes are not acceptable on a copy-on-write filesystem. Orchard's writer needs a precise, testable mutation contract.

## Recommended architecture choices

### 1. Shared core plus adapters

Use a shared APFS core library that is independent from the Windows mount layer.

Responsibilities of the core:

- Parse and validate on-disk APFS structures.
- Expose read-only object models for containers, volumes, B-trees, extents, and snapshots.
- Perform feature detection and mount gating.
- Later, plan and apply APFS transactions in a deterministic way.

Responsibilities of the Windows adapter:

- Translate Windows file-system operations into core library operations.
- Handle Windows caching, locking, notifications, and security integration.
- Keep Windows-specific concerns out of on-disk APFS logic.

This split is the cleanest way to support:

- Offline tooling.
- Fuzzing.
- Image-based tests.
- A future native driver and a user-mode harness sharing the same filesystem logic.

### 2. Start with a user-mode mount target

WinFsp's documentation makes two things clear:

- Developing filesystems for Windows is unusually difficult.
- WinFsp reduces that complexity and supports user-mode filesystems, with kernel-mode options available as well.

Inference: Orchard should start with a user-mode mount implementation over a shared core, using it as the semantic harness for:

- Path lookup.
- Directory enumeration.
- Read path correctness.
- Basic Windows behavior validation.

Then move to a native production-quality driver only after the core reader and test corpus are strong.

### 3. Treat unsupported APFS features as hard gates

The first writable release should reject or force read-only mode for:

- Sealed system volumes.
- Fusion drives.
- Volumes with unsupported incompatible feature flags.
- Hardware-encrypted/T2-style configurations.
- Volumes in the middle of encryption/decryption transitions.

That is an inference, but it follows directly from Apple's mount rules and from the limits of current open implementations.

## Recommended phased roadmap

### Phase 0: Research and corpus

- Collect APFS images from multiple macOS generations and volume types.
- Build image metadata inventory: encrypted vs plain, case-sensitive vs insensitive, snapshot-heavy, cloned-data, compressed, sealed system/data pairs.
- Add a parser-only test harness that never mounts anything.

Exit criteria:

- Can identify container and volume feature flags, roles, snapshots, encryption state, and unsupported features.

### Phase 1: Read-only APFS core

- Implement container parsing, checkpoints, object maps, volume superblocks, filesystem B-tree walking, inodes, dentries, extents, xattrs, symlinks, hard links.
- Add decompression support for the compression methods needed by the test corpus.
- Add read-only mount gating exactly per feature flags.

Exit criteria:

- Can enumerate and read ordinary user-data volumes from images with deterministic output.

### Phase 2: Windows read-only mount

- Put the core behind a Windows mount layer, ideally user-mode first.
- Validate Explorer behavior, path normalization, case behavior, notifications, file attributes, and symlink representation.
- Measure performance and cache strategy.

Exit criteria:

- Stable read-only mount for non-encrypted, non-sealed user volumes.

### Phase 3: Safer real-world read support

- Add encrypted-volume support for the subset that can be implemented safely.
- Add snapshot browsing.
- Add broader compression coverage.
- Add more compatibility with macOS-created edge cases.

Exit criteria:

- Competitive read-only support against existing open tools for common APFS volumes.

### Phase 4: Writable non-system volumes only

- Implement a restricted write path for plain, non-sealed, non-Fusion, non-transitioning user-data volumes.
- Support create, delete, rename, truncate, write, mkdir, rmdir, xattr update, and metadata updates.
- Add crash-consistency tests with power-fail simulation against image copies.
- Add strict automatic rollback to read-only mode on internal consistency errors.

Exit criteria:

- Orchard can survive aggressive mutation test suites without silent corruption.

### Phase 5: Production hardening

- Extensive fuzzing.
- Differential testing against macOS behavior.
- Repair/inspection tools.
- Driver packaging, signing, install/uninstall flow, and update safety.

## What should be explicitly out of scope at first

These are the right non-goals for an initial public milestone:

- Boot support.
- Writing to sealed macOS system volumes.
- Fusion-drive support.
- Hardware-encrypted/T2-style internal Mac storage.
- Full Finder metadata parity from day one.
- A "supports every APFS volume ever created" promise.

If Orchard tries to solve those on day one, the project will slow to a crawl.

## Engineering standards Orchard should adopt immediately

- Parser-first development, with no mount layer until the parser is heavily tested.
- Image-based fixtures committed or reproducibly generated.
- Differential tests against known-good outputs.
- Fuzzing of every on-disk structure parser.
- Read-only-by-default behavior whenever integrity or feature support is uncertain.
- Strict separation between format logic and Windows integration logic.
- Internal corruption assertions that flip mounts to read-only instead of risking bad writes.
- Explicit support matrix by APFS feature, not marketing claims like "APFS supported."

## Key technical risks

- Modern APFS features are underdocumented relative to how much production macOS uses them.
- Encryption and sealed-volume behavior make "full APFS" much larger than "regular file data on a user volume."
- Windows filesystem behavior bugs can cause hangs, cache incoherency, or corruption even if the APFS logic is correct.
- Open-source driver distribution on Windows has operational friction around packaging and signing.
- There is likely a long interoperability tail around Unicode, normalization, case-folding, xattrs, and special metadata.

## Strategic conclusion

Orchard should not begin as "full APFS read/write for Windows."

It should begin as:

1. A rigorously tested APFS core.
2. A conservative read-only Windows mount.
3. A feature-gated writable implementation for plain user volumes only.

That is the most credible path to eventually reaching broad APFS read/write support without corrupting disks.

## Source links

- Apple File System Reference (Apple): <https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf>
- Apple Platform Security (Apple): <https://support.apple.com/guide/security/welcome/web>
- WinFsp Documentation: <https://winfsp.dev/doc/>
- `apfs-fuse`: <https://github.com/sgan81/apfs-fuse>
- `libfsapfs`: <https://github.com/libyal/libfsapfs>
- `linux-apfs-rw`: <https://github.com/linux-apfs/linux-apfs-rw>
- `apfsprogs`: <https://github.com/linux-apfs/apfsprogs>
- Paragon APFS SDK Community Edition: <https://github.com/Paragon-Software-Group/paragon_apfs_sdk_ce>
- ApfsAware: <https://www.apfsaware.com/>
- WinBtrfs: <https://github.com/maharmstone/btrfs>
