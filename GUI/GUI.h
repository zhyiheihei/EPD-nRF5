#ifndef __GUI_H
#define __GUI_H

#include "Adafruit_GFX.h"

typedef enum {
    MODE_PICTURE = 0,
    MODE_CALENDAR = 1,
    MODE_CLOCK = 2,
} display_mode_t;

#define GUI_MAX_SCHEDULES 6
#define GUI_MAX_FOODS 4

typedef enum {
    FOOD_TYPE_FOOD = 0,
    FOOD_TYPE_DRINK = 1,
} gui_food_type_t;

typedef struct {
    uint32_t start_time;
    char title[32];
} gui_schedule_t;

typedef struct {
    uint32_t expires_at;
    char name[20];
    gui_food_type_t type;
} gui_food_t;

typedef struct {
    display_mode_t mode;
    uint16_t color;
    uint16_t width;
    uint16_t height;
    uint32_t timestamp;
    uint8_t week_start;  // 0: Sunday, 1: Monday
    int8_t temperature;
    uint16_t voltage;
    char ssid[20];
    uint8_t schedule_count;
    gui_schedule_t schedules[GUI_MAX_SCHEDULES];
    uint8_t food_count;
    gui_food_t foods[GUI_MAX_FOODS];
} gui_data_t;

void DrawGUI(gui_data_t* data, buffer_callback callback, void* callback_data);

#endif
