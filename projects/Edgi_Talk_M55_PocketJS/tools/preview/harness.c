/*
 * Desktop preview harness: runs the embedded PocketJS package with the same
 * native UI core and RGB565 renderer as the firmware, with a scripted
 * `__edgi` bridge and touch input, and dumps chosen frames as PPM images.
 *
 *   harness <prelude.js> <scenario.js> <outdir> <frames>
 */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs_package_edgitalk_smoke.h"
#include "esp_heap_caps.h"

void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    (void)caps;
    void *p = NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
void heap_caps_free(void *pointer) { free(pointer); }

extern const uint8_t pocketjs_package_edgitalk_smoke_end[];

/* Diagnostics stubs normally provided by the firmware's quickjs-libc shim. */
void js_std_init_handlers(JSRuntime *runtime) { (void)runtime; }
void js_std_add_helpers(JSContext *c, int argc, char **argv) { (void)c; (void)argc; (void)argv; }
void js_std_free_handlers(JSRuntime *runtime) { (void)runtime; }
void js_std_dump_error(JSContext *context)
{
    JSValue exception = JS_GetException(context);
    const char *text = JS_ToCString(context, exception);
    fprintf(stderr, "[js error] %s\n", text ? text : "<unprintable>");
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    const char *stack_text = JS_ToCString(context, stack);
    if (stack_text) fprintf(stderr, "%s\n", stack_text);
    if (stack_text) JS_FreeCString(context, stack_text);
    JS_FreeValue(context, stack);
    if (text) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
}

static JSContext *g_context;

static esp_err_t capture_context(JSContext *context, void *user)
{
    (void)user;
    g_context = context;
    return ESP_OK;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc((size_t)size + 1);
    if (fread(data, 1, (size_t)size, file) != (size_t)size) exit(2);
    data[size] = 0;
    fclose(file);
    return data;
}

static void eval_source(const char *source, const char *label)
{
    JSValue result = JS_Eval(g_context, source, strlen(source), label, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        js_std_dump_error(g_context);
        exit(3);
    }
    JS_FreeValue(g_context, result);
}

/* Evaluate an expression and copy its string value (empty on failure). */
static void eval_string(const char *expression, char *out, size_t capacity)
{
    JSValue result = JS_Eval(g_context, expression, strlen(expression), "<harness>", JS_EVAL_TYPE_GLOBAL);
    out[0] = 0;
    if (JS_IsException(result)) {
        js_std_dump_error(g_context);
        return;
    }
    const char *text = JS_ToCString(g_context, result);
    if (text) {
        strncpy(out, text, capacity - 1);
        out[capacity - 1] = 0;
        JS_FreeCString(g_context, text);
    }
    JS_FreeValue(g_context, result);
}

static uint16_t *g_framebuffer;
static uint32_t g_width = 800, g_height = 480;

static void dump_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%u %u\n255\n", g_width, g_height);
    for (uint32_t i = 0; i < g_width * g_height; ++i) {
        uint16_t p = g_framebuffer[i];
        uint8_t rgb[3] = {(uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63), (uint8_t)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

/* Damage statistics: logical pixels repainted per frame. */
int g_frame_no;
uint64_t g_damage_px_total;
uint32_t g_damage_px_frame;
uint32_t g_damage_px_max;
uint32_t g_damage_frames_over_10k;
uint32_t g_damage_full_frames;
uint32_t g_damage_hist[8];
uint32_t g_damage_frames_nonzero;

static int present(pocketjs_rgb565_renderer_t *renderer, pocketjs_rgb565_target_t *target,
                   const pocketjs_ui_frame_view_t *frame, uint32_t scale)
{
    pocketjs_rgb565_damage_plan_t plan = {.struct_size = sizeof(plan)};
    if (pocketjs_rgb565_prepare(renderer, target, frame, &plan) != ESP_OK) return -1;
    g_damage_px_frame = 0;
    for (uint32_t index = 0; index < plan.region_count; ++index) {
        g_damage_px_frame += plan.regions[index].width * plan.regions[index].height;
    }
    if (getenv("DUMP_REGIONS") && g_frame_no >= 140 && g_frame_no < 152) {
        printf("f%d:", g_frame_no);
        for (uint32_t index = 0; index < plan.region_count; ++index)
            printf(" [%u,%u %ux%u]", plan.regions[index].x, plan.regions[index].y, plan.regions[index].width, plan.regions[index].height);
        printf("\n");
    }
    if (g_damage_px_frame) g_damage_frames_nonzero++;
    if (g_damage_px_frame > 10000) g_damage_frames_over_10k++;
    if (getenv("DUMP_OPS") && g_frame_no >= 143 && g_frame_no <= 145) {
        FILE *f = fopen(g_frame_no == 143 ? "/tmp/ops143.txt" : g_frame_no == 144 ? "/tmp/ops144.txt" : "/tmp/ops145.txt", "w");
        for (size_t i = 0; i < frame->draw_word_count; ++i) fprintf(f, "%08x%c", frame->draw_words[i], (i % 8 == 7) ? '\n' : ' ');
        fclose(f);
    }
    if (getenv("DUMP_WORDS") && g_frame_no >= 140 && g_frame_no < 152) printf("W f%d words=%u area=%u\n", g_frame_no, (unsigned)frame->draw_word_count, g_damage_px_frame);
    if (g_damage_px_frame >= 90000) { g_damage_full_frames++; if (getenv("DUMP_FULL")) printf("full@%d\n", g_frame_no); }
    { uint32_t b = g_damage_px_frame == 0 ? 0 : g_damage_px_frame < 2000 ? 1 : g_damage_px_frame < 5000 ? 2 : g_damage_px_frame < 10000 ? 3 : g_damage_px_frame < 20000 ? 4 : g_damage_px_frame < 50000 ? 5 : 6; g_damage_hist[b]++; }
    if (g_damage_px_frame > g_damage_px_max) g_damage_px_max = g_damage_px_frame;
    g_damage_px_total += g_damage_px_frame;
    for (uint32_t index = 0; index < plan.region_count; ++index) {
        const pocketjs_rgb565_rect_t region = plan.regions[index];
        uint32_t row = region.y, remaining = region.height;
        while (remaining) {
            uint32_t rows = remaining > 48 ? 48 : remaining;
            pocketjs_rgb565_rect_t strip = {.x = region.x, .y = row, .width = region.width, .height = rows};
            pocketjs_rgb565_render_stats_t stats = {.struct_size = sizeof(stats)};
            uint16_t *dst = g_framebuffer + (size_t)(row * scale) * g_width;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (pocketjs_rgb565_render_strip(renderer, frame, dst, (size_t)g_width * rows * scale, strip, NULL, &stats) != ESP_OK) return -2;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (getenv("DUMP_TIMES") && g_frame_no >= 140 && g_frame_no < 400) {
                double us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
                printf("T f%d [%u,%u %ux%u] %.1fus %.3fus/px\n", g_frame_no, strip.x, strip.y, strip.width, strip.height, us, us / (double)(strip.width * strip.height));
            }
            row += rows;
            remaining -= rows;
        }
    }
    return pocketjs_rgb565_commit(renderer, target, frame) == ESP_OK ? 0 : -3;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: harness prelude.js scenario.js outdir frames\n"); return 1; }
    const char *outdir = argv[3];
    const int frames = atoi(argv[4]);
    const pocketjs_package_host_contract_t *contract = &pocketjs_package_edgitalk_smoke_contract;
    const size_t package_size = (size_t)((uintptr_t)pocketjs_package_edgitalk_smoke_end - (uintptr_t)pocketjs_package_edgitalk_smoke.data);
    pocketjs_package_t *package = NULL;
    pocketjs_package_variant_t app = {0};
    pocketjs_ui_core_config_t core_config;
    pocketjs_guest_config_t guest_config;
    pocketjs_ui_qjs_config_t binding_config;
    pocketjs_rgb565_renderer_config_t renderer_config;
    pocketjs_ui_core_t *core = NULL;
    pocketjs_guest_t *guest = NULL;
    pocketjs_ui_qjs_t *binding = NULL;
    pocketjs_rgb565_renderer_t *renderer = NULL;
    pocketjs_rgb565_target_t *target = NULL;

    g_framebuffer = calloc((size_t)g_width * g_height, sizeof(uint16_t));
    if (pocketjs_package_open(pocketjs_package_edgitalk_smoke.data, package_size, 0U, &package) != ESP_OK) { fprintf(stderr, "package open failed\n"); return 4; }
    app = (pocketjs_package_variant_t){.struct_size = sizeof(app)};
    if (pocketjs_package_select(package, contract, &app) != ESP_OK) { fprintf(stderr, "package select failed\n"); return 4; }

    pocketjs_ui_core_config_defaults(&core_config);
    core_config.logical_width = contract->logical_width;
    core_config.logical_height = contract->logical_height;
    core_config.raster_density = contract->raster_density;
    core_config.tick_hz = contract->tick_hz;
    if (pocketjs_ui_core_create(&core_config, &core) != ESP_OK) return 5;
    pocketjs_guest_config_defaults(&guest_config);
    guest_config.heap_limit = 64U * 1024U * 1024U;
    guest_config.stack_limit = 1024U * 1024U;
    if (pocketjs_guest_create(&guest_config, &guest) != ESP_OK) return 5;
    binding_config = (pocketjs_ui_qjs_config_t){.struct_size = sizeof(binding_config), .target_id = contract->target_id, .host_abi = contract->host_abi};
    if (pocketjs_ui_qjs_create(guest, core, &binding_config, &binding) != ESP_OK) return 5;
    if (pocketjs_ui_qjs_feed_pak(binding, app.pak.data, app.pak.size) != ESP_OK) { fprintf(stderr, "pak load failed\n"); return 5; }
    if (pocketjs_ui_qjs_mount(binding) != ESP_OK) return 5;
    if (pocketjs_guest_quickjs_install_once(guest, "preview.capture", capture_context, NULL) != ESP_OK) return 5;

    char *prelude = read_file(argv[1]);
    char *scenario = read_file(argv[2]);
    eval_source(prelude, "prelude");
    if (pocketjs_guest_eval(guest, (const char *)app.javascript.data, app.javascript.size - 1U, "app") != ESP_OK) { fprintf(stderr, "app eval failed\n"); return 6; }
    eval_source(scenario, "scenario");

    pocketjs_rgb565_renderer_config_defaults(&renderer_config);
    renderer_config.scale = core_config.raster_density;
    if (pocketjs_rgb565_renderer_create(&renderer_config, &renderer) != ESP_OK) return 7;
    if (pocketjs_rgb565_target_create(&target) != ESP_OK) return 7;

    for (int n = 0; n < frames; ++n) {
        char expression[128], answer[128], path[512];
        pocketjs_ui_touch_t touch = {0};
        pocketjs_ui_input_t input = {.struct_size = sizeof(input)};
        pocketjs_ui_frame_view_t frame = {.struct_size = sizeof(frame)};
        unsigned x, y;

        snprintf(expression, sizeof(expression), "__sim.frame=%d; __sim.input(%d)", n, n);
        eval_string(expression, answer, sizeof(answer));
        if (sscanf(answer, "%u,%u", &x, &y) == 2) {
            touch.id = 0; touch.x = (uint16_t)x; touch.y = (uint16_t)y;
            input.touches = &touch; input.touch_count = 1;
        }
        esp_err_t result = pocketjs_ui_turn(binding, &input, &frame);
        if (result != ESP_OK) { fprintf(stderr, "frame %d failed: %d\n", n, result); return 8; }
        g_frame_no = n;
        if (present(renderer, target, &frame, core_config.raster_density) != 0) { fprintf(stderr, "present failed at %d\n", n); return 9; }
        snprintf(expression, sizeof(expression), "__sim.shot(%d)", n);
        eval_string(expression, answer, sizeof(answer));
        if (answer[0]) {
            snprintf(path, sizeof(path), "%s/%s.ppm", outdir, answer);
            dump_ppm(path);
            printf("frame %d -> %s\n", n, path);
        }
    }
    eval_string("__sim.report()", (char[4096]){0}, 4096);
    {
        static char report[8192];
        eval_string("__sim.report()", report, sizeof(report));
        printf("%s\n", report);
        printf("hist 0:%u <2k:%u <5k:%u <10k:%u <20k:%u <50k:%u >=50k:%u full:%u\n", g_damage_hist[0], g_damage_hist[1], g_damage_hist[2], g_damage_hist[3], g_damage_hist[4], g_damage_hist[5], g_damage_hist[6], g_damage_full_frames);
        printf("damage: total=%llu px over %d frames, avg=%llu px/frame, max=%u, frames>10k px: %u, nonzero: %u\n",
               (unsigned long long)g_damage_px_total, frames, (unsigned long long)(g_damage_px_total / (frames ? frames : 1)),
               g_damage_px_max, g_damage_frames_over_10k, g_damage_frames_nonzero);
    }
    return 0;
}
