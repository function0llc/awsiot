#include "time_sync.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "time_sync";

esp_err_t time_sync_wait_for_time(int max_wait_seconds)
{
    if (max_wait_seconds <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();

    for (int i = 0; i < max_wait_seconds; i++) {
        time_t now = 0;
        time(&now);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "Time synchronized: %ld", (long)now);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "Failed to synchronize time within %d seconds", max_wait_seconds);
    return ESP_ERR_TIMEOUT;
}
