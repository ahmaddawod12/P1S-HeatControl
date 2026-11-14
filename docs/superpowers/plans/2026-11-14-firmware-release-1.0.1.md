# Firmware 1.0.1 Release Plan

> **For agentic workers:** Use superpowers:executing-plans for native execution and superpowers:requesting-code-review for the final release review. Track steps with checkboxes.

**Goal:** Publish the current firmware as the stable GitHub release v1.0.1.

**Architecture:** Preserve the current firmware behavior, update its version and
release notes, and publish the exact committed source through the existing
GitHub Actions release workflow. Validate the signed public downloads afterward.

**Tech Stack:** ESP-IDF 6.1, ESP32, CMake/CTest, Node tests, Python, GitHub Actions.

**Spec:** User request on 2026-11-14: upload the current firmware as release 1.0.1.

## Global Constraints

- Preserve current firmware changes and saved device settings.
- Publish v1.0.1 without replacing any existing release or tag.
- Publish exactly the signed generic application and firmware.json.
- Keep credentials, signing keys, build outputs, and device backups private.
- Record hardware validation as pending; do not claim a successful USB upload.

## Review Focus

- Embedded version, signed identity, tag, and manifest all identify 1.0.1.
- The public package matches the exact tested source commit.
- Generic builds contain no personal credentials and retain the existing key.
- Release automation refuses an existing tag and publishes only the two assets.
- Software verification is not presented as physical commissioning.

### Task 1: Release metadata

- [x] Update version.txt to 1.0.1 and add docs/releases/v1.0.1.md.
- [x] Review the existing source changes and release workflow for release blockers.

### Task 2: Verification and source snapshot

- [x] Build and run the host tests, dashboard tests, Python tool tests, and publisher safeguard tests.
- [ ] Commit the current firmware, workflow, tests, documentation, and release metadata without ignored private files.
- [ ] Verify tools/release_workflow.py accepts the clean source snapshot for main.

### Task 3: Publication

- [ ] Push the reviewed commit to main as a fast-forward and follow the release workflow to completion.
- [ ] Confirm stable v1.0.1, the exact source tag, and both public assets.
- [ ] Download both assets anonymously and verify their signature, version, and manifest digest.
