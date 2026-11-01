#include "esp_event.h"
#include "esp_netif.h"
#include "unity.h"

void app_main(void)
{
    /* The [api] cases speak real HTTP over 127.0.0.1. That needs the lwIP
     * TCP/IP thread, which esp_netif_init() starts; without it every socket
     * call blocks forever and the case never returns to the menu. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    unity_run_menu();
}
