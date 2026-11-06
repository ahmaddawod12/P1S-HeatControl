# Sensor calibration offset

Add a dashboard control for the existing DS18B20 calibration setting. New
settings default to 0 °C; existing saved offsets remain intact. The accepted
range remains −10 to +10 °C, with a numeric input and an explicit Save button.
Positive offsets raise both displayed and control temperatures, and negative
offsets lower them. Save through NVS and apply immediately without rebooting.

## Approach

Reuse the existing settings schema and median filter. Add a calibration runtime
command using the same serialized persistence and reserved command slot as target
changes. Rebase buffered measurements when applying the offset so old and new
calibration never mix. Guard monitor access with the runtime admission mutex;
sensor conversion and NVS writes stay outside it. Expose the current offset in
status/SSE and accept a protected `PUT /api/calibration` containing exactly one
numeric `calibration_offset_c` field. Maintenance and printer automation block
new calibration saves, as they already block target changes.

A compile-time constant would require reflashing for each calibration; a
display-only correction would leave regulation inconsistent with the readout.
Neither serves the requested adjustable sensor offset.

## Validation

Cover signed offsets, unchanged default, persistence failure, restart loading,
filter consistency, stale/fault preservation, over-temperature response,
request validation and Origin/CSRF checks, and dashboard editing and save behavior.
Run host and dashboard suites and build firmware. Physical flashing is outside
this source change.
