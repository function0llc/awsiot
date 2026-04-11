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

static const char AWS_ROOT_CA_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
"-----END CERTIFICATE-----\n";

static const char AWS_DEVICE_CERT_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDWTCCAkGgAwIBAgIUaTn9QsHZ/PnSye6mgp2eVJvBj0owDQYJKoZIhvcNAQEL\n"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI2MDQxMTIzMTUx\n"
"MFoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\n"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAOSBLRyD8uBJdFfON7z9\n"
"Q3cDBoMSsW1xPYGkROr4mxHFXQcpYumFxduIYEpzmCKvrRBsS2/pLUrOpXNwZyIm\n"
"baTIUVdPnhbfA0tJ1iBASeq4K2cO20DImZ8/AJws3ZdPYj7pXHQEztp1TlwVHPAY\n"
"1RqzKFzos7r+ikAWnoHxnDT/ESM+eiNGrCwbf1isTSGNovj8QCqIS+XBvhJkGOon\n"
"Zrhbc3lKqETPJrfGZMpbcUbbTfcztKgScz8nEO5xXWP0s1qWEhYJtjwZpe0VzExg\n"
"aogLIz/fGh9CmATjEn9a0fM/bCMbU7pE+XDRIQBg2vk/b1tWekWHU1F7Z1iubfIl\n"
"TeMCAwEAAaNgMF4wHwYDVR0jBBgwFoAUemsZHoRqvvRx3su3RBJAuEFLO9MwHQYD\n"
"VR0OBBYEFFpG4+7PQomOWIcSfmvyuqpRdi2VMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQCMf0wItKHyl93TKk8IYWlBh1qj\n"
"WRpOAN5Rn1r812WRSRy1kK3mYIF+eLKtkWLce0/m9cIRjreRzyjbQKvM9hy6UpZA\n"
"btcbmYby4SOZ7efZvftvPPkEsvSqw1bn00tApMElghZZwhiwx/7lrMvPmFuxGdFL\n"
"DzxCX8MTlXi9wS7xAfAAiyTjfwWUHc1IfZJks761VIOAMjBEZZDuZhDKkSH3GNO/\n"
"lRg1gto83fkXPbLYO3wH9mWP8Arqb7/zH3xw53UdzMHST7lHuBrm0bUG0QXteM5g\n"
"IyvaaLIzBGkcPgK15Kxnem2t03WsZDlMcY8gkssDpyebvBGIt2xE0wISC5Da\n"
"-----END CERTIFICATE-----\n";

static const char AWS_PRIVATE_KEY_PEM[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"MIIEpQIBAAKCAQEA5IEtHIPy4El0V843vP1DdwMGgxKxbXE9gaRE6vibEcVdByli\n"
"6YXF24hgSnOYIq+tEGxLb+ktSs6lc3BnIiZtpMhRV0+eFt8DS0nWIEBJ6rgrZw7b\n"
"QMiZnz8AnCzdl09iPulcdATO2nVOXBUc8BjVGrMoXOizuv6KQBaegfGcNP8RIz56\n"
"I0asLBt/WKxNIY2i+PxAKohL5cG+EmQY6idmuFtzeUqoRM8mt8ZkyltxRttN9zO0\n"
"qBJzPycQ7nFdY/SzWpYSFgm2PBml7RXMTGBqiAsjP98aH0KYBOMSf1rR8z9sIxtT\n"
"ukT5cNEhAGDa+T9vW1Z6RYdTUXtnWK5t8iVN4wIDAQABAoIBAQCfQn0bi8eClQ+W\n"
"hy8H9IjJ8Pzf6+5nte5FZiV2k3EY8LLG5AyBb/AX8zQRkAFv43s+oAvv4tfjdKWS\n"
"ELyA68GtgMRYkzA/Bo44Mi0ga05ZXqU5ffxIacfQWsvlDcwfwn9aE7GRVyyIzAui\n"
"WEDEArq8kjPGlZV29iOLSXiOXsQdlnttJ+xJbvzbo7krZ3EH+M2O0pHrzmzYWGU9\n"
"eGk11aL2xAWVb3dBe1VxmoVRaFfqAQNp7Uu6i+HxS6WbD0xUtwHlMOvVaoCLQY1q\n"
"7BhjhRw9y+1C9mEnIBQ6G4a5SLkxXf9gN6EsUVL4MbXrjSg1GAXvL3lu66X9ltBJ\n"
"BLI02O2pAoGBAP1QPdSLVqeGgyYiw9klWN6j2EhiJ5b/NWQHTgwJLFb5DVmrtmNW\n"
"yNCDZkfG27QClsQACoAMFf4p/6QfsiuYZgs3ciip/NmxhP2Lp7TyJJlr/53nuLOV\n"
"IDw2UHx14GwyjpWbRefcnFA9B8O95q5yivAZhqGzHkVylvE7ECUqBkO/AoGBAObt\n"
"k9N1lercwIXq4y098WzO3v5u6Pj4KX1KjHRLAkO3Wi6qkp+M5luDMVaIpg/DllGI\n"
"i4/i4Wx79gElO0Wc4tbyDnUWCmVMwZGpoBDAoFpcRE/JpQNEwnCkx9vFKVdr2nKi\n"
"LkS33Uq3r7S3sNkOT93qyCW+oUYtsC1TEEevj67dAoGBANk6W5uOViRZlNQ6UetA\n"
"PMLIjOwdyEeT2axLG2H05+I5t3ojZ8gunw048Zgx7yyiX7n7MwX111Jbn8+WIsuB\n"
"6FhMFwjKJXxl4F8xyQLB+CLdW5qFIX5XLwqTpVfmGiuO+3lOa5dXN3ETtTnl+xG0\n"
"RnN54FuAaEAz8T8zYDgOnsqvAoGADyFl3CcKKZdJLf0T4XT7iouLZNRTg2yPG1x1\n"
"E3GOtm0fBYu7i6Spuzk/VFIjv0irYrA1pLnS107P8YdE+OysmDTH6bgF+lkrHaYl\n"
"jLNyTjlZaHjnzO0GaGcDiUzDxqyUZYpmya3aW/jpFEgs2Cvt47ZsYry4yGZZ8r5M\n"
"hMXl/ikCgYEA0T2onkP2graCGzMqBj6HIY1Gql7Bs9gxvS64r0VdWnAIkqWwftlE\n"
"mliHPnCxu9+rGLlqVqbZyrVt9wQ+bmee5lIJmJTtyiVWIqQ/p7swn7cmdsKyiL11\n"
"310t3qs9xp0TrxZ8w6ikDY4UXcx+X0/JvvOaONxsMJmSN/hEm+W72ns=\n"
"-----END RSA PRIVATE KEY-----\n";

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
        .broker.verification.certificate = AWS_ROOT_CA_PEM,
        .credentials.client_id = "basicPubSub",
        .credentials.authentication.certificate = AWS_DEVICE_CERT_PEM,
        .credentials.authentication.key = AWS_PRIVATE_KEY_PEM,
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
