# Relay fault lockout and printer Save race fixes

The user authorized fixing the two issues reproduced in the firmware review.

## Behavior

- A latched `HEAT_FAULT_OUTPUT_INIT` survives later NVS faults. Acknowledgement
  and Enable remain rejected until controller initialization after a restart.
- Printer Save preserves its access-code draft and pending state when a poll
  starts before the PUT settles, including when that poll arrives afterward.
  A fresh poll after acceptance may confirm persistence and clear an unchanged
  draft. Rejection, failure and newer edits keep the draft.

## Approach

Preserve the output fault in the shared fault-latching helper so later fault
sources cannot weaken the lockout. Reuse the existing Forget request-timing
guards for Save. Separate fault flags or server operation IDs would expand the
interfaces without being needed for these two reproduced bugs.

## Constraints and verification

Keep the current checkout and preserve the existing `version.txt` edit. Keep
NVS-only recovery and existing Save/Forget behavior covered by their tests.
Add failing host/controller and browser regressions first; mirror the safety
case in the on-device unit suite. Run host, dashboard and tooling tests, build
the ESP32 firmware and unit application, and obtain a read-only final review.
No commit, flash, signing or publication is part of this fix.
