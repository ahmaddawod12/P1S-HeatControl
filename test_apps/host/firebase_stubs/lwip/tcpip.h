#pragma once
#include "lwip/dns.h"

err_t tcpip_try_callback(void (*callback)(void *), void *arg);
