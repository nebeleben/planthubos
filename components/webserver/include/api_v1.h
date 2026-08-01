#pragma once
#include "esp_http_server.h"
#include <stdbool.h>

void api_v1_register(httpd_handle_t server);
bool      api_auth_ok(httpd_req_t *req);
esp_err_t api_send_401(httpd_req_t *req);
