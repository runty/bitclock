#include "weather.h"

#include "esp_log.h"
#include "libs/nvs.h"
#include "tasks/wifi.h"
#include <string.h>
#include <time.h>

static const char *TAG = "weather-task";

StaticTask_t weatherTaskBuffer;
StackType_t weatherTaskStack[WEATHER_STACK_SIZE];

EventGroupHandle_t weather_event_group_handle;
static StaticEventGroup_t weather_event_group;

wifi_weather_t latest_weather;

// When Home Assistant last pushed a forecast (0 = never).
static time_t home_assistant_weather_at = 0;

static bool home_assistant_weather_is_fresh() {
  if (home_assistant_weather_at == 0) {
    return false;
  }
  time_t now;
  time(&now);
  return now - home_assistant_weather_at < WEATHER_HOME_ASSISTANT_FRESH_S;
}

void weather_set_from_home_assistant(int16_t high_c, int16_t low_c,
                                     weather_icon_t icon) {
  latest_weather.temp_celsius = true;
  latest_weather.temp_max = high_c;
  latest_weather.temp_min = low_c;
  latest_weather.icon = icon;
  time(&latest_weather.requested_at);
  home_assistant_weather_at = latest_weather.requested_at;
  ESP_LOGI(TAG, "Forecast from Home Assistant: %d/%d C, icon %d", high_c,
           low_c, icon);
}

void weather_task_run(void *pvParameters) {
  weather_event_group_handle = xEventGroupCreateStatic(&weather_event_group);
  TickType_t delay_time = portMAX_DELAY;
  time_t now;
  struct tm timeinfo;

  while (1) {
    xEventGroupWaitBits(weather_event_group_handle,
                        WEATHER_EVENT_WIFI_CONNECTED |
                            WEATHER_EVENT_LOCATION_CHANGED,
                        true,  // clear = true
                        false, // wait for all = false
                        delay_time);

    if (home_assistant_weather_is_fresh()) {
      delay_time = pdMS_TO_TICKS(1000 * 60 * 30);
      continue;
    }

    const char *path = bitclock_nvs_get_weather_path();

    if (path == NULL || strlen(path) < 1) {
      ESP_LOGW(TAG, "No location so defaulting to San Francisco");
      path = "/gridpoints/MTR/85,105/forecast";
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < 2024 - 1900) {
      ESP_LOGW(TAG, "Time not synced yet, unable to show forecast");
      delay_time = pdMS_TO_TICKS(1000 * 1);
      continue;
    }

    esp_err_t refresh_error = ESP_FAIL;
    if (xSemaphoreTake(wifi_req_semaphore, 0)) {
      refresh_error = refresh_daily_weather(&latest_weather, path, &timeinfo);

      xSemaphoreGive(wifi_req_semaphore);
    }

    if (!refresh_error) {
      delay_time = pdMS_TO_TICKS(1000 * 60 * 30); // Sync every 30 minutes
    } else {
      delay_time = pdMS_TO_TICKS(1000 * 30); // Try again in 30 seconds
    }
  }
}
