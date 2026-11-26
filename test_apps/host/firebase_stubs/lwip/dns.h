#pragma once
#include <stdint.h>

typedef int err_t;
typedef struct { uint32_t address; } ip_addr_t;
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);

#define ERR_OK 0
#define ERR_INPROGRESS (-5)
#define LWIP_DNS_ADDRTYPE_DEFAULT 0
#define IPADDR_STRLEN_MAX 16

err_t dns_gethostbyname_addrtype(const char *, ip_addr_t *, dns_found_callback, void *, uint8_t);
char *ipaddr_ntoa_r(const ip_addr_t *, char *, int);
