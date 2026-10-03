/* Host-preview LVGL configuration for the notification mirror.
 * Mirrors the device-relevant settings from sdkconfig.defaults; everything else
 * keeps LVGL's defaults. Used only by tools/mirror_preview.py. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
/* Same pool size as the device (CONFIG_LV_MEM_SIZE_KILOBYTES). Host pointers are
 * wider, so usage here is an upper bound of the device's. */
#define LV_MEM_SIZE (32 * 1024U)
#define LV_USE_OS LV_OS_NONE
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_NULL 1

#endif
