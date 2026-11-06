#pragma once

#include <stdbool.h>

/* Pause the fake controller's command dequeue without holding runtime locks. */
void host_control_queue_pause(bool paused);
bool host_control_queue_is_paused(void);
/* Model a tick-aligned reply wait ending before the absolute time deadline. */
void host_reply_queue_timeout_early(void);
