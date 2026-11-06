# Firmware review fixes

Implement the three confirmed findings from the firmware review approved by
the user's instruction to fix them.

## Approach

Use one internal HTTP request utility rather than duplicating body loops across
chamber, firmware, Wi-Fi and printer handlers. Keep each route's existing input
limits, Origin/CSRF policy and successful responses. Body acquisition has a
five-second absolute deadline, with socket receive timeouts limited to the
remaining budget. Rejected or incomplete bodies close the connection instead
of entering ESP-IDF's automatic body drain.

Keep the controller task authoritative for fault acknowledgement. A bounded,
serialized reply channel reports its actual decision after the control step
and status publication. Refusal returns HTTP 409, queue/task unavailability 503,
and a successful acknowledgement returns 204 with heating disabled. A request
that times out before execution must not clear a fault later.

Initialize the diagnostics ring before runtime tasks start. Record every change
of controller state or latched fault once, using the actual transition time;
stable control iterations do not create duplicate history entries.

## Constraints and verification

Preserve the existing uncommitted calibration and GPIO changes. Keep sensor
acquisition, urgent Disable, relay shutoffs and regulation independent of HTTP
and persistence waits. Do not change restart timing, public JSON fields or
hardware. Add regressions against real handlers and threaded runtime, then run
the host/dashboard/tool suites, release and unit-image builds and HTTP stack
budget check. Physical execution and flashing remain outside this source fix.
