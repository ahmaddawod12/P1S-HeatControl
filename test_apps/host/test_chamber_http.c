#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/web_server/web_server.c"
#include "firmware_api_policy.h"
#include "printer_api.h"
#include "wifi_api.h"
#include "lwip/sockets.h"

/* Only hardware/HTTP transport and the service boundary are replaced. The
 * handlers, JSON parser, broadcaster and Origin/CSRF implementation are real. */
static const char *input, *host, *origin, *csrf;
static const char *local_ip = "192.168.1.42";
static const char *content_type = "application/json";
static size_t input_offset;
static char response_status[64], response_body[1024], emitted[API_SSE_FRAME_BYTES];
static app_status_t fixture;
static unsigned commands, receives, broadcasts, ticks;
static unsigned printer_stops;
static uint64_t monotonic_ms;
static esp_err_t command_result;
static enum { NORMAL, TIMEOUT, TRICKLE, DISCONNECTED, SLOW_FINAL } receive_mode;
static bool connection_closed;
static uint64_t socket_timeout_ms = 5000;
static httpd_uri_t registered[12];
static size_t registered_count;
static jmp_buf stalled;

int64_t esp_timer_get_time(void) { return (int64_t)monotonic_ms * 1000; }
void esp_fill_random(void *out, size_t size) { memset(out, 0xAB, size); }
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key) {
    (void)req;
    const char *value = !strcmp(key, "Host") ? host : !strcmp(key, "Origin") ? origin :
        !strcmp(key, "X-P1S-CSRF") ? csrf : content_type;
    return value ? strlen(value) : 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *out, size_t cap) {
    const char *value = !strcmp(key, "Host") ? host : !strcmp(key, "Origin") ? origin :
        !strcmp(key, "X-P1S-CSRF") ? csrf : content_type;
    if (!httpd_req_get_hdr_value_len(req,key) || strlen(value) >= cap) return ESP_FAIL;
    strcpy(out,value); return ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 7; }
int getsockopt(int fd, int level, int option, void *value, socklen_t *size) {
    assert(fd==7 && level==SOL_SOCKET && option==SO_RCVTIMEO && *size==sizeof(struct timeval));
    struct timeval *timeout=value;
    timeout->tv_sec=(long)(socket_timeout_ms/1000);
    timeout->tv_usec=(long)(socket_timeout_ms%1000)*1000;
    return 0;
}
int setsockopt(int fd, int level, int option, const void *value, socklen_t size) {
    assert(fd==7 && level==SOL_SOCKET && option==SO_RCVTIMEO && size==sizeof(struct timeval));
    const struct timeval *timeout=value;
    socket_timeout_ms=(uint64_t)timeout->tv_sec*1000+(uint64_t)timeout->tv_usec/1000;
    assert(socket_timeout_ms>0);
    return 0;
}
int getsockname(int fd, struct sockaddr *address, socklen_t *size) {
    assert(fd == 7 && *size >= sizeof(struct sockaddr_in));
    struct sockaddr_in *v4=(struct sockaddr_in *)address;
    v4->sin_family=AF_INET; v4->sin_port=80; return 0;
}
const char *inet_ntop(int family,const void *address,char *out,socklen_t size) {
    (void)address; assert(family==AF_INET && strlen(local_ip)<size); strcpy(out,local_ip); return out;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route) {
    assert(server && route);
    for(size_t i=0;i<registered_count;++i) {
        if(registered[i].method==route->method && !strcmp(registered[i].uri,route->uri)) {
            registered[i]=*route;return ESP_OK;
        }
    }
    assert(registered_count<sizeof(registered)/sizeof(registered[0]));
    registered[registered_count++]=*route;return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *req,const char *status) {
    (void)req; strcpy(response_status,status); return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) { (void)req;(void)type;return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value) {
    (void)req;if(!strcmp(key,"Connection") && !strcmp(value,"close"))connection_closed=true;
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *body) {
    (void)req; assert(strlen(body)<sizeof(response_body)); strcpy(response_body,body);return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *req,const char *body,int length) {
    assert(!body && !length);return httpd_resp_sendstr(req,"");
}
int httpd_req_recv(httpd_req_t *req,char *out,size_t size) {
    (void)req;
    if (++receives > 200) longjmp(stalled,1);
    if (receive_mode==DISCONNECTED) return 0;
    uint64_t delay=receive_mode==TIMEOUT?socket_timeout_ms:
        receive_mode==TRICKLE?2000:receive_mode==SLOW_FINAL?(input_offset?5000:4900):0;
    if(delay>=socket_timeout_ms){monotonic_ms+=socket_timeout_ms;return HTTPD_SOCK_ERR_TIMEOUT;}
    monotonic_ms+=delay;
    if(receive_mode==TRICKLE || (receive_mode==SLOW_FINAL && !input_offset))size=1;
    memcpy(out,input+input_offset,size); input_offset+=size;return (int)size;
}
static void status(app_status_t *out) {*out=fixture;}
static esp_err_t target(float value) {assert(value==60);++commands;return command_result;}
static esp_err_t calibration(float value) {assert(value==2.5f);++commands;return command_result;}
static esp_err_t enabled(bool value) {(void)value;++commands;return command_result;}
static esp_err_t fan(bool value) {(void)value;++commands;return command_result;}
static esp_err_t ack(void) {++commands;return command_result;}
static esp_err_t update(void) {++commands;return command_result;}
static esp_err_t wifi_change(const wifi_credentials_t *value) {
    assert(!strcmp(value->ssid,"network") && !strcmp(value->password,"password"));
    ++commands;return command_result;
}
static esp_err_t printer_config(const printer_config_request_t *value) {
    assert(value->value.enabled && !strcmp(value->value.serial,"P1S123"));
    ++commands;return command_result;
}
static esp_err_t printer_arm(const printer_arm_request_t *value) {
    assert(value->initial_pause_confirmed);++commands;return command_result;
}
static esp_err_t printer_stop(void) {++printer_stops;return command_result;}
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
        .set_manual_fan=fan,.ack_fault=ack,.set_calibration=calibration,
        .request_firmware_update=update,.request_wifi_change=wifi_change,
        .request_printer_config=printer_config,.arm_printer_cycle=printer_arm,
        .forget_printer_config=ack,.stop_printer_cycle=printer_stop};
    assert(firmware_api_register((void *)1,&s_ops)==ESP_OK);
    assert(wifi_api_register((void *)1,&s_ops)==ESP_OK);
    assert(printer_api_register((void *)1,&s_ops)==ESP_OK);
    host="192.168.1.42";origin="http://192.168.1.42";local_ip="192.168.1.42";
    csrf=firmware_api_csrf_token();commands=receives=0;monotonic_ms=0;input_offset=0;
    receive_mode=NORMAL;command_result=ESP_OK;response_status[0]=response_body[0]=0;
    connection_closed=false;socket_timeout_ms=5000;content_type="application/json";
    printer_stops=0;
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
static void test_calibration(void) {
    const char *valid[]={"{\"calibration_offset_c\":0}","{\"calibration_offset_c\":-10}",
        "{\"calibration_offset_c\":10}","{\"calibration_offset_c\":-1.25}"};
    const float expected[]={0,-10,10,-1.25f};
    float value=17;
    for(size_t i=0;i<sizeof(valid)/sizeof(valid[0]);++i) {
        assert(api_parse_calibration(valid[i],&value)==ESP_OK && value==expected[i]);
    }
    const char *bad[]={"{}","[]","null","{\"calibration_offset_c\":\"2\"}",
        "{\"calibration_offset_c\":null}","{\"calibration_offset_c\":10.01}",
        "{\"calibration_offset_c\":-10.01}","{\"calibration_offset_c\":1e999}",
        "{\"calibration_offset_c\":2,\"calibration_offset_c\":3}",
        "{\"calibration_offset_c\":2,\"unknown\":0}","{\"calibration_offset_c\":[1]}",
        "{\"calibration_offset_c\":{\"nested\":1}}","{\"calibration_offset_c\":1}tail",
        "{\"calibration_offset_c\\u0000\":1}","{\"target_c\":1}"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        value=17;assert(api_parse_calibration(bad[i],&value)==ESP_ERR_INVALID_ARG && value==17);
        reset();input=bad[i];httpd_req_t req={.content_len=strlen(input)};
        calibration_handler(&req);assert(!strncmp(response_status,"400",3) && commands==0);
    }
    assert(api_parse_calibration(NULL,&value)==ESP_ERR_INVALID_ARG);
    assert(api_parse_calibration(valid[0],NULL)==ESP_ERR_INVALID_ARG);
    char oversized[API_MAX_BODY_BYTES+2U];memset(oversized,' ',sizeof(oversized));
    oversized[sizeof(oversized)-1U]=0;
    assert(api_parse_calibration(oversized,&value)==ESP_ERR_INVALID_SIZE);
    reset();httpd_req_t req={.content_len=sizeof(oversized)-1U};
    calibration_handler(&req);assert(!strncmp(response_status,"400",3) && commands==0 && receives==0);
    reset();input="{\"calibration_offset_c\":2.5}";req.content_len=strlen(input);
    command_result=ESP_FAIL;calibration_handler(&req);
    assert(!strncmp(response_status,"500",3) && commands==1);
    reset();input="{\"calibration_offset_c\":2.5}";req.content_len=strlen(input);
    command_result=ESP_ERR_NOT_ALLOWED;calibration_handler(&req);
    assert(!strncmp(response_status,"409",3) && commands==1);

    reset();req.content_len=0;status_handler(&req);
    assert(strstr(response_body,"\"calibration_offset_c\":0"));
    fixture.calibration_offset_c=-1.25f;status_handler(&req);
    assert(strstr(response_body,"\"calibration_offset_c\":-1.25"));
    fixture.state=HEAT_STATE_HOLDING;fixture.fault=HEAT_FAULT_OUTPUT_INIT;
    fixture.temperature.fault=HEAT_FAULT_SENSOR_INVALID;
    fixture.temperature.raw_c=125.125f;fixture.temperature.filtered_c=-54.875f;
    fixture.temperature.consecutive_valid=255;fixture.calibration_offset_c=-0.1f;
    fixture.uptime_ms=UINT64_MAX;fixture.wifi_rssi=-128;
    strcpy(fixture.ip_addr,"255.255.255.255");
    char frame[API_SSE_FRAME_BYTES];size_t length=0;
    assert(api_status_event(&fixture,frame,sizeof(frame),&length)==ESP_OK);
    assert(length<API_STATUS_JSON_BYTES && strstr(frame,"\"calibration_offset_c\":"));
}
static void test_origin(void) {
    handler_t routes[]={target_handler,enable_handler,disable_handler,manual_fan_enable_handler,
        manual_fan_disable_handler,acknowledge_handler,calibration_handler};
    for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        const bool body_allowed=i==0 || i==6;
        for(unsigned bad=0;bad<5;++bad){
            reset();fixture.state=HEAT_STATE_FAULT;input="{\"target_c\":60}";
            httpd_req_t req={.content_len=body_allowed?strlen(input):0};
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
            input=i==6?"{\"calibration_offset_c\":2.5}":"{\"target_c\":60}";
            httpd_req_t req={.content_len=body_allowed?strlen(input):0};
            routes[i](&req);assert(!strncmp(response_status,"204",3) && commands==1);
        }
        if(!body_allowed) {
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
    handler_t routes[]={target_handler,calibration_handler};
    for(size_t i=0;i<2;++i) {
        reset();input=i?"{\"calibration_offset_c\":2.5}":"{\"target_c\":60}";
        receive_mode=trickle?TRICKLE:TIMEOUT;httpd_req_t req={.content_len=strlen(input)};
        assert(setjmp(stalled)==0);const esp_err_t result=routes[i](&req);
        assert(!strncmp(response_status,"408",3) && commands==0 && receives<=4);
        assert(result!=ESP_OK); /* IDF otherwise drains the trickling body after 408. */
        reset();req.content_len=strlen(input);routes[i](&req);
        assert(!strncmp(response_status,"204",3) && commands==1);
    }
}
static esp_err_t (*registered_handler(const char *uri,httpd_method_t method))(httpd_req_t *) {
    for(size_t i=0;i<registered_count;++i)
        if(registered[i].method==method && !strcmp(registered[i].uri,uri))return registered[i].handler;
    assert(!"route was not registered");return NULL;
}
static const struct {
    const char *uri;
    httpd_method_t method;
    const char *body;
    const char *success;
} service_mutations[]={
    {"/api/firmware/update",HTTP_POST,"{}","202"},
    {"/api/wifi",HTTP_PUT,"{\"ssid\":\"network\",\"password\":\"password\"}","202"},
    {"/api/printer/config",HTTP_PUT,"{\"enabled\":true,\"ipv4\":\"192.168.1.12\",\"serial\":\"P1S123\",\"access_code\":\"password\"}","202"},
    {"/api/printer/config",HTTP_DELETE,"{}","204"},
    {"/api/printer/cycle",HTTP_POST,"{\"arm_token\":\"0123456789abcdef0123456789abcdef\",\"initial_pause_confirmed\":true}","202"},
    {"/api/printer/cycle/stop",HTTP_POST,"{}","204"},
};
static void test_service_rejection(void) {
    for(size_t i=0;i<sizeof(service_mutations)/sizeof(service_mutations[0]);++i) {
        handler_t handler=registered_handler(service_mutations[i].uri,service_mutations[i].method);
        for(unsigned bad=0;bad<4;++bad) {
            reset();input="{}";httpd_req_t req={.method=service_mutations[i].method,.content_len=1000000};
            if(bad==0)csrf=NULL;
            if(bad==1)origin="http://evil.example";
            if(bad==3){req.content_len=2;content_type="text/plain";}
            esp_err_t result=handler(&req);
            assert(!strncmp(response_status,bad<2?"403":"400",3));
            assert(result!=ESP_OK && connection_closed && receives==0 && commands==0 && printer_stops==0);
        }
    }
}
static void test_service_body(unsigned mode) {
    for(size_t i=0;i<sizeof(service_mutations)/sizeof(service_mutations[0]);++i) {
        reset();handler_t handler=registered_handler(service_mutations[i].uri,service_mutations[i].method);
        char padded[1025];snprintf(padded,sizeof(padded),"%s     ",service_mutations[i].body);input=padded;
        httpd_req_t req={.method=service_mutations[i].method,.content_len=strlen(input)};
        receive_mode=mode;assert(setjmp(stalled)==0);
        esp_err_t result=handler(&req);
        assert(!strncmp(response_status,mode==DISCONNECTED?"400":"408",3));
        assert(result!=ESP_OK && connection_closed && commands==0 && printer_stops==0 && monotonic_ms<=5000);
        assert(socket_timeout_ms==5000);
        reset();input=service_mutations[i].body;req.content_len=strlen(input);
        handler(&req);assert(!strncmp(response_status,service_mutations[i].success,3) && commands+printer_stops==1);
    }
}
static void test_service_nul(void) {
    for(size_t i=0;i<sizeof(service_mutations)/sizeof(service_mutations[0]);++i) {
        reset();handler_t handler=registered_handler(service_mutations[i].uri,service_mutations[i].method);
        const char embedded[]="{}\0junk";input=embedded;
        httpd_req_t req={.method=service_mutations[i].method,.content_len=sizeof(embedded)-1};
        handler(&req);assert(!strncmp(response_status,"400",3) && commands==0);
    }
}
static void test_get_body(void) {
    handler_t handlers[]={status_handler,diagnostics_handler,index_handler,styles_handler,
        script_handler,wifi_script_handler,printer_script_handler,events_handler,
        registered_handler("/api/firmware",HTTP_GET),registered_handler("/api/wifi",HTTP_GET),
        registered_handler("/api/printer",HTTP_GET)};
    for(size_t i=0;i<sizeof(handlers)/sizeof(handlers[0]);++i) {
        reset();csrf=NULL;httpd_req_t req={.method=HTTP_GET,.content_len=1000000};
        esp_err_t result=handlers[i](&req);
        assert(!strncmp(response_status,"400",3) && result!=ESP_OK && connection_closed && receives==0);
    }
}
static void test_chamber_final_chunk(void) {
    handler_t handlers[]={target_handler,calibration_handler};
    for(size_t i=0;i<sizeof(handlers)/sizeof(handlers[0]);++i) {
        reset();input=i?"{\"calibration_offset_c\":2.5}":"{\"target_c\":60}";
        httpd_req_t req={.content_len=strlen(input)};receive_mode=SLOW_FINAL;
        assert(handlers[i](&req)!=ESP_OK && !strncmp(response_status,"408",3));
        assert(monotonic_ms<=5000 && commands==0 && connection_closed && socket_timeout_ms==5000);
    }
}
static void test_acknowledgement_refusal(void) {
    reset();fixture.state=HEAT_STATE_FAULT;fixture.fault=HEAT_FAULT_OUTPUT_INIT;
    command_result=ESP_ERR_NOT_ALLOWED;httpd_req_t req={.method=HTTP_POST};
    acknowledge_handler(&req);
    assert(!strncmp(response_status,"409",3) && commands==1);
    assert(strstr(response_body,"acknowledge_refused") && !strstr(response_body,"maintenance"));
    reset();fixture.state=HEAT_STATE_FAULT;command_result=ESP_ERR_TIMEOUT;
    acknowledge_handler(&req);assert(!strncmp(response_status,"503",3));
    reset();fixture.state=HEAT_STATE_FAULT;
    acknowledge_handler(&req);assert(!strncmp(response_status,"204",3));
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
    else if(!strcmp(argv[1],"calibration"))test_calibration();
    else if(!strcmp(argv[1],"origin"))test_origin();
    else if(!strcmp(argv[1],"timeout"))test_body(false);
    else if(!strcmp(argv[1],"deadline"))test_body(true);
    else if(!strcmp(argv[1],"service_rejection"))test_service_rejection();
    else if(!strcmp(argv[1],"service_timeout"))test_service_body(TIMEOUT);
    else if(!strcmp(argv[1],"service_trickle"))test_service_body(TRICKLE);
    else if(!strcmp(argv[1],"service_final_chunk"))test_service_body(SLOW_FINAL);
    else if(!strcmp(argv[1],"service_disconnect"))test_service_body(DISCONNECTED);
    else if(!strcmp(argv[1],"service_nul"))test_service_nul();
    else if(!strcmp(argv[1],"get_body"))test_get_body();
    else if(!strcmp(argv[1],"final_chunk"))test_chamber_final_chunk();
    else if(!strcmp(argv[1],"acknowledgement"))test_acknowledgement_refusal();
    else if(!strcmp(argv[1],"sse"))test_sse();else assert(!"unknown scenario");
    printf("chamber HTTP %s passed\n",argv[1]);return 0;
}
