#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// 4kB
#define SCD4X_STACK_SIZE 1024 * 4

extern StaticTask_t scd4xTaskBuffer;
extern StackType_t scd4xTaskStack[SCD4X_STACK_SIZE];

bool scd4x_current_ppm_available();

uint16_t scd4x_current_co2_ppm();

// Queue a forced recalibration to target_ppm (400-2000), run by the sensor task
// between readings. ESP_ERR_INVALID_STATE until it has measured for 3 minutes.
esp_err_t scd4x_request_forced_recalibration(uint16_t target_ppm);
// Result of the last FRC: ESP_ERR_NOT_FINISHED if none has run yet.
esp_err_t scd4x_last_forced_recalibration(int16_t *correction_ppm);

void scd4x_task_run(void *pvParameters);
