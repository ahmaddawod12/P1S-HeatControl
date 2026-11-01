#include "wifi_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char trace[256]; unsigned used, ap_calls, connects;
    esp_err_t save_result, ap_result, connect_result;
    wifi_credentials_t saved, requested;
} fixture_t;
static void record(fixture_t *f,char action) { assert(f->used+1<sizeof(f->trace)); f->trace[f->used++]=action; f->trace[f->used]=0; }
static esp_err_t save(const wifi_credentials_t *c,void *ctx)
{ fixture_t *f=ctx; record(f,'S'); if(f->save_result==ESP_OK)f->saved=*c; return f->save_result; }
static esp_err_t ap(bool enabled,void *ctx)
{ fixture_t *f=ctx; ++f->ap_calls; record(f,enabled?'A':'a'); return f->ap_result; }
static esp_err_t connect(const wifi_credentials_t *c,void *ctx)
{ fixture_t *f=ctx; ++f->connects; record(f,'C'); f->requested=*c; return f->connect_result; }
static wifi_engine_ops_t ops(fixture_t *f) { return (wifi_engine_ops_t){.save=save,.set_ap=ap,.connect=connect,.ctx=f}; }
static const wifi_credentials_t old={"old network", "old-password"};

static void no_credentials_and_storage_error(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_engine_init(&e,NULL,ESP_OK,0);
    wifi_engine_tick(&e,0,true,&o);
    assert(e.status.ap_active && !e.status.configured);
    assert(e.status.result==WIFI_RESULT_SETUP_REQUIRED && f.connects==0);
    wifi_engine_tick(&e,100000,true,&o); assert(f.ap_calls==1);
    wifi_engine_init(&e,NULL,ESP_FAIL,0);
    wifi_engine_tick(&e,0,true,&o);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_STORAGE_FAILED);
    assert(wifi_engine_apply(&e,&old,0,&o)!=ESP_OK && f.connects==0);
}
static void repeated_failures_do_not_postpone_fallback(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_engine_init(&e,&old,ESP_OK,0); wifi_engine_tick(&e,0,true,&o);
    assert(f.connects==1 && !e.status.ap_active);
    for(uint64_t t=0;t<29000;t+=2000) {
        wifi_engine_disconnected(&e,WIFI_RESULT_NO_NETWORK,t);
        wifi_engine_tick(&e,t,true,&o);
    }
    wifi_engine_tick(&e,29999,true,&o); assert(!e.status.ap_active);
    wifi_engine_tick(&e,30000,true,&o); assert(e.status.ap_active);
}
static void grace_is_cancelled_and_deferred(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_engine_init(&e,NULL,ESP_OK,0); wifi_engine_tick(&e,0,true,&o);
    assert(wifi_engine_apply(&e,&old,0,&o)==ESP_OK);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50,1000);
    wifi_engine_tick(&e,10999,true,&o); assert(e.status.ap_active);
    wifi_engine_tick(&e,11000,false,&o); assert(e.status.ap_active);
    wifi_engine_tick(&e,11001,true,&o); assert(!e.status.ap_active);
    assert(wifi_engine_apply(&e,&old,12000,&o)==ESP_OK);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50,13000);
    wifi_engine_disconnected(&e,WIFI_RESULT_NO_NETWORK,22000);
    wifi_engine_tick(&e,23000,true,&o); assert(e.status.ap_active);
    wifi_engine_tick(&e,60000,true,&o); assert(e.status.ap_active);
}
static void exact_replacement_and_stale_ip(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t next={" new network ", " password\\with spaces "};
    wifi_engine_init(&e,&old,ESP_OK,0);
    assert(wifi_engine_apply(&e,&next,250,&o)==ESP_OK);
    assert(strcmp(f.trace,"ASC")==0);
    assert(strcmp(f.saved.ssid,next.ssid)==0 && strcmp(f.saved.password,next.password)==0);
    assert(strcmp(e.status.ssid,next.ssid)==0 && strcmp(e.status.password,next.password)==0);
    assert(e.status.ap_active && !e.status.connected);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50,500);
    assert(!e.status.connected);
    wifi_engine_connected(&e,next.ssid,"10.0.0.23",-44,600);
    assert(e.status.connected && strcmp(e.status.ip_addr,"10.0.0.23")==0);
    next=(wifi_credentials_t){"open network", ""};
    assert(wifi_engine_apply(&e,&next,700,&o)==ESP_OK);
    assert(f.saved.password[0]==0);
}
static void failed_ap_or_commit_keeps_old_configuration(void)
{
    fixture_t f={.saved={"old network","old-password"},.ap_result=ESP_FAIL};
    wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t next={"new", "new-password"};
    wifi_engine_init(&e,&old,ESP_OK,0);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50,0);
    assert(wifi_engine_apply(&e,&next,0,&o)!=ESP_OK);
    assert(strcmp(f.trace,"A")==0 && e.status.connected);
    assert(strcmp(e.status.ssid,old.ssid)==0 && e.status.result==WIFI_RESULT_AP_FAILED);
    f=(fixture_t){.saved={"old network","old-password"},.save_result=ESP_FAIL};
    assert(wifi_engine_apply(&e,&next,1,&o)==ESP_FAIL);
    assert(strcmp(f.trace,"AS")==0 && e.status.ap_active && e.status.connected);
    assert(strcmp(f.saved.ssid,old.ssid)==0 && e.status.result==WIFI_RESULT_SAVE_FAILED);
}
static void wrong_network_and_driver_error_remain_recoverable(void)
{
    fixture_t f={.connect_result=ESP_FAIL}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t wrong={"wrong network", "wrong-password"};
    wifi_engine_init(&e,&old,ESP_OK,0);
    assert(wifi_engine_apply(&e,&wrong,0,&o)==ESP_FAIL);
    assert(e.status.ap_active && !e.status.connected && strcmp(f.saved.ssid,wrong.ssid)==0);
    assert(e.status.result==WIFI_RESULT_DRIVER_FAILED);
    f.connect_result=ESP_OK; wifi_engine_tick(&e,1000,true,&o);
    wifi_engine_disconnected(&e,WIFI_RESULT_AUTH_FAILED,1100);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_AUTH_FAILED);
    assert(wifi_engine_apply(&e,&old,1200,&o)==ESP_OK);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-40,2000);
    wifi_engine_tick(&e,12000,true,&o);
    assert(e.status.connected && !e.status.ap_active && e.status.result==WIFI_RESULT_CONNECTED);
}
static void ap_failure_is_retried_with_a_delay(void)
{
    fixture_t f={.ap_result=ESP_FAIL}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_engine_init(&e,NULL,ESP_OK,0); wifi_engine_tick(&e,0,true,&o);
    assert(!e.status.ap_active && f.ap_calls==1);
    wifi_engine_tick(&e,999,true,&o); assert(f.ap_calls==1);
    f.ap_result=ESP_OK; wifi_engine_tick(&e,1000,true,&o);
    assert(e.status.ap_active && f.ap_calls==2);
}
static void deferred_close_is_not_failure_and_failed_close_recovers_promptly(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_engine_init(&e,NULL,ESP_OK,0); wifi_engine_tick(&e,0,true,&o);
    assert(wifi_engine_apply(&e,&old,0,&o)==ESP_OK);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50,0);
    f.ap_result=ESP_ERR_NOT_ALLOWED;
    wifi_engine_tick(&e,10000,true,&o);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_CONNECTED);
    /* If restoration across a late disconnect fails, don't leave AP status
     * falsely active or wait another 30 seconds before retrying it. */
    f.ap_result=ESP_FAIL;
    wifi_engine_tick(&e,10001,true,&o);
    assert(!e.status.ap_active && e.status.result==WIFI_RESULT_AP_FAILED);
    wifi_engine_disconnected(&e,WIFI_RESULT_NO_NETWORK,10002);
    f.ap_result=ESP_OK;
    wifi_engine_tick(&e,11001,true,&o);
    assert(e.status.ap_active);
}
int main(void)
{
    no_credentials_and_storage_error(); repeated_failures_do_not_postpone_fallback();
    grace_is_cancelled_and_deferred(); exact_replacement_and_stale_ip();
    failed_ap_or_commit_keeps_old_configuration();
    wrong_network_and_driver_error_remain_recoverable(); ap_failure_is_retried_with_a_delay();
    deferred_close_is_not_failure_and_failed_close_recovers_promptly();
    puts("Wi-Fi recovery engine passed"); return 0;
}
