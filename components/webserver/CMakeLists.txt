set(DIST "${CMAKE_CURRENT_LIST_DIR}/../../../planthubos/webui/dist-gz")
if(NOT EXISTS "${DIST}/index.html.gz")
    message(FATAL_ERROR "Web assets missing. Run: cd planthubos/webui && npm install && npm run build")
endif()

idf_component_register(
    SRCS "webserver.c" "api_v1.c" "dns_hijack.c" "devices_json.c" "sse.c" "ota_post.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer json wifi_manager app_config esp_wifi esp_event data_core lwip storage timekeeper claiming joltwallet__littlefs app_update swarm integrations plants
    PRIV_REQUIRES rules event_log mbedtls ble_collector wrappers bthome psvm gatt actors
    EMBED_FILES "${DIST}/index.html.gz" "${DIST}/app.js.gz" "${DIST}/app.css.gz"
)
