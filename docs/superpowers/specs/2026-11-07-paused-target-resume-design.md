# Paused printer, target, then Resume

> **Superseded (2026-11-08)** by the [Monitor P1S design](2026-11-07-monitor-p1s-design.md).

The owner's 2026-11-07 instructions supersede the initial-startup/job-identity
admission and 30-second settling rules in the earlier printer workflow design.
They explicitly chose to wait for the chamber target and clarified that a
temperature already at or above target must Resume immediately.

## Agreed behavior

- A live, fresh paused printer is eligible without a run start timestamp,
  observed idle/start history, file/task identity, layer-zero restriction,
  complete optional report metadata or an initial-pause confirmation checkbox.
- Pressing **Preheat & Resume** is the explicit request for that pause. If the
  current filtered chamber temperature is at or above the captured target,
  send one Resume on the next coordinator tick. Otherwise enable chamber
  control and send one Resume as soon as that temperature reaches target.
- Remove the 30-second delay and upper one-degree temperature band. A hotter
  healthy chamber meets the threshold; it must not wait to cool toward target.
- Each deliberate button press starts a new cycle; another pause in the same
  print may be deliberately armed again. There is no automatic Resume replay.
- Preserve live connection/freshness checks, consumed arm tokens, connection
  and observed-run/pause generation changes, dispatch deadlines, Stop/Disable,
  independent heater/sensor fault and ownership protections. Missing or newly
  supplied optional identity metadata must never block the action.
- Printer-specific admission uses the live paused state. Optional error/HMS
  metadata is not an additional admission requirement; the printer itself can
  reject Resume, and the controller ends an unconfirmed attempt after its
  existing acknowledgement deadline. Failed/idle/unknown states end control.
- Keep current source changes in the existing checkout. Build and sign a new
  image, preserve provisioned settings, and do not reset the board or claim the
  new behavior runs on it before a separately verified firmware installation.

## Approach

Update the existing cycle policy, status projection, final native dispatcher
and real dashboard together. Merely hiding the old reason would leave the
backend blocked; merely relaxing admission would fail again at dispatch.
Immediate unconditional Resume was considered and rejected by the owner in
favor of the temperature threshold above.

## Verification

Exercise cold already-paused reports without job metadata, later-layer pauses,
below/equal/above-target boundaries, one send per press, explicit re-arming,
disconnection/Stop/thermal faults at dispatch, and a direct button click with
no checkbox. Run complete host/browser/package checks, compile ESP32 and Unity
images, verify signature/profile/size, and obtain a fresh independent review.
Real Resume and the separately reported download interaction remain hardware
observations, not conclusions from unit tests.

## Software verification — 2026-11-07

The final source passed all 147 host tests, 43 browser tests and 19 packaging
tests. Both the ESP32 release and Unity test applications compiled; Unity was
not flashed or run on hardware. The independent review's late-report dispatch
finding was reproduced by failing tests and fixed with pause/run transition
fencing, including optional start-time enrichment regressions.

Generic hardware/security profile, partition layout, signing-key match, image
signature and compiled HTTP arm stack checks passed. The signed application is
1,245,172 bytes with SHA-256
`2391afb63e25e0080b5952f7641292d3babbd9bdec5d4395adb2266a67c5c0a6`.
Installation and live printer qualification are recorded separately in
[printer integration qualification](../../printer-integration-qualification.md).
The signed image was uploaded and fully read back at 115200 baud without reset;
settings were preserved. First boot and physical Resume remain pending.
