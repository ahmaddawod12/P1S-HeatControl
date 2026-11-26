#pragma once
typedef unsigned socklen_t;
#define SOL_SOCKET 0xfff
#define SO_RCVTIMEO 0x1006
struct timeval { long tv_sec; long tv_usec; };
struct ifreq { char ifr_name[16]; };
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
