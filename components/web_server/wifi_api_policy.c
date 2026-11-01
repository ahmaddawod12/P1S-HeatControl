#include "wifi_api_policy.h"
#include "cJSON.h"
#include <string.h>

/* Only a flat object of strings is useful here. Bound nesting before invoking
 * cJSON so an invalid deeply nested body cannot exhaust the HTTP task stack. */
static bool flat_without_nul(const char *body,size_t length)
{
    bool quoted=false;
    unsigned depth=0;
    for(size_t i=0;i<length;++i) {
        const char c=body[i];
        if (c==0) return false;
        if (quoted && c=='\\') {
            if (i+5<length && body[i+1]=='u' && memcmp(body+i+2,"0000",4)==0) return false;
            if (++i>=length) return false;
            continue;
        }
        if (c=='"') { quoted=!quoted; continue; }
        if (quoted) {
            if ((unsigned char)c<0x20) return false;
        } else if (c=='{' || c=='[') {
            if (++depth>1) return false;
        } else if (c=='}' || c==']') {
            if (!depth) return false;
            --depth;
        }
    }
    return !quoted && depth==0;
}
bool wifi_request_parse(const char *body,size_t length,wifi_credentials_t *out)
{
    if (!body || !out || !length || length>WIFI_API_MAX_BODY || !flat_without_nul(body,length)) return false;
    char buffer[WIFI_API_MAX_BODY+1];
    memcpy(buffer,body,length); buffer[length]=0;
    cJSON *root=cJSON_ParseWithLengthOpts(buffer,length+1,NULL,true);
    if (!root) return false;
    bool valid=cJSON_IsObject(root);
    unsigned count=0,seen=0;
    const cJSON *item;
    cJSON_ArrayForEach(item,root) {
        ++count;
        if (!cJSON_IsString(item) || !item->string) { valid=false; break; }
        const unsigned field=strcmp(item->string,"ssid")==0?1:strcmp(item->string,"password")==0?2:0;
        if (!field || (seen&field)) { valid=false; break; }
        seen|=field;
    }
    if (valid && count==2 && seen==3) {
        valid=wifi_credentials_init(out,cJSON_GetObjectItemCaseSensitive(root,"ssid")->valuestring,
            cJSON_GetObjectItemCaseSensitive(root,"password")->valuestring)==ESP_OK;
    } else valid=false;
    cJSON_Delete(root);
    return valid;
}
