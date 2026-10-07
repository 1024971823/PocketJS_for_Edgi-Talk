#pragma once

#include "esp_err.h"
#include "quickjs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pocketjs_dashboard_start(void);
void pocketjs_dashboard_note_frame(void);

/* Set the wall clock from a UTC unix time and a local offset in minutes. A time pushed by the
 * PC outranks the one read from the weather server's HTTP Date header for ten minutes. */
bool pocketjs_dashboard_set_time(int64_t unix_utc, int tz_minutes, bool from_pc);

/* Stats pushed by the PC console (PUT /api/pc). `json` is a flat object; returns false when it
 * carries no usable CPU figure. A "t" (unix seconds) and "tz" (minutes) pair also sets the clock. */
bool pocketjs_dashboard_pc_ingest(const char *json);
/* GET /api/pc body, including the age of the last update. Returns the length written. */
size_t pocketjs_dashboard_pc_json(char *out, size_t capacity);
esp_err_t pocketjs_dashboard_install(JSContext *context, void *user_data);

#ifdef __cplusplus
}
#endif
