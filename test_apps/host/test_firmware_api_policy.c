#include "firmware_api_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(firmware_request_origin_valid("192.168.1.20", "http://192.168.1.20", "192.168.1.20", 80));
    assert(firmware_request_origin_valid("p1s-heat.local", "http://p1s-heat.local", "192.168.1.20", 80));
    assert(firmware_request_origin_valid("127.0.0.1:8180", "http://127.0.0.1:8180", "127.0.0.1", 8180));
    assert(firmware_request_origin_valid("192.168.4.1", "http://192.168.4.1", "192.168.4.1", 80));
    assert(!firmware_request_origin_valid("192.168.4.1", "http://192.168.4.1", "10.0.0.31", 80));
    assert(!firmware_request_origin_valid("evil.example", "http://evil.example", "192.168.1.20", 80));
    assert(!firmware_request_origin_valid("p1s-heat.local.evil.example", "http://p1s-heat.local.evil.example", "192.168.1.20", 80));
    assert(!firmware_request_origin_valid("192.168.1.20", "null", "192.168.1.20", 80));
    assert(!firmware_request_origin_valid("192.168.1.20", "http://evil.example", "192.168.1.20", 80));
    assert(!firmware_request_origin_valid("192.168.1.20:81", "http://192.168.1.20:81", "192.168.1.20", 80));
    assert(firmware_request_body_valid("{}", 2));
    assert(firmware_request_body_valid(" \t{ }\r\n", 7));
    assert(!firmware_request_body_valid("{\"url\":\"https://evil.example/firmware\"}", 40));
    assert(!firmware_request_body_valid("{}{}", 4));
    assert(!firmware_request_body_valid("{}\0junk", 7));
    puts("firmware API policy passed");
    return 0;
}
