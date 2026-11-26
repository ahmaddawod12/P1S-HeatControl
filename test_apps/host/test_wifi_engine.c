#include "wifi_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char trace[256]; unsigned used, ap_calls, connects;
    esp_err_t save_result, ap_result, connect_result;
    bool stop_lost_ap;
    wifi_credentials_t saved, requested;
} fixture_t;
static void record(fixture_t *f,char action) { assert(f->used+1<sizeof(f->trace)); f->trace[f->used++]=action; f->trace[f->used]=0; }
static esp_err_t save(const wifi_credentials_t *c,void *ctx)
{ fixture_t *f=ctx; record(f,'S'); if(f->save_result==ESP_OK)f->saved=*c; return f->save_result; }
static esp_err_t ap(bool enabled, bool *active, void *ctx)
{
    fixture_t *f = ctx;
    ++f->ap_calls;
    record(f, enabled ? 'A' : 'a');
    if (f->ap_result == ESP_OK) *active = enabled;
    else if (!enabled && f->stop_lost_ap) *active = false;
    return f->ap_result;
}
static esp_err_t connect(const wifi_credentials_t *c,void *ctx)
{ fixture_t *f=ctx; ++f->connects; record(f,'C'); f->requested=*c; return f->connect_result; }
static wifi_engine_ops_t ops(fixture_t *f) { return (wifi_engine_ops_t){.save=save,.set_ap=ap,.connect=connect,.ctx=f}; }
static const wifi_credentials_t old={.ssid="old network", .password="old-password"};

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
static void three_setup_modes(void)
{
    fixture_t f = {0};
    wifi_engine_t e;
    const wifi_engine_ops_t o = ops(&f);
    wifi_engine_init(&e, &old, ESP_OK, 0);
    wifi_engine_tick(&e, 0, false, &o);
    assert(e.status.ap_active && f.connects == 1);
    assert(wifi_setup_mode(e.status.connected, false) == WIFI_SETUP_NETWORK);

    wifi_engine_connected(&e, old.ssid, "10.0.0.22", -50);
    wifi_engine_tick(&e, 1000, false, &o);
    assert(e.status.ap_active && f.ap_calls == 1);
    assert(wifi_setup_mode(e.status.connected, false) == WIFI_SETUP_PAIRING);

    wifi_engine_tick(&e, 1100, true, &o);
    assert(!e.status.ap_active && f.ap_calls == 2);
    assert(wifi_setup_mode(e.status.connected, true) == WIFI_SETUP_NONE);
    wifi_engine_tick(&e, 200000, true, &o);
    assert(!e.status.ap_active && f.ap_calls == 2);

    wifi_engine_disconnected(&e, WIFI_RESULT_NO_NETWORK, 201000);
    wifi_engine_tick(&e, 201000, true, &o);
    assert(e.status.ap_active && f.ap_calls == 3);
    assert(wifi_setup_mode(e.status.connected, true) == WIFI_SETUP_NETWORK);
    wifi_engine_connected(&e, old.ssid, "10.0.0.22", -50);
    wifi_engine_tick(&e, 202000, true, &o);
    assert(!e.status.ap_active && f.ap_calls == 4);
}
static void exact_replacement_and_stale_ip(void)
{
    fixture_t f={0}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t next={.ssid=" new network ", .password=" password\\with spaces "};
    wifi_engine_init(&e,&old,ESP_OK,0);
    assert(wifi_engine_apply(&e,&next,250,&o)==ESP_OK);
    assert(strcmp(f.trace,"ASC")==0);
    assert(strcmp(f.saved.ssid,next.ssid)==0 && strcmp(f.saved.password,next.password)==0);
    assert(strcmp(e.status.ssid,next.ssid)==0 && strcmp(e.password,next.password)==0);
    assert(e.status.ap_active && !e.status.connected);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50);
    assert(!e.status.connected);
    wifi_engine_connected(&e,next.ssid,"10.0.0.23",-44);
    assert(e.status.connected && strcmp(e.status.ip_addr,"10.0.0.23")==0);
    next=(wifi_credentials_t){.ssid="open network", .password=""};
    assert(wifi_engine_apply(&e,&next,700,&o)==ESP_OK);
    assert(f.saved.password[0]==0);
}
static void failed_ap_or_commit_keeps_old_configuration(void)
{
    fixture_t f={.saved={.ssid="old network", .password="old-password"},.ap_result=ESP_FAIL};
    wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t next={.ssid="new", .password="new-password"};
    wifi_engine_init(&e,&old,ESP_OK,0);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50);
    assert(wifi_engine_apply(&e,&next,0,&o)!=ESP_OK);
    assert(strcmp(f.trace,"A")==0 && e.status.connected);
    assert(strcmp(e.status.ssid,old.ssid)==0 && e.status.result==WIFI_RESULT_AP_FAILED);
    f=(fixture_t){.saved={.ssid="old network", .password="old-password"},.save_result=ESP_FAIL};
    assert(wifi_engine_apply(&e,&next,1,&o)==ESP_FAIL);
    assert(strcmp(f.trace,"AS")==0 && e.status.ap_active && e.status.connected);
    assert(strcmp(f.saved.ssid,old.ssid)==0 && e.status.result==WIFI_RESULT_SAVE_FAILED);
}
static void wrong_network_and_driver_error_remain_recoverable(void)
{
    fixture_t f={.connect_result=ESP_FAIL}; wifi_engine_t e; wifi_engine_ops_t o=ops(&f);
    wifi_credentials_t wrong={.ssid="wrong network", .password="wrong-password"};
    wifi_engine_init(&e,&old,ESP_OK,0);
    assert(wifi_engine_apply(&e,&wrong,0,&o)==ESP_FAIL);
    assert(e.status.ap_active && !e.status.connected && strcmp(f.saved.ssid,wrong.ssid)==0);
    assert(e.status.result==WIFI_RESULT_DRIVER_FAILED);
    f.connect_result=ESP_OK; wifi_engine_tick(&e,1000,true,&o);
    wifi_engine_disconnected(&e,WIFI_RESULT_AUTH_FAILED,1100);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_AUTH_FAILED);
    assert(wifi_engine_apply(&e,&old,1200,&o)==ESP_OK);
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-40);
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
    wifi_engine_connected(&e,old.ssid,"10.0.0.22",-50);
    f.ap_result=ESP_ERR_NOT_ALLOWED;
    wifi_engine_tick(&e,10000,true,&o);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_CONNECTED);
    /* If restoration across a late disconnect fails, don't leave AP status
     * falsely active before retrying it. */
    f.ap_result=ESP_FAIL;
    wifi_engine_tick(&e,10001,true,&o);
    assert(e.status.ap_active && e.status.result==WIFI_RESULT_AP_FAILED);
    f.stop_lost_ap=true;
    wifi_engine_tick(&e,11001,true,&o);
    assert(!e.status.ap_active && e.status.result==WIFI_RESULT_AP_FAILED);
    wifi_engine_disconnected(&e,WIFI_RESULT_NO_NETWORK,11002);
    f.ap_result=ESP_OK;
    wifi_engine_tick(&e,12001,true,&o);
    assert(e.status.ap_active);
}
int main(void)
{
    no_credentials_and_storage_error(); three_setup_modes();
    exact_replacement_and_stale_ip();
    failed_ap_or_commit_keeps_old_configuration();
    wrong_network_and_driver_error_remain_recoverable(); ap_failure_is_retried_with_a_delay();
    deferred_close_is_not_failure_and_failed_close_recovers_promptly();
    puts("Wi-Fi recovery engine passed"); return 0;
}
