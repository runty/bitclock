#include "home_assistant.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "libs/aqi_alert.h"
#include "libs/nvs.h"
#include "mqtt_client.h"
#include "tasks/scd4x.h"
#include "tasks/sgp4x.h"
#include "tasks/sht4x.h"
#include "tasks/weather.h"
#include "tasks/wifi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "home-assistant";

#define MQTT_PUBLISH_INTERVAL pdMS_TO_TICKS(1000 * 30)
#define MQTT_RETRY_INTERVAL pdMS_TO_TICKS(1000 * 5)
#define MQTT_TOPIC_LEN 128
#define MQTT_DEVICE_ID_LEN 32
#define MQTT_MAC_SLUG_LEN 16
#define MQTT_BASE_TOPIC_LEN 32 // bitclock/<12 hex digits>
#define MQTT_PAYLOAD_MAX_LEN 256
#define UNIT_TEMP_C "\xC2\xB0" "C"

// Outdoor-air reference for the CO2 calibration button: the SCD4x is set to
// read this after it has sat in fresh air for a few minutes.
#define CO2_CALIBRATION_TARGET_PPM 420

typedef struct {
  const char *slug;
  const char *name;
  const char *state_field;
  const char *device_class;
  const char *unit;
  const char *entity_category; // NULL, "diagnostic" or "config"
  int display_precision;       // -1 to leave unset
} ha_sensor_config_t;

StaticTask_t homeAssistantTaskBuffer;
StackType_t homeAssistantTaskStack[HOME_ASSISTANT_STACK_SIZE];

EventGroupHandle_t home_assistant_event_group_handle;
static StaticEventGroup_t home_assistant_event_group;

static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;
static char mac_slug[MQTT_MAC_SLUG_LEN] = {0};
static char device_id[MQTT_DEVICE_ID_LEN] = {0};
static char base_topic[MQTT_BASE_TOPIC_LEN] = {0};
static char state_topic[MQTT_TOPIC_LEN] = {0};
static char availability_topic[MQTT_TOPIC_LEN] = {0};

// Status line pushed from Home Assistant, shown under the clock in place of
// the date. Guarded by status_lock because the display task reads it.
static char status_text[HOME_ASSISTANT_STATUS_MAX_LEN] = {0};
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

static const ha_sensor_config_t ha_sensors[] = {
    {.slug = "temperature",
     .name = "Temperature",
     .state_field = "temperature_c",
     .device_class = "temperature",
     .unit = UNIT_TEMP_C,
     .display_precision = 1},
    {.slug = "humidity",
     .name = "Humidity",
     .state_field = "humidity",
     .device_class = "humidity",
     .unit = "%",
     .display_precision = 0},
    {.slug = "co2",
     .name = "CO2",
     .state_field = "co2",
     .device_class = "carbon_dioxide",
     .unit = "ppm",
     .display_precision = 0},
    {.slug = "voc_index",
     .name = "VOC Index",
     .state_field = "voc_index",
     .display_precision = 0},
    {.slug = "nox_index",
     .name = "NOx Index",
     .state_field = "nox_index",
     .display_precision = 0},
    {.slug = "rssi",
     .name = "Wi-Fi signal",
     .state_field = "rssi",
     .device_class = "signal_strength",
     .unit = "dBm",
     .entity_category = "diagnostic",
     .display_precision = 0},
    {.slug = "uptime",
     .name = "Uptime",
     .state_field = "uptime_s",
     .device_class = "duration",
     .unit = "s",
     .entity_category = "diagnostic",
     .display_precision = 0},
    {.slug = "co2_calibration_correction",
     .name = "CO2 calibration correction",
     .state_field = "co2_frc_correction",
     .unit = "ppm",
     .entity_category = "diagnostic",
     .display_precision = 0},
};

//
// Status text
//

void home_assistant_get_status_text(char *dst, size_t dst_size) {
  if (dst_size == 0) {
    return;
  }
  taskENTER_CRITICAL(&status_lock);
  strlcpy(dst, status_text, dst_size);
  taskEXIT_CRITICAL(&status_lock);
}

static void set_status_text(const char *src, size_t len) {
  char clean[HOME_ASSISTANT_STATUS_MAX_LEN] = {0};
  size_t out = 0;
  // The sublabel font only has upper case, digits and a little punctuation;
  // upper-case here and let unknown glyphs render blank.
  for (size_t i = 0; i < len && out < sizeof(clean) - 1; i++) {
    char c = src[i];
    if (c >= 'a' && c <= 'z') {
      c = c - 'a' + 'A';
    }
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    clean[out++] = c;
  }
  taskENTER_CRITICAL(&status_lock);
  strlcpy(status_text, clean, sizeof(status_text));
  taskEXIT_CRITICAL(&status_lock);
  ESP_LOGI(TAG, "Status text: \"%s\"", clean);
}

//
// Topics and discovery
//

static void init_mqtt_topics() {
  uint8_t mac[6] = {0};
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  snprintf(mac_slug, sizeof(mac_slug), "%02x%02x%02x%02x%02x%02x", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(device_id, sizeof(device_id), "bitclock_%s", mac_slug);
  snprintf(base_topic, sizeof(base_topic), "bitclock/%s", mac_slug);
  snprintf(state_topic, sizeof(state_topic), "%s/state", base_topic);
  snprintf(availability_topic, sizeof(availability_topic), "%s/availability",
           base_topic);
}

static void topic_for(char *dst, size_t dst_size, const char *suffix) {
  if (snprintf(dst, dst_size, "%s/%s", base_topic, suffix) >= (int)dst_size) {
    ESP_LOGE(TAG, "Topic for %s truncated", suffix);
  }
}

static cJSON *create_entity_base(const char *slug, const char *name) {
  char unique_id[MQTT_TOPIC_LEN] = {0};
  snprintf(unique_id, sizeof(unique_id), "%s_%s", device_id, slug);

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return NULL;
  }
  cJSON_AddStringToObject(root, "name", name);
  cJSON_AddStringToObject(root, "unique_id", unique_id);
  cJSON_AddStringToObject(root, "availability_topic", availability_topic);
  cJSON_AddStringToObject(root, "payload_available", "online");
  cJSON_AddStringToObject(root, "payload_not_available", "offline");

  cJSON *device = cJSON_AddObjectToObject(root, "device");
  cJSON *identifiers = cJSON_AddArrayToObject(device, "identifiers");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id));
  cJSON_AddStringToObject(device, "name", "Bitclock");
  cJSON_AddStringToObject(device, "manufacturer", "Bitclock");
  cJSON_AddStringToObject(device, "model", "Bitclock");
  cJSON_AddStringToObject(device, "sw_version",
                          esp_app_get_description()->version);
  cJSON_AddStringToObject(device, "configuration_url",
                          "https://bitclock.io/connect");
  return root;
}

static void publish_config(const char *component, const char *slug,
                           cJSON *root) {
  char config_topic[MQTT_TOPIC_LEN] = {0};
  snprintf(config_topic, sizeof(config_topic), "homeassistant/%s/%s_%s/config",
           component, device_id, slug);
  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (payload == NULL) {
    ESP_LOGE(TAG, "Failed to create discovery payload for %s", slug);
    return;
  }
  int msg_id =
      esp_mqtt_client_publish(mqtt_client, config_topic, payload, 0, 1, true);
  ESP_LOGI(TAG, "Published discovery for %s, msg_id=%d", slug, msg_id);
  cJSON_free(payload);
}

static void publish_sensor_discovery(const ha_sensor_config_t *sensor) {
  cJSON *root = create_entity_base(sensor->slug, sensor->name);
  if (root == NULL) {
    return;
  }
  char value_template[MQTT_TOPIC_LEN] = {0};
  snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}",
           sensor->state_field);
  cJSON_AddStringToObject(root, "state_topic", state_topic);
  cJSON_AddStringToObject(root, "value_template", value_template);
  cJSON_AddStringToObject(root, "state_class", "measurement");
  if (sensor->device_class != NULL) {
    cJSON_AddStringToObject(root, "device_class", sensor->device_class);
  }
  if (sensor->unit != NULL) {
    cJSON_AddStringToObject(root, "unit_of_measurement", sensor->unit);
  }
  if (sensor->entity_category != NULL) {
    cJSON_AddStringToObject(root, "entity_category", sensor->entity_category);
  }
  if (sensor->display_precision >= 0) {
    cJSON_AddNumberToObject(root, "suggested_display_precision",
                            sensor->display_precision);
  }
  publish_config("sensor", sensor->slug, root);
}

static void publish_discovery() {
  char topic[MQTT_TOPIC_LEN] = {0};

  for (size_t i = 0; i < sizeof(ha_sensors) / sizeof(ha_sensors[0]); i++) {
    publish_sensor_discovery(&ha_sensors[i]);
  }

  // Air quality alert the display already computes, as text.
  cJSON *root = create_entity_base("air_quality_alert", "Air quality alert");
  if (root != NULL) {
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.alert }}");
    cJSON_AddStringToObject(root, "icon", "mdi:fan-alert");
    publish_config("sensor", "air_quality_alert", root);
  }

  // Temperature correction, for matching a reference sensor.
  root = create_entity_base("temperature_offset", "Temperature offset");
  if (root != NULL) {
    topic_for(topic, sizeof(topic), "temperature_offset/set");
    cJSON_AddStringToObject(root, "command_topic", topic);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template",
                            "{{ value_json.temperature_offset }}");
    cJSON_AddNumberToObject(root, "min",
                            BITCLOCK_NVS_TEMP_OFFSET_MIN_TENTHS / 10.0);
    cJSON_AddNumberToObject(root, "max",
                            BITCLOCK_NVS_TEMP_OFFSET_MAX_TENTHS / 10.0);
    cJSON_AddNumberToObject(root, "step", 0.1);
    cJSON_AddStringToObject(root, "mode", "box");
    cJSON_AddStringToObject(root, "unit_of_measurement", UNIT_TEMP_C);
    cJSON_AddStringToObject(root, "device_class", "temperature");
    cJSON_AddStringToObject(root, "entity_category", "config");
    publish_config("number", "temperature_offset", root);
  }

  // CO2 forced recalibration to outdoor air.
  root = create_entity_base("co2_calibrate", "Calibrate CO2 to outdoor air");
  if (root != NULL) {
    topic_for(topic, sizeof(topic), "co2_calibrate/press");
    cJSON_AddStringToObject(root, "command_topic", topic);
    cJSON_AddStringToObject(root, "entity_category", "config");
    cJSON_AddStringToObject(root, "icon", "mdi:molecule-co2");
    publish_config("button", "co2_calibrate", root);
  }

  // Clock face.
  root = create_entity_base("clock_face", "Clock face");
  if (root != NULL) {
    topic_for(topic, sizeof(topic), "clock_face/set");
    cJSON_AddStringToObject(root, "command_topic", topic);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template",
                            "{{ value_json.clock_face }}");
    cJSON *options = cJSON_AddArrayToObject(root, "options");
    cJSON_AddItemToArray(options, cJSON_CreateString("Clock"));
    cJSON_AddItemToArray(options, cJSON_CreateString("Weather"));
    cJSON_AddStringToObject(root, "icon", "mdi:clock-digital");
    publish_config("select", "clock_face", root);
  }

  // Status line under the clock. Home Assistant automations usually publish
  // it retained to status/set; the text entity makes it settable by hand too.
  root = create_entity_base("status", "Status line");
  if (root != NULL) {
    topic_for(topic, sizeof(topic), "status/set");
    cJSON_AddStringToObject(root, "command_topic", topic);
    cJSON_AddBoolToObject(root, "retain", true);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "value_template", "{{ value_json.status }}");
    cJSON_AddNumberToObject(root, "max", HOME_ASSISTANT_STATUS_MAX_LEN - 1);
    cJSON_AddStringToObject(root, "icon", "mdi:message-text");
    publish_config("text", "status", root);
  }
}

static void subscribe_commands() {
  char topic[MQTT_TOPIC_LEN] = {0};
  topic_for(topic, sizeof(topic), "+/set");
  esp_mqtt_client_subscribe(mqtt_client, topic, 1);
  topic_for(topic, sizeof(topic), "co2_calibrate/press");
  esp_mqtt_client_subscribe(mqtt_client, topic, 1);
}

//
// State
//

// Round in double so cJSON prints 24.8 rather than 24.799999237060547.
static double round_to(double value, double places) {
  double scale = pow(10, places);
  return round(value * scale) / scale;
}

static const char *alert_name(aqi_alert_reason_t reason) {
  switch (reason) {
  case AQI_ALERT_TEMP_HIGH:
    return "Hot";
  case AQI_ALERT_TEMP_LOW:
    return "Cold";
  case AQI_ALERT_HUMDIITY_HIGH:
    return "Humid";
  case AQI_ALERT_HUMDIITY_LOW:
    return "Dry";
  case AQI_ALERT_CO2_HIGH:
    return "CO2";
  case AQI_ALERT_NOX_HIGH:
    return "NOx";
  case AQI_ALERT_VOC_HIGH:
    return "VOC";
  case AQI_ALERT_NONE:
  case AQI_ALERT_COUNT:
    break;
  }
  return "None";
}

static void publish_state() {
  if (!mqtt_connected || mqtt_client == NULL) {
    return;
  }

  bool has_readings = false;
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return;
  }

  aqi_data_t aqi_data = {0};
  if (sht4x_current_temp_available()) {
    aqi_data.temp_celsius = sht4x_current_temp_celsius();
    aqi_data.humidity = sht4x_current_relative_humidity();
    cJSON_AddNumberToObject(root, "temperature_c",
                            round_to(aqi_data.temp_celsius, 1));
    cJSON_AddNumberToObject(root, "humidity", round_to(aqi_data.humidity, 1));
    has_readings = true;
  }

  if (scd4x_current_ppm_available()) {
    aqi_data.co2_ppm = scd4x_current_co2_ppm();
    cJSON_AddNumberToObject(root, "co2", aqi_data.co2_ppm);
    has_readings = true;
  }

  if (sgp4x_current_voc_available()) {
    aqi_data.voc_index = sgp4x_current_voc_index();
    cJSON_AddNumberToObject(root, "voc_index", aqi_data.voc_index);
    has_readings = true;
  }

  if (sgp41_current_nox_available()) {
    aqi_data.nox_index = sgp41_current_nox_index();
    cJSON_AddNumberToObject(root, "nox_index", aqi_data.nox_index);
    has_readings = true;
  }

  if (!has_readings) {
    cJSON_Delete(root);
    return;
  }

  cJSON_AddStringToObject(root, "alert",
                          alert_name(aqi_alerts_check(&aqi_data)));

  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
  }
  cJSON_AddNumberToObject(root, "uptime_s",
                          (double)(esp_timer_get_time() / 1000000));

  cJSON_AddNumberToObject(root, "temperature_offset",
                          bitclock_nvs_get_temp_offset_tenths() / 10.0);

  int16_t frc_correction = 0;
  if (scd4x_last_forced_recalibration(&frc_correction) == ESP_OK) {
    cJSON_AddNumberToObject(root, "co2_frc_correction", frc_correction);
  }

  cJSON_AddStringToObject(root, "clock_face",
                          bitclock_nvs_get_app_selection() ==
                                  BITCLOCK_NVS_APP_SELECTION_VAL_WEATHER
                              ? "Weather"
                              : "Clock");

  char status[HOME_ASSISTANT_STATUS_MAX_LEN];
  home_assistant_get_status_text(status, sizeof(status));
  cJSON_AddStringToObject(root, "status", status);

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (payload == NULL) {
    return;
  }

  int msg_id =
      esp_mqtt_client_publish(mqtt_client, state_topic, payload, 0, 0, true);
  ESP_LOGD(TAG, "Published state, msg_id=%d", msg_id);
  cJSON_free(payload);
}

//
// Commands
//

// Home Assistant weather conditions to the clock's icons.
static weather_icon_t icon_for_condition(const char *condition) {
  static const struct {
    const char *condition;
    weather_icon_t icon;
  } map[] = {
      {"clear-night", WEATHER_ICON_CLEAR_NIGHT},
      {"cloudy", WEATHER_ICON_CLOUD},
      {"exceptional", WEATHER_ICON_CLOUD},
      {"fog", WEATHER_ICON_FOGGY},
      {"hail", WEATHER_ICON_SNOWY},
      {"lightning-rainy", WEATHER_ICON_THUNDERSTORM},
      {"lightning", WEATHER_ICON_THUNDERSTORM},
      {"partlycloudy", WEATHER_ICON_PARTLY_CLOUDY_DAY},
      {"pouring", WEATHER_ICON_RAINY},
      {"rainy", WEATHER_ICON_RAINY},
      {"snowy-rainy", WEATHER_ICON_SNOWY},
      {"snowy", WEATHER_ICON_SNOWY},
      {"sunny", WEATHER_ICON_SUNNY},
      {"windy-variant", WEATHER_ICON_CLOUD},
      {"windy", WEATHER_ICON_PARTLY_CLOUDY_DAY},
  };
  if (condition == NULL) {
    return WEATHER_ICON_NONE;
  }
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    if (strcmp(condition, map[i].condition) == 0) {
      return map[i].icon;
    }
  }
  return WEATHER_ICON_CLOUD;
}

// {"high": 12, "low": 6, "condition": "rainy"}, temperatures in Celsius.
static void handle_weather(const char *payload) {
  cJSON *root = cJSON_Parse(payload);
  if (root == NULL) {
    ESP_LOGW(TAG, "Ignoring weather that is not JSON");
    return;
  }
  cJSON *high = cJSON_GetObjectItem(root, "high");
  cJSON *low = cJSON_GetObjectItem(root, "low");
  cJSON *condition = cJSON_GetObjectItem(root, "condition");
  if (cJSON_IsNumber(high) && cJSON_IsNumber(low)) {
    weather_set_from_home_assistant(
        (int16_t)lround(high->valuedouble), (int16_t)lround(low->valuedouble),
        icon_for_condition(cJSON_IsString(condition) ? condition->valuestring
                                                     : NULL));
  } else {
    ESP_LOGW(TAG, "Weather needs numeric high and low");
  }
  cJSON_Delete(root);
}

static void handle_command(const char *command, const char *payload,
                           size_t payload_len) {
  ESP_LOGI(TAG, "Command %s", command);

  if (strcmp(command, "status/set") == 0) {
    set_status_text(payload, payload_len);
  } else if (strcmp(command, "weather/set") == 0) {
    handle_weather(payload);
  } else if (strcmp(command, "temperature_offset/set") == 0) {
    double degrees = strtod(payload, NULL);
    esp_err_t err = bitclock_nvs_set_temp_offset_tenths(
        (int16_t)lround(degrees * 10.0));
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Temperature offset %.1f rejected: %s", degrees,
               esp_err_to_name(err));
    }
  } else if (strcmp(command, "clock_face/set") == 0) {
    if (strcmp(payload, "Weather") == 0) {
      bitclock_nvs_set_app_selection(BITCLOCK_NVS_APP_SELECTION_VAL_WEATHER);
    } else if (strcmp(payload, "Clock") == 0) {
      bitclock_nvs_set_app_selection(BITCLOCK_NVS_APP_SELECTION_VAL_CLOCK);
    }
  } else if (strcmp(command, "co2_calibrate/press") == 0) {
    esp_err_t err =
        scd4x_request_forced_recalibration(CO2_CALIBRATION_TARGET_PPM);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "CO2 calibration not started: %s", esp_err_to_name(err));
    }
  } else {
    return;
  }

  // Echo the new state promptly rather than at the next 30 s publish.
  if (home_assistant_event_group_handle != NULL) {
    xEventGroupSetBits(home_assistant_event_group_handle,
                       HOME_ASSISTANT_PUBLISH_NOW_EVENT);
  }
}

static void handle_data(esp_mqtt_event_handle_t event) {
  // Multi-part messages are larger than any command we accept.
  if (event->current_data_offset != 0 ||
      event->data_len != event->total_data_len) {
    return;
  }

  size_t base_len = strlen(base_topic);
  if (event->topic_len <= (int)base_len + 1 ||
      strncmp(event->topic, base_topic, base_len) != 0 ||
      event->topic[base_len] != '/') {
    return;
  }

  char command[MQTT_TOPIC_LEN] = {0};
  size_t command_len = event->topic_len - base_len - 1;
  if (command_len >= sizeof(command)) {
    return;
  }
  memcpy(command, event->topic + base_len + 1, command_len);

  char payload[MQTT_PAYLOAD_MAX_LEN] = {0};
  size_t payload_len = event->data_len;
  if (payload_len >= sizeof(payload)) {
    payload_len = sizeof(payload) - 1;
  }
  memcpy(payload, event->data, payload_len);

  handle_command(command, payload, payload_len);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    mqtt_connected = true;
    ESP_LOGI(TAG, "MQTT connected");
    esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 1,
                            true);
    publish_discovery();
    subscribe_commands();
    if (home_assistant_event_group_handle != NULL) {
      xEventGroupSetBits(home_assistant_event_group_handle,
                         HOME_ASSISTANT_PUBLISH_NOW_EVENT);
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT disconnected");
    break;
  case MQTT_EVENT_DATA:
    handle_data((esp_mqtt_event_handle_t)event_data);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error");
    break;
  default:
    break;
  }
}

static void stop_mqtt_client() {
  if (mqtt_client == NULL) {
    return;
  }

  if (mqtt_connected) {
    esp_mqtt_client_publish(mqtt_client, availability_topic, "offline", 0, 1,
                            true);
  }

  mqtt_connected = false;
  esp_mqtt_client_stop(mqtt_client);
  esp_mqtt_client_destroy(mqtt_client);
  mqtt_client = NULL;
}

static esp_err_t start_mqtt_client() {
  const char *uri = bitclock_nvs_get_ha_mqtt_uri();
  const char *username = bitclock_nvs_get_ha_mqtt_username();
  const char *password = bitclock_nvs_get_ha_mqtt_password();

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = uri,
      .credentials.username = strlen(username) > 0 ? username : NULL,
      .credentials.authentication.password =
          strlen(password) > 0 ? password : NULL,
      .session.last_will.topic = availability_topic,
      .session.last_will.msg = "offline",
      .session.last_will.qos = 1,
      .session.last_will.retain = true,
  };

  mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  if (mqtt_client == NULL) {
    return ESP_FAIL;
  }

  ESP_ERROR_CHECK(esp_mqtt_client_register_event(
      mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));

  return esp_mqtt_client_start(mqtt_client);
}

void home_assistant_notify_config_update() {
  if (home_assistant_event_group_handle == NULL) {
    return;
  }

  xEventGroupSetBits(home_assistant_event_group_handle,
                     HOME_ASSISTANT_CONFIG_UPDATED_EVENT);
}

void home_assistant_task_run(void *pvParameters) {
  home_assistant_event_group_handle =
      xEventGroupCreateStatic(&home_assistant_event_group);
  init_mqtt_topics();

  while (1) {
    bool enabled = bitclock_nvs_get_ha_mqtt_enabled();
    const char *uri = bitclock_nvs_get_ha_mqtt_uri();

    if (!enabled || uri == NULL || strlen(uri) == 0) {
      stop_mqtt_client();
      xEventGroupWaitBits(home_assistant_event_group_handle,
                          HOME_ASSISTANT_CONFIG_UPDATED_EVENT,
                          true,  // clear on exit
                          false, // wait for all
                          portMAX_DELAY);
      continue;
    }

    if (!bitclock_wifi_has_ip()) {
      stop_mqtt_client();
      xEventGroupWaitBits(home_assistant_event_group_handle,
                          HOME_ASSISTANT_CONFIG_UPDATED_EVENT,
                          true,  // clear on exit
                          false, // wait for all
                          MQTT_RETRY_INTERVAL);
      continue;
    }

    if (mqtt_client == NULL) {
      esp_err_t err = start_mqtt_client();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
        stop_mqtt_client();
        xEventGroupWaitBits(home_assistant_event_group_handle,
                            HOME_ASSISTANT_CONFIG_UPDATED_EVENT,
                            true,  // clear on exit
                            false, // wait for all
                            MQTT_RETRY_INTERVAL);
        continue;
      }
    }

    EventBits_t bits = xEventGroupWaitBits(
        home_assistant_event_group_handle,
        HOME_ASSISTANT_CONFIG_UPDATED_EVENT | HOME_ASSISTANT_PUBLISH_NOW_EVENT,
        true,  // clear on exit
        false, // wait for all
        MQTT_PUBLISH_INTERVAL);

    if (bits & HOME_ASSISTANT_CONFIG_UPDATED_EVENT) {
      stop_mqtt_client();
      continue;
    }

    publish_state();
  }
}
