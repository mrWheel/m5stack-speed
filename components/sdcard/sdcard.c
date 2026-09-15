#include "sdcard.h"

#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
#define SDCARD_NVS_NAMESPACE "sdcard"
#define SDCARD_NVS_ACTIVE_GPX "active_gpx"

//-- TEMPORARY debug switch: keep trip files with fewer than 20 entries
//-- instead of deleting them, so recorded points can be inspected while the
//-- GPX/CSV export bug is being diagnosed. Set back to 0 once confirmed fixed.
#define SDCARD_KEEP_SMALL_TRIP_FILES 1

static const char* TAG = "sdcard";
static sdmmc_card_t* s_card;
static int s_trip_gpx_fd = -1;
static int s_trip_csv_fd = -1;
static bool s_mounted;
static uint32_t s_last_sequence;
static char s_trip_gpx_path[64];
static char s_trip_csv_path[64];
static bool s_has_last_coord;
static double s_last_lat;
static double s_last_lon;
static float s_trip_distance_m;
static float s_last_export_distance_m;
static uint32_t s_entry_count;
static uint16_t s_current_trip_number;
static bool s_waiting_for_gps_time;
static bool s_waiting_for_new_filename;
static char s_closed_gpx_path[64];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-- Create both date-time-based trip filenames in the form trip-EEYYMMDD-HHmm.ext.
static void build_trip_filenames(const gps_data_t* gps)
{
  snprintf(s_trip_gpx_path, sizeof(s_trip_gpx_path),
           SD_MOUNT_POINT "/trip-%04d%02d%02d-%02d%02d.gpx", (int)gps->year, (int)gps->month,
           (int)gps->day, (int)gps->hour, (int)gps->minute);
  snprintf(s_trip_csv_path, sizeof(s_trip_csv_path),
           SD_MOUNT_POINT "/trip-%04d%02d%02d-%02d%02d.csv", (int)gps->year, (int)gps->month,
           (int)gps->day, (int)gps->hour, (int)gps->minute);
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

static void clear_status(sdcard_status_t* status)
{
  memset(status, 0, sizeof(*status));
}

static esp_err_t update_status(sdcard_status_t* status)
{
  FATFS* filesystem = NULL;
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
  status->free_percent =
      status->total_bytes > 0 ? (uint8_t)((status->free_bytes * 100U) / status->total_bytes) : 0;
  if (status->free_percent > 100)
    status->free_percent = 100;
  return ESP_OK;
}

static esp_err_t write_text(int file, const char* path, const char* text)
{
  size_t length = strlen(text);
  const char* cursor = text;
  while (length > 0)
  {
    ssize_t written = write(file, cursor, length);
    if (written <= 0)
    {
      ESP_LOGE(TAG, "Write failed for %s, errno=%d", path, errno);
      return ESP_FAIL;
    }
    cursor += written;
    length -= (size_t)written;
  }
  return ESP_OK;
}

static esp_err_t remove_gpx_closing_tags(void)
{
  if (s_trip_gpx_fd < 0)
    return ESP_ERR_INVALID_STATE;

  off_t file_size = lseek(s_trip_gpx_fd, 0, SEEK_END);
  size_t read_size = file_size > 128 ? 128 : (size_t)file_size;
  char tail[129] = {0};
  if (file_size < 0 || lseek(s_trip_gpx_fd, -((off_t)read_size), SEEK_END) < 0 ||
      read(s_trip_gpx_fd, tail, read_size) != (ssize_t)read_size)
  {
    return ESP_FAIL;
  }

  char* closing_tags = strstr(tail, "</trkseg>");
  if (closing_tags)
  {
    off_t truncate_at = file_size - (off_t)read_size + (off_t)(closing_tags - tail);
    if (ftruncate(s_trip_gpx_fd, truncate_at) != 0)
    {
      return ESP_FAIL;
    }
  }

  //-- The preceding read() left the file offset at the old EOF; without
  //-- repositioning, the next write() lands past the truncated end and
  //-- leaves a zero-filled gap instead of appending visible content.
  if (lseek(s_trip_gpx_fd, 0, SEEK_END) < 0)
  {
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t finalize_gpx_file(void)
{
  if (write_text(s_trip_gpx_fd, s_trip_gpx_path, "  </trkseg></trk>\n</gpx>\n") != ESP_OK ||
      fsync(s_trip_gpx_fd) != 0)
  {
    return ESP_FAIL;
  }
  return ESP_OK;
}

static bool is_trip_filename(const char* name)
{
  size_t length = strlen(name);
  return strncmp(name, "trip-", 5) == 0 && length > 9 &&
         (strcmp(name + length - 4, ".gpx") == 0 || strcmp(name + length - 4, ".csv") == 0);
}

static uint32_t count_trip_entries(const char* path, bool gpx)
{
  FILE* file = fopen(path, "r");
  if (!file)
    return 0;

  char line[256];
  uint32_t count = 0;
  while (fgets(line, sizeof(line), file))
  {
    if ((gpx && strstr(line, "<trkpt ")) || (!gpx && strstr(line, "\n") && line[0] != 't'))
    {
      ++count;
    }
  }
  fclose(file);
  return count;
}

static void restore_last_trip_point(void)
{
  FILE* file = fopen(s_trip_csv_path, "r");
  if (!file)
    return;

  char line[256];
  char date[16];
  char time[16];
  double latitude;
  double longitude;
  float altitude;
  float speed;
  float course;
  unsigned satellites;
  float distance;
  while (fgets(line, sizeof(line), file))
  {
    if (sscanf(line, "%15[^,],%15[^,],%lf,%lf,%f,%f,%f,%u,%f", date, time, &latitude, &longitude,
               &altitude, &speed, &course, &satellites, &distance) == 9)
    {
      s_last_lat = latitude;
      s_last_lon = longitude;
      s_trip_distance_m = distance;
      s_has_last_coord = true;
    }
  }
  fclose(file);
}

static esp_err_t close_trip_files(void)
{
  esp_err_t result = ESP_OK;
  if (s_trip_gpx_fd >= 0)
  {
    if (remove_gpx_closing_tags() != ESP_OK || finalize_gpx_file() != ESP_OK ||
        close(s_trip_gpx_fd) != 0)
    {
      result = ESP_FAIL;
    }
    s_trip_gpx_fd = -1;
  }
  if (s_trip_csv_fd >= 0)
  {
    if (fsync(s_trip_csv_fd) != 0 || close(s_trip_csv_fd) != 0)
    {
      result = ESP_FAIL;
    }
    s_trip_csv_fd = -1;
  }
  return result;
}

static esp_err_t save_active_gpx_path(void)
{
  nvs_handle_t handle = 0;
  esp_err_t result = nvs_open(SDCARD_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (result != ESP_OK)
    return result;
  result = nvs_set_str(handle, SDCARD_NVS_ACTIVE_GPX, s_trip_gpx_path);
  if (result == ESP_OK)
    result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

static void clear_active_gpx_path(void)
{
  nvs_handle_t handle;
  if (nvs_open(SDCARD_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_erase_key(handle, SDCARD_NVS_ACTIVE_GPX);
  nvs_commit(handle);
  nvs_close(handle);
}

static esp_err_t recover_active_trip(void)
{
  nvs_handle_t handle = 0;
  size_t path_size = sizeof(s_trip_gpx_path);
  if (nvs_open(SDCARD_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK ||
      nvs_get_str(handle, SDCARD_NVS_ACTIVE_GPX, s_trip_gpx_path, &path_size) != ESP_OK)
  {
    if (handle)
      nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
  }
  nvs_close(handle);

  size_t path_length = strlen(s_trip_gpx_path);
  if (path_length <= 4 || strcmp(s_trip_gpx_path + path_length - 4, ".gpx") != 0)
  {
    clear_active_gpx_path();
    return ESP_ERR_INVALID_ARG;
  }

  snprintf(s_trip_csv_path, sizeof(s_trip_csv_path), "%.*s.csv", (int)(path_length - 4),
           s_trip_gpx_path);

  int file = open(s_trip_gpx_path, O_RDWR);
  if (file < 0)
  {
    ESP_LOGW(TAG, "Active GPX file is unavailable: %s", s_trip_gpx_path);
    clear_active_gpx_path();
    return ESP_ERR_NOT_FOUND;
  }

  off_t file_size = lseek(file, 0, SEEK_END);
  size_t read_size = file_size > 512 ? 512 : (size_t)file_size;
  char tail[513] = {0};
  if (file_size < 0 || lseek(file, -((off_t)read_size), SEEK_END) < 0 ||
      read(file, tail, read_size) != (ssize_t)read_size)
  {
    ESP_LOGE(TAG, "Cannot read the end of %s, errno=%d", s_trip_gpx_path, errno);
    close(file);
    return ESP_FAIL;
  }

  char* closing_tags = strstr(tail, "</trkseg>");
  if (closing_tags)
  {
    off_t truncate_at = file_size - (off_t)read_size + (off_t)(closing_tags - tail);
    if (ftruncate(file, truncate_at) != 0)
    {
      ESP_LOGE(TAG, "Cannot reopen active GPX %s, errno=%d", s_trip_gpx_path, errno);
      close(file);
      return ESP_FAIL;
    }
  }
  close(file);

  //-- O_RDWR so remove_gpx_closing_tags() can keep reading the tail of the
  //-- file on this same descriptor before every append, as in create_trip_file().
  s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_APPEND);
  s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_APPEND);
  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    close_trip_files();
    ESP_LOGE(TAG, "Cannot reopen active trip files, errno=%d", errno);
    return ESP_FAIL;
  }
  s_entry_count = count_trip_entries(s_trip_gpx_path, true);
  restore_last_trip_point();
  s_last_export_distance_m = s_trip_distance_m;
  s_waiting_for_gps_time = false;
  ESP_LOGI(TAG, "Resumed active trip %s with %u entries", s_trip_gpx_path, s_entry_count);
  return ESP_OK;
}

esp_err_t sdcard_remove_small_trip_files(void)
{
  if (!s_mounted)
  {
    return ESP_ERR_INVALID_STATE;
  }

  bool waiting_for_new_trip = false;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
    bool too_small = s_entry_count < 20;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    too_small = false;
#endif
    if (too_small)
    {
      remove(s_trip_gpx_path);
      remove(s_trip_csv_path);
      waiting_for_new_trip = true;
    }
    else
    {
      s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_APPEND);
      s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_APPEND);
      if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
      {
        close_trip_files();
        return ESP_FAIL;
      }
    }
  }

  DIR* directory = opendir(SD_MOUNT_POINT);
  if (!directory)
  {
    ESP_LOGE(TAG, "Cannot open SD card directory for trip cleanup");
    return ESP_FAIL;
  }

  struct dirent* entry;
  esp_err_t result = ESP_OK;
  while ((entry = readdir(directory)) != NULL)
  {
    if (!is_trip_filename(entry->d_name))
    {
      continue;
    }

    char path[sizeof(s_trip_gpx_path)];
    int written = snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
    {
      result = ESP_FAIL;
      continue;
    }

    bool gpx = strcmp(path + strlen(path) - 4, ".gpx") == 0;
    bool small_file = count_trip_entries(path, gpx) < 20;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    small_file = false;
#endif
    if (small_file && remove(path) != 0)
    {
      ESP_LOGW(TAG, "Cannot delete small trip file %s", path);
      result = ESP_FAIL;
    }
    else if (small_file)
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
    s_entry_count = 0;
  }
  return result;
}

static esp_err_t create_trip_file(const gps_data_t* gps)
{
  if (!gps || !gps->date_valid)
  {
    s_waiting_for_gps_time = true;
    return ESP_ERR_INVALID_STATE;
  }
  build_trip_filenames(gps);
  if (s_waiting_for_new_filename && strcmp(s_trip_gpx_path, s_closed_gpx_path) == 0)
  {
    s_waiting_for_gps_time = true;
    return ESP_ERR_INVALID_STATE;
  }
  s_waiting_for_new_filename = false;

  //-- The GPX descriptor must stay readable: remove_gpx_closing_tags() reads the
  //-- current tail of the file before every write to locate and strip the
  //-- closing tags, which fails with EBADF on an O_WRONLY descriptor.
  s_trip_gpx_fd = open(s_trip_gpx_path, O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0666);
  s_trip_csv_fd = open(s_trip_csv_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    ESP_LOGE(TAG, "Cannot create GPX/CSV trip files");
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }
  if (write_text(s_trip_gpx_fd, s_trip_gpx_path,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<gpx version=\"1.1\" creator=\"tripTracker\" "
                 "xmlns=\"http://www.topografix.com/GPX/1/1\" "
                 "xmlns:trkptx=\"https://triptracker.local/gpx\">\n"
                 "  <trk><name>GPS Trip</name><trkseg>\n") != ESP_OK ||
      write_text(s_trip_csv_fd, s_trip_csv_path,
                 "date,time,latitude_deg,longitude_deg,altitude_m,speed_kmh,course_deg,satellites,"
                 "distance_m\n") != ESP_OK ||
      fsync(s_trip_gpx_fd) != 0 || fsync(s_trip_csv_fd) != 0 || finalize_gpx_file() != ESP_OK)
  {
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }

  s_current_trip_number = 0;
  s_last_sequence = 0;
  s_has_last_coord = false;
  s_trip_distance_m = 0.0f;
  s_last_export_distance_m = 0.0f;
  s_entry_count = 0;
  s_waiting_for_gps_time = false;
  if (save_active_gpx_path() != ESP_OK)
  {
    ESP_LOGE(TAG, "Cannot persist active trip path");
    close_trip_files();
    remove(s_trip_gpx_path);
    remove(s_trip_csv_path);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Writing %s and %s", s_trip_gpx_path, s_trip_csv_path);
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

  esp_err_t err =
      esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    return err;
  }

  s_mounted = true;
  esp_err_t recover_err = recover_active_trip();
  if (recover_err == ESP_OK)
  {
    return ESP_OK;
  }
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
  if (!s_mounted)
    return ESP_ERR_INVALID_STATE;
  bool retained_closed_trip = false;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    snprintf(s_closed_gpx_path, sizeof(s_closed_gpx_path), "%s", s_trip_gpx_path);
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
    bool too_small = s_entry_count < 20;
#if SDCARD_KEEP_SMALL_TRIP_FILES
    too_small = false;
#endif
    if (too_small)
    {
      remove(s_trip_gpx_path);
      remove(s_trip_csv_path);
    }
    else
    {
      retained_closed_trip = true;
    }
    clear_active_gpx_path();
  }
  s_waiting_for_new_filename = retained_closed_trip;
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

  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      return ESP_FAIL;
    }
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

esp_err_t sdcard_append_fix(const gps_data_t* gps, float trip_distance_m)
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

  if (s_trip_gpx_fd < 0 || s_trip_csv_fd < 0)
  {
    return ESP_ERR_INVALID_STATE;
  }
  if (!gps->fix_valid || gps->sequence == s_last_sequence)
  {
    return ESP_OK;
  }
  if (gps->sequence == 0)
    return ESP_ERR_INVALID_ARG;

  //-- Use the same integrated trip distance shown on the display.
  if (s_has_last_coord)
  {
    float distance_since_export_m = trip_distance_m - s_last_export_distance_m;
    float required_dist_m = 5.0f;

    if (distance_since_export_m < required_dist_m)
    {
      s_last_sequence = gps->sequence;
      return ESP_OK;
    }
    ESP_LOGI(TAG, "Recording GPS point: sequence=%u trip-distance=%.2f since-last=%.2f",
             gps->sequence, trip_distance_m, distance_since_export_m);
  }

  if (remove_gpx_closing_tags() != ESP_OK)
  {
    ESP_LOGE(TAG, "Cannot prepare GPX file for the next point");
    return ESP_FAIL;
  }
  else
  {
    ESP_LOGI(TAG, "Recording first GPS point: sequence=%u", gps->sequence);
  }

  double segment_distance_m =
      s_has_last_coord
          ? calculate_distance_m(s_last_lat, s_last_lon, gps->latitude_deg, gps->longitude_deg)
          : 0.0;
  s_trip_distance_m += (float)segment_distance_m;

  char date[16];
  char time[16];
  char gpx_point[512];
  char csv_row[256];
  snprintf(date, sizeof(date), "%04u-%02u-%02u", gps->year, gps->month, gps->day);
  snprintf(time, sizeof(time), "%02u:%02u:%02uZ", gps->hour, gps->minute, gps->second);
  int gpx_written =
      snprintf(gpx_point, sizeof(gpx_point),
               "    <trkpt lat=\"%.7f\" "
               "lon=\"%.7f\"><ele>%.2f</ele><time>%sT%s</time><extensions><speed_kmh>%.3f</"
               "speed_kmh><course_deg>%.3f</course_deg><satellites>%u</"
               "satellites><distance_m>%.2f</distance_m></extensions></trkpt>\n",
               gps->latitude_deg, gps->longitude_deg, gps->altitude_m, date, time, gps->speed_kmh,
               gps->course_deg, gps->satellites, s_trip_distance_m);
  int csv_written = snprintf(csv_row, sizeof(csv_row), "%s,%s,%.7f,%.7f,%.2f,%.3f,%.3f,%u,%.2f\n",
                             date, time, gps->latitude_deg, gps->longitude_deg, gps->altitude_m,
                             gps->speed_kmh, gps->course_deg, gps->satellites, s_trip_distance_m);
  if (gpx_written < 0 || (size_t)gpx_written >= sizeof(gpx_point) || csv_written < 0 ||
      (size_t)csv_written >= sizeof(csv_row) ||
      write_text(s_trip_gpx_fd, s_trip_gpx_path, gpx_point) != ESP_OK ||
      write_text(s_trip_csv_fd, s_trip_csv_path, csv_row) != ESP_OK || fsync(s_trip_gpx_fd) != 0 ||
      fsync(s_trip_csv_fd) != 0 || finalize_gpx_file() != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to write GPX/CSV GPS point");
    return ESP_FAIL;
  }
  s_last_sequence = gps->sequence;
  s_last_lat = gps->latitude_deg;
  s_last_lon = gps->longitude_deg;
  s_has_last_coord = true;
  s_last_export_distance_m = trip_distance_m;
  ++s_entry_count;
  return ESP_OK;
}

void sdcard_get_status(sdcard_status_t* status)
{
  if (!status)
    return;
  update_status(status);
}

uint16_t sdcard_get_trip_number(void)
{
  return s_current_trip_number;
}

uint32_t sdcard_get_entry_count(void)
{
  return s_entry_count;
}

esp_err_t sdcard_finish(void)
{
  esp_err_t result = ESP_OK;
  if (s_trip_gpx_fd >= 0 || s_trip_csv_fd >= 0)
  {
    if (close_trip_files() != ESP_OK)
    {
      result = ESP_FAIL;
    }
  }
  if (s_mounted)
  {
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (result == ESP_OK)
      result = err;
    s_card = NULL;
    s_mounted = false;
  }
  return result;
}
