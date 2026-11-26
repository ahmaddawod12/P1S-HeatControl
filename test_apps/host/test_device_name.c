#include "device_name.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *valid[] = {"", "left-1", "abcdefghijklmnopqrstuv"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i)
        assert(device_name_valid_n(valid[i], strlen(valid[i]) + 1));
    const char *bad[] = {"-left", "left-", "two words", "left.local", "a/b", "é", "Left", "abcdefghijklmnopqrstuvw"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(!device_name_valid_n(bad[i], strlen(bad[i]) + 1));
    char name[DEVICE_NAME_LEN];
    memset(name, 'x', sizeof(name));
    assert(!device_name_valid_n(name, sizeof(name)));
    puts("Stored controller names passed");
    return 0;
}
