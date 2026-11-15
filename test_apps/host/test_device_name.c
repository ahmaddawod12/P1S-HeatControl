#include "device_name.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char name[DEVICE_NAME_LEN] = "unchanged";
    assert(device_name_normalize(" Left-1 \t", name) == ESP_OK);
    assert(!strcmp(name, "left-1"));
    assert(device_name_normalize(" \t", name) == ESP_OK && !name[0]);
    const char *bad[] = {"-left", "left-", "two words", "left.local", "a/b", "é", "abcdefghijklmnopqrstuvw"};
    for (size_t i=0; i<sizeof(bad)/sizeof(*bad); ++i) {
        strcpy(name, "unchanged");
        assert(device_name_normalize(bad[i], name) == ESP_ERR_INVALID_ARG);
        assert(!strcmp(name, "unchanged"));
    }
    assert(device_name_normalize("abcdefghijklmnopqrstuv", name) == ESP_OK);
    memset(name, 'x', sizeof(name));
    assert(!device_name_valid_n(name, sizeof(name)));
    assert(!device_name_valid_n("Left", 5));
    char hostname[DEVICE_HOSTNAME_LEN];
    assert(device_hostname("", hostname) == ESP_OK && !strcmp(hostname, "p1s-1"));
    assert(device_hostname("left", hostname) == ESP_OK && !strcmp(hostname, "p1s-left"));
    assert(device_hostname("abcdefghijklmnopqrstuv", hostname) == ESP_OK);
    assert(!strcmp(hostname, "p1s-abcdefghijklmnopqrstuv"));
    assert(device_name_next("left", name) == ESP_OK && !strcmp(name, "1"));
    assert(device_name_next("1", name) == ESP_OK && !strcmp(name, "2"));
    assert(device_name_next("2", name) == ESP_OK && !strcmp(name, "3"));
    assert(device_name_next("09", name) == ESP_OK && !strcmp(name, "1"));
    assert(device_name_next("4294967295", name) == ESP_ERR_INVALID_STATE);
    assert(device_name_next("1234567890123456789012", name) == ESP_OK && !strcmp(name, "1"));
    puts("Controller names and board identity passed");
    return 0;
}
