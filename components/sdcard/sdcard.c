#include "sdcard.h"

#include <math.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT SDCARD_MOUNT_POINT
#define SD_CS GPIO_NUM_4
#define SD_HOST SPI3_HOST

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;
static FILE *s_trip_file;
static bool s_mounted;
static uint32_t s_last_sequence;
static char s_trip_path[64];
static bool s_has_last_coord;
static double s_last_lat;
static double s_last_lon;
static uint16_t s_current_trip_number;
static bool s_waiting_for_gps_time;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-- Create a date-time-based trip filename in the form trip-EEYYMMDD-HHmm.kml.
static void build_trip_filename(char *buffer, size_t buffer_size, const gps_data_t *gps)
{
  snprintf(buffer, buffer_size,
           SD_MOUNT_POINT "/trip-%04d%02d%02d-%02d%02d.kml",
           (int)gps->year,
           (int)gps->month,
           (int)gps->day,
           (int)gps->hour,
           (int)gps->minute);
}

//-- Calculate distance in meters between two lat/lon coordinates using the Haversine formula.
static double calculate_distance_m(double lat1, double lon1, double lat2, double lon2)
{
  double lat1_rad = lat1 * (M_PI / 180.0);
  double lat2_rad = lat2 * (M_PI / 180.0);
  double delta_lat = (lat2 - lat1) * (M_PI / 180.0);
  double delta_lon = (lon2 - lon1) * (M_PI / 180.0);

  double sin_dlat = sin(delta_lat / 2.0);
  double sin_dlon = sin(delta_lon / 2.0);

  double a = sin_dlat * sin_dlat + cos(lat1_rad) * cos(lat2_rad) * sin_dlon * sin_dlon;
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));

  const double earth_radius_m = 6371000.0;
  return earth_radius_m * c;
}

//-- Calculate required distance threshold based on current speed in km/h.
//-- Speed <= 0 km/h: 5.0 meters minimum.
//-- Speed >= 100 km/h: 50.0 meters.
//-- In between: linear interpolation between 5.0m and 50.0m.
static float calculate_required_distance_m(float speed_kmh)
{
  if (speed_kmh <= 0.0f)
  {
    return 5.0f;
  }
  if (speed_kmh >= 100.0f)
  {
    return 50.0f;
  }
  return 5.0f + (speed_kmh / 100.0f) * 45.0f;
}

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

static bool is_trip_filename(const char *name)
{
  size_t length = strlen(name);
  return strncmp(name, "trip-", 5) == 0 &&
         length > 9 && strcmp(name + length - 4, ".kml") == 0;
}

esp_err_t sdcard_remove_small_trip_files(void)
{
  if (!s_mounted)
  {
    return ESP_ERR_INVALID_STATE;
  }

  bool waiting_for_new_trip = false;
  if (s_trip_file)
  {
    if (fflush(s_trip_file) != 0 ||
        write_text("</coordinates></LineString></Placemark></Document></kml>\n") != ESP_OK ||
        fflush(s_trip_file) != 0 || fclose(s_trip_file) != 0)
    {
      s_trip_file = NULL;
      return ESP_FAIL;
    }
    s_trip_file = NULL;

    struct stat active_file_info;
    if (stat(s_trip_path, &active_file_info) != 0)
    {
      ESP_LOGW(TAG, "Cannot inspect active trip file %s", s_trip_path);
      waiting_for_new_trip = true;
    }
    else if (active_file_info.st_size < 1024)
    {
      if (remove(s_trip_path) != 0)
      {
        ESP_LOGW(TAG, "Cannot delete small active trip file %s", s_trip_path);
      }
      else
      {
        ESP_LOGI(TAG, "Deleted small trip file %s", s_trip_path);
      }
      waiting_for_new_trip = true;
    }
    else
    {
      s_trip_file = fopen(s_trip_path, "a");
      if (!s_trip_file)
      {
        ESP_LOGE(TAG, "Cannot reopen active trip file %s", s_trip_path);
        return ESP_FAIL;
      }
    }
  }

  DIR *directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory for trip cleanup");
    return ESP_FAIL;
  }

  struct dirent *entry;
  esp_err_t result = ESP_OK;
  while ((entry = readdir(directory)) != NULL)
  {
    if (!is_trip_filename(entry->d_name))
    {
      continue;
    }

    char path[sizeof(s_trip_path)];
    int written = snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
      result = ESP_FAIL;
      continue;
    }

    struct stat file_info;
    if (stat(path, &file_info) != 0)
    {
      ESP_LOGW(TAG, "Cannot inspect %s", path);
      result = ESP_FAIL;
      continue;
    }
    if (file_info.st_size < 1024 && remove(path) != 0)
    {
      ESP_LOGW(TAG, "Cannot delete small trip file %s", path);
      result = ESP_FAIL;
    }
    else if (file_info.st_size < 1024)
    {
      ESP_LOGI(TAG, "Deleted small trip file %s", path);
    }
  }
  closedir(directory);

  s_waiting_for_gps_time = waiting_for_new_trip;
  if (waiting_for_new_trip)
  {
    s_last_sequence = 0;
    s_has_last_coord = false;
  }
  return result;
}

static esp_err_t create_trip_file(const gps_data_t *gps)
{
  if (!gps || !gps->date_valid)
  {
    s_waiting_for_gps_time = true;
    return ESP_ERR_INVALID_STATE;
  }
  build_trip_filename(s_trip_path, sizeof(s_trip_path), gps);

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

  s_current_trip_number = 0;
  s_last_sequence = 0;
  s_has_last_coord = false;
  s_waiting_for_gps_time = false;
  ESP_LOGI(TAG, "Writing %s", s_trip_path);
  return ESP_OK;
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
  err = create_trip_file(NULL);
  if (err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "Waiting for GPS date/time before creating the trip file");
    return ESP_OK;
  }
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
  esp_err_t err = create_trip_file(NULL);
  if (err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "Trip reset is waiting for GPS date/time");
    return ESP_OK;
  }
  return err;
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
  esp_err_t create_err = create_trip_file(NULL);
  if (create_err == ESP_ERR_INVALID_STATE)
  {
    ESP_LOGI(TAG, "SD format complete; waiting for GPS date/time before creating the trip file");
    return ESP_OK;
  }
  return create_err;
}

esp_err_t sdcard_append_fix(const gps_data_t *gps)
{
  if (!s_mounted || !gps)
  {
    return ESP_ERR_INVALID_STATE;
  }

  if (s_waiting_for_gps_time)
  {
    if (!gps->date_valid)
    {
      return ESP_OK;
    }
    if (create_trip_file(gps) != ESP_OK)
    {
      return ESP_FAIL;
    }
  }

  if (!s_trip_file)
  {
    return ESP_ERR_INVALID_STATE;
  }
  if (!gps->fix_valid || gps->sequence == s_last_sequence)
  {
    return ESP_OK;
  }
  if (gps->sequence == 0) return ESP_ERR_INVALID_ARG;

  //-- If we already wrote a coordinate for this trip, check distance and speed threshold.
  if (s_has_last_coord)
  {
    double dist_m = calculate_distance_m(s_last_lat, s_last_lon, gps->latitude_deg, gps->longitude_deg);
    float required_dist_m = calculate_required_distance_m(gps->speed_kmh);

    if (dist_m < (double)required_dist_m)
    {
      s_last_sequence = gps->sequence;
      return ESP_OK;
    }
  }

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
  s_last_lat = gps->latitude_deg;
  s_last_lon = gps->longitude_deg;
  s_has_last_coord = true;
  return ESP_OK;
}

void sdcard_get_status(sdcard_status_t *status)
{
  if (!status) return;
  update_status(status);
}

uint16_t sdcard_get_trip_number(void)
{
  return s_current_trip_number;
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
