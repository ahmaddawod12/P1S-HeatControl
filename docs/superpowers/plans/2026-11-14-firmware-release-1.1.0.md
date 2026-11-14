# Firmware 1.1.0 Release Plan

> **For agentic workers:** Use superpowers:executing-plans for native execution and superpowers:requesting-code-review for the final release review. Track steps with checkboxes.

**Goal:** Publish the current firmware as stable GitHub release v1.1.0 without CI.

**Architecture:** Preserve the current firmware behavior, update its version and
release notes, and publish the exact committed source with the local publisher.
Use the existing local signing key and validate the public downloads afterward.

**Tech Stack:** ESP-IDF 6.1, ESP32, CMake/CTest, Node tests, Python, GitHub CLI.

**Spec:** Latest user request on 2026-11-14: publish the current firmware directly
as 1.1.0 without CI, superseding the earlier 1.0.1 release request.

## Global Constraints

- Preserve current firmware changes and saved device settings.
- Cancel the pending v1.0.1 workflow; do not publish a v1.0.1 release.
- Publish v1.1.0 directly without CI or replacement of existing releases/tags.
- Publish exactly the signed generic application and firmware.json.
- Keep credentials, signing keys, build outputs, and device backups private.
- Record hardware validation as pending; do not claim a successful USB upload.

## Review Focus

- Embedded version, signed identity, tag, and manifest all identify 1.1.0.
- The public package matches the exact tested source commit.
- Generic builds contain no personal credentials and retain the existing key.
- Release automation refuses an existing tag and publishes only the two assets.
- Software verification is not presented as physical commissioning.

### Task 1: Release metadata

- [x] Update version.txt to 1.1.0 and add docs/releases/v1.1.0.md.
- [x] Review the existing source changes and release workflow for release blockers.

### Task 2: Verification and source snapshot

- [x] Build and run the host tests, dashboard tests, Python tool tests, and publisher safeguard tests.
- [x] Commit the version metadata with [skip ci] and verify clean source state.
- [x] Rebuild v1.1.0, check stack use, sign with the existing key, and validate the generic package.

### Task 3: Publication

- [x] Push the reviewed source to main without CI and use tools/publish_firmware.ps1 to publish directly.
- [x] Confirm stable v1.1.0, the exact source tag, both public assets, and cancellation of the old workflow.
- [x] Download both assets anonymously and verify their signature, version, and manifest digest.
