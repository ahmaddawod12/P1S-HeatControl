# Full firmware review fixes

The user authorized fixing all seven findings from the whole-firmware review.
Keep the current ESP-IDF 6.1 platform, hardware profile, control behavior,
public API, signed OTA layout, and default-off automatic Resume gate.

## Approach

Make focused changes at the faulty boundaries rather than changing dependencies
or redesigning the controller:

- Reject flash encryption in generic and personal build validation before an
  installer can write a bootloader. Require the configured ECDSA verification
  key to match the packaging key and verify that the application contains it.
- Feed station IP loss into the existing Wi-Fi offline/backoff/fallback state
  machine. Ignore hotspot address assignments in printer invalidation.
- Initialize the SSE lock before the HTTP server accepts requests. Close dropped
  stream sockets so EventSource can reconnect instead of keeping stale readings.
- Keep normal SDK receive semantics. Limit nonblocking socket mode to bounded
  writes and restore its prior state on every path, preserving send deadlines.

These changes reuse the existing components and tests. Increasing MQTT buffers
would only hide fragmentation; changing dependencies would broaden this fix.

## Verification and boundaries

Each reviewed failure gets a regression that fails before the fix. Exercise
actual production handlers, state machines, transports and signed packaging;
replace only hardware, SDK I/O and RTOS boundaries where host tests require it.
Include failed initialization/cleanup, DHCP recovery, irrelevant IP events,
partial reads, and write errors/deadlines. Run the complete host, dashboard,
packaging and publisher suites, release and Unity-image builds, and the compiled
HTTP stack check. No board flashing, eFuse changes, publishing, or key rotation.
