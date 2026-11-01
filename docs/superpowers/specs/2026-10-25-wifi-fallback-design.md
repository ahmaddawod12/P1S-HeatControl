# Wi-Fi settings and fallback hotspot

## Intent and accepted behavior

The owner approved immediate Save & Connect without rebooting. The enclosed
printer must remain configurable after moving it, changing its router or saving
an incorrect Wi-Fi password. Reuse the existing dashboard on both network
interfaces. SSID and password are visible editable text, as explicitly requested;
there is no new login, password masking or provisioning application.

The agreed flow is: no saved credentials starts a setup hotspot immediately;
an unavailable configured network starts it after 30 seconds; Save & Connect
persists the pair and tries it immediately while keeping setup access; wrong
credentials leave the hotspot available; success displays the station address
and closes the hotspot after a short grace period. Credentials survive power
cycles and OTA. Ordinary network loss does not change heater regulation.

This supersedes the previous OTA design's exclusion of runtime Wi-Fi setup.
The existing OTA signature, partition layout, safe maintenance and rollback
requirements remain binding.

## Approaches

1. Dashboard credentials with a fallback SoftAP, using the same HTTP service:
   selected by the owner; works with a phone/browser and recovers unavailable
   networks without access to the board.
2. Dashboard-only changes: cannot recover when the stored network is absent.
3. BLE/native-app provisioning: requires another client and does not match the
   requested existing-page flow.

## Builder decisions

- ESP-IDF 6.1, classic ESP32 / 4 MB, existing C components and vanilla JS/CSS.
- Open setup SSID `P1S-Heat-Setup`, AP address `192.168.4.1/24`, up to four AP
  clients; AP+STA coexistence keeps station retries running. This is an explicit
  local-use design, consistent with the owner's stated preference.
- Fallback starts at 30,000 ms of continuous station unavailability. Repeated
  failed joins do not restart that timer. Retry delays remain 1, 2, 4, 8, 30 s.
- Successful station connection starts a 10,000 ms AP shutdown grace period.
  A new credential request or disconnection cancels shutdown. Do not close the
  AP while an OTA/trial/configuration operation owns the service gate.
- Save first ensures AP startup succeeded, commits the existing version-1
  `wifi_v1` record, then disconnects/reconfigures only the station. If AP startup
  or persistence fails, retain the old saved/running configuration and report
  the failure. Wrong but syntactically valid credentials are saved and remain
  editable through the hotspot. No trimming of SSID/password whitespace.
- Native driver calls and flash writes run on the Wi-Fi worker, with a 250 ms
  response grace before applying a queued request. Snapshot locks never cover
  SDK calls or flash commits. HTTP returns 202 once admitted; never retry an
  uncertain mutation automatically.
- The native adapter buffers the latest network event and checks an associated
  SSID before accepting an IP event. Disconnects cancel AP shutdown; stale
  events for an earlier SSID cannot mark a replacement network connected.
- Missing credentials are a normal setup state. Corrupt/newer storage remains
  an NVS fault and is not erased or overwritten; AP diagnostics may still run.
- Use a single atomic service-operation gate with owners NONE, WIFI, OTA and
  TRIAL. Save and OTA requests are mutually exclusive, including OTA preflight.
  A real pending trial with a previous confirmed image reserves TRIAL until
  confirmation/reboot. Wi-Fi edits during that trial return 409, preserving the
  rollback-readable credential record.
- Initial setup with no rollback image allows Wi-Fi edits despite its boot
  maintenance gate. At its existing 120 s deadline it remains inhibited and
  reports setup-required, but continues checking local health so a later Save
  can complete setup without a reboot. A hotspot alone never satisfies trial
  station-connectivity health. Real OTA rollback deadlines remain unchanged.
- The normal dashboard gets a Wi-Fi section with visible labels, plain text
  inputs, Save & Connect, connection/error feedback, hotspot name/address and
  station address. Polling must preserve unfinished edits. Provide reconnect
  instructions before changing interfaces and retain them when the old URL
  becomes unreachable. Keep the existing look and touch-sized controls.
- `GET /api/wifi` returns saved SSID/password, configured/connected/applying,
  AP state/addresses, result, `can_save` and the existing per-boot CSRF token.
  `PUT /api/wifi` accepts exactly string fields `ssid` and `password`, bounds
  the body at 768 bytes and preserves existing byte limits (SSID 1..32;
  password empty or 8..63). Reject duplicate fields, NUL and trailing JSON.
  Mutations retain existing local Host/Origin/token checks on STA and AP.
- Public firmware contains no compiled credentials; the key, fixed OTA layout,
  shared settings schema and Wi-Fi record format are unchanged. Increment to
  1.0.5 for the eventual signed feature package. Public publication needs a new
  concrete approval; previous approvals named older test versions only.

## Boundaries and validation

Use a small host-executable Wi-Fi engine with real timing, request application
and error behavior, and injected SDK/flash boundaries. The native adapter owns
its state and publishes bounded snapshots. Cover no-credentials setup, exact
fallback/grace boundaries, repeated failures, wrong credentials, later recovery,
storage/AP failures, stale SSID events, rapid replacements and operation races.

Native HTTP cases cover clear saved credentials, strict request validation,
AP/STA origin handling, busy conflicts and all routes with four SSE clients.
Browser tests cover visible/editable fields, preserved drafts, one mutation,
lost response, failed connection/retry and successful reconnect instructions.
Run existing host/browser/package/publisher suites and ESP-IDF main/bootstrap/
unit builds. Preserve the owner's exact staged index and the existing OTA
working-tree integration.

On the unloaded board, preserve settings/configuration before native tests,
restore the confirmed baseline afterward, and test the signed new image through
the approved update path. Exercise missing/wrong credentials, the actual hotspot
page and immediate Save without reset, station recovery, AP shutdown and saved
credentials after a reboot. A PC Wi-Fi switch or phone assistance may require
coordination; do not claim tests that were not physically executed. Do not
change eFuses or connect heater loads.
