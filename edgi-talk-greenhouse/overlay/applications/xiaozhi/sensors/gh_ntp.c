/*
 * gh_ntp.c - 菜园管家 NTP 网络校时
 *
 * 极简 SNTP v4 客户端：向公共 NTP 服务器（UDP/123）发 48 字节请求，
 * 取回 Transmit Timestamp（秒字段），换算成 Unix 秒后用 settimeofday()
 * 写进 RTC。RTC 内部存 UTC，localtime_r() 配合
 * RT_LIBC_USING_LIGHT_TZ_DST(+8) 自动显示北京时间。
 *
 * 实现要点：
 * - 独立线程 + 事件触发，gh_ntp_sync_async() 只置位不阻塞，UI 可安全调用；
 * - 依次尝试多个服务器（ntp.aliyun.com / ntp1.aliyun.com / time.windows.com /
 *   pool.ntp.org），单次超时 3s，全部失败置 FAIL 供 UI 提示；
 * - 成功仅写一次 RTC；gh_ntp_synced() 报告"校时成功过"让日历页点亮。
 *
 * Change Logs:
 * 2026-11-01 新增：SNTP v4 极简实现（UDP/123），多服务器轮询，自动+手动触发。
 */
#include <rtthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>

#include "gh_ntp.h"

/* 与 sensor_monitor.c 同款：直接用 rt_kprintf 打日志 */

/* NTP 端口与请求包长 */
#define NTP_PORT        123
#define NTP_PACKET_LEN  48
/* 2208988800 = 1900-01-01 到 1970-01-01 的秒数，NTP 纪元偏移 */
#define NTP_DELTA       2208988800ULL
/* 单个服务器响应等待超时 */
#define NTP_TIMEOUT_MS  3000
/* 同步线程优先级（低于传感器，避免抢占采样） */
#define NTP_PRIO        26
#define NTP_STACK       2048

/* 触发同步的事件位 */
#define EV_NTP_RUN      (1u << 0)

static rt_event_t  s_ev;
static rt_thread_t s_tid;
static volatile gh_ntp_state_t s_state = GH_NTP_IDLE;
static volatile rt_bool_t      s_ever_synced = RT_FALSE;

/* 公共 NTP 服务器池（国内优先，依次回退） */
static const char * const NTP_SERVERS[] =
{
    "ntp.aliyun.com",
    "ntp1.aliyun.com",
    "ntp.ntsc.ac.cn",
    "pool.ntp.org",
    "time.windows.com",
};
#define NTP_SERVER_NUM  (sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]))

gh_ntp_state_t gh_ntp_get_state(void)
{
    return s_state;
}

rt_bool_t gh_ntp_synced(void)
{
    return s_ever_synced;
}

/* 向单个服务器请求一次 Unix 秒，成功返回 0 并写入 *out_sec */
static int ntp_query_one(const char *host, time_t *out_sec)
{
    int ret = -RT_ERROR;
    int sock = -1;
    struct addrinfo hints = {0}, *res = RT_NULL;
    char port_str[8];

    rt_snprintf(port_str, sizeof(port_str), "%d", NTP_PORT);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == RT_NULL)
    {
        rt_kprintf("[NTP] resolve %s failed\n", host);
        return -RT_ERROR;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        rt_kprintf("[NTP] socket failed\n");
        goto _exit;
    }

    /* 收/发超时 3s */
    {
        struct timeval tv = { NTP_TIMEOUT_MS / 1000, (NTP_TIMEOUT_MS % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    /* 构造 SNTP 请求：LI=0 VN=4 Mode=3(客户端) -> 0x23 */
    {
        rt_uint8_t pkt[NTP_PACKET_LEN];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x23;

        if (sendto(sock, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen) != sizeof(pkt))
        {
            rt_kprintf("[NTP] sendto %s failed\n", host);
            goto _exit;
        }

        if (recvfrom(sock, pkt, sizeof(pkt), 0, RT_NULL, RT_NULL) != NTP_PACKET_LEN)
        {
            rt_kprintf("[NTP] recv %s timeout\n", host);
            goto _exit;
        }

        /* Transmit Timestamp 在字节 40-43（big-endian） */
        {
            rt_uint32_t ntp_sec = ((rt_uint32_t)pkt[40] << 24) |
                                  ((rt_uint32_t)pkt[41] << 16) |
                                  ((rt_uint32_t)pkt[42] << 8)  |
                                  ((rt_uint32_t)pkt[43]);
            if (ntp_sec < NTP_DELTA)
            {
                rt_kprintf("[NTP] invalid ts %lu\n", (unsigned long)ntp_sec);
                goto _exit;
            }
            *out_sec = (time_t)(ntp_sec - NTP_DELTA);
            ret = 0;
        }
    }

_exit:
    if (sock >= 0)
        closesocket(sock);
    if (res)
        freeaddrinfo(res);
    return ret;
}

static void ntp_thread_entry(void *param)
{
    (void)param;
    s_ev = rt_event_create("gh_ntp", RT_IPC_FLAG_FIFO);
    if (s_ev == RT_NULL)
    {
        rt_kprintf("[NTP] create event failed\n");
        return;
    }

    while (1)
    {
        rt_uint32_t recv = 0;
        /* 阻塞等待一次同步触发（开机由 gh_ntp_init 置位，按钮/命令也可置位） */
        rt_event_recv(s_ev, EV_NTP_RUN, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &recv);

        s_state = GH_NTP_RUNNING;
        rt_kprintf("[NTP] sync start\n");

        rt_bool_t ok = RT_FALSE;
        for (rt_size_t i = 0; i < NTP_SERVER_NUM; i++)
        {
            time_t sec = 0;
            if (ntp_query_one(NTP_SERVERS[i], &sec) == 0)
            {
                struct timeval tv = { sec, 0 };
                if (settimeofday(&tv, RT_NULL) == 0)
                {
                    s_ever_synced = RT_TRUE;
                    ok = RT_TRUE;
                    rt_kprintf("[NTP] %s -> %lu (RTC set)\n", NTP_SERVERS[i],
                          (unsigned long)sec);
                }
                else
                {
                    rt_kprintf("[NTP] settimeofday failed\n");
                }
                break;
            }
        }

        s_state = ok ? GH_NTP_OK : GH_NTP_FAIL;
        if (!ok)
            rt_kprintf("[NTP] all servers failed\n");
    }
}

rt_err_t gh_ntp_sync_async(void)
{
    if (s_ev == RT_NULL)
        return -RT_ERROR;
    rt_event_send(s_ev, EV_NTP_RUN);
    return RT_EOK;
}

void gh_ntp_init(void)
{
    if (s_tid != RT_NULL)
        return;
    s_tid = rt_thread_create("gh_ntp", ntp_thread_entry, RT_NULL,
                             NTP_STACK, NTP_PRIO, 10);
    if (s_tid == RT_NULL)
    {
        rt_kprintf("[NTP] create thread failed\n");
        return;
    }
    rt_thread_startup(s_tid);
    /* 事件可能尚未建好，延迟一拍再触发首次自动同步 */
    rt_thread_mdelay(50);
    gh_ntp_sync_async();
}

/* 串口手动校时：msh /> ntp_sync */
static int gh_ntp_cmd(int argc, char **argv)
{
    (void)argv;
    if (argc < 2)
    {
        rt_kprintf("ntp_sync state=%d synced=%d\n", gh_ntp_get_state(), gh_ntp_synced());
        return 0;
    }
    gh_ntp_init();
    gh_ntp_sync_async();
    rt_kprintf("ntp sync triggered\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(gh_ntp_cmd, ntp_sync, trigger NTP time sync);
