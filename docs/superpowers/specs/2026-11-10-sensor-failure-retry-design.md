# Sensor failure retry design

Approved in the chat on 2026-11-10: sample every 1 second, allow five
consecutive failed acquisitions before latching a sensor fault, and reset the
failure count on one successful acquisition. Keep heating running during the
retry window. After shutdown, keep the fan running for 10 seconds. Preserve
immediate over-temperature protection and acknowledgement of latched faults.

## Approaches

- Immediate shutdown on every failed read is the current behavior; the owner
  explicitly rejected it because brief sensor failures interrupt heating.
- Pausing the heater during retries was proposed and explicitly rejected.
- Selected: hold the existing heater output for failures 1 through 4; failure
  5 latches the reported sensor fault and disables heating. Count all sensor
  acquisition failures together, including changing error kinds. A success
  resets the count immediately. Only the acquisition task advances the count.

## Behavior and boundaries

- The 1000 ms acquisition cycle includes the existing 400 ms, 11-bit DS18B20
  conversion. The independent controller checks every 500 ms, as requested in
  the subsequent chat clarification.
- An invalid reading remains invalid and is never inserted into the median
  filter or presented as a new valid temperature. During retries, retain the
  controller's existing output; do not start a disabled heater from bad data.
- Keep Disable, maintenance, monitor inhibition, and zero-target shutdown
  responsive throughout retries. A stopped acquisition task still triggers
  the independent 2000 ms stale-task fault, including after a failed read.
- Any new valid over-temperature reading (raw or filtered >= 82 C) shuts down
  immediately, even immediately after a retry. This protection depends on
  receiving a usable temperature; retries intentionally allow a short period
  without fresh temperature feedback, as requested.
- Successful acquisition resets failures to zero. Keep the existing three
  consecutive valid samples required for enabling and acknowledging sensor
  faults. If an enabled controller needs to restart its heater after bad
  reads, require those three valid samples and the existing 5000 ms minimum
  restart interval. A latched fault never automatically clears or restarts.
- A fault that ends an enabled heating session starts a 10000 ms fan cooldown,
  including faults from holding. Repeated status/control ticks and early
  acknowledgement must not extend or cancel it. Preserve manual fan control
  and the existing target-reached and operator-disable cooldown behavior.
- Five failed reads at one-second intervals represent approximately five
  seconds without a successful reading; the fifth failed read is the trigger,
  rather than a separate elapsed-time delay.

## Scope and validation

Change acquisition timing, the temperature monitor's failure counter, the
controller's retry/restart handling, and the fault fan cooldown. Leave the
GPIO short detection unchanged: its physical cause remains unconfirmed.
Preserve unrelated working-tree edits. Verify with deterministic host tests,
threaded runtime tests, the host suite, and firmware compilation where the
installed toolchain permits. Do not flash hardware as part of this change.
