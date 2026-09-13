#pragma once

#include "esp_http_server.h"

//-- Registers the REST API URI handlers (list/download/upload/delete) on the given server.
esp_err_t webserver_api_register(httpd_handle_t server);

//-- Registers the static GUI file URI handlers (index.html, style.css, app.js) on the given server.
esp_err_t webserver_static_register(httpd_handle_t server);
