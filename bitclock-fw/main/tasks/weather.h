#include "freertos/FreeRTOS.h"

#include "libs/weather_api.h"

#define WEATHER_STACK_SIZE 1024 * 4

#define WEATHER_EVENT_WIFI_CONNECTED BIT0
#define WEATHER_EVENT_LOCATION_CHANGED BIT1

extern EventGroupHandle_t weather_event_group_handle;

extern StaticTask_t weatherTaskBuffer;
extern StackType_t weatherTaskStack[WEATHER_STACK_SIZE];

extern wifi_weather_t latest_weather;

void weather_task_run(void *pvParameters);

// Forecast pushed by Home Assistant (Celsius). While one is fresher than
// WEATHER_HOME_ASSISTANT_FRESH_S the task stops polling weather.gov, which only
// covers the US.
#define WEATHER_HOME_ASSISTANT_FRESH_S (60 * 60 * 3)
void weather_set_from_home_assistant(int16_t high_c, int16_t low_c,
                                     weather_icon_t icon);
