#pragma once

#include <rtthread.h>
#include <stdbool.h>

/* Bring up the CYW55513 over its HCI UART, advertise as "EdgiTalk", and accept
 * one PC-stats write. Safe to call once after Wi-Fi has started. */
void pocketjs_bt_start(void);
bool pocketjs_bt_advertising(void);
