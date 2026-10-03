// main/badge_sfx.h —— 记账提示音。合成音,不占 Flash 素材;在独立低优先级任务里播放。
#pragma once

#include "esp_err.h"

typedef enum {
    BADGE_SFX_RECORD = 0,   // 记账成功:上行"叮咚"
    BADGE_SFX_UNDO,         // 撤销:下行两声
    BADGE_SFX_CLEAR,        // 清零:低音长音
    BADGE_SFX_ERROR,        // 失败:短促低音
} badge_sfx_t;

// 需在 bsp_audio_init() 成功后调用。失败时胸牌照常工作,只是没有声音。
esp_err_t badge_sfx_init(void);

// 非阻塞:排队后立即返回,队列满时丢弃。可在 lifecycle 任务中调用,不能在按键回调中调用。
void badge_sfx_play(badge_sfx_t sfx);
