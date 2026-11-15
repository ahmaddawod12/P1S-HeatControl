# Controller names

Approved in chat: choose an optional name during Wi-Fi setup and use
`p1s-<name>.local`, without `heat`. Show an address preview, retain the name
across restarts, and allow edits through the same settings form.

Names normalize surrounding whitespace and uppercase ASCII, then permit
1–22 lowercase letters, digits, or internal hyphens. An empty name starts at
`p1s-1.local`. The existing `P1S-Heat-Setup` hotspot and setup flow stay the
same; only the controller-name field and preview are added.

If mDNS changes a requested custom hostname because of a collision, try
`p1s-1.local`, then `p1s-2.local`, continuing until available. When an already
numbered name conflicts, continue to the next number. Before claiming a
candidate, make a bounded mDNS ownership query on the station network while
using an internal temporary identity. This preserves an existing owner's
name regardless of the library's IP tie-break. The temporary identity cannot
equal a valid user name and is never persisted as the controller's name.
Use real probing/negotiation for simultaneous claims. Require three seconds
with a stable hostname on the connected station network before confirming it;
continue monitoring for later conflicts and restart the check after every
station IP event. Save and reuse the final accepted name across restarts.
IP access remains available. No shared `p1s-heat.local` alias or MAC suffix.

Save Wi-Fi credentials and the normalized controller name in one versioned
NVS blob. Persist requested settings atomically before connecting, so an
interrupted setup can resume. After probing, commit any resolved numbered
name with those credentials; only then mark the name confirmed. Read existing
`wifi_v1` records with an empty name; preserve saved credentials on upgrade.
A failed settings save retains the previous configuration. Failed final name
commits retry and keep setup available. Reopen setup on late conflicts or
name/discovery failures, then keep it open through checking and for ten
seconds after confirmation. mDNS initialization and retries are worker-owned;
discovery failures must never prevent Wi-Fi connectivity or IP access.

The Wi-Fi API returns the current name, requested name, actual hostname,
and whether confirmation is pending. Dashboard links and recovery guidance use those
values. Request-origin checks authorize only this board's actual hostname
or the socket's local IP, with existing Origin and CSRF checks intact.
Publish the pending state before an ownership query and return the worker's
coherent status snapshot; the status getter must never expose the temporary
SDK hostname. A browser poll begun during an outstanding Save must not confirm
or discard its pending draft, even if that GET arrives after the PUT settles.

Approaches considered: MAC suffixes were superseded by the user's numbered
fallback rule; rejecting a duplicate would require another setup attempt.
The approved approach combines custom names with automatic numbering and
persists the resolved name. Reuse the current form, worker, and mDNS component.

Verification: storage migration/failed commits, parsing and name limits,
distinct boards, mDNS collision handling, startup retry behavior, dynamic
request-origin checks, and dashboard preview/save/recovery tests. Build
generic ESP32 firmware and unit firmware; two-board radio checks need hardware.
