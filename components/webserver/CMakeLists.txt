set(DIST "${CMAKE_CURRENT_LIST_DIR}/../../../planhubos/webui/dist-gz")
if(NOT EXISTS "${DIST}/index.html.gz")
    message(FATAL_ERROR "Web assets missing. Run: cd planhubos/webui && npm install && npm run build")
endif()

idf_component_register(
    SRCS "webserver.c" "api_v1.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer json wifi_manager app_config
    EMBED_FILES "${DIST}/index.html.gz" "${DIST}/app.js.gz" "${DIST}/app.css.gz"
)
