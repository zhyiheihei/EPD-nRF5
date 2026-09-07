#include "EPD_driver.h"

bool UC81xx_ReadBusy(epd_model_t* epd) { return EPD_ReadBusy() == false; }

static void UC81xx_WaitBusy(uint16_t timeout) { EPD_WaitBusy(false, timeout); }

static void UC81xx_PowerOn(epd_model_t* epd) {
    EPD_WriteCmd(UC81xx_PON);
    UC81xx_WaitBusy(200);
}

static void UC81xx_PowerOff(epd_model_t* epd) {
    EPD_WriteCmd(UC81xx_POF);
    UC81xx_WaitBusy(200);
}

// Read temperature from driver chip
int8_t UC81xx_ReadTemp(epd_model_t* epd) {
    EPD_WriteCmd(UC81xx_TSC);
    UC81xx_WaitBusy(100);
    return (int8_t)EPD_ReadByte();
}

static void UC81xx_SetWindowEx(epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint8_t pt_scan) {
    uint16_t xe = (x + w - 1) | 0x0007;
    uint16_t ye = y + h - 1;
    x &= 0xFFF8;
    // 末字节 PT_SCAN：局部刷新必须置 1（仅扫描窗口内），否则控制器全屏扫描，
    // 窗口外像素（含红色）会被局部波形错误驱动（实测：左侧变红、周圈变灰）
    EPD_Write(UC81xx_PTL, x / 256, x % 256, xe / 256, xe % 256, y / 256, y % 256, ye / 256, ye % 256,
              pt_scan);
}

void UC81xx_SetWindow(epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    UC81xx_SetWindowEx(epd, x, y, w, h, 0x00);
}

void UC81xx_Refresh(epd_model_t* epd) {
    EPD_DEBUG("refresh begin");

    UC81xx_SetWindow(epd, 0, 0, epd->width, epd->height);

    EPD_WriteCmd(UC81xx_DRF);
    delay(100);
    UC81xx_WaitBusy(UINT16_MAX);

    EPD_DEBUG("refresh end");
}

// 局部刷新：OTP 波形在 PTL 窗口内生效（GD7965/UC8179 官方部分窗口流程），
// 仅更新指定区域，速度远快于全刷且不闪屏。BWR 屏上仅适用于黑白内容区域；
// 残影由每日午夜全刷与开机全刷清除。
// 注意：这块 BWR 屏局部 DRF 后 BUSY 线实测不释放（波形视觉上已完成），
// 因此短探测 BUSY 后固定等待波形执行完毕再发 PTOUT，不依赖 BUSY 释放。
#define UC81xx_PARTIAL_BUSY_PROBE 500
#define UC81xx_PARTIAL_WAVEFORM_WAIT 4000

void UC81xx_RefreshPartial(epd_model_t* epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    EPD_DEBUG("partial refresh begin");

    EPD_WriteCmd(UC81xx_PTIN);  // partial in
    UC81xx_SetWindowEx(epd, x, y, w, h, 0x01);  // PT_SCAN=1：仅扫描窗口内

    EPD_WriteCmd(UC81xx_DRF);
    delay(100);
    EPD_WaitBusy(false, UC81xx_PARTIAL_BUSY_PROBE);
    nrf_delay_ms(UC81xx_PARTIAL_WAVEFORM_WAIT);

    EPD_WriteCmd(UC81xx_PTOUT);  // partial out
    EPD_DEBUG("partial refresh end");
}

void UC81xx_Init(epd_model_t* epd) {
    EPD_Reset(true, 50);
    EPD_Write(UC81xx_PSR, 0x0F);
    EPD_Write(UC81xx_CDI, 0x77);
    UC81xx_PowerOn(epd);
    UC81xx_SetWindow(epd, 0, 0, epd->width, epd->height);
}

#if 0  // Not used by the dedicated dashboard protocol.
void UC81xx_Clear(epd_model_t* epd, bool refresh) {
    uint32_t wb = (epd->width + 7) / 8;
    EPD_FillRAM(UC81xx_DTM1, 0xFF, wb * epd->height);
    EPD_FillRAM(UC81xx_DTM2, 0xFF, wb * epd->height);
    if (refresh) UC81xx_Refresh(epd);
}
#endif

void UC8176_WriteImage(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                       uint16_t h) {
    uint16_t wb = (w + 7) / 8;  // width bytes, bitmaps are padded
    x -= x % 8;                 // byte boundary
    w = wb * 8;                 // byte boundary
    if (x + w > epd->width || y + h > epd->height) return;

    EPD_WriteCmd(UC81xx_PTIN);  // partial in
    UC81xx_SetWindow(epd, x, y, w, h);
    if (epd->color == COLOR_BWR) {
        EPD_WriteCmd(UC81xx_DTM1);
        for (uint16_t i = 0; i < h; i++) {
            for (uint16_t j = 0; j < w / 8; j++) EPD_WriteByte(black ? black[j + i * wb] : 0xFF);
        }
    }
    EPD_WriteCmd(UC81xx_DTM2);
    for (uint16_t i = 0; i < h; i++) {
        for (uint16_t j = 0; j < w / 8; j++) {
            if (epd->color == COLOR_BWR)
                EPD_WriteByte(color ? color[j + i * wb] : 0xFF);
            else
                EPD_WriteByte(black[j + i * wb]);
        }
    }
    EPD_WriteCmd(UC81xx_PTOUT);  // partial out
}

#if 0  // Other controller families are excluded from the dedicated UC8179 build.
static void UC8159_SendPixel(uint8_t black_data, uint8_t color_data) {
    uint8_t data;
    for (uint8_t j = 0; j < 8; j++) {
        if ((color_data & 0x80) == 0x00)
            data = 0x04;  // red
        else if ((black_data & 0x80) == 0x00)
            data = 0x00;  // black
        else
            data = 0x03;  // white
        data = (data << 4) & 0xFF;
        black_data = (black_data << 1) & 0xFF;
        color_data = (color_data << 1) & 0xFF;
        j++;
        if ((color_data & 0x80) == 0x00)
            data |= 0x04;  // red
        else if ((black_data & 0x80) == 0x00)
            data |= 0x00;  // black
        else
            data |= 0x03;  // white
        black_data = (black_data << 1) & 0xFF;
        color_data = (color_data << 1) & 0xFF;
        EPD_WriteByte(data);
    }
}

void UC8159_WriteImage(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                       uint16_t h) {
    uint16_t wb = (w + 7) / 8;  // width bytes, bitmaps are padded
    x -= x % 8;                 // byte boundary
    w = wb * 8;                 // byte boundary
    if (x + w > epd->width || y + h > epd->height) return;

    EPD_WriteCmd(UC81xx_PTIN);  // partial in
    UC81xx_SetWindow(epd, x, y, w, h);
    EPD_WriteCmd(UC81xx_DTM1);
    for (uint16_t i = 0; i < h; i++) {
        for (uint16_t j = 0; j < w / 8; j++) {
            uint8_t black_data = 0xFF;
            uint8_t color_data = 0xFF;
            if (black) black_data = black[j + i * wb];
            if (color) color_data = color[j + i * wb];
            UC8159_SendPixel(black_data, color_data);
        }
    }
    EPD_WriteCmd(UC81xx_PTOUT);  // partial out
}

void JD79668_WriteImage(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                        uint16_t h) {
    uint16_t wb = (w + 3) / 4;  // width bytes, bitmaps are padded
    x -= x % 4;                 // byte boundary
    w = wb * 4;                 // byte boundary
    if (x + w > epd->width || y + h > epd->height) return;

    UC81xx_SetWindow(epd, x, y, w, h);
    EPD_WriteCmd(UC81xx_DTM1);
    for (uint16_t i = 0; i < h; i++) {
        for (uint16_t j = 0; j < wb; j++) {
            // black buffer contains the packed 2bpp data
            // If black is NULL, write 0x55 (White: 01 01 01 01)
            EPD_WriteByte(black ? black[j + i * wb] : 0x55);
        }
    }
}
#endif

void UC81xx_WriteImage(epd_model_t* epd, uint8_t* black, uint8_t* color, uint16_t x, uint16_t y, uint16_t w,
                       uint16_t h) {
    UC8176_WriteImage(epd, black, color, x, y, w, h);
}

#if 0  // Legacy full-screen RAM streaming is excluded.
void UC81xx_WriteRam(epd_model_t* epd, uint8_t cfg, uint8_t* data, uint8_t len) {
    bool begin = (cfg >> 4) == 0x00;
    bool black = (cfg & 0x0F) == 0x0F;
    if (begin && black) UC81xx_SetWindow(epd, 0, 0, epd->width, epd->height);
    if (begin) {
        EPD_WriteCmd(black ? UC81xx_DTM1 : UC81xx_DTM2);
    }
    EPD_WriteData(data, len);
}
#endif

void UC81xx_Sleep(epd_model_t* epd) {
    UC81xx_PowerOff(epd);
    delay(100);
    EPD_Write(UC81xx_DSLP, 0xA5);
}

// Declare driver and models
static const epd_driver_t epd_drv_uc81xx = {
    .init = UC81xx_Init,
    .clear = NULL,
    .write_image = UC81xx_WriteImage,
    .write_ram = NULL,
    .refresh = UC81xx_Refresh,
    .refresh_partial = UC81xx_RefreshPartial,
    .sleep = UC81xx_Sleep,
    .read_temp = UC81xx_ReadTemp,
    .read_busy = UC81xx_ReadBusy,
};

// UC8176 400x300 Black/White
const epd_model_t epd_uc8176_420_bw = {UC8176_420_BW, COLOR_BW, &epd_drv_uc81xx, DRV_IC_UC8176, 400, 300};
// UC8176 400x300 Black/White/Red
const epd_model_t epd_uc8176_420_bwr = {UC8176_420_BWR, COLOR_BWR, &epd_drv_uc81xx, DRV_IC_UC8176, 400, 300};
// UC8159 640x384 Black/White
const epd_model_t epd_uc8159_750_bw = {UC8159_750_LOW_BW, COLOR_BW, &epd_drv_uc81xx, DRV_IC_UC8159, 640, 384};
// UC8159 640x384 Black/White/Red
const epd_model_t epd_uc8159_750_bwr = {UC8159_750_LOW_BWR, COLOR_BWR, &epd_drv_uc81xx, DRV_IC_UC8159, 640, 384};
// UC8179 800x480 Black/White/Red
const epd_model_t epd_uc8179_750_bw = {UC8179_750_BW, COLOR_BW, &epd_drv_uc81xx, DRV_IC_UC8179, 800, 480};
// UC8179 800x480 Black/White/Red
const epd_model_t epd_uc8179_750_bwr = {UC8179_750_BWR, COLOR_BWR, &epd_drv_uc81xx, DRV_IC_UC8179, 800, 480};
// JD79668 400x300 Black/White/Red/Yellow
const epd_model_t epd_jd79668_420_bwry = {JD79668_420_BWRY, COLOR_BWRY, &epd_drv_uc81xx, DRV_IC_JD79668, 400, 300};
// JD79665 800x480 Black/White/Red/Yellow
const epd_model_t epd_jd79665_750_bwry = {JD79665_750_BWRY, COLOR_BWRY, &epd_drv_uc81xx, DRV_IC_JD79665, 800, 480};
// JD79665 648x480 Black/White/Red/Yellow
const epd_model_t epd_jd79665_583_bwry = {JD79665_583_BWRY, COLOR_BWRY, &epd_drv_uc81xx, DRV_IC_JD79665, 648, 480};
