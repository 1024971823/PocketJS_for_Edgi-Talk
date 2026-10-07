#pragma once

#include <rtthread.h>

rt_err_t pocketjs_app_start(void);
rt_err_t pocketjs_app_wait_ready(rt_int32_t timeout);
