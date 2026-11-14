# Release Workflow Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Create signed firmware releases from version changes on main or a manual Actions run.

**Architecture:** A small Python preflight reads committed version/notes and Git
tags. Actions separates Windows checks, ESP-IDF build/signing, and publication;
existing packaging validation remains authoritative for the signed image.

**Tech Stack:** GitHub Actions, Python stdlib, ESP-IDF 6.1 Docker, Windows MinGW, GitHub CLI.

**Spec:** `docs/superpowers/specs/2026-11-08-release-workflow-design.md`

## Global Constraints

- Preserve existing OTA trust key, generic identity and two-asset allowlist.
- Publish only committed `main` source from `ahmaddawod12/P1S-HeatControl`.
- No key upload, source push, version bump or remote publication during implementation.

## Review Focus

- Existing tags must never be overwritten or accidentally republished.
- Numeric version ordering must reject downgrades without lexicographic errors.
- Dirty or mismatched source must not reach signing/publication.
- Forks and manual runs on other branches must not receive release credentials.
- Key files and build outputs must not enter public release assets.

### Task 1: Release preflight

**Files:** Create `tools/release_workflow.py` and `test_apps/tools/test_release_workflow.py`.

**Interfaces:** CLI accepts `--root`, `--ref`, `--repository`, `--commit`, and
`--github-output`; defaults come from the workflow environment. Outputs are
`version`, `tag`, `commit`, and `publish`.

- [x] Write integration tests with real temporary Git repositories for successful
  planning, existing tags, numeric ordering, invalid versions, missing notes,
  dirty trees, wrong repository/ref/commit and actionable CLI failures.
- [x] Run the new tests and observe the missing preflight failure.
- [x] Implement preflight by reusing `package_firmware.stable_version`.
- [x] Run the new tests and the full packaging/publisher suites.

### Task 2: Actions pipeline and operator documentation

**Files:** Create `.github/workflows/release.yml`; modify `docs/ota-release-guide.md` and `README.md`.

**Interfaces:** Consume Task 1 outputs; publish `v<version>` at `commit` using
`docs/releases/v<version>.md` and the two validated OTA assets.

- [x] Add the two triggers, serial execution, main/repository gates and preflight.
- [x] Add Windows host/dashboard/publisher checks, Linux ESP-IDF build/tool tests,
  signing through a temporary secret file, and allowlisted artifact upload.
- [x] Add a separate publication job with package revalidation, fresh tag
  preflight, exact source target, and anonymous download verification.
- [x] Document one-time secret setup, automatic/manual use, retries and the local fallback.
- [x] Lint workflow, run relevant suites and a release build, review the final
  diff and record any unexecuted remote checks.

## Local validation

- Policy tests: 12 passed; all Python tools: 31 passed.
- Host tests: 152 passed; dashboard tests: 43 passed; publisher scenarios: 10 passed.
- ESP-IDF 6.1 release firmware built; compiled HTTP stack: 3456/4096 bytes.
- Actionlint 1.7.12 passed; upstream action revisions and the official ESP-IDF
  image digest were resolved and pinned.
- Independent review reran all 12 policy tests and found one queueing issue:
  wrong-branch manual runs could displace pending main runs. Scope concurrency
  by ref to prevent this. Retain the standard one-pending-run behavior and
  document it; the newer `queue` option is not supported by current actionlint.
  No other material review findings remained.
- Full Linux container and Actions execution remain unexecuted: Docker Desktop
  is stopped locally and the Actions signing secret is not configured. No remote
  workflow, publication or hardware action was performed.

## Owner follow-up: release reset and Actions setup

The owner subsequently requested retaining version `1.0.0`, deleting the original
GitHub release, and configuring the Actions signing secret without publishing.
The original release and its remote/local tag were removed. The existing private
key was verified against `config/ota_signing_public.bin` and stored as the
repository Actions secret `OTA_SIGNING_PRIVATE_KEY`. No workflow was dispatched
and no replacement release was published. Workflow source changes still need
merging into `main` before a later manual run.
