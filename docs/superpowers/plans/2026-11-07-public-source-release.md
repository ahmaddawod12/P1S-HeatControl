# Public Source Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish signed v1.0.0 active-high firmware from the public source repository.

**Architecture:** Keep existing control and OTA validation; change the selected
hardware profile and fixed release source. Publish only a validated generic
package, tagged to its matching source commit.

**Tech Stack:** ESP-IDF 6.1, C, PowerShell, Python, GitHub CLI.

**Spec:** `docs/superpowers/specs/2026-11-07-public-source-release-design.md`

## Global Constraints

- Version 1.0.0; repository `ahmaddawod12/P1S-HeatControl`.
- Active-high relay inputs; both relays and heating start disabled.
- Preserve signing key, credential isolation, partitions and downgrade protection.
- No hardware flashing; no repository visibility changes or tag overwrites.

## Review Focus

- A cached sdkconfig must not silently retain the previous repo or polarity.
- Generic and personal packaging must validate the active-high profile.
- Published tag and firmware must correspond to committed source.
- Missing/private/empty repositories must not cause automatic repository creation.
- Old 1.0.5 devices need a documented USB baseline instead of bypassed OTA checks.

### Task 1: Configure active-high v1.0.0 and release tooling

**Files:** Board/firmware Kconfig, sdkconfig.defaults, version.txt,
tools/package_firmware.py, tools/publish_firmware.ps1, tool tests and current docs.

**Interfaces:** Preserve package and manifest schemas. Publisher defaults to the
source repository and derives its target commit from a clean local HEAD.

- [x] Run the production GPIO probe against the old generated config and observe failure.
- [x] Add failing package/publisher regressions for the new profile and destination.
- [x] Update configuration, package profile and publisher; run those tests green.
- [x] Update release notes and current guidance; retain historical test records.

### Task 2: Verify and prepare signed artifacts

**Files:** Existing host/web/tool tests; generated release/bootstrap/unit builds;
`dist/firmware-1.0.0` (ignored).

- [x] Run all suites and build release, bootstrap and unit images.
- [x] Verify production GPIO mapping and HTTP stack budget.
- [x] Obtain independent review and address substantive findings with regressions.
- [x] Sign and validate generic and personal images; review the two-file public package.

### Task 3: Publish matching source and release

**Files:** Approved working-tree firmware changes and release documentation.

- [x] Check the full source change and verify private artifacts stay excluded.
- [x] Rebuild the clean committed source and require its payload to match the public package; reject existing remote version tags.
- [x] Commit and push the matching source revision on the existing branch.
- [x] Publish v1.0.0 to the public source repo with that exact target commit.
- [x] Verify anonymous latest-release metadata and both downloadable assets.
- [x] Summarize the release URL, relay mapping and one-time USB transition.

**Result:** [Published v1.0.0](https://github.com/ahmaddawod12/P1S-HeatControl/releases/tag/v1.0.0),
source `6a3a3bc6ecb675a5d379eee696cbf7eb763bebd5`.
See `docs/releases/v1.0.0-verification.md` for the measured software checks.
