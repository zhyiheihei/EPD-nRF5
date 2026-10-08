#ifndef _MAIN_H_
#define _MAIN_H_

#include <stdint.h>

uint32_t timestamp(void);
void set_timestamp(uint32_t timestamp);
void sleep_mode_enter(void);
void app_feed_wdt(void);
// 连续复位计数 / 本次运行秒数 / 本次复位原因：main.c 维护（static）
// 语义的内部状态，这里以 extern + 内联读出，省掉跨文件调用桩
// （应用闪存预算 0xE000 已贴线，能省则省）。
extern uint32_t m_boot_count;
extern uint32_t m_uptime_seconds;
extern uint32_t m_resetreas;

static inline uint32_t app_boot_count(void) { return m_boot_count; }
static inline uint32_t app_uptime_seconds(void) { return m_uptime_seconds; }
static inline uint32_t app_reset_reason(void) { return m_resetreas; }

#endif
