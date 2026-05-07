#include "mqtt_aws.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "certs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_aws";

static esp_mqtt_client_handle_t s_client;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    const app_config_t *cfg = app_config_get();

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to AWS IoT Core");
        esp_mqtt_client_subscribe(s_client, cfg->sub_topic, 1);
        esp_mqtt_client_publish(s_client, cfg->pub_topic, "{\"status\":\"boot\"}", 0, 1, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from AWS IoT Core");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed to topic, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Published message, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Incoming topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len,
                 event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT transport error");
        break;

    default:
        break;
    }
}

static void mqtt_publish_task(void *arg)
{
    const app_config_t *cfg = app_config_get();
    uint32_t counter = 0;
    char payload[128];

    while (true) {
        if (s_client != NULL) {
            int len = snprintf(payload, sizeof(payload),
                               "{\"device\":\"%s\",\"counter\":%lu}",
                               cfg->aws_client_id,
                               (unsigned long)counter++);
            if (len > 0 && len < (int)sizeof(payload)) {
                esp_mqtt_client_publish(s_client, cfg->pub_topic, payload, 0, 1, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(cfg->publish_interval_ms));
    }
}

esp_err_t mqtt_aws_start(void)
{
    const app_config_t *cfg = app_config_get();
    static char broker_uri[256];

    int uri_len = snprintf(broker_uri, sizeof(broker_uri), "mqtts://%s:%d", cfg->aws_endpoint, cfg->mqtt_port);
    if (uri_len <= 0 || uri_len >= (int)sizeof(broker_uri)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .broker.verification.certificate = aws_root_ca_pem_start,
        .credentials.client_id = cfg->aws_client_id,
        .credentials.authentication.certificate = device_cert_pem_start,
        .credentials.authentication.key = private_key_pem_start,
        .session.keepalive = 60,
        .network.disable_auto_reconnect = false,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL), TAG,
                        "Failed to register MQTT event handler");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "Failed to start MQTT client");

    BaseType_t task_ok = xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
