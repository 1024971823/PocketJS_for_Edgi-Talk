#pragma once

#include "quickjs.h"

/* The embedded runtime needs diagnostics but intentionally omits POSIX libc. */
void js_std_init_handlers(JSRuntime *runtime);
void js_std_add_helpers(JSContext *context, int argc, char **argv);
void js_std_free_handlers(JSRuntime *runtime);
void js_std_dump_error(JSContext *context);
