#include "mqtt_aws.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "certs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_control.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "mqtt_aws";

static esp_mqtt_client_handle_t s_client;

static void mqtt_publish_shadow_update(const char *gpio_state);

static void mqtt_apply_gpio_state(const char *gpio_state)
{
    if (gpio_state == NULL) return;

    if (strcmp(gpio_state, "on") == 0 || strcmp(gpio_state, "HIGH") == 0) {
        led_control_set(true);
        ESP_LOGI(TAG, "GPIO pin set HIGH");
        mqtt_publish_shadow_update("on");
    } else if (strcmp(gpio_state, "off") == 0 || strcmp(gpio_state, "LOW") == 0) {
        led_control_set(false);
        ESP_LOGI(TAG, "GPIO pin set LOW");
        mqtt_publish_shadow_update("off");
    } else {
        ESP_LOGW(TAG, "Ignoring unsupported GPIO state: %s", gpio_state);
    }
}

static void mqtt_publish_shadow_update(const char *gpio_state)
{
    const app_config_t *cfg = app_config_get();
    char payload[128];
    time_t now;
    time(&now);
    int len = snprintf(payload, sizeof(payload),
        "{\"state\":{\"reported\":{\"gpio\":\"%s\"}},\"timestamp\":%ld}",
        gpio_state, (long)now);
    esp_mqtt_client_publish(s_client, cfg->shadow_update_topic, payload, len, 1, 0);
    ESP_LOGI(TAG, "Published shadow update: gpio=%s", gpio_state);
}

static void mqtt_handle_shadow_delta(const char *topic, int topic_len, const char *data, int data_len)
{
    if (topic == NULL || data == NULL) return;
    const app_config_t *cfg = app_config_get();
    int shadow_topic_len = (int)strlen(cfg->shadow_delta_topic);
    if (topic_len != shadow_topic_len) return;
    if (strncmp(topic, cfg->shadow_delta_topic, topic_len) != 0) return;

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) return;

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsObject(state)) {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(state, "delta");
        cJSON *gpio = NULL;
        if (cJSON_IsObject(delta)) {
            gpio = cJSON_GetObjectItemCaseSensitive(delta, "gpio");
        }
        if (!cJSON_IsString(gpio)) {
            cJSON *desired = cJSON_GetObjectItemCaseSensitive(state, "desired");
            if (cJSON_IsObject(desired)) {
                gpio = cJSON_GetObjectItemCaseSensitive(desired, "gpio");
            }
        }
        if (cJSON_IsString(gpio) && gpio->valuestring != NULL) {
            mqtt_apply_gpio_state(gpio->valuestring);
        }
    }
    cJSON_Delete(root);
}

static void mqtt_handle_direct_gpio_command(const char *topic, int topic_len, const char *data, int data_len)
{
    if (topic == NULL || data == NULL) return;

    const app_config_t *cfg = app_config_get();
    int cmd_topic_len = (int)strlen(cfg->sub_topic);
    if (topic_len != cmd_topic_len) return;
    if (strncmp(topic, cfg->sub_topic, topic_len) != 0) return;

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (root == NULL) return;

    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && state->valuestring != NULL) {
        mqtt_apply_gpio_state(state->valuestring);
    }

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    const app_config_t *cfg = app_config_get();

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to AWS IoT Core");
        esp_mqtt_client_subscribe(s_client, cfg->sub_topic, 1);
        ESP_LOGI(TAG, "Subscribed to direct GPIO commands: %s", cfg->sub_topic);
        esp_mqtt_client_subscribe(s_client, cfg->shadow_delta_topic, 1);
        ESP_LOGI(TAG, "Subscribed to shadow delta: %s", cfg->shadow_delta_topic);
        esp_mqtt_client_publish(s_client, cfg->shadow_get_topic, "", 0, 0, 0);
        ESP_LOGI(TAG, "Requested shadow state via %s", cfg->shadow_get_topic);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from AWS IoT Core");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Incoming topic=%.*s data=%.*s", event->topic_len, event->topic, event->data_len,
                 event->data);
        mqtt_handle_direct_gpio_command(event->topic, event->topic_len, event->data, event->data_len);
        mqtt_handle_shadow_delta(event->topic, event->topic_len, event->data, event->data_len);
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
    char payload[256];

    while (true) {
        if (s_client != NULL) {
            time_t now;
            time(&now);
            bool gpio_on = led_control_get();
            const char *gpio_state = gpio_on ? "on" : "off";
            int len = snprintf(payload, sizeof(payload),
                               "{\"thing\":\"esp32-c3_awsiot1\",\"pins\":{\"5\":\"%s\"},\"count\":%lu,\"ts\":%ld,\"fw\":\"1.0.0\"}",
                               gpio_state, (unsigned long)counter++, (long)now);
            if (len > 0 && len < (int)sizeof(payload)) {
                esp_mqtt_client_publish(s_client, cfg->pub_topic, payload, len, 1, 0);
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
