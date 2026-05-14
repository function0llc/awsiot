#pragma once

typedef struct {
    const char *wifi_ssid;
    const char *wifi_pass;
    const char *aws_endpoint;
    const char *aws_client_id;
    const char *pub_topic;
    const char *sub_topic;
    const char *shadow_get_topic;
    const char *shadow_update_topic;
    const char *shadow_delta_topic;
    const char *led_cmd_topic;
    const char *led_state_topic;
    int mqtt_port;
    int publish_interval_ms;
} app_config_t;

const app_config_t *app_config_get(void);
