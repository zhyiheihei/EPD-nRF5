// GUI emulator, cross-platform (SDL2)
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "GUI.h"

#define BITMAP_WIDTH 800
#define BITMAP_HEIGHT 480
#define WINDOW_WIDTH 850
#define WINDOW_HEIGHT 560

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0xFF000000
#define COLOR_RED 0xFFFF0000

static display_mode_t g_display_mode = MODE_CALENDAR;
static int g_bwr_mode = 1;
static uint8_t g_week_start = 0;
static time_t g_display_time;
static uint32_t g_framebuffer[BITMAP_WIDTH * BITMAP_HEIGHT];

static void DrawBitmap(void* user_data, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const int bytes_per_row = (w + 7) / 8;

    for (int row = 0; row < h; row++) {
        uint32_t* dst = &g_framebuffer[(uint32_t)(y + row) * BITMAP_WIDTH + x];
        const uint8_t* black_row = &black[row * bytes_per_row];
        const uint8_t* color_row = color ? &color[row * bytes_per_row] : NULL;

        for (int col = 0; col < w; col++) {
            const int byte = col / 8, bit = 7 - (col % 8);
            const int ink_black = !((black_row[byte] >> bit) & 0x01);
            const int ink_color = color_row ? !((color_row[byte] >> bit) & 0x01) : 0;

            dst[col] = ink_color ? COLOR_RED : (ink_black ? COLOR_BLACK : COLOR_WHITE);
        }
    }
}

static void set_title(SDL_Window* win) {
    SDL_SetWindowTitle(win, g_display_mode == MODE_CLOCK ? "模拟器 - 时钟模式" : "模拟器 - 日历模式");
}

static void redraw(SDL_Renderer* ren, SDL_Texture* tex) {
    gui_data_t data = {
        .mode = g_display_mode,
        .color = g_bwr_mode ? 2 : 1,
        .width = BITMAP_WIDTH,
        .height = BITMAP_HEIGHT,
        .timestamp = (uint32_t)g_display_time,
        .week_start = g_week_start,
        .temperature = 25,
        .voltage = 2920,
        .ssid = "NRF_EPD_84AC",
        .schedule_count = 2,
        .schedules = {
            {g_display_time + 3600, g_display_time + 7200, "面包"},
            {g_display_time + 86400 + 9 * 3600, g_display_time + 3 * 86400 + 18 * 3600, "牛奶"},
        },
        .food_count = 4,
        .foods = {
            {g_display_time + 2 * 86400, "牛奶", FOOD_TYPE_DRINK},
            {g_display_time + 5 * 86400, "咖啡", FOOD_TYPE_DRINK},
            {g_display_time + 12 * 86400, "巧克力", FOOD_TYPE_FOOD},
            {g_display_time + 7 * 86400, "面包", FOOD_TYPE_FOOD},
        },
    };

    memset(g_framebuffer, 0xFF, sizeof(g_framebuffer));
    DrawGUI(&data, DrawBitmap, NULL);
    SDL_UpdateTexture(tex, NULL, g_framebuffer, BITMAP_WIDTH * sizeof(uint32_t));

    SDL_Rect dst = {(WINDOW_WIDTH - BITMAP_WIDTH) / 2, (WINDOW_HEIGHT - BITMAP_HEIGHT) / 2, BITMAP_WIDTH, BITMAP_HEIGHT};
    SDL_SetRenderDrawColor(ren, 240, 240, 240, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 0, 0, 255, 255);
    SDL_RenderDrawRect(ren, &(SDL_Rect){dst.x - 1, dst.y - 1, dst.w + 2, dst.h + 2});
    SDL_RenderCopy(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}

static void tick_clock(SDL_Renderer* ren, SDL_Texture* tex) {
    time_t now = time(NULL) + 8 * 3600;
    if (g_display_mode == MODE_CLOCK && now / 60 != g_display_time / 60) {
        g_display_time = now;
        redraw(ren, tex);
    }
}

static void save_ppm(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", BITMAP_WIDTH, BITMAP_HEIGHT);
    for (int i = 0; i < BITMAP_WIDTH * BITMAP_HEIGHT; i++) {
        uint8_t rgb[3] = {(g_framebuffer[i] >> 16) & 0xFF, (g_framebuffer[i] >> 8) & 0xFF, g_framebuffer[i] & 0xFF};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void handle_key(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex, SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_SPACE) {
        g_display_mode = (g_display_mode == MODE_CLOCK) ? MODE_CALENDAR : MODE_CLOCK;
        set_title(win);
    } else if (sc == SDL_SCANCODE_R) {
        g_bwr_mode = !g_bwr_mode;
    } else if (sc == SDL_SCANCODE_W) {
        g_week_start = (g_week_start + 1) % 7;
    } else if (sc >= SDL_SCANCODE_UP && sc <= SDL_SCANCODE_RIGHT) {
        struct tm tm_time = *localtime(&g_display_time);

        // 上/下调整月份，左/右调整日期
        switch (sc) {
            case SDL_SCANCODE_UP:
                tm_time.tm_mon = tm_time.tm_mon == 11 ? 0 : tm_time.tm_mon + 1;
                break;
            case SDL_SCANCODE_DOWN:
                tm_time.tm_mon = tm_time.tm_mon == 0 ? 11 : tm_time.tm_mon - 1;
                break;
            case SDL_SCANCODE_RIGHT:
                tm_time.tm_mday++;
                break;
            case SDL_SCANCODE_LEFT:
                tm_time.tm_mday--;
                break;
            default:
                return;
        }

        g_display_time = mktime(&tm_time);
    } else {
        return;
    }

    redraw(ren, tex);
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("模拟器", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH,
                                       WINDOW_HEIGHT, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture* tex =
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, BITMAP_WIDTH, BITMAP_HEIGHT);

    g_display_time = time(NULL) + 8 * 3600;
    set_title(win);
    redraw(ren, tex);

    if (argc > 1) {
        save_ppm(argv[1]);
        goto done;
    }

    printf("空格 - 切换模式 | R - 切换颜色 | W - 星期起点 | 方向键 - 调整日期/月份\n");

    for (;;) {
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 200)) {
            tick_clock(ren, tex);
            continue;
        }

        do {
            if (ev.type == SDL_QUIT) goto done;
            if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) handle_key(win, ren, tex, ev.key.keysym.scancode);
        } while (SDL_PollEvent(&ev));
    }

done:
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
