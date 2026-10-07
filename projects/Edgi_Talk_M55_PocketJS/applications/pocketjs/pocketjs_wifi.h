#ifndef POCKETJS_WIFI_H
#define POCKETJS_WIFI_H

#include <rtthread.h>

typedef struct
{
    rt_bool_t ap_active;
    char ap_ssid[33];
    char ap_password[17];
    char sta_ip[16];
} pocketjs_wifi_status_t;

void pocketjs_wifi_start(void);
void pocketjs_wifi_status(pocketjs_wifi_status_t *status);

#endif
