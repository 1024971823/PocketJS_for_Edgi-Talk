/*
 * Edgi-Talk host for PocketJS's native UI core and RGB565 renderer.
 * The JavaScript guest owns the retained UI tree; this file owns the board
 * framebuffer and presents PocketJS damage rectangles through the LCD driver.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/touch.h>
#include <stdbool.h>
#include <stdint.h>

#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"

#include "pocketjs/pocketjs_app.h"
#include "pocketjs_dashboard.h"
#include "pocketjs_game.h"
#include "pocketjs_music.h"
#include "pocketjs_package_edgitalk_smoke.h"

#define POCKETJS_TASK_STACK_SIZE       (128U * 1024U)
#define POCKETJS_TASK_PRIORITY         20U
#define POCKETJS_GUEST_HEAP_LIMIT      (6U * 1024U * 1024U)
#define POCKETJS_GUEST_STACK_LIMIT     (96U * 1024U)
#define POCKETJS_RENDER_STRIP_ROWS     48U
/* Strips are composed in fast internal SRAM and then burst-copied to the
 * framebuffer: blending straight into the SOC framebuffer reads it back
 * pixel by pixel, which is many times slower. */
#define POCKETJS_SCRATCH_PHYS_ROWS     24U
#define POCKETJS_SCRATCH_MAX_WIDTH     800U
#define POCKETJS_TOUCH_DEVICE_NAME     "ST7102"
#define POCKETJS_TOUCH_RAW_WIDTH       480U
#define POCKETJS_TOUCH_RAW_HEIGHT      800U

static struct rt_semaphore s_ready;
static rt_bool_t s_started;
static rt_err_t s_init_result = -RT_ERROR;

static rt_device_t s_lcd;
static uint16_t *s_framebuffer;

/* drv_lcd.c: CPU rotation of just the dirty rectangles (render-buffer pixels). */
extern rt_bool_t lcd_rect_present_supported(void);
extern rt_bool_t lcd_stage_rect_rgb565(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
extern rt_bool_t lcd_commit_rect_rgb565(void);
static uint16_t s_scratch[POCKETJS_SCRATCH_MAX_WIDTH * POCKETJS_SCRATCH_PHYS_ROWS]
    __attribute__((aligned(32)));
static uint32_t s_lcd_width;
static uint32_t s_lcd_height;
static uint32_t s_tick_hz;

static rt_device_t s_touch;
static rt_bool_t s_touch_opened;
static rt_bool_t s_touch_active;
static uint32_t s_touch_raw_width = POCKETJS_TOUCH_RAW_WIDTH;
static uint32_t s_touch_raw_height = POCKETJS_TOUCH_RAW_HEIGHT;

static pocketjs_package_t *s_package;
static pocketjs_package_variant_t s_app;
static pocketjs_guest_t *s_guest;
static pocketjs_ui_core_t *s_core;
static pocketjs_ui_qjs_t *s_binding;
static pocketjs_rgb565_renderer_t *s_renderer;
static pocketjs_rgb565_target_t *s_target;
static uint32_t s_ui_time_ms;
static uint32_t s_present_time_ms;
static uint32_t s_ui_max_ms;
static uint32_t s_present_max_ms;
static volatile uint32_t s_touch_worst_ticks;
static volatile uint32_t s_ui_worst_ticks;
static volatile uint32_t s_present_worst_ticks;

extern int rt_hw_ST7102_port(void);
extern const uint8_t pocketjs_package_edgitalk_smoke_end[];

static void pocketjs_destroy(void)
{
    if ((s_touch != RT_NULL) && s_touch_opened)
    {
        (void)rt_device_close(s_touch);
    }
    s_touch = RT_NULL;
    s_touch_opened = RT_FALSE;
    s_touch_active = RT_FALSE;

    if (s_target != RT_NULL)
    {
        pocketjs_rgb565_target_destroy(s_target);
        s_target = RT_NULL;
    }
    if (s_renderer != RT_NULL)
    {
        pocketjs_rgb565_renderer_destroy(s_renderer);
        s_renderer = RT_NULL;
    }
    if (s_binding != RT_NULL)
    {
        pocketjs_ui_qjs_destroy(s_binding);
        s_binding = RT_NULL;
    }
    if (s_guest != RT_NULL)
    {
        pocketjs_guest_destroy(s_guest);
        s_guest = RT_NULL;
    }
    if (s_core != RT_NULL)
    {
        pocketjs_ui_core_destroy(s_core);
        s_core = RT_NULL;
    }
    if (s_package != RT_NULL)
    {
        pocketjs_package_close(s_package);
        s_package = RT_NULL;
    }
    s_app = (pocketjs_package_variant_t){0};
}

static rt_err_t pocketjs_open_lcd(void)
{
    struct rt_device_graphic_info info;

    for (uint32_t attempt = 0U; attempt < 100U; ++attempt)
    {
        s_lcd = rt_device_find("lcd");
        if (s_lcd != RT_NULL)
        {
            break;
        }
        rt_thread_mdelay(20);
    }
    if (s_lcd == RT_NULL)
    {
        rt_kprintf("[PocketJS] LCD device not found\n");
        return -RT_ENOSYS;
    }

    rt_memset(&info, 0, sizeof(info));
    if (rt_device_control(s_lcd, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK)
    {
        rt_kprintf("[PocketJS] LCD info query failed\n");
        return -RT_ERROR;
    }

    if ((info.framebuffer == RT_NULL) || (info.width == 0U) ||
        (info.height == 0U) ||
        (info.pitch != (info.width * sizeof(uint16_t))))
    {
        rt_kprintf("[PocketJS] unsupported framebuffer %ux%u pitch=%u\n",
                   info.width, info.height, info.pitch);
        return -RT_EINVAL;
    }

    s_framebuffer = (uint16_t *)info.framebuffer;
    s_lcd_width = info.width;
    s_lcd_height = info.height;
    rt_kprintf("[PocketJS] RGB565 framebuffer %ux%u\n", s_lcd_width,
               s_lcd_height);
    return RT_EOK;
}

static uint16_t pocketjs_scale_touch_coordinate(uint32_t value,
                                                 uint32_t source_max,
                                                 uint32_t destination_max)
{
    if (source_max == 0U)
    {
        return 0U;
    }
    if (value > source_max)
    {
        value = source_max;
    }
    return (uint16_t)((value * destination_max + (source_max / 2U)) /
                      source_max);
}

static rt_err_t pocketjs_open_touch(void)
{
    rt_err_t result;

    s_touch = rt_device_find(POCKETJS_TOUCH_DEVICE_NAME);
    if (s_touch == RT_NULL)
    {
        result = rt_hw_ST7102_port();
        if (result != RT_EOK)
        {
            rt_kprintf("[PocketJS] ST7102 init failed: %d\n", result);
            return result;
        }
        s_touch = rt_device_find(POCKETJS_TOUCH_DEVICE_NAME);
    }
    if (s_touch == RT_NULL)
    {
        rt_kprintf("[PocketJS] touch device %s not found\n",
                   POCKETJS_TOUCH_DEVICE_NAME);
        return -RT_ENOSYS;
    }

    result = rt_device_open(s_touch, RT_DEVICE_FLAG_RDONLY);
    if (result == RT_EOK)
    {
        s_touch_opened = RT_TRUE;
    }
    else if (result != -RT_EBUSY)
    {
        rt_kprintf("[PocketJS] touch open failed: %d\n", result);
        s_touch = RT_NULL;
        return result;
    }

    /* This panel's controller reports uncalibrated 1007x1007 maxima through
     * RT_TOUCH_CTRL_GET_INFO. Actual event coordinates use its 480x800
     * portrait sensor space, so keep the board calibration here. */
    s_touch_raw_width = POCKETJS_TOUCH_RAW_WIDTH;
    s_touch_raw_height = POCKETJS_TOUCH_RAW_HEIGHT;

    rt_kprintf("[PocketJS] touch %s calibrated raw=%ux%u -> logical=%ux%u\n",
               POCKETJS_TOUCH_DEVICE_NAME, s_touch_raw_width,
               s_touch_raw_height,
               pocketjs_package_edgitalk_smoke_contract.logical_width,
               pocketjs_package_edgitalk_smoke_contract.logical_height);
    return RT_EOK;
}

/* Monotonic counters a debug probe can read without disturbing the app. */
volatile uint32_t g_pocketjs_frames;
volatile uint32_t g_pocketjs_ui_ms_total;
volatile uint32_t g_pocketjs_present_ms_total;
volatile uint32_t g_pocketjs_prepare_ms_total;
volatile uint32_t g_pocketjs_render_ms_total;
volatile uint32_t g_pocketjs_lcd_ms_total;
volatile uint32_t g_pocketjs_regions_total;
volatile uint32_t g_pocketjs_rows_total;

#ifdef POCKETJS_DEBUG_TOUCH
/* Bench hook: write (1<<31 | x<<16 | y) here over SWD to inject a touch. */
volatile uint32_t g_pocketjs_debug_touch;
#endif

static void pocketjs_sample_touch(pocketjs_ui_input_t *input,
                                  pocketjs_ui_touch_t *contact)
{
    struct rt_touch_data raw = {0};
    const uint32_t logical_width =
        pocketjs_package_edgitalk_smoke_contract.logical_width;
    const uint32_t logical_height =
        pocketjs_package_edgitalk_smoke_contract.logical_height;
    const uint32_t raw_x_max =
        s_touch_raw_width > 1U ? s_touch_raw_width - 1U : 0U;
    const uint32_t raw_y_max =
        s_touch_raw_height > 1U ? s_touch_raw_height - 1U : 0U;
    rt_size_t count;

    if ((input == RT_NULL) || (contact == RT_NULL) || (s_touch == RT_NULL) ||
        (logical_width == 0U) || (logical_height == 0U))
    {
        return;
    }

#ifdef POCKETJS_DEBUG_TOUCH
    if ((g_pocketjs_debug_touch & 0x80000000U) != 0U)
    {
        contact->id = 0;
        contact->x = (uint16_t)((g_pocketjs_debug_touch >> 16) & 0x7FFU);
        contact->y = (uint16_t)(g_pocketjs_debug_touch & 0xFFFFU);
        input->touches = contact;
        input->touch_count = 1U;
        return;
    }
#endif
    count = rt_device_read(s_touch, 0, &raw, 1U);
    if ((count == 0U) ||
        ((raw.event != RT_TOUCH_EVENT_DOWN) &&
         (raw.event != RT_TOUCH_EVENT_MOVE)))
    {
        if (s_touch_active)
        {
            rt_kprintf("[PocketJS] touch up\n");
            s_touch_active = RT_FALSE;
        }
        return;
    }

    /* The ST7102 sensor is portrait (480x800); the M55 LCD is landscape
     * after the board's fixed 90-degree rotation. */
    contact->id = raw.track_id;
    contact->x = pocketjs_scale_touch_coordinate(
        raw.y_coordinate, raw_y_max, logical_width - 1U);
    contact->y = pocketjs_scale_touch_coordinate(
        raw_x_max > raw.x_coordinate ? raw_x_max - raw.x_coordinate : 0U,
        raw_x_max, logical_height - 1U);
    input->touches = contact;
    input->touch_count = 1U;

    if (!s_touch_active)
    {
        rt_kprintf("[PocketJS] touch down raw=%u,%u logical=%u,%u\n",
                   raw.x_coordinate, raw.y_coordinate, contact->x,
                   contact->y);
        s_touch_active = RT_TRUE;
    }
}

static rt_err_t pocketjs_open_package(void)
{
    const pocketjs_package_host_contract_t *contract =
        &pocketjs_package_edgitalk_smoke_contract;
    const size_t embedded_package_size = (size_t)(
        (uintptr_t)pocketjs_package_edgitalk_smoke_end -
        (uintptr_t)pocketjs_package_edgitalk_smoke.data);
    esp_err_t result;

    if ((s_lcd_width != contract->physical_width) ||
        (s_lcd_height != contract->physical_height) ||
        (contract->raster_density == 0U) ||
        (contract->logical_width != (s_lcd_width / contract->raster_density)) ||
        (contract->logical_height != (s_lcd_height / contract->raster_density)) ||
        (contract->host_abi != POCKETJS_UI_CORE_ABI_VERSION))
    {
        rt_kprintf("[PocketJS] package contract does not match this host\n");
        return -RT_EINVAL;
    }

    if (embedded_package_size == 0U)
    {
        rt_kprintf("[PocketJS] embedded package is empty\n");
        return -RT_EINVAL;
    }

    result = pocketjs_package_open(pocketjs_package_edgitalk_smoke.data,
                                   embedded_package_size, 0U,
                                   &s_package);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] package open failed: %d\n", result);
        return -RT_ERROR;
    }

    s_app = (pocketjs_package_variant_t){.struct_size = sizeof(s_app)};
    result = pocketjs_package_select(s_package, contract, &s_app);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] package select failed: %d\n", result);
        return -RT_ERROR;
    }

    s_tick_hz = contract->tick_hz;
    rt_kprintf("[PocketJS] package accepted: js=%u pak=%u logical=%ux%u x%u\n",
               (uint32_t)s_app.javascript.size, (uint32_t)s_app.pak.size,
               contract->logical_width, contract->logical_height,
               contract->raster_density);
    return RT_EOK;
}

static rt_err_t pocketjs_init(void)
{
    pocketjs_ui_core_config_t core_config;
    pocketjs_guest_config_t guest_config;
    pocketjs_ui_qjs_config_t binding_config;
    pocketjs_rgb565_renderer_config_t renderer_config;
    esp_err_t result;

    if (pocketjs_open_lcd() != RT_EOK)
    {
        return -RT_ERROR;
    }
    if (pocketjs_open_touch() != RT_EOK)
    {
        rt_kprintf("[PocketJS] continuing without touch input\n");
    }
    if (pocketjs_open_package() != RT_EOK)
    {
        return -RT_ERROR;
    }

    pocketjs_ui_core_config_defaults(&core_config);
    core_config.logical_width = pocketjs_package_edgitalk_smoke_contract.logical_width;
    core_config.logical_height = pocketjs_package_edgitalk_smoke_contract.logical_height;
    core_config.raster_density = pocketjs_package_edgitalk_smoke_contract.raster_density;
    core_config.tick_hz = s_tick_hz;
    result = pocketjs_ui_core_create(&core_config, &s_core);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] UI core create failed: %d\n", result);
        return -RT_ENOMEM;
    }

    pocketjs_guest_config_defaults(&guest_config);
    guest_config.heap_limit = POCKETJS_GUEST_HEAP_LIMIT;
    guest_config.stack_limit = POCKETJS_GUEST_STACK_LIMIT;
    guest_config.prefer_psram = true;
    result = pocketjs_guest_create(&guest_config, &s_guest);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] QuickJS guest create failed: %d\n", result);
        return -RT_ENOMEM;
    }

    binding_config = (pocketjs_ui_qjs_config_t){
        .struct_size = sizeof(binding_config),
        .target_id = pocketjs_package_edgitalk_smoke_contract.target_id,
        .host_abi = pocketjs_package_edgitalk_smoke_contract.host_abi,
    };
    result = pocketjs_ui_qjs_create(s_guest, s_core, &binding_config, &s_binding);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] UI binding create failed: %d\n", result);
        return -RT_ERROR;
    }
    result = pocketjs_ui_qjs_feed_pak(s_binding, s_app.pak.data, s_app.pak.size);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] PAK load failed: %d\n", result);
        return -RT_ERROR;
    }
    result = pocketjs_ui_qjs_mount(s_binding);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] UI binding mount failed: %d\n", result);
        return -RT_ERROR;
    }
    result = pocketjs_guest_quickjs_install_once(
        s_guest, "edgitalk.dashboard", pocketjs_dashboard_install, RT_NULL);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] dashboard install failed: %d\n", result);
        return -RT_ERROR;
    }
    pocketjs_dashboard_start();
    if (pocketjs_music_start() != RT_EOK)
    {
        rt_kprintf("[PocketJS] music worker unavailable\n");
    }
    if (pocketjs_game_start_service() != RT_EOK)
    {
        rt_kprintf("[PocketJS] game service unavailable\n");
    }
    result = pocketjs_guest_eval(s_guest, (const char *)s_app.javascript.data,
                                 s_app.javascript.size - 1U,
                                 "edgitalk-m55-smoke");
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] JavaScript app load failed: %d\n", result);
        return -RT_ERROR;
    }

    pocketjs_rgb565_renderer_config_defaults(&renderer_config);
    renderer_config.scale = core_config.raster_density;
    result = pocketjs_rgb565_renderer_create(&renderer_config, &s_renderer);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] RGB565 renderer create failed: %d\n", result);
        return -RT_ENOMEM;
    }
    result = pocketjs_rgb565_target_create(&s_target);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] RGB565 target create failed: %d\n", result);
        return -RT_ENOMEM;
    }

    rt_kprintf("[PocketJS] QuickJS guest mounted at %u Hz\n",
               core_config.tick_hz);
    return RT_EOK;
}

static rt_err_t pocketjs_present(const pocketjs_ui_frame_view_t *frame)
{
    pocketjs_rgb565_damage_plan_t plan = {.struct_size = sizeof(plan)};
    const uint32_t scale =
        pocketjs_package_edgitalk_smoke_contract.raster_density;
    esp_err_t result;

    if ((scale == 0U) || (scale > POCKETJS_SCRATCH_PHYS_ROWS) ||
        (s_lcd_width > POCKETJS_SCRATCH_MAX_WIDTH))
    {
        return -RT_EINVAL;
    }

    rt_tick_t t_prep = rt_tick_get();
    result = pocketjs_rgb565_prepare(s_renderer, s_target, frame, &plan);
    g_pocketjs_prepare_ms_total += (uint32_t)(rt_tick_get() - t_prep);
    g_pocketjs_regions_total += plan.region_count;
    rt_tick_t t_render = rt_tick_get();
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] damage preparation failed: %d\n", result);
        pocketjs_rgb565_abort(s_renderer, s_target);
        return -RT_ERROR;
    }

    for (uint32_t index = 0U; index < plan.region_count; ++index)
    {
        const pocketjs_rgb565_rect_t region = plan.regions[index];
        g_pocketjs_rows_total += region.height * region.width / 100U;
        uint32_t row = region.y;
        uint32_t remaining = region.height;

        while (remaining != 0U)
        {
            const uint32_t max_rows = POCKETJS_SCRATCH_PHYS_ROWS / scale;
            const uint32_t rows = remaining > max_rows ? max_rows : remaining;
            const uint32_t physical_row = row * scale;
            const uint32_t physical_rows = rows * scale;
            const pocketjs_rgb565_rect_t strip = {
                .x = region.x,
                .y = row,
                .width = region.width,
                .height = rows,
            };
            pocketjs_rgb565_render_stats_t stats = {.struct_size = sizeof(stats)};

            if ((physical_row >= s_lcd_height) ||
                (physical_rows > (s_lcd_height - physical_row)))
            {
                rt_kprintf("[PocketJS] invalid physical render strip %u+%u/%u\\n",
                           physical_row, physical_rows, s_lcd_height);
                pocketjs_rgb565_abort(s_renderer, s_target);
                return -RT_EINVAL;
            }

            uint16_t *destination = s_scratch;

            result = pocketjs_rgb565_render_strip(
                s_renderer, frame, destination,
                (size_t)s_lcd_width * physical_rows,
                strip, RT_NULL, &stats);
            if (result != ESP_OK)
            {
                rt_kprintf("[PocketJS] RGB565 strip render failed: %d\n", result);
                pocketjs_rgb565_abort(s_renderer, s_target);
                return -RT_ERROR;
            }
            {
                const size_t copy_bytes =
                    (size_t)region.width * scale * sizeof(uint16_t);
                for (uint32_t line = 0U; line < physical_rows; ++line)
                {
                    rt_memcpy(s_framebuffer +
                                  ((size_t)(physical_row + line) * s_lcd_width) +
                                  (size_t)region.x * scale,
                              s_scratch + ((size_t)line * s_lcd_width) +
                                  (size_t)region.x * scale,
                              copy_bytes);
                }
            }
            row += rows;
            remaining -= rows;
        }
    }

    g_pocketjs_render_ms_total += (uint32_t)(rt_tick_get() - t_render);
    rt_tick_t t_lcd = rt_tick_get();
    /* Rotate and flush only what changed; fall back to the driver's full-frame present when the
     * driver cannot do partial updates or a frame carries no usable region. */
    rt_bool_t partial = RT_FALSE;
    if ((plan.region_count != 0U) && lcd_rect_present_supported())
    {
        partial = RT_TRUE;
        for (uint32_t index = 0U; index < plan.region_count; ++index)
        {
            const pocketjs_rgb565_rect_t region = plan.regions[index];
            if (!lcd_stage_rect_rgb565(region.x * scale, region.y * scale,
                                       region.width * scale, region.height * scale))
            {
                partial = RT_FALSE;
                break;
            }
        }
        if (partial)
        {
            partial = lcd_commit_rect_rgb565();
        }
    }
    if ((plan.region_count != 0U) && !partial &&
        (rt_device_control(s_lcd, RTGRAPHIC_CTRL_RECT_UPDATE, RT_NULL) != RT_EOK))
    {
        rt_kprintf("[PocketJS] LCD present failed\n");
        pocketjs_rgb565_abort(s_renderer, s_target);
        return -RT_ERROR;
    }

    g_pocketjs_lcd_ms_total += (uint32_t)(rt_tick_get() - t_lcd);
    result = pocketjs_rgb565_commit(s_renderer, s_target, frame);
    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] damage commit failed: %d\n", result);
        return -RT_ERROR;
    }
    return RT_EOK;
}

static rt_err_t pocketjs_turn(void)
{
    pocketjs_ui_touch_t touch = {0};
    pocketjs_ui_input_t input = {.struct_size = sizeof(input)};
    pocketjs_ui_frame_view_t frame = {.struct_size = sizeof(frame)};
    rt_tick_t started;
    uint32_t elapsed;
    esp_err_t result;

    started = rt_tick_get();
    pocketjs_sample_touch(&input, &touch);
    elapsed = (uint32_t)(rt_tick_get() - started);
    if (elapsed > s_touch_worst_ticks)
    {
        s_touch_worst_ticks = elapsed;
    }
    started = rt_tick_get();
    result = pocketjs_ui_turn(s_binding, &input, &frame);

    if (result != ESP_OK)
    {
        rt_kprintf("[PocketJS] JavaScript frame failed: %d\n", result);
        return -RT_ERROR;
    }
    elapsed = (uint32_t)(rt_tick_get() - started);
    s_ui_time_ms += elapsed;
    g_pocketjs_ui_ms_total += elapsed;
    if (elapsed > s_ui_max_ms)
    {
        s_ui_max_ms = elapsed;
    }
    if (elapsed > s_ui_worst_ticks)
    {
        s_ui_worst_ticks = elapsed;
    }

    started = rt_tick_get();
    if (pocketjs_present(&frame) != RT_EOK)
    {
        return -RT_ERROR;
    }
    elapsed = (uint32_t)(rt_tick_get() - started);
    s_present_time_ms += elapsed;
    g_pocketjs_present_ms_total += elapsed;
    if (elapsed > s_present_max_ms)
    {
        s_present_max_ms = elapsed;
    }
    if (elapsed > s_present_worst_ticks)
    {
        s_present_worst_ticks = elapsed;
    }
    pocketjs_dashboard_note_frame();
    ++g_pocketjs_frames;
    return RT_EOK;
}

static void pocketjs_task(void *parameter)
{
    rt_tick_t last_report = rt_tick_get();
    rt_tick_t next_frame;
    rt_tick_t now;
    rt_tick_t frame_ticks;
    uint32_t frame_fraction = 0U;
    uint32_t frame_remainder;
    uint32_t presented = 0U;

    (void)parameter;
    s_init_result = pocketjs_init();
    if (s_init_result == RT_EOK)
    {
        s_init_result = pocketjs_turn();
    }
    rt_sem_release(&s_ready);

    if (s_init_result != RT_EOK)
    {
        pocketjs_destroy();
        return;
    }
    frame_ticks = RT_TICK_PER_SECOND / s_tick_hz;
    frame_remainder = RT_TICK_PER_SECOND % s_tick_hz;
    next_frame = rt_tick_get();

    while (1)
    {
        if (pocketjs_turn() == RT_EOK)
        {
            ++presented;
        }

        /* Frame statistics. The JS heap walk behind pocketjs_guest_stats() costs more than a
         * frame and used to stall the UI every five seconds, so it is not part of the report. */
        if (((rt_tick_get() - last_report) >= rt_tick_from_millisecond(5000)) &&
            !pocketjs_game_running())
        {
            rt_kprintf("[PocketJS] frames=%u ui=%u/%u ms lcd=%u/%u ms\n",
                       presented,
                       presented == 0U ? 0U : s_ui_time_ms / presented,
                       s_ui_max_ms,
                       presented == 0U ? 0U : s_present_time_ms / presented,
                       s_present_max_ms);
            last_report = rt_tick_get();
            presented = 0U;
            s_ui_time_ms = 0U;
            s_present_time_ms = 0U;
            s_ui_max_ms = 0U;
            s_present_max_ms = 0U;
        }
        next_frame += frame_ticks;
        frame_fraction += frame_remainder;
        if (frame_fraction >= s_tick_hz)
        {
            ++next_frame;
            frame_fraction -= s_tick_hz;
        }
        now = rt_tick_get();
        if ((rt_int32_t)(next_frame - now) > 0)
        {
            rt_thread_delay(next_frame - now);
        }
        else
        {
            next_frame = now;
        }
    }
}

rt_err_t pocketjs_app_start(void)
{
    rt_thread_t thread;

    if (s_started)
    {
        return -RT_EBUSY;
    }
    if (rt_sem_init(&s_ready, "pjsready", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        return -RT_ERROR;
    }

    thread = rt_thread_create("pocketjs", pocketjs_task, RT_NULL,
                              POCKETJS_TASK_STACK_SIZE,
                              POCKETJS_TASK_PRIORITY, 10U);
    if (thread == RT_NULL)
    {
        rt_sem_detach(&s_ready);
        return -RT_ENOMEM;
    }
    s_started = RT_TRUE;
    rt_thread_startup(thread);
    return RT_EOK;
}

rt_err_t pocketjs_app_wait_ready(rt_int32_t timeout)
{
    if (!s_started)
    {
        return -RT_EINVAL;
    }
    if (rt_sem_take(&s_ready, timeout) != RT_EOK)
    {
        return -RT_ETIMEOUT;
    }
    return s_init_result;
}
