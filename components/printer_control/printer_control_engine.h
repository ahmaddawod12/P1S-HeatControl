#pragma once
/* One worker calls this; deterministic SDK-boundary tests drive the same tick.
 */
void printer_control_poll(void);
