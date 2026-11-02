#pragma once
#include <stdint.h>
typedef unsigned socklen_t;
#define AF_INET 2
struct in_addr { uint32_t s_addr; };
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_storage { unsigned short ss_family; char data[30]; };
struct sockaddr_in { unsigned short sin_family; uint16_t sin_port; struct in_addr sin_addr; };
int getsockname(int, struct sockaddr *, socklen_t *);
static inline uint16_t ntohs(uint16_t value) { return value; }
