idf_component_register(
    SRCS "main.c"
    EMBED_TXTFILES
        "certs/AmazonRootCA1.pem"
        "certs/device.cert.pem"
        "certs/private.key.pem"
    REQUIRES esp_event esp_netif esp_wifi mqtt nvs_flash
)
