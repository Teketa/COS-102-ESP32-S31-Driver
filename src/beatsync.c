#include "beatsync.h"
#include "beatclock.h"
#include "settings.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#define TAG "beat"
#define TIMEOUT_US 1500000      // no report for this long = not synced

beat_frame_t g_bf = { .synced = false, .beats = 0, .bpm = 120.0f, .vu = 0 };

static beatclock_t s_bc;
static SemaphoreHandle_t s_lock;
static uint32_t s_rx;           // datagrams with a valid header

// Mixxx sampler play state (sample pads), independent of beat sync.
static uint64_t s_smask;
static uint32_t s_sorder[64];
static uint32_t s_sorder_ctr;
static int64_t s_sstart[64];    // when each sampler last started (us)
static int s_slast;
static int64_t s_srx;

static float rd_f32(const uint8_t *p)
{
    float f;
    memcpy(&f, p, 4);           // RISC-V and x86 are both little-endian
    return f;
}

static void udp_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(BEAT_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "bind :%d failed", BEAT_UDP_PORT);
        close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening for beat reports on UDP %d", BEAT_UDP_PORT);
    uint8_t buf[64];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        // Timestamp first: the arrival time is the beat clock's reference.
        int64_t t = esp_timer_get_time();
        if (n < 20 || memcmp(buf, "C102", 4) || buf[4] != 1) continue;
        s_rx++;
        if (n >= 28) {
            uint64_t m;
            memcpy(&m, buf + 20, 8);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint64_t started = m & ~s_smask;
            for (int i = 0; i < 64; i++)
                if (started >> i & 1) { s_sorder[i] = ++s_sorder_ctr; s_sstart[i] = t; s_slast = i + 1; }
            s_smask = m;
            s_srx = t;
            xSemaphoreGive(s_lock);
        }
        if (!g_beat.enabled) continue;
        bool playing = buf[5] & 1;
        uint8_t deck = buf[6];
        float bpm = rd_f32(buf + 8), phase = rd_f32(buf + 12), vu = rd_f32(buf + 16);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bc_update(&s_bc, t, bpm, phase, playing, deck, vu);
        xSemaphoreGive(s_lock);
    }
}

void beatsync_start(void)
{
    bc_init(&s_bc);
    s_lock = xSemaphoreCreateMutex();
    // Core 0 with the network stack, above the content task (5) so the
    // arrival timestamp isn't delayed by effect rendering.
    xTaskCreatePinnedToCore(udp_task, "beat_udp", 3072, NULL, 8, NULL, 0);
}

void beatsync_frame(int64_t t_vis, uint64_t anim_us, beat_frame_t *out)
{
    double b = 0;
    bool ok = false;
    float bpm = 120.0f, vu = 0.0f;
    if (s_lock && g_beat.enabled) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ok = bc_beats_at(&s_bc, t_vis - (int64_t)g_beat.offset_ms * 1000, TIMEOUT_US, &b);
        if (ok) { bpm = s_bc.bpm; vu = s_bc.vu; }
        xSemaphoreGive(s_lock);
    }
    if (!ok) b = (double)anim_us / 500000.0;   // 120 BPM at speed 50
    out->synced = ok;
    out->beats = b;
    out->bpm = bpm;
    out->vu = vu;
}

void beatsync_status(beat_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->age_ms = -1;
    if (!s_lock) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st->synced = g_beat.enabled && s_bc.locked && s_bc.playing &&
                 s_bc.last_rx && now - s_bc.last_rx <= TIMEOUT_US;
    st->bpm = s_bc.bpm;
    st->deck = s_bc.deck;
    st->vu = s_bc.vu;
    if (s_bc.last_rx) st->age_ms = (int32_t)((now - s_bc.last_rx) / 1000);
    st->err_ms = s_bc.err_ms;
    st->packets = s_rx;
    st->deck_changes = s_bc.deck_changes;
    xSemaphoreGive(s_lock);
}

uint64_t beatsync_samplers(uint32_t *order, int64_t *start)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t m = (s_srx && esp_timer_get_time() - s_srx <= TIMEOUT_US) ? s_smask : 0;
    if (order) memcpy(order, s_sorder, sizeof(s_sorder));
    if (start) memcpy(start, s_sstart, sizeof(s_sstart));
    xSemaphoreGive(s_lock);
    return m;
}

int beatsync_sampler_last(void) { return s_slast; }
