#pragma once
#include "wifi_manager.h"
#define WIFI_API_MAX_BODY 768U
bool wifi_request_parse(const char *body,size_t length,wifi_credentials_t *out);
