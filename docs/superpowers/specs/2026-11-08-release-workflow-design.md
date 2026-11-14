# Releases from main and manual workflow runs

The owner wants releases from `main` or a GitHub Actions workflow trigger.
Today the owner builds, signs, packages and publishes locally with PowerShell.

## Approach

Add one GitHub Actions workflow with a push trigger restricted to changes to
`version.txt` on `main`, plus `workflow_dispatch`. Both use the version committed
in `version.txt`; ordinary source pushes do not publish. Manual runs on another
branch are rejected. An existing version tag is a successful no-op. A new version
must exceed every existing stable version tag and have nonempty committed notes
at `docs/releases/v<version>.md`.

A local wrapper would reduce commands but would still require the owner's PC.
A tag-push trigger would require a separate manual tag operation. The selected
approach supports both requested entry points without changing device OTA.

## Implementation constraints

- Keep the existing OTA key, hardware profile, partition layout and stable versions.
- Build generic firmware with ESP-IDF 6.1 and bootstrap credentials OFF.
- Run Windows host, dashboard and publisher tests, then Linux packaging tests,
  firmware builds and the compiled stack-budget check before signing.
- Use the existing key from the `OTA_SIGNING_PRIVATE_KEY` Actions secret. Write it
  only to runner temporary storage, mount it read-only for signing, and remove it.
- Validate the signature, generic identity and application payload with the
  existing packager. Transfer exactly the signed application and `firmware.json`.
- Only the publishing job gets `contents: write`; tag the exact tested commit.
- Serialize workflow runs, refuse tag replacement, and verify public downloads.
- Preserve the local publisher. Do not change versions, upload a key, push source,
  run a remote workflow, publish firmware or flash hardware as part of this task.

## Validation

Exercise release policy against real temporary Git repositories, including stale
versions, malformed versions, missing notes, dirty source, wrong commits and
branches, and existing tags. Run the tool and publisher suites, dashboard tests,
host suite and an ESP-IDF release build. Lint the workflow. A live signed Actions
run requires the owner to configure the signing secret after merging this change.
