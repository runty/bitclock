#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <stddef.h>

#define HOME_ASSISTANT_STACK_SIZE 1024 * 8
#define HOME_ASSISTANT_CONFIG_UPDATED_EVENT BIT0
#define HOME_ASSISTANT_PUBLISH_NOW_EVENT BIT1

// Status line from Home Assistant, including the terminator. About as much as
// fits under the clock in the sublabel font.
#define HOME_ASSISTANT_STATUS_MAX_LEN 25

extern StaticTask_t homeAssistantTaskBuffer;
extern StackType_t homeAssistantTaskStack[HOME_ASSISTANT_STACK_SIZE];
extern EventGroupHandle_t home_assistant_event_group_handle;

void home_assistant_task_run(void *pvParameters);
void home_assistant_notify_config_update();

// Copies the current status line (empty when none) for the display.
void home_assistant_get_status_text(char *dst, size_t dst_size);
