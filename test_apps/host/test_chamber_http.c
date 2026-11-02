#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/web_server/web_server.c"
#include "firmware_api_policy.h"
#include "lwip/sockets.h"

/* Only hardware/HTTP transport and the service boundary are replaced. The
 * handlers, JSON parser, broadcaster and Origin/CSRF implementation are real. */
static const char *input, *host, *origin, *csrf;
static const char *local_ip = "192.168.1.42";
static size_t input_offset;
static char response_status[64], response_body[1024], emitted[API_SSE_FRAME_BYTES];
static app_status_t fixture;
static unsigned commands, receives, broadcasts, ticks;
static uint64_t monotonic_ms;
static esp_err_t command_result;
static enum { NORMAL, TIMEOUT, TRICKLE } receive_mode;
static jmp_buf stalled;

int64_t esp_timer_get_time(void) { return (int64_t)monotonic_ms * 1000; }
void esp_fill_random(void *out, size_t size) { memset(out, 0xAB, size); }
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key) {
    (void)req;
    const char *value = !strcmp(key, "Host") ? host : !strcmp(key, "Origin") ? origin :
        !strcmp(key, "X-P1S-CSRF") ? csrf : "application/json";
    return value ? strlen(value) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *out, size_t cap) {
    const char *value = !strcmp(key, "Host") ? host : !strcmp(key, "Origin") ? origin :
        !strcmp(key, "X-P1S-CSRF") ? csrf : "application/json";
    if (!httpd_req_get_hdr_value_len(req,key) || strlen(value) >= cap) return ESP_FAIL;
    strcpy(out,value); return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 7; }
int getsockname(int fd, struct sockaddr *address, socklen_t *size) {
    assert(fd == 7 && *size >= sizeof(struct sockaddr_in));
    struct sockaddr_in *v4=(struct sockaddr_in *)address;
    v4->sin_family=AF_INET; v4->sin_port=80; return 0;
}
const char *inet_ntop(int family,const void *address,char *out,socklen_t size) {
    (void)address; assert(family==AF_INET && strlen(local_ip)<size); strcpy(out,local_ip); return out;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route) {
    assert(server && route); return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status) {
    (void)req; strcpy(response_status,status); return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) { (void)req;(void)type;return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value) {
    (void)req;(void)key;(void)value;return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body) {
    (void)req; assert(strlen(body)<sizeof(response_body)); strcpy(response_body,body);return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,int length) {
    assert(!body && !length);return httpd_resp_sendstr(req,"");
}
int httpd_req_recv(httpd_req_t *req,char *out,size_t size) {
    (void)req;
    if (++receives > 20) longjmp(stalled,1);
    if (receive_mode==TIMEOUT) {monotonic_ms+=5000;return HTTPD_SOCK_ERR_TIMEOUT;}
    if (receive_mode==TRICKLE) {monotonic_ms+=2000;size=1;}
    memcpy(out,input+input_offset,size); input_offset+=size;return (int)size;
}
static void status(app_status_t *out) {*out=fixture;}
static esp_err_t target(float value) {assert(value==60);++commands;return command_result;}
static esp_err_t enabled(bool value) {(void)value;++commands;return command_result;}
static esp_err_t fan(bool value) {(void)value;++commands;return command_result;}
static esp_err_t ack(void) {++commands;return command_result;}
service_operation_t service_operation_current(void) {return SERVICE_OPERATION_NONE;}
size_t sse_hub_client_count(void) {return 1;}
void sse_hub_broadcast(const char *frame,size_t length) {
    assert(length<sizeof(emitted));memcpy(emitted,frame,length);emitted[length]=0;++broadcasts;
}
void vTaskDelay(TickType_t delay) {
    (void)delay;++ticks;
    fixture.uptime_ms=ticks==1?123456:ticks==2?654321:789012;
    if(ticks==3){fixture.temperature.filtered_c=43;s_broadcasting=false;}
}
void vTaskDelete(TaskHandle_t task) {(void)task;}

static void reset(void) {
    fixture=app_status_initial();fixture.state=HEAT_STATE_IDLE;fixture.target_c=60;
    fixture.temperature.valid=true;fixture.temperature.consecutive_valid=3;
    fixture.temperature.raw_c=42;fixture.temperature.filtered_c=42;
    s_ops=(web_server_ops_t){.get_status=status,.set_target=target,.set_enabled=enabled,
        .set_manual_fan=fan,.ack_fault=ack};
    host="192.168.1.42";origin="http://192.168.1.42";local_ip="192.168.1.42";
    csrf=firmware_api_csrf_token();commands=receives=0;monotonic_ms=0;input_offset=0;
    receive_mode=NORMAL;command_result=ESP_OK;response_status[0]=response_body[0]=0;
}
static void test_target_parser(void) {
    char nested[257];size_t used=(size_t)sprintf(nested,"{\"target_c\":60,\"x\":");
    memset(nested+used,'[',110);used+=110;nested[used++]='0';
    memset(nested+used,']',110);used+=110;nested[used++]='}';nested[used]=0;
    float value=17;
    assert(api_parse_target(nested,&value)==ESP_ERR_INVALID_ARG && value==17);
    const char *bad[]={"{\"target_c\":60,\"target_c\":20}","{\"target_c\":60}tail",
        "{\"target_c\":60,\"x\":1}","{\"target_c\":60,\"x\":{}}","{\"target_c\":\"hot\"}",
        "{\"target_c\":60.2}","[]","{\"target_c\\u0000\":60}","{\"target_c\":60"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        value=17;assert(api_parse_target(bad[i],&value)==ESP_ERR_INVALID_ARG && value==17);
    }
    assert(api_parse_target(" \n{\"target_c\":60.0}\t",&value)==ESP_OK && value==60);
}
typedef esp_err_t (*handler_t)(httpd_req_t *);
static void test_origin(void) {
    handler_t routes[]={target_handler,enable_handler,disable_handler,manual_fan_enable_handler,
        manual_fan_disable_handler,acknowledge_handler};
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        for(unsigned bad=0;bad<5;++bad){
            reset();fixture.state=HEAT_STATE_FAULT;input="{\"target_c\":60}";
            httpd_req_t req={.content_len=i==0?strlen(input):0};
            if(bad==0)origin=NULL;
            if(bad==1)csrf=NULL;
            if(bad==2)origin="http://evil.example";
            if(bad==3)csrf="00000000000000000000000000000000";
            if(bad==4)host="evil.example";
            const esp_err_t result=routes[i](&req);
            assert(!strncmp(response_status,"403",3) && commands==0 && receives==0);
            assert(result!=ESP_OK); /* Do not let IDF purge an unread request body. */
        }
        for(unsigned ap=0;ap<2;++ap){
            reset();if(i==5)fixture.state=HEAT_STATE_FAULT;
            if(ap){host=local_ip="192.168.4.1";origin="http://192.168.4.1";}
            input="{\"target_c\":60}";httpd_req_t req={.content_len=i==0?strlen(input):0};
            routes[i](&req);assert(!strncmp(response_status,"204",3) && commands==1);
        }
        if(i>0) {
            reset();if(i==5)fixture.state=HEAT_STATE_FAULT;
            httpd_req_t req={.content_len=1};receive_mode=TIMEOUT;
            const esp_err_t result=routes[i](&req);
            assert(!strncmp(response_status,"400",3) && commands==0 && receives==0);
            assert(result!=ESP_OK);
        }
    }
    reset();command_result=ESP_ERR_INVALID_STATE;httpd_req_t req={0};disable_handler(&req);
    assert(!strncmp(response_status,"503",3));
}
static void test_body(bool trickle) {
    reset();input="{\"target_c\":60}";receive_mode=trickle?TRICKLE:TIMEOUT;
    httpd_req_t req={.content_len=strlen(input)};
    assert(setjmp(stalled)==0);const esp_err_t result=target_handler(&req);
    assert(!strncmp(response_status,"408",3) && commands==0 && receives<=4);
    assert(result!=ESP_OK); /* IDF otherwise drains the trickling body after 408. */
    reset();req.content_len=strlen(input);target_handler(&req);
    assert(!strncmp(response_status,"204",3) && commands==1);
}
static void test_sse(void) {
    reset();broadcasts=ticks=0;s_broadcasting=true;broadcast_task(NULL);
    assert(broadcasts==2);assert(strstr(emitted,"\"uptime_ms\":789012"));
}
int main(int argc,char **argv) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    assert(argc==2);web_server_ops_t ops={0};assert(firmware_api_register((void *)1,&ops)==ESP_OK);
    reset();
    if(!strcmp(argv[1],"json"))test_target_parser();
    else if(!strcmp(argv[1],"origin"))test_origin();
    else if(!strcmp(argv[1],"timeout"))test_body(false);
    else if(!strcmp(argv[1],"deadline"))test_body(true);
    else if(!strcmp(argv[1],"sse"))test_sse();else assert(!"unknown scenario");
    printf("chamber HTTP %s passed\n",argv[1]);return 0;
}
