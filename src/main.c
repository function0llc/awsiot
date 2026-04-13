#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "wifi_credentials.h"

#define AWS_IOT_ENDPOINT "a3qhmfu2zenmjt-ats.iot.us-west-2.amazonaws.com"
#define AWS_IOT_PORT 8883
#define AWS_IOT_THING_NAME "esp32-c3_awsiot1"
#define PING_TARGET_HOST AWS_IOT_ENDPOINT

extern const uint8_t certs_AmazonRootCA1_pem_start[] asm("_binary_certs_AmazonRootCA1_pem_start");
extern const uint8_t certs_device_cert_pem_start[] asm("_binary_certs_device_cert_pem_start");
extern const uint8_t certs_private_key_pem_start[] asm("_binary_certs_private_key_pem_start");

static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t wifi_event_group;
static const char *TAG = "aws_iot_demo";

static const char *wifi_disconnect_reason_to_str(wifi_err_reason_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        default: return "OTHER";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d %s), retrying", disconn->reason, wifi_disconnect_reason_to_str(disconn->reason));
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&got_ip->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to AWS IoT Core");
            esp_mqtt_client_subscribe(client, "sdk/test/python", 1);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data:  %.*s", event->data_len, event->data);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            break;
    }
}

static void ping_on_success(esp_ping_handle_t hdl, void *args) {
    uint32_t elapsed_time_ms = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr;

    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time_ms, sizeof(elapsed_time_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    ESP_LOGI(TAG, "ping success: seq=%lu time=%lums target=%s",
             (unsigned long)recv_len,
             (unsigned long)elapsed_time_ms,
             ipaddr_ntoa(&target_addr));
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args) {
    uint32_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout: seq=%lu", (unsigned long)seqno);
}

static void ping_on_end(esp_ping_handle_t hdl, void *args) {
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t total_time_ms = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    ESP_LOGI(TAG, "ping finished: tx=%lu rx=%lu time=%lums",
             (unsigned long)transmitted,
             (unsigned long)received,
             (unsigned long)total_time_ms);
}

static esp_err_t run_ping_test(const char *host) {
    struct addrinfo hints = {
        .ai_family = AF_INET,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s (err=%d)", host, err);
        return ESP_FAIL;
    }

    struct in_addr addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;
    ping_config.target_addr.u_addr.ip4.addr = addr.s_addr;
    ping_config.count = 4;
    ping_config.interval_ms = 1000;
    ping_config.timeout_ms = 1000;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };

    esp_ping_handle_t ping_handle = NULL;
    esp_err_t ret = esp_ping_new_session(&ping_config, &callbacks, &ping_handle);
    if (ret != ESP_OK) {
        freeaddrinfo(res);
        ESP_LOGE(TAG, "esp_ping_new_session failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pinging %s (%s)", host, inet_ntoa(addr));
    esp_ping_start(ping_handle);

    vTaskDelay(pdMS_TO_TICKS((ping_config.count + 1) * ping_config.interval_ms));

    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    freeaddrinfo(res);
    return ESP_OK;
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    ESP_ERROR_CHECK(run_ping_test(PING_TARGET_HOST));

    char uri[256];
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", AWS_IOT_ENDPOINT, AWS_IOT_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .broker.verification.certificate = (const char *)certs_AmazonRootCA1_pem_start,
        .credentials.client_id = "basicPubSub",
        .credentials.authentication.certificate = (const char *)certs_device_cert_pem_start,
        .credentials.authentication.key = (const char *)certs_private_key_pem_start,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    uint32_t counter = 0;
    char payload[128];

    while (1) {
        int len = snprintf(payload, sizeof(payload), "{\"thing\":\"%s\",\"count\":%lu}", AWS_IOT_THING_NAME, (unsigned long)counter++);
        esp_mqtt_client_publish(client, "sdk/test/python", payload, len, 1, 0);
        ESP_LOGI(TAG, "Published: %s", payload);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
