#include "webserver.h"

#include <stdbool.h>

#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi_provisioner.h"

#include "webserver_internal.h"

//-- mDNS hostname, reachable as "M5STACKSPEED.local" while WiFi is active.
#define WEBSERVER_HOSTNAME "M5StackSpeed"

static const char *TAG = "webserver";
static httpd_handle_t s_server;
static bool s_network_active;
static bool s_mdns_active;

//-- Live connection state, tracked independently of wifi_prov_is_connected()
//-- (which is never cleared again once a STA connection later drops).
static volatile bool s_connecting;
static volatile bool s_sta_connected;
static volatile bool s_ap_mode;
static bool s_wifi_event_handlers_registered;

static esp_err_t mount_littlefs(void)
{
  esp_vfs_littlefs_conf_t conf = {
    .base_path = WEBSERVER_LITTLEFS_MOUNT_POINT,
    .partition_label = "littlefs",
    .format_if_mount_failed = true,
    .dont_mount = false,
  };

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(err));
  }
  return err;
}

static void start_http_server(void)
{
  if (s_server != NULL)
  {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 16;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
    s_server = NULL;
    return;
  }

  webserver_api_register(s_server);
  webserver_static_register(s_server);

  ESP_LOGI(TAG, "File-manager web server started");
}

static void stop_http_server(void)
{
  if (s_server == NULL)
  {
    return;
  }
  httpd_stop(s_server);
  s_server = NULL;
  ESP_LOGI(TAG, "File-manager web server stopped");
}

static void on_wifi_connected(void)
{
  //-- Fires when the portal flow hands over a working STA connection.
  s_sta_connected = true;
  s_ap_mode = false;
  start_http_server();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
  {
    s_sta_connected = false;
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    s_sta_connected = true;
    s_ap_mode = false;
  }
}

static void register_wifi_event_handlers(void)
{
  if (s_wifi_event_handlers_registered)
  {
    return;
  }
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
  s_wifi_event_handlers_registered = true;
}

static void unregister_wifi_event_handlers(void)
{
  if (!s_wifi_event_handlers_registered)
  {
    return;
  }
  esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event);
  s_wifi_event_handlers_registered = false;
}

static void start_mdns(void)
{
  if (s_mdns_active)
  {
    return;
  }

  esp_err_t err = mdns_init();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to start mDNS: %s", esp_err_to_name(err));
    return;
  }

  mdns_hostname_set(WEBSERVER_HOSTNAME);
  mdns_instance_name_set(WEBSERVER_HOSTNAME);
  mdns_service_add(WEBSERVER_HOSTNAME, "_http", "_tcp", 80, NULL, 0);
  s_mdns_active = true;
  ESP_LOGI(TAG, "mDNS hostname set to %s.local", WEBSERVER_HOSTNAME);
}

static void stop_mdns(void)
{
  if (!s_mdns_active)
  {
    return;
  }
  mdns_free();
  s_mdns_active = false;
}

//-- Runs the (potentially slow, multi-second) connect-or-provision attempt on
//-- its own task so opening the System Menu never blocks the UI loop.
static void wifi_connect_task(void *arg)
{
  wifi_prov_config_t config = WIFI_PROV_DEFAULT_CONFIG();
  config.ap_ssid = WEBSERVER_HOSTNAME;
  config.on_connected = on_wifi_connected;

  esp_err_t err = wifi_prov_start(&config);
  if (err == ESP_OK)
  {
    start_mdns();
    if (wifi_prov_is_connected())
    {
      s_sta_connected = true;
      start_http_server();
    }
    else
    {
      s_ap_mode = true;
    }
  }
  else
  {
    ESP_LOGE(TAG, "Failed to start WiFi provisioning: %s", esp_err_to_name(err));
  }

  s_connecting = false;
  vTaskDelete(NULL);
}

esp_err_t webserver_init(void)
{
  return mount_littlefs();
}

esp_err_t webserver_check_wifi_credentials(void)
{
  ESP_LOGI(TAG, "Checking stored WiFi credentials");

  wifi_prov_config_t config = WIFI_PROV_DEFAULT_CONFIG();
  config.ap_ssid = WEBSERVER_HOSTNAME;
  esp_err_t err = wifi_prov_start(&config);
  if (wifi_prov_is_connected())
  {
    ESP_LOGI(TAG, "WiFi credentials OK, station connected");
  }
  else
  {
    ESP_LOGW(TAG, "WiFi station not connected (no/invalid credentials)");
  }

  wifi_prov_stop();
  return err;
}

esp_err_t webserver_start(void)
{
  if (s_network_active)
  {
    return ESP_OK;
  }

  s_sta_connected = false;
  s_ap_mode = false;
  s_connecting = true;
  s_network_active = true;

  wifi_prov_init();
  register_wifi_event_handlers();

  if (xTaskCreate(wifi_connect_task, "wifi_connect", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create WiFi connect task");
    s_connecting = false;
    s_network_active = false;
    unregister_wifi_event_handlers();
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t webserver_stop(void)
{
  if (!s_network_active)
  {
    return ESP_OK;
  }

  //-- Let a still-running connect attempt finish before tearing WiFi down.
  while (s_connecting)
  {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  unregister_wifi_event_handlers();
  stop_http_server();
  stop_mdns();
  esp_err_t err = wifi_prov_stop();
  s_sta_connected = false;
  s_ap_mode = false;
  s_network_active = false;
  return err;
}

webserver_wifi_status_t webserver_get_wifi_status(void)
{
  if (!s_network_active || s_connecting)
  {
    return WEBSERVER_WIFI_OFF;
  }
  if (s_sta_connected)
  {
    return WEBSERVER_WIFI_STA_CONNECTED;
  }
  if (s_ap_mode)
  {
    return WEBSERVER_WIFI_AP_MODE;
  }
  return WEBSERVER_WIFI_OFF;
}
