#include "wifi_api_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse(const char *s,wifi_credentials_t *out) { return wifi_request_parse(s,strlen(s),out); }
int main(void)
{
    wifi_credentials_t out;
    assert(parse(" \n{\"ssid\":\" space \",\"password\":\" pass\\\\word\\\" \"}\t",&out));
    assert(strcmp(out.ssid," space ")==0 && strcmp(out.password," pass\\word\" ")==0);
    assert(parse("{\"password\":\"\",\"ssid\":\"open\"}",&out) && out.password[0]==0);
    assert(parse("{\"ssid\":\"\\u00e9\\u00e9\",\"password\":\"12345678\"}",&out));
    assert(strlen(out.ssid)==4);
    assert(parse("{\"ssid\":\"literal\\\\u0000\",\"password\":\"12345678\"}",&out));
    assert(strcmp(out.ssid,"literal\\u0000")==0);
    static const char *invalid[]={
        "{}", "[]", "null", "{\"ssid\":\"x\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\",\"extra\":1}",
        "{\"ssid\":\"x\",\"ssid\":\"y\",\"password\":\"12345678\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\",\"password\":\"abcdefgh\"}",
        "{\"SSID\":\"x\",\"password\":\"12345678\"}",
        "{\"ssid\":1,\"password\":\"12345678\"}",
        "{\"ssid\":\"x\",\"password\":null}",
        "{\"ssid\":\"\",\"password\":\"12345678\"}",
        "{\"ssid\":\"x\",\"password\":\"short\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\"} {}",
        "{\"ssid\":\"x\",\"password\":\"12345678\"}junk",
        "{\"ssid\":\"x\\u0000hidden\",\"password\":\"12345678\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\\u0000hidden\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\",\"ssid\\u0000extra\":\"y\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678\",}"
    };
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        memset(&out,0x55,sizeof(out));
        assert(!parse(invalid[i],&out));
        assert((unsigned char)out.ssid[0]==0x55);
    }
    char body[900], ssid[35], password[66];
    memset(ssid,'s',32); ssid[32]=0;
    memset(password,'p',63); password[63]=0;
    snprintf(body,sizeof(body),"{\"ssid\":\"%s\",\"password\":\"%s\"}",ssid,password);
    assert(parse(body,&out));
    ssid[32]='s'; ssid[33]=0;
    snprintf(body,sizeof(body),"{\"ssid\":\"%s\",\"password\":\"%s\"}",ssid,password);
    assert(!parse(body,&out));
    ssid[32]=0; password[63]='p'; password[64]=0;
    snprintf(body,sizeof(body),"{\"ssid\":\"%s\",\"password\":\"%s\"}",ssid,password);
    assert(!parse(body,&out));
    /* UTF-8 bounds are bytes, independent of the JSON escape representation. */
    memset(body,0,sizeof(body)); strcpy(body,"{\"ssid\":\"");
    for(int i=0;i<16;++i) strcat(body,"\\u00e9");
    strcat(body,"\",\"password\":\"\"}"); assert(parse(body,&out) && strlen(out.ssid)==32);
    char *end=strstr(body,"\",\"password\""); memmove(end+6,end,strlen(end)+1); memcpy(end,"\\u00e9",6);
    assert(!parse(body,&out));
    static const char raw_nul[]="{\"ssid\":\"x\",\"password\":\"12345678\"}\0{}";
    assert(!wifi_request_parse(raw_nul,sizeof(raw_nul)-1,&out));
    memset(body,' ',sizeof(body)); assert(!wifi_request_parse(body,sizeof(body),&out));
    assert(!wifi_request_parse(NULL,0,&out));
    assert(!parse("{\"ssid\":\"x\",\"password\":\"12345678\"}",NULL));
    puts("strict Wi-Fi request parsing passed"); return 0;
}
