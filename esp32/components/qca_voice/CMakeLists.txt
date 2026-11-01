# QCA 实时语音（voice.realtime.v1 over WSS）
idf_component_register(
    SRCS "qca_link.c" "qca_esp.c" "qca_console.c" "qca_session.c"
    INCLUDE_DIRS "."
    REQUIRES muse esp_websocket_client esp_http_client esp_timer esp_netif esp_event esp_wifi console nvs_flash )
