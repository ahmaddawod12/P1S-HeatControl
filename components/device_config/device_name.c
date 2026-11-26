#include "device_name.h"
#include <string.h>

bool device_name_valid_n(const char *name, size_t capacity)
{
    if (!name || !capacity || !memchr(name, 0, capacity)) return false;
    const size_t length = strlen(name);
    if (length >= DEVICE_NAME_LEN) return false;
    if (!length) return true;
    if (name[0] == '-' || name[length-1] == '-') return false;
    for (size_t i=0; i<length; ++i)
        if (!((name[i]>='a' && name[i]<='z') || (name[i]>='0' && name[i]<='9') || name[i]=='-'))
            return false;
    return true;
}
