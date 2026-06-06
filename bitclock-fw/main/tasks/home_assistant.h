#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define HOME_ASSISTANT_STACK_SIZE 1024 * 6
#define HOME_ASSISTANT_CONFIG_UPDATED_EVENT BIT0
#define HOME_ASSISTANT_PUBLISH_NOW_EVENT BIT1

extern StaticTask_t homeAssistantTaskBuffer;
extern StackType_t homeAssistantTaskStack[HOME_ASSISTANT_STACK_SIZE];
extern EventGroupHandle_t home_assistant_event_group_handle;

void home_assistant_task_run(void *pvParameters);
void home_assistant_notify_config_update();
