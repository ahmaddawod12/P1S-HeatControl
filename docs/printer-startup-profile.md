# PC-prepared job and paused-printer Resume

> **Obsolete (2026-11-08).** Monitor P1S needs no startup pause and no paused
> job: start prints normally from Bambu Studio. This page is kept as history of
> the retired Preheat & Resume workflow
> ([Monitor P1S design](superpowers/specs/2026-11-07-monitor-p1s-design.md)).

Current button behavior: **connected paused printer → target → one Resume.**
The 2026-11-07 monitoring session observed advertised P1S firmware
`01.10.00.00` and Studio config version `02.08.02.61`; no startup-pause
placement has been verified for that combination. A special startup pause is
optional; the button also accepts an already-paused or later-layer print.
Preheat/Resume is built into all current firmware. Hardware test evidence is listed in
[integration qualification](printer-integration-qualification.md).

The intended workflow keeps the complete print configuration on the PC:

1. Prepare and slice the job in Bambu Studio. Choose the plate, material/AMS,
   calibration and other print options there.
2. Submit the job. Merely
   opening a file on the PC, or uploading a file to the printer, does not create
   the paused job that this firmware can resume.
3. Pause the accepted job when you want chamber preheating. The ESP32 does not
   need to have observed the job start, and missing job metadata does not block it.
4. Choose the chamber target using the
   existing Target control, then select **Preheat & Resume**. Connecting to the printer or seeing a paused state
   does not arm anything.
5. At or above target, the controller sends Resume immediately. Below target,
   it heats until the current filtered temperature reaches target and sends one
   Resume. There is no upper temperature band or settling delay. It maintains
   the captured target during that job and ends chamber
   control when the printer reports finish.

The ESP32 never selects a print file or sends a new-job command. It carries no
plate, filament, AMS, bed-levelling or slicer settings. Once accepted by the
printer, the job does not depend on the PC remaining connected.

## Optional pre-extrusion startup profile

If your process requires the pause before all purge/priming/model extrusion,
validate that separate PC profile on your printer. This placement requirement
does not gate the controller's paused-printer button.

With separate authorization and supervision, duplicate the stock PC printer
profile and give the copy a distinct name. Preserve an ordinary stock profile
for jobs that do not use chamber automation.

Determine a supported, resumable pause command and its earliest safe location
against the actual installed firmware. Demonstrate the pause before every
source of extrusion, including preparation purge and priming. Record any
firmware-controlled motion or heating that occurs before user startup G-code;
the profile must not claim that no preparation happens beforehand.

Do not publish the profile as usable until it preserves the PC-selected print
options, provides reliable paused status, tolerates the intended
preheat wait, and resumes the same job through normal preparation and printing.
Use the [qualification record](printer-integration-qualification.md) for measured
results and sanitized captures.

| Profile detail | Verified value |
|---|---|
| P1S firmware / Bambu Studio versions | Pending |
| LAN/developer authorization settings | Pending |
| Saved profile name and exported revision | Pending |
| Exact pause command | Pending; intentionally unspecified |
| Exact placement in startup sequence | Pending |
| Motion/heating observed before pause | Pending |
| Proof of no purge/prime/model extrusion before pause | Pending |
| Maximum demonstrated pause duration | Pending |
| Options preserved and same-job Resume evidence | Pending |

## Normal use and interruptions

All current controller images include **Preheat & Resume**. Arming is available
only with a live connection, fresh paused status and a healthy chamber; pressing
the button requests each cycle. Enter the
printer IPv4, serial, LAN access code and a reachable time server in Printer
connection. The access code is write only. Leaving it blank preserves the saved
code only when the serial number is unchanged.

Each cycle consumes a fresh offered token when the button is pressed.
An uncertain Resume is never automatically repeated, including
after reconnecting to the same run. A later printer pause ends chamber control;
resuming the printer manually does not restore automatic chamber heat. A new
deliberate button press can arm a new cycle for the same paused print.

**Stop chamber control** and the existing **Disable** control revoke chamber
automation. They do not cancel the printer's job. The normal fan cooldown and
independent manual-fan request remain governed by the existing controller.
Use the printer's own controls when you intend to cancel its print.

Reboot and connection loss consume active chamber control. No heat enable or
pending Resume is restored. Review the printer directly before making any new
explicit decision.
