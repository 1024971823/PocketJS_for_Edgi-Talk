/*
 * CYW55513 HCI bring-up on SCB4.
 *
 * The controller patch (HCD, format 0x01) is already linked from
 * bt-fw-ifx-cyw55500a1. This file ships it over the Bluetooth UART, then
 * advertises as EdgiTalk and accepts one writable GATT characteristic whose
 * value is the same JSON the Wi-Fi PUT /api/pc carries, plus "token".
 */

#include "pocketjs_bt.h"
#include "pocketjs_dashboard.h"
#include "pocketjs_game.h"

#include <rtdevice.h>
#include <cycfg_peripherals.h>
#include <cycfg_pins.h>
#include <cycfg_connectivity_bt.h>
#include <cy_scb_uart.h>
#include <string.h>

#define HCI_RESET            0x0C03U
#define HCI_SET_EVENT_MASK   0x0C01U
#define HCI_DL_MINIDRIVER    0xFC2EU
#define HCI_LAUNCH_RAM       0xFC4EU
#define HCI_LE_SET_EVENT_MASK 0x2001U
#define HCI_LE_SET_RANDOM_ADDR 0x2005U
#define HCI_LE_SET_ADV_PARAM  0x2006U
#define HCI_LE_SET_ADV_DATA   0x2008U
#define HCI_LE_SET_ADV_ENABLE 0x200AU
#define HCI_LE_SET_DATA_LEN   0x2022U

#define ATT_CID              0x0004U
#define ATT_ERROR            0x01U
#define ATT_MTU_REQ          0x02U
#define ATT_FIND_INFO_REQ    0x04U
#define ATT_READ_TYPE_REQ    0x08U
#define ATT_READ_GROUP_REQ   0x10U
#define ATT_WRITE_REQ        0x12U
#define ATT_WRITE_CMD        0x52U

#define HANDLE_SVC           0x0001U
#define HANDLE_DECL          0x0002U
#define HANDLE_VALUE         0x0003U
#define UUID_PRIMARY         0x2800U
#define UUID_CHAR_DECL       0x2803U
#define UUID_SERVICE         0xFFF0U
#define UUID_STATS           0xFFF1U
#define CHAR_PROPS           0x0CU /* write + write without response */

extern const uint8_t brcm_patchram_buf[];
extern const int brcm_patch_ram_length;

static rt_device_t s_bt_uart;
static struct rt_semaphore s_rx_sem;
static rt_bool_t s_advertising;
static uint16_t s_handle = 0xFFFFU;
static uint8_t s_json[320];
static uint8_t s_l2cap[320];
static uint16_t s_l2cap_len;

bool pocketjs_bt_advertising(void)
{
    return s_advertising == RT_TRUE;
}

static rt_err_t bt_rx_indicate(rt_device_t dev, rt_size_t size)
{
    (void)dev;
    (void)size;
    rt_sem_release(&s_rx_sem);
    return RT_EOK;
}

/* SCB4 is the RT-Thread device "uart4". Interrupt RX fills the serial ring
 * buffer so the controller's RTS stays asserted while this thread sleeps. */
static int uart_open(void)
{
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;

    s_bt_uart = rt_device_find("uart4");
    if (s_bt_uart == RT_NULL)
    {
        rt_kprintf("[pjsbt] uart4 is not registered\n");
        return -1;
    }
    rt_sem_init(&s_rx_sem, "btuart", 0, RT_IPC_FLAG_FIFO);
    cfg.baud_rate = BAUD_RATE_115200;
    cfg.data_bits = DATA_BITS_8;
    cfg.stop_bits = STOP_BITS_1;
    cfg.parity = PARITY_NONE;
    cfg.bufsz = 512;
    rt_device_control(s_bt_uart, RT_DEVICE_CTRL_CONFIG, &cfg);
    if (rt_device_open(s_bt_uart, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        rt_kprintf("[pjsbt] uart4 open failed\n");
        return -1;
    }
    rt_device_set_rx_indicate(s_bt_uart, bt_rx_indicate);
    /* The controller holds CTS idle until it has seen a command. RTS stays
     * with the UART block so it can still send to us. */
    Cy_SCB_UART_DisableCts(CYBSP_BT_UART_HW);
    return 0;
}

static int uart_read(uint32_t timeout_ms)
{
    rt_uint8_t byte;

    if (rt_device_read(s_bt_uart, 0, &byte, 1) == 1)
    {
        return byte;
    }
    if (rt_sem_take(&s_rx_sem, rt_tick_from_millisecond(timeout_ms)) != RT_EOK)
    {
        return -1;
    }
    if (rt_device_read(s_bt_uart, 0, &byte, 1) == 1)
    {
        return byte;
    }
    return -1;
}

static rt_bool_t uart_write(const uint8_t *data, uint32_t length)
{
    rt_size_t sent = rt_device_write(s_bt_uart, 0, data, length);

    return sent == length ? RT_TRUE : RT_FALSE;
}

static void uart_flush_rx(void)
{
    rt_uint8_t dump[32];

    while (rt_device_read(s_bt_uart, 0, dump, sizeof(dump)) > 0)
    {
    }
    while (rt_sem_take(&s_rx_sem, 0) == RT_EOK)
    {
    }
}

/* Read one HCI event (0x04) or ACL (0x02) packet into buf. Returns the HCI
 * packet length including the indicator, or 0 on timeout. */
static uint16_t hci_read(uint8_t *buf, uint16_t capacity, uint32_t timeout_ms)
{
    int marker;
    int code;
    int length;
    int i;
    uint16_t total;

    marker = uart_read(timeout_ms);
    if (marker < 0)
    {
        return 0U;
    }
    buf[0] = (uint8_t)marker;
    if (marker == 0x04)
    {
        code = uart_read(timeout_ms);
        length = uart_read(timeout_ms);
        if (code < 0 || length < 0)
        {
            return 0U;
        }
        buf[1] = (uint8_t)code;
        buf[2] = (uint8_t)length;
        for (i = 0; i < length; ++i)
        {
            int byte = uart_read(timeout_ms);
            if (byte < 0)
            {
                return 0U;
            }
            if ((uint16_t)(3 + i) < capacity)
            {
                buf[3 + i] = (uint8_t)byte;
            }
        }
        total = (uint16_t)(3 + length);
        return total <= capacity ? total : 0U;
    }
    if (marker == 0x02)
    {
        int header[4];
        for (i = 0; i < 4; ++i)
        {
            header[i] = uart_read(timeout_ms);
            if (header[i] < 0)
            {
                return 0U;
            }
            buf[1 + i] = (uint8_t)header[i];
        }
        total = (uint16_t)(header[2] | (header[3] << 8));
        if ((uint16_t)(5 + total) > capacity)
        {
            return 0U;
        }
        for (i = 0; i < (int)total; ++i)
        {
            int byte = uart_read(timeout_ms);
            if (byte < 0)
            {
                return 0U;
            }
            buf[5 + i] = (uint8_t)byte;
        }
        return (uint16_t)(5 + total);
    }
    return 0U;
}

static uint8_t s_last_event[16];
static uint16_t s_last_event_len;
static uint32_t s_cmd_wait_ms = 1000U;
static uint32_t s_cmd_tries = 8U;
static uint32_t s_first_byte_gap_ms;
static void feed_acl(const uint8_t *packet, uint16_t length);

static rt_bool_t hci_command(const uint8_t *body, uint8_t body_len, rt_bool_t wait_complete)
{
    uint8_t packet[4];
    uint8_t event[260];
    uint16_t opcode;
    uint32_t spins;

    packet[0] = 0x01U;
    if (!uart_write(packet, 1U))
    {
        return RT_FALSE;
    }
    /* Autobaud: the controller measures the baud rate on the first byte and
     * drops it, so give it time before the rest of the command arrives. */
    if (s_first_byte_gap_ms != 0U)
    {
        rt_thread_mdelay(s_first_byte_gap_ms);
    }
    if (!uart_write(body, body_len))
    {
        return RT_FALSE;
    }
    if (!wait_complete)
    {
        return RT_TRUE;
    }
    opcode = (uint16_t)(body[0] | (body[1] << 8));
    for (spins = 0U; spins < s_cmd_tries; ++spins)
    {
        uint16_t got = hci_read(event, sizeof(event), s_cmd_wait_ms);
        uint16_t copy = got < sizeof(s_last_event) ? got : (uint16_t)sizeof(s_last_event);

        s_last_event_len = copy;
        if (copy != 0U)
        {
            memcpy(s_last_event, event, copy);
        }
        if (got >= 5U && event[0] == 0x02U)
        {
            feed_acl(event, got);
            continue;
        }
        if (got < 6U || event[0] != 0x04U)
        {
            continue;
        }
        if (event[1] == 0x0EU && event[4] == (uint8_t)opcode && event[5] == (uint8_t)(opcode >> 8))
        {
            return event[6] == 0U;
        }
        if (event[1] == 0x0FU && event[4] == (uint8_t)opcode && event[5] == (uint8_t)(opcode >> 8))
        {
            return event[3] == 0U;
        }
    }
    return RT_FALSE;
}

static rt_bool_t hci_simple(uint16_t opcode, const uint8_t *param, uint8_t param_len)
{
    uint8_t body[260];

    body[0] = (uint8_t)opcode;
    body[1] = (uint8_t)(opcode >> 8);
    body[2] = param_len;
    if (param_len != 0U)
    {
        memcpy(body + 3, param, param_len);
    }
    return hci_command(body, (uint8_t)(3U + param_len), RT_TRUE);
}

static rt_bool_t command_status_is(uint8_t status)
{
    return s_last_event_len >= 7U && s_last_event[1] == 0x0EU && s_last_event[6] == status;
}

/* The controller only accepts the HCD while it is still in the reset-time
 * download window. An HCI Reset before that closes the window, and the
 * already-running image then cannot share the antenna with Wi-Fi. */
static rt_bool_t download_patch(void)
{
    const uint8_t *cursor = brcm_patchram_buf;
    const uint8_t *end = brcm_patchram_buf + brcm_patch_ram_length;
    uint32_t records = 0U;

    /* Infineon's recipe for CYW555xx: the controller enters autobaud/download
     * mode only if its CTS (our RTS, driven by the SCB while the RX FIFO has
     * room) is low on the rising edge of BT_REG_ON. Then the first command
     * is HCI Reset, with a gap after its first byte. */
    uart_flush_rx();
    Cy_GPIO_Write(CYBSP_BT_POWER_PORT, CYBSP_BT_POWER_PIN, 0U);
    rt_thread_mdelay(100);
    Cy_GPIO_Write(CYBSP_BT_POWER_PORT, CYBSP_BT_POWER_PIN, 1U);
    rt_thread_mdelay(150);
    uart_flush_rx();

    s_first_byte_gap_ms = 10U;
    s_cmd_wait_ms = 500U;
    s_cmd_tries = 3U;
    s_last_event_len = 0U;
    if (!hci_simple(HCI_RESET, RT_NULL, 0U))
    {
        rt_kprintf("[pjsbt] autobaud reset got no answer, retrying\n");
        uart_flush_rx();
        (void)hci_simple(HCI_RESET, RT_NULL, 0U);
    }
    s_first_byte_gap_ms = 0U;
    s_cmd_wait_ms = 1000U;
    s_cmd_tries = 8U;
    rt_kprintf("[pjsbt] reset answer");
    {
        uint16_t n;
        for (n = 0U; n < s_last_event_len; ++n)
        {
            rt_kprintf(" %02x", s_last_event[n]);
        }
    }
    rt_kprintf("\n");

    s_cmd_wait_ms = 400U;
    s_cmd_tries = 2U;
    s_last_event_len = 0U;
    (void)hci_simple(HCI_DL_MINIDRIVER, RT_NULL, 0U);
    s_cmd_wait_ms = 1000U;
    s_cmd_tries = 8U;
    rt_kprintf("[pjsbt] minidriver answer");
    {
        uint16_t n;
        for (n = 0U; n < s_last_event_len; ++n)
        {
            rt_kprintf(" %02x", s_last_event[n]);
        }
    }
    rt_kprintf("\n");
    if (command_status_is(0x01U))
    {
        /* Normal mode, not download mode. Nothing more can be written. */
        rt_kprintf("[pjsbt] not in download mode, skip patch\n");
        return RT_TRUE;
    }
    rt_kprintf("[pjsbt] download window open\n");
    rt_thread_mdelay(50);
    while (cursor + 3 <= end)
    {
        uint16_t opcode = (uint16_t)(cursor[0] | (cursor[1] << 8));
        uint8_t length = cursor[2];
        rt_bool_t launch = opcode == HCI_LAUNCH_RAM;

        if (cursor + 3 + length > end)
        {
            rt_kprintf("[pjsbt] patch record truncated\n");
            return RT_FALSE;
        }
        if (!hci_command(cursor, (uint8_t)(3U + length), launch ? RT_FALSE : RT_TRUE))
        {
            if (records == 0U && s_cmd_tries > 2U)
            {
                /* Autobaud may drop the first command. One more try, then recover. */
                s_cmd_wait_ms = 400U;
                s_cmd_tries = 2U;
                uart_flush_rx();
                if (hci_command(cursor, (uint8_t)(3U + length), RT_TRUE))
                {
                    s_cmd_wait_ms = 1000U;
                    s_cmd_tries = 8U;
                    cursor += 3 + length;
                    ++records;
                    continue;
                }
            }
            if (records == 0U)
            {
                uint32_t n;

                rt_kprintf("[pjsbt] patch not accepted");
                for (n = 0U; n < s_last_event_len && n < 8U; ++n)
                {
                    rt_kprintf(" %02x", s_last_event[n]);
                }
                rt_kprintf(", Wi-Fi stays up\n");
                s_cmd_wait_ms = 1000U;
                s_cmd_tries = 4U;
                uart_flush_rx();
                rt_thread_mdelay(300);
                if (!hci_simple(HCI_RESET, RT_NULL, 0U))
                {
                    rt_thread_mdelay(300);
                    (void)hci_simple(HCI_RESET, RT_NULL, 0U);
                }
                s_cmd_wait_ms = 1000U;
                s_cmd_tries = 8U;
                return RT_TRUE;
            }
            rt_kprintf("[pjsbt] patch record %u failed, last", (unsigned int)records);
            {
                uint16_t n;
                for (n = 0U; n < s_last_event_len; ++n)
                {
                    rt_kprintf(" %02x", s_last_event[n]);
                }
            }
            rt_kprintf("\n");
            return RT_FALSE;
        }
        cursor += 3 + length;
        ++records;
        if ((records % 100U) == 0U)
        {
            rt_kprintf("[pjsbt] patch %u\n", (unsigned int)records);
        }
        if (launch)
        {
            break;
        }
    }
    rt_thread_mdelay(250);
    if (!hci_simple(HCI_RESET, RT_NULL, 0U))
    {
        rt_kprintf("[pjsbt] controller did not reset after patch\n");
        return RT_FALSE;
    }
    rt_kprintf("[pjsbt] patch download ok (%u records)\n", (unsigned int)records);
    return RT_TRUE;
}

static rt_bool_t start_advertising(void)
{
    static const uint8_t event_mask[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x3FU};
    static const uint8_t le_mask[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    /* ADV_IND, public address, all three channels, ~100 ms. */
    static const uint8_t adv_param[15] = {
        0xA0, 0x00, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00
    };
    uint8_t adv[32];
    uint8_t enable = 0x01U;
    static const char name[] = "EdgiTalk";

    static const uint8_t le_host[2] = {0x01U, 0x00U};
    uint8_t host_buf[7] = {0x1BU, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U};
    static const uint8_t flow_off = 0x00U;

    if (!hci_simple(HCI_SET_EVENT_MASK, event_mask, sizeof(event_mask)) ||
        !hci_simple(HCI_LE_SET_EVENT_MASK, le_mask, sizeof(le_mask)))
    {
        return RT_FALSE;
    }
    /* The controller drops LE links unless the host has declared LE support. */
    if (!hci_simple(0x0C6DU, le_host, sizeof(le_host)))
    {
        rt_kprintf("[pjsbt] le host support rejected\n");
    }
    if (!hci_simple(0x0C31U, &flow_off, 1U))
    {
        rt_kprintf("[pjsbt] host flow control left on\n");
    }
    if (hci_simple(0x1005U, RT_NULL, 0U) && s_last_event_len >= 14U && s_last_event[6] == 0U)
    {
        /* Read Buffer Size: status, ACL length, sync length, ACL count, sync count. */
        memcpy(host_buf, &s_last_event[7], sizeof(host_buf));
        rt_kprintf("[pjsbt] controller acl %u sync %u\n",
                   (unsigned int)(s_last_event[7] | (s_last_event[8] << 8)),
                   (unsigned int)s_last_event[9]);
    }
    if (!hci_simple(0x0C33U, host_buf, sizeof(host_buf)))
    {
        rt_kprintf("[pjsbt] host buffer size rejected %02x\n",
                   s_last_event_len >= 7U ? s_last_event[6] : 0xFFU);
    }
    if (!hci_simple(0x1009U, RT_NULL, 0U))
    {
        rt_kprintf("[pjsbt] read address failed\n");
    }
    else if (s_last_event_len >= 13U)
    {
        rt_kprintf("[pjsbt] public %02x:%02x:%02x:%02x:%02x:%02x\n",
                   s_last_event[12], s_last_event[11], s_last_event[10],
                   s_last_event[9], s_last_event[8], s_last_event[7]);
    }
    if (!hci_simple(HCI_LE_SET_ADV_PARAM, adv_param, sizeof(adv_param)))
    {
        return RT_FALSE;
    }
    memset(adv, 0, sizeof(adv));
    adv[0] = 13U;
    adv[1] = 0x02U;
    adv[2] = 0x01U;
    adv[3] = 0x06U;
    adv[4] = 0x09U;
    adv[5] = 0x09U;
    memcpy(adv + 6, name, 8);
    if (!hci_simple(HCI_LE_SET_ADV_DATA, adv, 31U))
    {
        return RT_FALSE;
    }
    if (!hci_simple(HCI_LE_SET_ADV_ENABLE, &enable, 1U))
    {
        return RT_FALSE;
    }
    s_advertising = RT_TRUE;
    rt_kprintf("[pjsbt] advertising EdgiTalk\n");
    return RT_TRUE;
}

static void l2cap_send(uint16_t cid, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t header[9];
    uint16_t acl = (uint16_t)(4U + payload_len);
    uint16_t packed = (uint16_t)(s_handle | 0x2000U);

    header[0] = 0x02U;
    header[1] = (uint8_t)packed;
    header[2] = (uint8_t)(packed >> 8);
    header[3] = (uint8_t)acl;
    header[4] = (uint8_t)(acl >> 8);
    header[5] = (uint8_t)payload_len;
    header[6] = (uint8_t)(payload_len >> 8);
    header[7] = (uint8_t)cid;
    header[8] = (uint8_t)(cid >> 8);
    (void)uart_write(header, sizeof(header));
    (void)uart_write(payload, payload_len);
}

static void acl_send(const uint8_t *att, uint16_t att_len)
{
    l2cap_send(ATT_CID, att, att_len);
}

static void att_error(uint8_t request, uint16_t handle, uint8_t status)
{
    uint8_t body[5] = {ATT_ERROR, request, (uint8_t)handle, (uint8_t)(handle >> 8), status};
    acl_send(body, sizeof(body));
}

static rt_bool_t token_matches(const char *json)
{
    char expected[8];
    const char *found;

    if (!pocketjs_game_token(expected))
    {
        return RT_FALSE;
    }
    found = rt_strstr(json, "\"token\":\"");
    if (found == RT_NULL)
    {
        return RT_FALSE;
    }
    found += 9;
    return rt_strncmp(found, expected, 6) == 0 && found[6] == '"';
}

static void accept_write(const uint8_t *value, uint16_t length, rt_bool_t respond)
{
    if (length >= sizeof(s_json))
    {
        length = sizeof(s_json) - 1U;
    }
    memcpy(s_json, value, length);
    s_json[length] = '\0';
    if (respond)
    {
        uint8_t ok = 0x13U;
        acl_send(&ok, 1U);
    }
    if (!token_matches((const char *)s_json) || !pocketjs_dashboard_pc_ingest((const char *)s_json))
    {
        rt_kprintf("[pjsbt] stats rejected\n");
        return;
    }
    rt_kprintf("[pjsbt] stats accepted\n");
}

static void att_request(const uint8_t *att, uint16_t length)
{
    uint8_t opcode;
    uint16_t start;
    uint16_t end;
    uint8_t response[24];

    if (length < 1U)
    {
        return;
    }
    opcode = att[0];
    if (opcode == ATT_MTU_REQ && length >= 3U)
    {
        response[0] = 0x03U;
        response[1] = 185U;
        response[2] = 0U;
        acl_send(response, 3U);
        return;
    }
    if (opcode == ATT_READ_GROUP_REQ && length >= 7U)
    {
        start = (uint16_t)(att[1] | (att[2] << 8));
        end = (uint16_t)(att[3] | (att[4] << 8));
        if (length == 7U && (uint16_t)(att[5] | (att[6] << 8)) != UUID_PRIMARY)
        {
            att_error(opcode, start, 0x0AU);
            return;
        }
        if (start <= HANDLE_SVC && end >= HANDLE_SVC)
        {
            response[0] = 0x11U;
            response[1] = 6U;
            response[2] = HANDLE_SVC;
            response[3] = 0U;
            response[4] = HANDLE_VALUE;
            response[5] = 0U;
            response[6] = (uint8_t)UUID_SERVICE;
            response[7] = (uint8_t)(UUID_SERVICE >> 8);
            acl_send(response, 8U);
            return;
        }
        att_error(opcode, start, 0x0AU);
        return;
    }
    if (opcode == ATT_READ_TYPE_REQ && length >= 7U)
    {
        start = (uint16_t)(att[1] | (att[2] << 8));
        end = (uint16_t)(att[3] | (att[4] << 8));
        /* Only the characteristic declaration (0x2803) exists. Included
         * services, descriptors and everything else are not found. */
        if (length != 7U || (uint16_t)(att[5] | (att[6] << 8)) != UUID_CHAR_DECL ||
            end < HANDLE_DECL)
        {
            att_error(opcode, start, 0x0AU);
            return;
        }
        response[0] = 0x09U;
        response[1] = 7U;
        response[2] = HANDLE_DECL;
        response[3] = 0U;
        response[4] = CHAR_PROPS;
        response[5] = HANDLE_VALUE;
        response[6] = 0U;
        response[7] = (uint8_t)UUID_STATS;
        response[8] = (uint8_t)(UUID_STATS >> 8);
        if (start <= HANDLE_DECL)
        {
            acl_send(response, 9U);
            return;
        }
        att_error(opcode, start, 0x0AU);
        return;
    }
    if (opcode == ATT_FIND_INFO_REQ && length >= 5U)
    {
        start = (uint16_t)(att[1] | (att[2] << 8));
        end = (uint16_t)(att[3] | (att[4] << 8));
        if (start <= HANDLE_VALUE && end >= HANDLE_VALUE)
        {
            response[0] = 0x05U;
            response[1] = 0x01U;
            response[2] = HANDLE_VALUE;
            response[3] = 0U;
            response[4] = (uint8_t)UUID_STATS;
            response[5] = (uint8_t)(UUID_STATS >> 8);
            acl_send(response, 6U);
            return;
        }
        att_error(opcode, start, 0x0AU);
        return;
    }
    if ((opcode == ATT_WRITE_REQ || opcode == ATT_WRITE_CMD) && length >= 3U)
    {
        uint16_t handle = (uint16_t)(att[1] | (att[2] << 8));
        if (handle != HANDLE_VALUE)
        {
            if (opcode == ATT_WRITE_REQ)
            {
                att_error(opcode, handle, 0x0AU);
            }
            return;
        }
        accept_write(att + 3, (uint16_t)(length - 3U), opcode == ATT_WRITE_REQ);
        return;
    }
    if (opcode != ATT_WRITE_CMD)
    {
        start = length >= 3U ? (uint16_t)(att[1] | (att[2] << 8)) : 0U;
        att_error(opcode, start, 0x06U);
    }
}

static void on_event(const uint8_t *event, uint16_t length)
{
    if (length < 3U)
    {
        return;
    }
    if (event[1] == 0x05U && length >= 6U)
    {
        s_handle = 0xFFFFU;
        s_l2cap_len = 0U;
        rt_kprintf("[pjsbt] disconnect status %02x reason %02x\n", event[3], event[6]);
        (void)start_advertising();
        return;
    }
    /* 0x01 legacy, 0x0A enhanced, 0x29 enhanced v2. Handle sits in the same place. */
    if (event[1] == 0x3EU && length >= 14U && event[3] == 0x06U)
    {
        /* Accept the central's connection parameters, or it drops the link. */
        uint8_t reply[14];

        reply[0] = event[4];
        reply[1] = event[5];
        memcpy(reply + 2, event + 6, 8);
        reply[10] = 0U;
        reply[11] = 0U;
        reply[12] = 0U;
        reply[13] = 0U;
        (void)hci_simple(0x2020U, reply, sizeof(reply));
        return;
    }
    if (event[1] != 0x3EU || length < 12U || event[4] != 0U)
    {
        if (event[1] != 0x13U)
        {
            rt_kprintf("[pjsbt] evt %02x/%02x\n", event[1], length > 3U ? event[3] : 0U);
        }
        return;
    }
    if (event[3] != 0x01U && event[3] != 0x0AU && event[3] != 0x29U)
    {
        rt_kprintf("[pjsbt] le %02x\n", event[3]);
        return;
    }
    s_handle = (uint16_t)(event[5] | (event[6] << 8));
    s_advertising = RT_FALSE;
    s_l2cap_len = 0U;
    rt_kprintf("[pjsbt] connected sub %02x handle %u\n", event[3], (unsigned int)s_handle);
    if (length >= 33U)
    {
        rt_kprintf("[pjsbt] interval %u timeout %u\n",
                   (unsigned int)(event[27] | (event[28] << 8)),
                   (unsigned int)(event[31] | (event[32] << 8)));
    }
}

static void l2cap_deliver(void)
{
    uint16_t l2_len;
    uint16_t cid;

    if (s_l2cap_len < 4U)
    {
        return;
    }
    l2_len = (uint16_t)(s_l2cap[0] | (s_l2cap[1] << 8));
    cid = (uint16_t)(s_l2cap[2] | (s_l2cap[3] << 8));
    if ((uint16_t)(4U + l2_len) > s_l2cap_len || cid != ATT_CID)
    {
        return;
    }
    att_request(s_l2cap + 4, l2_len);
    s_l2cap_len = 0U;
}

static void feed_acl(const uint8_t *packet, uint16_t length)
{
    uint16_t flags;
    uint16_t pb;
    uint16_t acl_len;

    if (length < 5U)
    {
        return;
    }
    flags = (uint16_t)(packet[1] | (packet[2] << 8));
    pb = (uint16_t)((flags >> 12) & 0x3U);
    acl_len = (uint16_t)(packet[3] | (packet[4] << 8));
    if ((uint16_t)(5U + acl_len) > length)
    {
        return;
    }
    /* PB 01 continues a fragment. PB 00 and 02 start a new L2CAP PDU. */
    if (pb != 1U)
    {
        s_l2cap_len = 0U;
    }
    if ((uint16_t)(s_l2cap_len + acl_len) > sizeof(s_l2cap))
    {
        s_l2cap_len = 0U;
        return;
    }
    memcpy(s_l2cap + s_l2cap_len, packet + 5, acl_len);
    s_l2cap_len = (uint16_t)(s_l2cap_len + acl_len);
    if (s_l2cap_len >= 4U &&
        s_l2cap_len >= (uint16_t)(4U + (s_l2cap[0] | (s_l2cap[1] << 8))))
    {
        l2cap_deliver();
    }
}

static void bt_task(void *parameter)
{
    (void)parameter;
    /* CYCFG_BT_DEV_WAKE_POLARITY is active-low. Driving high puts the controller to sleep. */
    Cy_GPIO_Write(CYBSP_BT_DEVICE_WAKE_PORT, CYBSP_BT_DEVICE_WAKE_PIN, CYCFG_BT_DEV_WAKE_POLARITY);
    rt_thread_mdelay(50);
    if (uart_open() != 0)
    {
        return;
    }
    rt_kprintf("[pjsbt] uart4 open, Wi-Fi ready=%d before bring-up\n", (int)rt_wlan_is_ready());
    if (!download_patch() || !start_advertising())
    {
        rt_kprintf("[pjsbt] bring-up stopped\n");
        return;
    }
    rt_kprintf("[pjsbt] Wi-Fi ready=%d after bring-up\n", (int)rt_wlan_is_ready());
    while (1)
    {
        uint8_t packet[280];
        uint16_t got = hci_read(packet, sizeof(packet), 200U);

        if (got == 0U)
        {
            continue;
        }
        if (packet[0] == 0x04U)
        {
            on_event(packet, got);
        }
        else if (packet[0] == 0x02U)
        {
            feed_acl(packet, got);
        }
    }
}

void pocketjs_bt_start(void)
{
    rt_thread_t thread = rt_thread_create("pjsbt", bt_task, RT_NULL, 6144U, 20U, 10U);

    if (thread != RT_NULL)
    {
        rt_thread_startup(thread);
    }
}
