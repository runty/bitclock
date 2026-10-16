#include "nvs.h"

#include "esp_random.h"

#include <nvs.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *NVS_NAMESPACE =
    "canary"; // original project codename was canary
static const char *NVS_ID_TIMEZONE = "tz";
static const char *NVS_ID_TEMP_UNIT = "temp_unit";
static const char *NVS_ID_CLOCK_FORMAT = "clk_fmt";
static const char *NVS_ID_WEATHER_PATH = "wea_path";
static const char *NVS_ID_APP_SELECTION = "app";
static const char *NVS_ID_HA_MQTT_ENABLED = "ha_mqtt_en";
static const char *NVS_ID_HA_MQTT_URI = "ha_mqtt_uri";
static const char *NVS_ID_HA_MQTT_USERNAME = "ha_mqtt_user";
static const char *NVS_ID_HA_MQTT_PASSWORD = "ha_mqtt_pass";
static const char *NVS_ID_BLE_STATIC_ADDR = "ble_addr";

static bitclock_nvs_temp_unit_val_t temp_unit = BITCLOCK_NVS_TEMP_UNIT_VAL_NONE;
static bitclock_nvs_clock_format_val_t clock_format =
    BITCLOCK_NVS_CLOCK_FORMAT_VAL_NONE;
static bitclock_nvs_app_selection_val_t app_selection =
    BITCLOCK_NVS_APP_SELECTION_VAL_NONE;

static const char *timezone_str = NULL;
static const char *weather_path_str = NULL;
static bool ha_mqtt_enabled = false;
static char ha_mqtt_uri[BITCLOCK_NVS_HA_MQTT_URI_MAX_LEN] = {0};
static char ha_mqtt_username[BITCLOCK_NVS_HA_MQTT_USERNAME_MAX_LEN] = {0};
static char ha_mqtt_password[BITCLOCK_NVS_HA_MQTT_PASSWORD_MAX_LEN] = {0};

static void copy_nvs_string(char *dst, size_t dst_size, const char *src,
                            size_t src_size) {
  if (dst_size == 0) {
    return;
  }

  size_t copy_len = 0;
  if (src != NULL && src_size > 0) {
    copy_len = strnlen(src, src_size);
    if (copy_len >= dst_size) {
      copy_len = dst_size - 1;
    }
    memcpy(dst, src, copy_len);
  }
  dst[copy_len] = '\0';
}

static bool ble_static_addr_is_valid(const uint8_t addr[6]) {
  if ((addr[5] & 0xc0) != 0xc0) {
    return false;
  }

  bool random_part_has_one = (addr[5] & 0x3f) != 0;
  bool random_part_has_zero = (addr[5] & 0x3f) != 0x3f;

  for (size_t i = 0; i < 5; i++) {
    random_part_has_one = random_part_has_one || addr[i] != 0;
    random_part_has_zero = random_part_has_zero || addr[i] != 0xff;
  }

  return random_part_has_one && random_part_has_zero;
}

static void generate_ble_static_addr(uint8_t addr[6]) {
  do {
    esp_fill_random(addr, 6);
    addr[5] = (addr[5] & 0x3f) | 0xc0;
  } while (!ble_static_addr_is_valid(addr));
}

esp_err_t bitclock_nvs_init() {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  // Timezone
  size_t required_size = 0;
  err = nvs_get_blob(handle, NVS_ID_TIMEZONE, NULL, &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }
  if (required_size > 0) {
    timezone_str = malloc(required_size);
    err = nvs_get_blob(handle, NVS_ID_TIMEZONE, (void *)timezone_str,
                       &required_size);
    if (err != ESP_OK) {
      free((void *)timezone_str);
      return err;
    }
  }

  // Weather path
  required_size = 0;
  err = nvs_get_blob(handle, NVS_ID_WEATHER_PATH, NULL, &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }
  if (required_size > 0) {
    weather_path_str = malloc(required_size);
    err = nvs_get_blob(handle, NVS_ID_WEATHER_PATH, (void *)weather_path_str,
                       &required_size);
    if (err != ESP_OK) {
      free((void *)weather_path_str);
      return err;
    }
  }

  // Temp unit
  err = nvs_get_u8(handle, NVS_ID_TEMP_UNIT, &temp_unit);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  // Clock format
  err = nvs_get_u8(handle, NVS_ID_CLOCK_FORMAT, &clock_format);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  // App selection
  err = nvs_get_u8(handle, NVS_ID_APP_SELECTION, &app_selection);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  // Home Assistant MQTT
  uint8_t ha_mqtt_enabled_val = 0;
  err = nvs_get_u8(handle, NVS_ID_HA_MQTT_ENABLED, &ha_mqtt_enabled_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }
  ha_mqtt_enabled = ha_mqtt_enabled_val != 0;

  required_size = sizeof(ha_mqtt_uri);
  err = nvs_get_str(handle, NVS_ID_HA_MQTT_URI, ha_mqtt_uri, &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  required_size = sizeof(ha_mqtt_username);
  err = nvs_get_str(handle, NVS_ID_HA_MQTT_USERNAME, ha_mqtt_username,
                    &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  required_size = sizeof(ha_mqtt_password);
  err = nvs_get_str(handle, NVS_ID_HA_MQTT_PASSWORD, ha_mqtt_password,
                    &required_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return err;
  }

  nvs_close(handle);

  return ESP_OK;
}

bitclock_nvs_temp_unit_val_t bitclock_nvs_get_temp_unit() { return temp_unit; }

esp_err_t
bitclock_nvs_set_temp_unit(bitclock_nvs_temp_unit_val_t new_temp_unit) {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_u8(handle, NVS_ID_TEMP_UNIT, new_temp_unit);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }
  temp_unit = new_temp_unit;

  nvs_close(handle);

  return ESP_OK;
}

const char *bitclock_nvs_get_tz() { return timezone_str; }

esp_err_t bitclock_nvs_set_tz(const char *tz, size_t size) {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_blob(handle, NVS_ID_TIMEZONE, tz, size);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }
  // FIXME: Free the old string? Malloc for the new one?
  timezone_str = tz;

  nvs_close(handle);

  return ESP_OK;
}

const char *bitclock_nvs_get_weather_path() { return weather_path_str; }

esp_err_t bitclock_nvs_set_weather_path(const char *weather_path, size_t size) {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_blob(handle, NVS_ID_WEATHER_PATH, weather_path, size);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }
  // FIXME: Free the old string? Malloc for the new one?
  weather_path_str = weather_path;

  nvs_close(handle);

  return ESP_OK;
}

bitclock_nvs_clock_format_val_t bitclock_nvs_get_clock_format() {
  return clock_format;
}

esp_err_t bitclock_nvs_set_clock_format(
    bitclock_nvs_clock_format_val_t new_clock_format) {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_u8(handle, NVS_ID_CLOCK_FORMAT, new_clock_format);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }
  clock_format = new_clock_format;

  nvs_close(handle);

  return ESP_OK;
}

bitclock_nvs_app_selection_val_t bitclock_nvs_get_app_selection() {
  return app_selection;
}

esp_err_t bitclock_nvs_set_app_selection(
    bitclock_nvs_app_selection_val_t new_app_selection) {
  nvs_handle_t handle;
  esp_err_t err;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_u8(handle, NVS_ID_APP_SELECTION, new_app_selection);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  if (err != ESP_OK) {
    nvs_close(handle);
    return err;
  }
  app_selection = new_app_selection;

  nvs_close(handle);

  return ESP_OK;
}

bool bitclock_nvs_get_ha_mqtt_enabled() { return ha_mqtt_enabled; }

const char *bitclock_nvs_get_ha_mqtt_uri() { return ha_mqtt_uri; }

const char *bitclock_nvs_get_ha_mqtt_username() { return ha_mqtt_username; }

const char *bitclock_nvs_get_ha_mqtt_password() { return ha_mqtt_password; }

esp_err_t bitclock_nvs_get_or_create_ble_static_addr(uint8_t addr[6]) {
  nvs_handle_t handle;
  esp_err_t err;
  size_t addr_size = 0;

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_get_blob(handle, NVS_ID_BLE_STATIC_ADDR, NULL, &addr_size);
  if (err == ESP_OK && addr_size == 6) {
    size_t read_size = 6;
    err = nvs_get_blob(handle, NVS_ID_BLE_STATIC_ADDR, addr, &read_size);
    if (err != ESP_OK) {
      nvs_close(handle);
      return err;
    }
    if (err == ESP_OK && ble_static_addr_is_valid(addr)) {
      nvs_close(handle);
      return ESP_OK;
    }
  } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return err;
  }

  generate_ble_static_addr(addr);

  err = nvs_set_blob(handle, NVS_ID_BLE_STATIC_ADDR, addr, 6);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  return err;
}

esp_err_t bitclock_nvs_set_ha_mqtt_config(bool enabled, const char *uri,
                                          size_t uri_size,
                                          const char *username,
                                          size_t username_size,
                                          const char *password,
                                          size_t password_size) {
  nvs_handle_t handle;
  esp_err_t err;

  char next_uri[BITCLOCK_NVS_HA_MQTT_URI_MAX_LEN] = {0};
  char next_username[BITCLOCK_NVS_HA_MQTT_USERNAME_MAX_LEN] = {0};
  char next_password[BITCLOCK_NVS_HA_MQTT_PASSWORD_MAX_LEN] = {0};

  copy_nvs_string(next_uri, sizeof(next_uri), uri, uri_size);
  copy_nvs_string(next_username, sizeof(next_username), username,
                  username_size);
  copy_nvs_string(next_password, sizeof(next_password), password,
                  password_size);

  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_u8(handle, NVS_ID_HA_MQTT_ENABLED, enabled ? 1 : 0);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_ID_HA_MQTT_URI, next_uri);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_ID_HA_MQTT_USERNAME, next_username);
  }
  if (err == ESP_OK) {
    err = nvs_set_str(handle, NVS_ID_HA_MQTT_PASSWORD, next_password);
  }
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);

  if (err != ESP_OK) {
    return err;
  }

  ha_mqtt_enabled = enabled;
  memcpy(ha_mqtt_uri, next_uri, sizeof(ha_mqtt_uri));
  memcpy(ha_mqtt_username, next_username, sizeof(ha_mqtt_username));
  memcpy(ha_mqtt_password, next_password, sizeof(ha_mqtt_password));

  return ESP_OK;
}
