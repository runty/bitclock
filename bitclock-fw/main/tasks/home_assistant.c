#include "home_assistant.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "libs/nvs.h"
#include "mqtt_client.h"
#include "tasks/scd4x.h"
#include "tasks/sgp4x.h"
#include "tasks/sht4x.h"
#include "tasks/wifi.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "home-assistant";

#define MQTT_PUBLISH_INTERVAL pdMS_TO_TICKS(1000 * 30)
#define MQTT_RETRY_INTERVAL pdMS_TO_TICKS(1000 * 5)
#define MQTT_TOPIC_LEN 128
#define MQTT_DEVICE_ID_LEN 32
#define MQTT_MAC_SLUG_LEN 16
#define UNIT_TEMP_C "\xC2\xB0" "C"

typedef struct {
  const char *slug;
  const char *name;
  const char *state_field;
  const char *device_class;
  const char *unit;
} ha_sensor_config_t;

StaticTask_t homeAssistantTaskBuffer;
StackType_t homeAssistantTaskStack[HOME_ASSISTANT_STACK_SIZE];

EventGroupHandle_t home_assistant_event_group_handle;
static StaticEventGroup_t home_assistant_event_group;

static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;

static char mac_slug[MQTT_MAC_SLUG_LEN] = {0};
static char device_id[MQTT_DEVICE_ID_LEN] = {0};
static char state_topic[MQTT_TOPIC_LEN] = {0};
static char availability_topic[MQTT_TOPIC_LEN] = {0};

static const ha_sensor_config_t ha_sensors[] = {
    {.slug = "temperature",
     .name = "Temperature",
     .state_field = "temperature_c",
     .device_class = "temperature",
     .unit = UNIT_TEMP_C},
    {.slug = "humidity",
     .name = "Humidity",
     .state_field = "humidity",
     .device_class = "humidity",
     .unit = "%"},
    {.slug = "co2",
     .name = "CO2",
     .state_field = "co2",
     .device_class = "carbon_dioxide",
     .unit = "ppm"},
    {.slug = "voc_index",
     .name = "VOC Index",
     .state_field = "voc_index",
     .device_class = NULL,
     .unit = NULL},
    {.slug = "nox_index",
     .name = "NOx Index",
     .state_field = "nox_index",
     .device_class = NULL,
     .unit = NULL},
};

static void init_mqtt_topics() {
  uint8_t mac[6] = {0};
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

  snprintf(mac_slug, sizeof(mac_slug), "%02x%02x%02x%02x%02x%02x", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(device_id, sizeof(device_id), "bitclock_%s", mac_slug);
  snprintf(state_topic, sizeof(state_topic), "bitclock/%s/state", mac_slug);
  snprintf(availability_topic, sizeof(availability_topic),
           "bitclock/%s/availability", mac_slug);
}

static char *create_discovery_payload(const ha_sensor_config_t *sensor) {
  char unique_id[MQTT_TOPIC_LEN] = {0};
  char value_template[MQTT_TOPIC_LEN] = {0};

  snprintf(unique_id, sizeof(unique_id), "%s_%s", device_id, sensor->slug);
  snprintf(value_template, sizeof(value_template), "{{ value_json.%s }}",
           sensor->state_field);

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return NULL;
  }

  cJSON_AddStringToObject(root, "name", sensor->name);
  cJSON_AddStringToObject(root, "unique_id", unique_id);
  cJSON_AddStringToObject(root, "state_topic", state_topic);
  cJSON_AddStringToObject(root, "availability_topic", availability_topic);
  cJSON_AddStringToObject(root, "payload_available", "online");
  cJSON_AddStringToObject(root, "payload_not_available", "offline");
  cJSON_AddStringToObject(root, "value_template", value_template);
  cJSON_AddStringToObject(root, "state_class", "measurement");

  if (sensor->device_class != NULL) {
    cJSON_AddStringToObject(root, "device_class", sensor->device_class);
  }
  if (sensor->unit != NULL) {
    cJSON_AddStringToObject(root, "unit_of_measurement", sensor->unit);
  }

  cJSON *device = cJSON_AddObjectToObject(root, "device");
  cJSON *identifiers = cJSON_AddArrayToObject(device, "identifiers");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id));
  cJSON_AddStringToObject(device, "name", "Bitclock");
  cJSON_AddStringToObject(device, "manufacturer", "Bitclock");
  cJSON_AddStringToObject(device, "model", "Bitclock");
  cJSON_AddStringToObject(device, "configuration_url",
                          "https://bitclock.io/connect");

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  return payload;
}

static void publish_discovery() {
  char config_topic[MQTT_TOPIC_LEN] = {0};

  for (size_t i = 0; i < sizeof(ha_sensors) / sizeof(ha_sensors[0]); i++) {
    const ha_sensor_config_t *sensor = &ha_sensors[i];
    snprintf(config_topic, sizeof(config_topic),
             "homeassistant/sensor/%s_%s/config", device_id, sensor->slug);

    char *payload = create_discovery_payload(sensor);
    if (payload == NULL) {
      ESP_LOGE(TAG, "Failed to create discovery payload for %s", sensor->slug);
      continue;
    }

    int msg_id = esp_mqtt_client_publish(mqtt_client, config_topic, payload, 0,
                                         1, true);
    ESP_LOGI(TAG, "Published discovery for %s, msg_id=%d", sensor->slug,
             msg_id);
    cJSON_free(payload);
  }
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

  if (sht4x_current_temp_available()) {
    cJSON_AddNumberToObject(root, "temperature_c",
                            sht4x_current_temp_celsius());
    cJSON_AddNumberToObject(root, "humidity",
                            sht4x_current_relative_humidity());
    has_readings = true;
  }

  if (scd4x_current_ppm_available()) {
    cJSON_AddNumberToObject(root, "co2", scd4x_current_co2_ppm());
    has_readings = true;
  }

  if (sgp4x_current_voc_available()) {
    cJSON_AddNumberToObject(root, "voc_index", sgp4x_current_voc_index());
    has_readings = true;
  }

  if (sgp41_current_nox_available()) {
    cJSON_AddNumberToObject(root, "nox_index", sgp41_current_nox_index());
    has_readings = true;
  }

  if (!has_readings) {
    cJSON_Delete(root);
    return;
  }

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

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    mqtt_connected = true;
    ESP_LOGI(TAG, "MQTT connected");
    esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 1,
                            true);
    publish_discovery();
    if (home_assistant_event_group_handle != NULL) {
      xEventGroupSetBits(home_assistant_event_group_handle,
                         HOME_ASSISTANT_PUBLISH_NOW_EVENT);
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT disconnected");
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
