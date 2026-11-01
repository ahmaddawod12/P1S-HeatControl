#include "wifi_manager.h"
#include <string.h>

bool wifi_credentials_are_valid(const char *ssid,const char *password)
{
    if (!ssid || !password) return false;
    const size_t s=strlen(ssid), p=strlen(password);
    return s>0 && s<WIFI_MANAGER_SSID_LEN && (p==0 || (p>=8 && p<WIFI_MANAGER_PASSWORD_LEN));
}
bool wifi_credentials_are_valid_n(const char *ssid,size_t s,const char *password,size_t p)
{
    return ssid && password && s && p && memchr(ssid,0,s) && memchr(password,0,p) &&
        wifi_credentials_are_valid(ssid,password);
}
esp_err_t wifi_credentials_init(wifi_credentials_t *out,const char *ssid,const char *password)
{
    if (!out || !wifi_credentials_are_valid(ssid,password)) return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    memcpy(out->ssid,ssid,strlen(ssid)); memcpy(out->password,password,strlen(password));
    return ESP_OK;
}
uint32_t wifi_manager_backoff_ms(size_t attempt)
{
    static const uint32_t delays[]={1000,2000,4000,8000,30000};
    return delays[attempt<4?attempt:4];
}
const char *wifi_manager_result_name(wifi_result_t result)
{
    static const char *names[]={"none","setup_required","connecting","connected","no_network",
        "auth_failed","save_failed","ap_failed","driver_failed","storage_failed"};
    return (unsigned)result<sizeof(names)/sizeof(names[0])?names[result]:"unknown";
}
