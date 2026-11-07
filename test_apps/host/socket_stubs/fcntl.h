#pragma once
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 1
int fcntl(int, int, ...);
