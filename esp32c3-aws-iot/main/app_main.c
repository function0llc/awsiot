#include "app_config.h"
#include "mqtt_aws.h"
#include "time_sync.h"
#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "app_main";

void app_main(void)
{
    const app_config_t *cfg = app_config_get();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Starting WiFi");
    ESP_ERROR_CHECK(wifi_manager_start_sta(cfg->wifi_ssid, cfg->wifi_pass));

    ESP_LOGI(TAG, "Synchronizing time");
    ESP_ERROR_CHECK(time_sync_wait_for_time(30));

    ESP_LOGI(TAG, "Starting MQTT");
    ESP_ERROR_CHECK(mqtt_aws_start());
}
