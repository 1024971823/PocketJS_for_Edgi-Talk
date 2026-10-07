#pragma once

#include <rtthread.h>

#define ESP_LOGE(tag, format, ...) rt_kprintf("[%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) rt_kprintf("[%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) rt_kprintf("[%s] " format "\n", tag, ##__VA_ARGS__)
