#pragma once
#include <stdint.h>
typedef unsigned socklen_t;
#define AF_INET 2
#define SOL_SOCKET 0xfff
#define SO_RCVTIMEO 0x1006
struct timeval { long tv_sec; long tv_usec; };
struct in_addr { uint32_t s_addr; };
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_storage { unsigned short ss_family; char data[30]; };
struct sockaddr_in { unsigned short sin_family; uint16_t sin_port; struct in_addr sin_addr; };
int getsockname(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
static inline uint16_t ntohs(uint16_t value) { return value; }
