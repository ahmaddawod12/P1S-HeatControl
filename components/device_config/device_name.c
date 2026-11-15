#include "device_name.h"
#include <stdio.h>
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
static bool whitespace(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
esp_err_t device_name_normalize(const char *name, char out[DEVICE_NAME_LEN])
{
    if (!name || !out) return ESP_ERR_INVALID_ARG;
    while (whitespace(*name)) ++name;
    size_t length = strlen(name);
    while (length && whitespace(name[length-1])) --length;
    if (length >= DEVICE_NAME_LEN) return ESP_ERR_INVALID_ARG;
    char normalized[DEVICE_NAME_LEN] = {0};
    for (size_t i=0; i<length; ++i)
        normalized[i] = name[i]>='A' && name[i]<='Z' ? name[i]+'a'-'A' : name[i];
    if (!device_name_valid_n(normalized, sizeof(normalized))) return ESP_ERR_INVALID_ARG;
    memcpy(out, normalized, sizeof(normalized));
    return ESP_OK;
}
esp_err_t device_hostname(const char *name, char hostname[DEVICE_HOSTNAME_LEN])
{
    if (!name || !hostname || !device_name_valid_n(name, strlen(name)+1))
        return ESP_ERR_INVALID_ARG;
    snprintf(hostname, DEVICE_HOSTNAME_LEN, "p1s-%s", name[0]?name:DEVICE_DEFAULT_NAME);
    return ESP_OK;
}
esp_err_t device_name_next(const char *name, char next[DEVICE_NAME_LEN])
{
    if (!name || !next) return ESP_ERR_INVALID_ARG;
    uint32_t number=0;
    bool numeric=name[0]>='1' && name[0]<='9';
    for (const char *p=name; numeric && *p; ++p) {
        if (*p<'0' || *p>'9') numeric=false;
        else if (number>(UINT32_MAX-(uint32_t)(*p-'0'))/10) numeric=false;
        else number=number*10+(uint32_t)(*p-'0');
    }
    if (!numeric) number=0;
    if (number==UINT32_MAX) return ESP_ERR_INVALID_STATE;
    snprintf(next, DEVICE_NAME_LEN, "%lu", (unsigned long)(number+1));
    return ESP_OK;
}
