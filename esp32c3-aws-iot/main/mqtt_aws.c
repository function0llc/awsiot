#include "mqtt_aws.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "certs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_control.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_aws";

static esp_mqtt_client_handle_t s_client;

static void mqtt_publish_led_state(const char *state_topic, bool on)
{
    char payload[64];
    int len = snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", on ? "on" : "off");
    esp_mqtt_client_publish(s_client, state_topic, payload, len, 1, 0);
    ESP_LOGI(TAG, "Published LED state: %s", on ? "on" : "off");
}

static void mqtt_handle_led_command(const char *led_cmd_topic, const char *led_state_topic,
                                    const char *topic, int topic_len, const char *data, int data_len)
{
    if (topic_len == (int)strlen(led_cmd_topic) &&
        strncmp(topic, led_cmd_topic, topic_len) == 0) {
        bool led_on = false;
        bool found = false;
        char *data_str = strndup(data, data_len);
        if (data_str != NULL) {
            if (strstr(data_str, "\"on\"") != NULL || strstr(data_str, "\"state\":\"on\"") != NULL) {
                led_on = true;
                found = true;
            } else if (strstr(data_str, "\"off\"") != NULL || strstr(data_str, "\"state\":\"off\"") != NULL) {
                led_on = false;
                found = true;
            }
            free(data_str);
        }

        if (found) {
            led_control_set(led_on);
            mqtt_publish_led_state(led_state_topic, led_on);
        } else {
            ESP_LOGW(TAG, "Unrecognized LED payload: %.*s", data_len, data);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    const app_config_t *cfg = app_config_get();

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to AWS IoT Core");
        esp_mqtt_client_subscribe(s_client, cfg->sub_topic, 1);
        esp_mqtt_client_subscribe(s_client, cfg->led_cmd_topic, 1);
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
        mqtt_handle_led_command(cfg->led_cmd_topic, cfg->led_state_topic,
                                event->topic, event->topic_len, event->data, event->data_len);
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

    led_control_init();

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
