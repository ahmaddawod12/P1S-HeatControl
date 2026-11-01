#pragma once
#include "firmware_types.h"

bool firmware_version_parse(const char *text, firmware_version_t *out);
int firmware_version_compare(const firmware_version_t *a, const firmware_version_t *b);
bool firmware_identity_matches(const firmware_identity_t *a, const firmware_identity_t *b);
bool firmware_identity_valid(const firmware_identity_t *identity);
const char *firmware_phase_name(firmware_phase_t phase);
const char *firmware_result_name(firmware_result_t result);
