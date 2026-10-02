// main/badge_sfx.c —— 合成提示音:正弦 + 短起音 + 线性淡出,避免方波的刺耳与爆音。
#include "badge_sfx.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>

static const char *TAG = "badge_sfx";

#define SAMPLE_RATE   16000
#define CHUNK         256
#define AMPLITUDE     9000
#define ATTACK        48            // 3 ms 起音,消除"咔哒"声
#define SINE_BITS     8
#define SINE_SIZE     (1 << SINE_BITS)
#define VOLUME        70

typedef struct {
    uint16_t hz;
    uint16_t ms;
} note_t;

// 每个音效最多 3 个音,0 Hz 表示静音间隔,0 ms 表示结束。
static const note_t SFX_NOTES[][3] = {
    [BADGE_SFX_RECORD] = { { 1319, 70 }, { 1760, 140 }, { 0, 0 } },
    [BADGE_SFX_UNDO]   = { { 1175, 80 }, { 784, 140 }, { 0, 0 } },
    [BADGE_SFX_CLEAR]  = { { 523, 120 }, { 0, 40 }, { 392, 260 } },
    [BADGE_SFX_ERROR]  = { { 220, 90 }, { 0, 40 }, { 220, 90 } },
};

static QueueHandle_t s_queue;
static int16_t s_sine[SINE_SIZE];
static int16_t s_buf[CHUNK];

static esp_err_t play_note(const note_t *note) {
    uint32_t total = (uint32_t)SAMPLE_RATE * note->ms / 1000;
    uint32_t step = (uint32_t)(((uint64_t)note->hz << 32) / SAMPLE_RATE);
    uint32_t phase = 0;
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done < CHUNK ? total - done : CHUNK;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t t = done + i;
            int32_t sample = 0;
            if (note->hz) {
                int32_t env = (int32_t)(total - t) * AMPLITUDE / (int32_t)total;
                if (t < ATTACK) env = env * (int32_t)t / ATTACK;
                sample = s_sine[phase >> (32 - SINE_BITS)] * env / 32767;
                phase += step;
            }
            s_buf[i] = (int16_t)sample;
        }
        esp_err_t e = bsp_audio_write(s_buf, n * sizeof(int16_t));
        if (e != ESP_OK) return e;
        done += n;
    }
    return ESP_OK;
}

static void sfx_task(void *arg) {
    (void)arg;
    uint8_t sfx;
    for (;;) {
        if (xQueueReceive(s_queue, &sfx, portMAX_DELAY) != pdTRUE) continue;
        if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) continue;
        bsp_audio_set_volume(VOLUME);
        for (size_t i = 0; i < 3 && SFX_NOTES[sfx][i].ms; i++) {
            if (play_note(&SFX_NOTES[sfx][i]) != ESP_OK) {
                ESP_LOGW(TAG, "提示音播放失败");
                break;
            }
        }
        // 尾部补一段静音,把 DMA 里残留的最后一帧推出去,避免下次开播时"余音"。
        play_note(&(note_t){ .hz = 0, .ms = 40 });
    }
}

esp_err_t badge_sfx_init(void) {
    if (s_queue) return ESP_OK;
    for (int i = 0; i < SINE_SIZE; i++) {
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(2.0f * (float)M_PI * (float)i / SINE_SIZE));
    }
    s_queue = xQueueCreate(4, sizeof(uint8_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    // 优先级高于 lifecycle(5)与 LVGL(4):写 I2S 时大部分时间阻塞在 DMA 上,
    // 不会抢占界面;但页面重建时不能让它饿死,否则提示音会断续。
    if (xTaskCreate(sfx_task, "badge_sfx", 3072, NULL, 6, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void badge_sfx_play(badge_sfx_t sfx) {
    if (!s_queue || (unsigned)sfx >= sizeof(SFX_NOTES) / sizeof(SFX_NOTES[0])) return;
    uint8_t v = (uint8_t)sfx;
    (void)xQueueSend(s_queue, &v, 0);
}
