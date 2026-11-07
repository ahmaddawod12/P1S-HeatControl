# Preheat/Resume availability

> **Superseded (2026-11-08)** by the [Monitor P1S design](2026-11-07-monitor-p1s-design.md).

The later same-day [paused-target Resume design](2026-11-07-paused-target-resume-design.md)
supersedes this document's initial-pause/job-identity and settling requirements.
The upload and hardware observations below remain historical evidence.

The owner explicitly requested on 2026-11-07 that Preheat/Resume always be
included and that it cannot work without the printer connection. This replaces
the build-time qualification gate in the 2026-11-01 design and plan.

## Behavior

- Remove `CONFIG_P1S_PRINTER_AUTO_RESUME`, the injected `resume_qualified` and
  `qualified` fields, and the `qualification_required` availability reason.
- Every standard build includes the existing Preheat/Resume workflow.
- Keep live connection, fresh complete telemetry, eligible paused-job identity,
  valid sensor, target, maintenance, explicit confirmation and one-use token
  checks. Connecting alone never arms or heats.
- Block an old arm offer immediately when the native live-link check fails,
  including before the next coordinator poll. Clear its public token and
  connection/readiness flags. Admission also rechecks the live link.
- A connection lost at the settle deadline revokes automatic heat without
  sending Resume. Recovery cannot automatically restart that cycle.
- Keep the 30-second thermal settling period, final dispatch guard, transport
  deadline, no-replay behavior, Stop and independent heat lease.
- Keep historical test evidence factual; update current operating instructions
  so they no longer prescribe a feature-enabled commissioning build.

## Implementation and verification

Remove the gate from the cycle policy and native coordinator interface, update
all host/Unity fixtures, and remove the obsolete dashboard explanation.
Regressions exercise the real coordinator with injected network/runtime
boundaries and the real dashboard script. The availability test first failed
at the old gate; the immediate-disconnect test first failed on its cached arm
offer. Both passed after the respective production changes. Run the complete
host/browser/tool suites and compile normal ESP32 and Unity firmware. Keep
actual device execution separate from build/test evidence.

## Verification results — 2026-11-07

| Check | Actual result |
|---|---|
| Complete host suite | 115/115 passed, including the new coordinator regressions |
| Complete browser suite | 42/42 passed, including cached-offer disconnected/stale/incomplete cases |
| Python packaging suite | 19/19 passed |
| Publisher fixtures | 10/10 passed; publication mocked |
| Reconfigured generic ESP32 build | Passed; unsigned application 1,245,104 bytes, fits the 2,031,616-byte OTA slot after the 68-byte signature allowance |
| Reconfigured native Unity image | Compiled successfully; not flashed or executed on ESP32 |
| Generated build option/menu | Removed in both builds; old availability reason absent from the generic application binary |
| Generic hardware/security profile | Validated; no personal Wi-Fi credentials compiled in |
| Compiled HTTP arm stack path | 3,456 of 4,096 bytes including ABI/interrupt allowance and 512-byte margin |
| Independent production/test review | No actionable findings; hardware behavior not reviewed |
| Whitespace | `git diff --check` passed |

The current image was signed with the existing trusted key and uploaded to
COM9 at 115200 baud on 2026-11-07 (completed 16:45:55 UTC). The 1,245,172-byte
signed application in `ota_0` at `0x20000` passed flash digest verification and
full byte-for-byte readback; SHA-256 is
`2ce9002b0eb6b511f2e882e8f9d5176b2eef29d53bb0b0415e04d6c9deb609bd`.
Bootloader, partition table, Wi-Fi/printer configuration and OTA selection
metadata matched their backup after the write. The owner power-cycled during
serial troubleshooting; the upload tool never reset the board. It closed the
port with the board left in the RAM flasher stub/download mode, as requested.
The owner subsequently left download mode. Application boot, restored saved
configuration, fresh connected printer monitoring and healthy sensor readings
were observed. The already-paused job remained ineligible with
`identity_required`: the controller had neither a usable job start time nor
observation of an idle/terminal-to-new-job transition on this connection.
A brief running-then-paused transition did not establish that history. Heating
and every output remained disabled; Resume was not attempted. Physical
Preheat/Resume execution remains pending for a fresh eligible test job. The
earlier manual-pause trace is evidence for the old image only. This image has
not been published.
