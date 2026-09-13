#include "sdcard.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT SDCARD_MOUNT_POINT
#define SD_CS GPIO_NUM_4
#define SD_HOST SPI3_HOST

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;
static FILE *s_trip_file;
static bool s_mounted;
static uint32_t s_last_sequence;
static char s_trip_path[32];

static void clear_status(sdcard_status_t *status)
{
  memset(status, 0, sizeof(*status));
}

static esp_err_t update_status(sdcard_status_t *status)
{
  FATFS *filesystem = NULL;
  DWORD free_clusters = 0;
  if (!s_mounted || !s_card)
  {
    clear_status(status);
    return ESP_FAIL;
  }

  status->mounted = true;
  FRESULT result = f_getfree("0:", &free_clusters, &filesystem);
  if (result != FR_OK || !filesystem)
  {
    ESP_LOGE(TAG, "Unable to read SD filesystem statistics: %d", result);
    status->mounted = false;
    status->total_bytes = 0;
    status->free_bytes = 0;
    status->free_percent = 0;
    return ESP_FAIL;
  }

  uint64_t sector_size = s_card->csd.sector_size;
  uint64_t total_sectors = ((uint64_t)(filesystem->n_fatent - 2)) * filesystem->csize;
  uint64_t free_sectors = (uint64_t)free_clusters * filesystem->csize;
  status->total_bytes = total_sectors * sector_size;
  status->free_bytes = free_sectors * sector_size;
  status->free_percent = status->total_bytes > 0
    ? (uint8_t)((status->free_bytes * 100U) / status->total_bytes)
    : 0;
  if (status->free_percent > 100) status->free_percent = 100;
  return ESP_OK;
}

static esp_err_t write_text(const char *text)
{
  if (!s_trip_file || fputs(text, s_trip_file) == EOF)
  {
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t create_trip_file(void)
{
  for (unsigned int id = 0; id <= 999; ++id)
  {
    snprintf(s_trip_path, sizeof(s_trip_path), SD_MOUNT_POINT "/trip-%03u.kml", id);
    FILE *file = fopen(s_trip_path, "r");
    if (file)
    {
      fclose(file);
      continue;
    }

    s_trip_file = fopen(s_trip_path, "w");
    if (!s_trip_file)
    {
      ESP_LOGE(TAG, "Cannot create %s", s_trip_path);
      return ESP_FAIL;
    }
    if (write_text("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>\n"
                   "<name>GPS Trip</name><Placemark><LineString><tessellate>1</tessellate>\n"
                   "<coordinates>\n") != ESP_OK || fflush(s_trip_file) != 0)
    {
      fclose(s_trip_file);
      s_trip_file = NULL;
      remove(s_trip_path);
      return ESP_FAIL;
    }
    s_last_sequence = 0;
    ESP_LOGI(TAG, "Writing %s", s_trip_path);
    return ESP_OK;
  }

  ESP_LOGE(TAG, "No unused trip file identifier remains");
  return ESP_ERR_NO_MEM;
}

esp_err_t sdcard_init(void)
{
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_HOST;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = SD_CS;
  slot_config.host_id = SD_HOST;

  esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 4,
    .allocation_unit_size = 16 * 1024,
  };

  esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    return err;
  }

  s_mounted = true;
  err = create_trip_file();
  if (err != ESP_OK)
  {
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    return err;
  }
  return ESP_OK;
}

esp_err_t sdcard_reset_trip(void)
{
  if (!s_mounted) return ESP_ERR_INVALID_STATE;
  if (s_trip_file)
  {
    if (write_text("</coordinates></LineString></Placemark></Document></kml>\n") != ESP_OK ||
        fflush(s_trip_file) != 0 || fclose(s_trip_file) != 0)
    {
      s_trip_file = NULL;
      return ESP_FAIL;
    }
    s_trip_file = NULL;
  }
  return create_trip_file();
}

esp_err_t sdcard_format(void)
{
  if (!s_mounted || !s_card)
  {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_trip_file)
  {
    if (fclose(s_trip_file) != 0)
    {
      s_trip_file = NULL;
      return ESP_FAIL;
    }
    s_trip_file = NULL;
  }

  esp_err_t err = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card format failed: %s", esp_err_to_name(err));
    return err;
  }

  s_last_sequence = 0;
  return create_trip_file();
}

esp_err_t sdcard_append_fix(const gps_data_t *gps)
{
  if (!s_mounted || !s_trip_file || !gps || !gps->fix_valid || gps->sequence == s_last_sequence)
  {
    return ESP_ERR_INVALID_STATE;
  }
  if (gps->sequence == 0) return ESP_ERR_INVALID_ARG;

  char coordinate[96];
  int written = snprintf(coordinate, sizeof(coordinate), "%.7f,%.7f,%.2f\n",
                         gps->longitude_deg, gps->latitude_deg, gps->altitude_m);
  if (written < 0 || (size_t)written >= sizeof(coordinate) || write_text(coordinate) != ESP_OK ||
      fflush(s_trip_file) != 0)
  {
    ESP_LOGE(TAG, "Failed to write GPS coordinate");
    return ESP_FAIL;
  }
  s_last_sequence = gps->sequence;
  return ESP_OK;
}

void sdcard_get_status(sdcard_status_t *status)
{
  if (!status) return;
  update_status(status);
}

esp_err_t sdcard_finish(void)
{
  esp_err_t result = ESP_OK;
  if (s_trip_file)
  {
    if (write_text("</coordinates></LineString></Placemark></Document></kml>\n") != ESP_OK ||
        fflush(s_trip_file) != 0 || fclose(s_trip_file) != 0)
    {
      result = ESP_FAIL;
    }
    s_trip_file = NULL;
  }
  if (s_mounted)
  {
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (result == ESP_OK) result = err;
    s_card = NULL;
    s_mounted = false;
  }
  return result;
}
