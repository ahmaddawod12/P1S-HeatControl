#include "firmware_api_policy.h"
#include <stdio.h>
#include <string.h>
static bool equal_host(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}
static bool authority(const char *host, const char *name, uint16_t port)
{
    if (port == 80 && equal_host(host, name)) return true;
    char with_port[80];
    const int length = snprintf(with_port, sizeof(with_port), "%s:%u", name, port);
    return length > 0 && length < (int) sizeof(with_port) && equal_host(host, with_port);
}
bool firmware_request_origin_valid(const char *host, const char *origin, const char *local_ip, uint16_t port)
{
    if (!host || !origin || !local_ip || !port || strlen(host) >= 80 ||
        (!authority(host, local_ip, port) && !authority(host, "p1s-heat.local", port))) return false;
    char expected[88];
    snprintf(expected, sizeof(expected), "http://%s", host);
    return equal_host(expected, origin);
}
static bool whitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
bool firmware_request_body_valid(const char *body, size_t length)
{
    if (!body || length < 2 || length > 64) return false;
    size_t p = 0;
    while (p < length && whitespace(body[p])) ++p;
    if (p == length || body[p++] != '{') return false;
    while (p < length && whitespace(body[p])) ++p;
    if (p == length || body[p++] != '}') return false;
    while (p < length && whitespace(body[p])) ++p;
    return p == length;
}
