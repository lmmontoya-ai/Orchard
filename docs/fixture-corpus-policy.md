# Fixture Corpus Policy

## Purpose

Define how Orchard tracks test fixtures before large APFS images are added to the repository.

## Goals

- Keep fixture metadata explicit and machine-readable.
- Make mount-policy expectations testable before the filesystem implementation exists.
- Avoid introducing large binary images without a documented reason.

## Manifest format

Fixture manifests are JSON documents with:

- `schema_version`
- `fixtures`

Each fixture entry must include:

- `id`
- `label`
- `source_type`
- `relative_path`
- `volume_role`
- `case_mode`
- `encryption`
- `snapshots`
- `system_volume_group`
- `compression_algorithms`
- `feature_flags`
- `expected_policy`
- `notes`

## Field conventions

- `source_type`
  - `image_file`
  - `raw_disk_capture`
  - `synthetic_sample`
- `volume_role`
  - `user_data`
  - `system`
  - `preboot`
  - `recovery`
  - `vm`
  - `unknown`
- `case_mode`
  - `case_insensitive`
  - `case_sensitive`
- `encryption`
  - `none`
  - `filevault`
  - `hardware`
  - `unknown`
- `snapshots`
  - `absent`
  - `present`
  - `unknown`
- `system_volume_group`
  - `none`
  - `paired`
  - `unknown`
- `expected_policy`
  - `MountReadWrite`
  - `MountReadOnly`
  - `Hide`
  - `Reject`

## Corpus rules

- M0 placeholder files may be tiny text fixtures when the test only validates plumbing.
- Real APFS images should be stored only after their provenance, features, and expected policy are captured in a manifest entry.
- Any new APFS image added for v1 must record the smallest set of features needed to justify its inclusion.
- Images that contain unsupported or risky features are still useful fixtures as long as their expected policy is explicit.

## Validation

- The sample manifest under `tests/corpus/manifests/sample-fixtures.json` is validated in CTest.
- Later milestones should extend validation to check duplicate IDs, path existence, and required corpus coverage by category.

