// 日程行时间文本的宿主机单测：gcc 直接编译 GUI.c 里的纯格式化函数，
// 不需要 SDL/面板。跑法：make -f Makefile.test test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GUI.h"

// 把 u8g2/GFX 这些渲染依赖挡掉：本测试只用 GUI_FormatScheduleLine
static int g_failures;

static void expect(const char* name, uint32_t start, uint32_t end, const char* want) {
    const char* got = GUI_FormatScheduleLine(start, end);
    if (strcmp(got, want) != 0) {
        printf("FAIL %-28s start=%u end=%u\n  want: %s\n  got : %s\n", name, start, end, want, got);
        g_failures++;
    } else {
        printf("ok   %-28s %s\n", name, got);
    }
}

// 固定东八区：transformTime 按 UTC+8 出年月日，测试数据用 UTC 秒
static uint32_t t(int y, int mo, int d, int h, int mi) {
    struct tm tm_time = {0};
    tm_time.tm_year = y - 1900;
    tm_time.tm_mon = mo - 1;
    tm_time.tm_mday = d;
    tm_time.tm_hour = h;
    tm_time.tm_min = mi;
    // 固件存的是「UTC 秒 + 时区偏移」，transformTime 再把它当 UTC 拆年月日，
    // 所以这里直接用 timegm 把墙上时间转成同口径的秒数
    return (uint32_t)timegm(&tm_time);
}

int main(void) {
    setenv("TZ", "UTC", 1);
    tzset();

    expect("同日事件只画开始", t(2026, 10, 27, 20, 0), t(2026, 10, 27, 22, 0), "10/27 20:00");
    expect("跨天事件画起止", t(2026, 10, 27, 20, 0), t(2026, 10, 29, 12, 0), "10/27 20:00-10/29 12:00");
    expect("跨月跨年", t(2026, 12, 31, 22, 30), t(2027, 1, 2, 9, 5), "12/31 22:30-01/02 09:05");
    expect("缺结束时间(0)", t(2026, 10, 27, 8, 0), 0, "10/27 08:00");
    expect("结束等于开始", t(2026, 10, 27, 8, 0), t(2026, 10, 27, 8, 0), "10/27 08:00");
    expect("跨天但结束更早(异常)", t(2026, 10, 27, 8, 0), t(2026, 10, 26, 8, 0), "10/27 08:00");
    expect("全天事件跨一天", t(2026, 10, 27, 0, 0), t(2026, 10, 28, 0, 0), "10/27 00:00-10/28 00:00");
    expect("多日长事件", t(2026, 10, 25, 9, 0), t(2026, 10, 31, 9, 0), "10/25 09:00-10/31 09:00");

    if (g_failures) {
        printf("\n%d case(s) failed\n", g_failures);
        return 1;
    }
    printf("\nall cases passed\n");
    return 0;
}
