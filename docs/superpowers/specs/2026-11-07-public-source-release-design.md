# Public source release baseline

The owner selected active-high relays (OFF=LOW, ON=HIGH, startup disabled),
resetting the firmware version to 1.0.0, and publishing releases in the now-public
source repository `ahmaddawod12/P1S-HeatControl`.

## Approach and constraints

Update the firmware's fixed release repository, publisher default, reproducible
defaults and current build configurations. Keep the existing ECDSA trust key,
partition layout, credential isolation, thermal controls and OTA version-order
checks. Packaging validates the new active-high hardware profile for both generic
releases and private bootstraps. Public assets remain exactly the signed generic
application and its manifest; personal bootstrap images and keys remain local.

Publish against a committed, pushed source revision, with the release tag targeting
that exact commit. The publisher requires an existing, initialized public repo
and a clean source tree; it does not create another repository or change visibility.
Retain its default dry run and refusal to overwrite an existing release tag.
Publication rebuilds the clean committed source and compares the package payload
with that application; a version match alone is insufficient. Any pre-existing
remote version tag is rejected. Both package profiles require the current update
repository as well as the selected polarity.

This replaces the former separate-distribution-repository policy. Preserve older
commissioning records as history. Version 1.0.0 is a new USB baseline: installed
1.0.5 firmware points at the former repository and refuses an older version.
Document that transition rather than weakening OTA checks.

## Validation

Exercise publisher target/visibility/source-commit guards and signed packaging
for active-high images. Run host, dashboard and tools suites, build release,
bootstrap and unit images, verify real GPIO write mapping using host fakes and
the generated production config, and check HTTP stack budget. Sign and validate
the concrete release locally, publish v1.0.0 to the source repository, then verify
anonymous latest-release metadata and both download assets. Do not flash hardware.
