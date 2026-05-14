idf_component_register(
    SRCS
        "app_main.c"
        "wifi_manager.c"
        "mqtt_aws.c"
        "app_config.c"
        "time_sync.c"
        "led_control.c"
    INCLUDE_DIRS "."
    REQUIRES driver json
    EMBED_TXTFILES
        "certs/AmazonRootCA1.pem"
        "certs/device_cert.pem"
        "certs/private_key.pem"
)
