#include "webserver_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

#include "sdcard.h"
#include "webserver.h"

static const char *TAG = "webserver_api";

//-- Resolves the "store" query parameter ("sd" or "fs") to its VFS mount point.
static esp_err_t resolve_store_base(httpd_req_t *req, char *store_value, size_t store_value_size, const char **base_path)
{
  char query[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
  {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(query, "store", store_value, store_value_size) != ESP_OK)
  {
    return ESP_FAIL;
  }

  if (strcmp(store_value, "sd") == 0)
  {
    *base_path = SDCARD_MOUNT_POINT;
    return ESP_OK;
  }
  if (strcmp(store_value, "fs") == 0)
  {
    *base_path = WEBSERVER_LITTLEFS_MOUNT_POINT;
    return ESP_OK;
  }
  return ESP_FAIL;
}

//-- Rejects empty names and any attempt to escape the store's root directory.
static bool file_name_is_valid(const char *name)
{
  if (name[0] == '\0')
  {
    return false;
  }
  if (strchr(name, '/') != NULL)
  {
    return false;
  }
  if (strstr(name, "..") != NULL)
  {
    return false;
  }
  return true;
}

static esp_err_t handle_list(httpd_req_t *req)
{
  char store_value[8];
  const char *base_path;
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or missing 'store' parameter");
    return ESP_FAIL;
  }

  DIR *dir = opendir(base_path);
  if (dir == NULL)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to open storage directory");
    return ESP_FAIL;
  }

  cJSON *array = cJSON_CreateArray();
  struct dirent *entry;
  char full_path[320];
  while ((entry = readdir(dir)) != NULL)
  {
    if (entry->d_name[0] == '.')
    {
      continue;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
    struct stat file_stat;
    long size = 0;
    if (stat(full_path, &file_stat) == 0)
    {
      size = (long)file_stat.st_size;
    }

    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", entry->d_name);
    cJSON_AddNumberToObject(item, "size", size);
    cJSON_AddItemToArray(array, item);
  }
  closedir(dir);

  char *json_text = cJSON_PrintUnformatted(array);
  cJSON_Delete(array);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json_text);
  free(json_text);
  return ESP_OK;
}

static esp_err_t handle_download(httpd_req_t *req)
{
  char store_value[8];
  const char *base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  FILE *file = fopen(full_path, "rb");
  if (file == NULL)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  char disposition[96];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);

  char buffer[512];
  size_t read_bytes;
  esp_err_t result = ESP_OK;
  while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
  {
    if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK)
    {
      result = ESP_FAIL;
      break;
    }
  }
  fclose(file);

  if (result == ESP_OK)
  {
    httpd_resp_send_chunk(req, NULL, 0);
  }
  return result;
}

static esp_err_t handle_upload(httpd_req_t *req)
{
  char store_value[8];
  const char *base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  FILE *file = fopen(full_path, "wb");
  if (file == NULL)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to create file");
    return ESP_FAIL;
  }

  char buffer[512];
  int remaining = req->content_len;
  while (remaining > 0)
  {
    int to_read = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
    int received = httpd_req_recv(req, buffer, to_read);
    if (received <= 0)
    {
      fclose(file);
      remove(full_path);
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload aborted");
      return ESP_FAIL;
    }
    fwrite(buffer, 1, received, file);
    remaining -= received;
  }
  fclose(file);

  ESP_LOGI(TAG, "Uploaded %s to %s (%d bytes)", name, base_path, req->content_len);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t handle_delete(httpd_req_t *req)
{
  char store_value[8];
  const char *base_path;
  char name[64];

  char query[128];
  if (resolve_store_base(req, store_value, sizeof(store_value), &base_path) != ESP_OK ||
      httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
      !file_name_is_valid(name))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request parameters");
    return ESP_FAIL;
  }

  char full_path[320];
  snprintf(full_path, sizeof(full_path), "%s/%s", base_path, name);

  if (remove(full_path) != 0)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Deleted %s from %s", name, base_path);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

esp_err_t webserver_api_register(httpd_handle_t server)
{
  httpd_uri_t list_uri = { .uri = "/api/files", .method = HTTP_GET, .handler = handle_list };
  httpd_uri_t download_uri = { .uri = "/api/download", .method = HTTP_GET, .handler = handle_download };
  httpd_uri_t upload_uri = { .uri = "/api/upload", .method = HTTP_POST, .handler = handle_upload };
  httpd_uri_t delete_uri = { .uri = "/api/delete", .method = HTTP_DELETE, .handler = handle_delete };

  esp_err_t err;
  if ((err = httpd_register_uri_handler(server, &list_uri)) != ESP_OK) return err;
  if ((err = httpd_register_uri_handler(server, &download_uri)) != ESP_OK) return err;
  if ((err = httpd_register_uri_handler(server, &upload_uri)) != ESP_OK) return err;
  if ((err = httpd_register_uri_handler(server, &delete_uri)) != ESP_OK) return err;
  return ESP_OK;
}
