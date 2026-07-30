#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t      webserver_start(void);
httpd_handle_t webserver_handle(void);
