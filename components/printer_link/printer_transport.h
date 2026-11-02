#pragma once
#include "printer_link.h"
const printer_link_ops_t *printer_transport_ops(void);
uint32_t printer_link_generation(void);
bool printer_link_cancelled(uint32_t generation);
void printer_link_invalidate(uint32_t generation);
void printer_link_event_connected(uint32_t generation);
void printer_link_event_subscribed(uint32_t generation);
void printer_link_event_report(uint32_t generation, const char *json,
                               size_t len, uint32_t report_revision,
                               uint64_t now_ms);
