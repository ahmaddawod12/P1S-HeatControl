# PC-prepared job and startup pause

Status: **no startup command or placement has been qualified yet.**
The P1S firmware and Bambu Studio versions are still unknown. Do not enable
automatic Resume using an assumed G-code command. The required evidence and
authorization are listed in [integration qualification](printer-integration-qualification.md).

The intended workflow keeps the complete print configuration on the PC:

1. Prepare and slice the job in Bambu Studio. Choose the plate, material/AMS,
   calibration and other print options there.
2. Submit the job using a separately saved, qualified startup profile. Merely
   opening a file on the PC, or uploading a file to the printer, does not create
   the paused job that this firmware can resume.
3. Observe the intentional initial pause before purge, prime or model
   extrusion. The printer must already have accepted the job.
4. On a qualified controller build, choose the chamber target using the
   existing Target control. Confirm the initial-pause checkbox and select
   **Preheat & Resume**. Connecting to the printer or seeing a paused state
   does not arm anything.
5. The controller waits until the target has been reached and temperature has
   stayed within ±1 °C for 30 continuous seconds, then permits one Resume
   attempt. It maintains the captured target during that job and ends chamber
   control when the printer reports finish.

The ESP32 never selects a print file or sends a new-job command. It carries no
plate, filament, AMS, bed-levelling or slicer settings. Once accepted by the
printer, the job does not depend on the PC remaining connected.

## Profile qualification procedure

With separate authorization and supervision, duplicate the stock PC printer
profile and give the copy a distinct name. Preserve an ordinary stock profile
for jobs that do not use chamber automation.

Determine a supported, resumable pause command and its earliest safe location
against the actual installed firmware. Demonstrate the pause before every
source of extrusion, including preparation purge and priming. Record any
firmware-controlled motion or heating that occurs before user startup G-code;
the profile must not claim that no preparation happens beforehand.

Do not publish the profile as usable until it preserves the PC-selected print
options, provides reliable paused-job identity/status, tolerates the intended
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

The default controller image supports connection setup and monitoring while
qualification is pending; **Preheat & Resume** remains unavailable. Enter the
printer IPv4, serial, LAN access code and a reachable time server in Printer
connection. The access code is write only. Leaving it blank preserves the saved
code only when the serial number is unchanged.

Each cycle needs a fresh offered token and an explicit confirmation of the
initial pause. An uncertain Resume is never automatically repeated, including
after reconnecting to the same run. A later printer pause ends chamber control;
resuming the printer manually does not restore automatic chamber heat.

**Stop chamber control** and the existing **Disable** control revoke chamber
automation. They do not cancel the printer's job. The normal fan cooldown and
independent manual-fan request remain governed by the existing controller.
Use the printer's own controls when you intend to cancel its print.

Reboot and connection loss consume active chamber control. No heat enable or
pending Resume is restored. Review the printer directly before making any new
explicit decision.
