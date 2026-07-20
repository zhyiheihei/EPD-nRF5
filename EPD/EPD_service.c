/* Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include "EPD_service.h"

#include <string.h>

#include "app_scheduler.h"
#include "ble_srv_common.h"
#include "main.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_log.h"
#include "nrf_pwr_mgmt.h"
#include "sdk_macros.h"

#if defined(S112)
#define EPD_CFG_52811 {0x14, 0x13, 0x06, 0x05, 0x04, 0x03, 0x02, 0x02, 0xFF, 0x12, 0x07}
#define EPD_CFG_52810 {0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x02, 0xFF, 0x0D, 0x02}
#else
#define EPD_CFG_DEFAULT {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x03, 0x09, 0x03}
// #define EPD_CFG_DEFAULT {0x05, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x01, 0x07}
#endif

typedef struct {
    bool active;
    uint8_t transaction_id;
    uint32_t local_timestamp;
    uint8_t week_start;
    uint8_t schedule_count;
    uint8_t food_count;
    gui_schedule_t schedules[EPD_DASH_MAX_SCHEDULES];
    gui_food_t foods[EPD_DASH_MAX_FOODS];
} epd_dashboard_data_t;

static epd_dashboard_data_t m_dashboard;
static epd_dashboard_data_t m_dashboard_staging;

typedef struct {
    bool active;
    uint8_t asset;
    uint8_t flags;
    uint16_t width;
    uint8_t height;
    uint16_t total;
    uint16_t received;
    uint16_t crc;
    uint8_t data[EPD_DASH_MAX_BITMAP_BYTES];
} epd_dashboard_bitmap_t;

static epd_dashboard_bitmap_t m_dashboard_bitmap;

static uint16_t dash_be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }

static uint32_t dash_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void dash_put16(uint8_t* p, uint16_t value) {
    p[0] = value >> 8;
    p[1] = value;
}

static uint16_t dash_crc16(uint16_t crc, const uint8_t* data, uint16_t length) {
    while (length--) {
        crc ^= (uint16_t)*data++ << 8;
        for (uint8_t bit = 0; bit < 8; bit++) crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static void dash_response(ble_epd_t* p_epd, uint8_t tx, uint8_t command, epd_dashboard_status_t status,
                          const uint8_t* payload, uint8_t payload_len) {
    uint8_t response[19] = {EPD_NOTIFY_DASH_RESPONSE, EPD_DASH_PROTOCOL_VERSION, tx, command, status};
    if (payload_len > sizeof(response) - 5) payload_len = sizeof(response) - 5;
    if (payload_len) memcpy(&response[5], payload, payload_len);
    (void)ble_epd_string_send(p_epd, response, 5 + payload_len);
}

static void epd_gui_render(ble_epd_t* p_epd, uint32_t render_timestamp, bool refresh, bool sleep) {
    EPD_GPIO_Init();
    epd_model_t* epd = epd_init((epd_model_id_t)p_epd->config.model_id);
    gui_data_t data = {
        .mode = (display_mode_t)p_epd->config.display_mode,
        .color = epd->color,
        .width = epd->width,
        .height = epd->height,
        .timestamp = render_timestamp,
        .week_start = p_epd->config.week_start,
        .temperature = epd->drv->read_temp(epd),
        .voltage = EPD_ReadVoltage(),
    };

    if (m_dashboard.active) {
        data.schedule_count = m_dashboard.schedule_count;
        data.food_count = m_dashboard.food_count;
        memcpy(data.schedules, m_dashboard.schedules, sizeof(data.schedules));
        memcpy(data.foods, m_dashboard.foods, sizeof(data.foods));
    }

    uint16_t dev_name_len = sizeof(data.ssid);
    uint32_t err_code = sd_ble_gap_device_name_get((uint8_t*)data.ssid, &dev_name_len);
    if (err_code == NRF_SUCCESS && dev_name_len > 0) data.ssid[dev_name_len] = '\0';

    DrawGUI(&data, (buffer_callback)epd->drv->write_image, epd);
    if (refresh) epd->drv->refresh(epd);
    if (sleep) {
        epd->drv->sleep(epd);
        nrf_delay_ms(200);
        EPD_GPIO_Uninit();
    }

    app_feed_wdt();
}

static void epd_gui_update(void* p_event_data, uint16_t event_size) {
    epd_gui_update_event_t* event = (epd_gui_update_event_t*)p_event_data;
    epd_gui_render(event->p_epd, event->timestamp, true, true);
}

static bool dash_asset_position(uint8_t asset, uint16_t* x, uint16_t* y) {
    if (asset <= EPD_DASH_ASSET_SCHEDULE_1) {
        *x = 444;
        *y = 80 + asset * 68;
        return true;
    }
    if (asset >= EPD_DASH_ASSET_FOOD_0 && asset <= EPD_DASH_ASSET_FOOD_3) {
        *x = 488;
        *y = 246 + (asset - EPD_DASH_ASSET_FOOD_0) * 64;
        return true;
    }
    return false;
}

/**@brief Function for handling the @ref BLE_GAP_EVT_CONNECTED event from the S110 SoftDevice.
 *
 * @param[in] p_epd     EPD Service structure.
 * @param[in] p_ble_evt Pointer to the event received from BLE stack.
 */
static void on_connect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    p_epd->conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
    EPD_GPIO_Init();
}

/**@brief Function for handling the @ref BLE_GAP_EVT_DISCONNECTED event from the S110 SoftDevice.
 *
 * @param[in] p_epd     EPD Service structure.
 * @param[in] p_ble_evt Pointer to the event received from BLE stack.
 */
static void on_disconnect(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    UNUSED_PARAMETER(p_ble_evt);
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    if (p_epd->epd) {
        p_epd->epd->drv->sleep(p_epd->epd);
        nrf_delay_ms(200);  // for sleep
    }
    EPD_GPIO_Uninit();
}

static void epd_update_display_mode(ble_epd_t* p_epd, display_mode_t mode) {
    p_epd->config.display_mode = mode;
}

static void epd_send_mtu(ble_epd_t* p_epd) {
    char buf[10] = {0};
    snprintf(buf, sizeof(buf), "mtu=%d", p_epd->max_data_len);
    ble_epd_string_send(p_epd, (uint8_t*)buf, strlen(buf));
}

static void epd_service_on_write(ble_epd_t* p_epd, uint8_t* p_data, uint16_t length) {
    NRF_LOG_DEBUG("[EPD]: on_write LEN=%d\n", length);
    NRF_LOG_HEXDUMP_DEBUG(p_data, length);
    if (p_data == NULL || length <= 0) return;

    switch (p_data[0]) {
        case EPD_CMD_DASH_CAPS: {
            if (length != 2 || p_data[1] != EPD_DASH_PROTOCOL_VERSION) {
                dash_response(p_epd, 0, EPD_CMD_DASH_CAPS,
                              length == 2 ? EPD_DASH_STATUS_BAD_VERSION : EPD_DASH_STATUS_BAD_LENGTH, NULL, 0);
                break;
            }
            uint8_t caps[14] = {APP_VERSION, EPD_DASH_PROTOCOL_VERSION, EPD_DASH_MAX_SCHEDULES,
                                EPD_DASH_MAX_FOODS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            uint16_t features = EPD_DASH_FEATURE_METADATA | EPD_DASH_FEATURE_BITMAPS | EPD_DASH_FEATURE_CRC16 |
                                EPD_DASH_FEATURE_TRANSACTION | EPD_DASH_FEATURE_LOCAL_ICONS |
                                EPD_DASH_FEATURE_SYNC_TIME;
            dash_put16(&caps[4], EPD_DASH_MAX_BITMAP_BYTES);
            dash_put16(&caps[6], features);
            dash_put16(&caps[8], p_epd->max_data_len);
            dash_put16(&caps[10], 800);
            dash_put16(&caps[12], 480);
            dash_response(p_epd, 0, EPD_CMD_DASH_CAPS, EPD_DASH_STATUS_OK, caps, sizeof(caps));
        } break;

        case EPD_CMD_DASH_BEGIN: {
            if (length < 13) {
                dash_response(p_epd, length > 2 ? p_data[2] : 0, EPD_CMD_DASH_BEGIN,
                              EPD_DASH_STATUS_BAD_LENGTH, NULL, 0);
                break;
            }
            uint8_t tx = p_data[2], schedules = p_data[11], foods = p_data[12];
            uint16_t expected = 13 + schedules * 10 + foods * 6;
            if (p_data[1] != EPD_DASH_PROTOCOL_VERSION || tx == 0 || schedules > EPD_DASH_MAX_SCHEDULES ||
                foods > EPD_DASH_MAX_FOODS || length != expected || p_data[10] > 6) {
                epd_dashboard_status_t status = p_data[1] != EPD_DASH_PROTOCOL_VERSION
                                                    ? EPD_DASH_STATUS_BAD_VERSION
                                                    : (length != expected ? EPD_DASH_STATUS_BAD_LENGTH
                                                                          : EPD_DASH_STATUS_BAD_SLOT);
                dash_response(p_epd, tx, EPD_CMD_DASH_BEGIN, status, NULL, 0);
                break;
            }
            memset(&m_dashboard_staging, 0, sizeof(m_dashboard_staging));
            int16_t timezone_minutes = (int16_t)dash_be16(&p_data[8]);
            int32_t timezone_seconds = (int32_t)timezone_minutes * 60;
            m_dashboard_staging.active = true;
            m_dashboard_staging.transaction_id = tx;
            m_dashboard_staging.local_timestamp = dash_be32(&p_data[4]) + timezone_seconds;
            m_dashboard_staging.week_start = p_data[10];
            m_dashboard_staging.schedule_count = schedules;
            m_dashboard_staging.food_count = foods;
            uint16_t offset = 13;
            for (uint8_t i = 0; i < schedules; i++, offset += 10) {
                uint8_t slot = p_data[offset];
                if (slot >= EPD_DASH_MAX_SCHEDULES) {
                    memset(&m_dashboard_staging, 0, sizeof(m_dashboard_staging));
                    dash_response(p_epd, tx, EPD_CMD_DASH_BEGIN, EPD_DASH_STATUS_BAD_SLOT, NULL, 0);
                    return;
                }
                m_dashboard_staging.schedules[slot].start_time = dash_be32(&p_data[offset + 2]) + timezone_seconds;
            }
            for (uint8_t i = 0; i < foods; i++, offset += 6) {
                uint8_t slot = p_data[offset];
                if (slot >= EPD_DASH_MAX_FOODS || p_data[offset + 1] > FOOD_TYPE_DRINK) {
                    memset(&m_dashboard_staging, 0, sizeof(m_dashboard_staging));
                    dash_response(p_epd, tx, EPD_CMD_DASH_BEGIN, EPD_DASH_STATUS_BAD_SLOT, NULL, 0);
                    return;
                }
                m_dashboard_staging.foods[slot].type = (gui_food_type_t)p_data[offset + 1];
                m_dashboard_staging.foods[slot].expires_at = dash_be32(&p_data[offset + 2]) + timezone_seconds;
            }
            m_dashboard = m_dashboard_staging;
            epd_gui_render(p_epd, m_dashboard.local_timestamp, false, false);
            dash_response(p_epd, tx, EPD_CMD_DASH_BEGIN, EPD_DASH_STATUS_OK, NULL, 0);
        } break;

        case EPD_CMD_DASH_BITMAP: {
            uint8_t tx = length > 2 ? p_data[2] : 0;
            if (length < 14 || !m_dashboard_staging.active || tx != m_dashboard_staging.transaction_id) {
                dash_response(p_epd, tx, EPD_CMD_DASH_BITMAP,
                              !m_dashboard_staging.active ? EPD_DASH_STATUS_BAD_STATE
                                                          : EPD_DASH_STATUS_BAD_TRANSACTION,
                              NULL, 0);
                break;
            }
            uint8_t asset = p_data[3], flags = p_data[4], height = p_data[7];
            uint16_t width = dash_be16(&p_data[5]), total = dash_be16(&p_data[8]), offset = dash_be16(&p_data[10]);
            uint16_t crc_bytes = flags & EPD_DASH_BITMAP_END ? 2 : 0;
            uint16_t chunk = length - 12 - crc_bytes;
            uint16_t x, y;
            if (!dash_asset_position(asset, &x, &y) || total == 0 || total > EPD_DASH_MAX_BITMAP_BYTES ||
                total != ((width + 7) / 8) * height || offset + chunk > total) {
                dash_response(p_epd, tx, EPD_CMD_DASH_BITMAP, EPD_DASH_STATUS_BAD_BITMAP, NULL, 0);
                break;
            }
            if (flags & EPD_DASH_BITMAP_BEGIN) {
                memset(&m_dashboard_bitmap, 0, sizeof(m_dashboard_bitmap));
                m_dashboard_bitmap.active = true;
                m_dashboard_bitmap.asset = asset;
                m_dashboard_bitmap.flags = flags;
                m_dashboard_bitmap.width = width;
                m_dashboard_bitmap.height = height;
                m_dashboard_bitmap.total = total;
                m_dashboard_bitmap.crc = 0xFFFF;
            }
            if (!m_dashboard_bitmap.active || asset != m_dashboard_bitmap.asset || offset != m_dashboard_bitmap.received ||
                width != m_dashboard_bitmap.width || height != m_dashboard_bitmap.height || total != m_dashboard_bitmap.total) {
                dash_response(p_epd, tx, EPD_CMD_DASH_BITMAP, EPD_DASH_STATUS_BAD_BITMAP, NULL, 0);
                break;
            }
            memcpy(&m_dashboard_bitmap.data[offset], &p_data[12], chunk);
            m_dashboard_bitmap.crc = dash_crc16(m_dashboard_bitmap.crc, &p_data[12], chunk);
            m_dashboard_bitmap.received += chunk;
            if (flags & EPD_DASH_BITMAP_END) {
                uint16_t expected_crc = dash_be16(&p_data[length - 2]);
                if (m_dashboard_bitmap.received != total || m_dashboard_bitmap.crc != expected_crc) {
                    memset(&m_dashboard_bitmap, 0, sizeof(m_dashboard_bitmap));
                    dash_response(p_epd, tx, EPD_CMD_DASH_BITMAP, EPD_DASH_STATUS_BAD_CRC, NULL, 0);
                    break;
                }
                for (uint16_t i = 0; i < total; i++) m_dashboard_bitmap.data[i] = ~m_dashboard_bitmap.data[i];
                p_epd->epd->drv->write_image(p_epd->epd, m_dashboard_bitmap.data, NULL, x, y, width, height);
                memset(&m_dashboard_bitmap, 0, sizeof(m_dashboard_bitmap));
                dash_response(p_epd, tx, EPD_CMD_DASH_BITMAP, EPD_DASH_STATUS_OK, NULL, 0);
            }
        } break;

        case EPD_CMD_DASH_COMMIT: {
            uint8_t tx = length > 2 ? p_data[2] : 0;
            if (length != 4 || !m_dashboard_staging.active || tx != m_dashboard_staging.transaction_id) {
                dash_response(p_epd, tx, EPD_CMD_DASH_COMMIT,
                              !m_dashboard_staging.active ? EPD_DASH_STATUS_BAD_STATE
                                                          : EPD_DASH_STATUS_BAD_TRANSACTION,
                              NULL, 0);
                break;
            }
            m_dashboard = m_dashboard_staging;
            memset(&m_dashboard_staging, 0, sizeof(m_dashboard_staging));
            memset(&m_dashboard_bitmap, 0, sizeof(m_dashboard_bitmap));
            set_timestamp(m_dashboard.local_timestamp);
            p_epd->config.week_start = m_dashboard.week_start;
            epd_update_display_mode(p_epd, MODE_CALENDAR);
            dash_response(p_epd, tx, EPD_CMD_DASH_COMMIT, EPD_DASH_STATUS_OK, NULL, 0);
            if (p_data[3] & EPD_DASH_COMMIT_REFRESH) p_epd->epd->drv->refresh(p_epd->epd);
            if (p_data[3] & EPD_DASH_COMMIT_SLEEP) {
                p_epd->epd->drv->sleep(p_epd->epd);
                nrf_delay_ms(200);
                EPD_GPIO_Uninit();
            }
        } break;

        case EPD_CMD_DASH_ABORT:
            if (length != 3)
                dash_response(p_epd, length > 2 ? p_data[2] : 0, EPD_CMD_DASH_ABORT,
                              EPD_DASH_STATUS_BAD_LENGTH, NULL, 0);
            else {
                memset(&m_dashboard_staging, 0, sizeof(m_dashboard_staging));
                memset(&m_dashboard_bitmap, 0, sizeof(m_dashboard_bitmap));
                dash_response(p_epd, p_data[2], EPD_CMD_DASH_ABORT, EPD_DASH_STATUS_OK, NULL, 0);
            }
            break;

        case EPD_CMD_DASH_SYNC_TIME: {
            uint8_t tx = length > 2 ? p_data[2] : 0;
            if (length != 9 || p_data[1] != EPD_DASH_PROTOCOL_VERSION) {
                dash_response(p_epd, tx, EPD_CMD_DASH_SYNC_TIME,
                              length == 9 ? EPD_DASH_STATUS_BAD_VERSION : EPD_DASH_STATUS_BAD_LENGTH, NULL, 0);
                break;
            }
            int16_t timezone_minutes = (int16_t)dash_be16(&p_data[7]);
            uint32_t local_time = dash_be32(&p_data[3]) + (int32_t)timezone_minutes * 60;
            set_timestamp(local_time);
            dash_response(p_epd, tx, EPD_CMD_DASH_SYNC_TIME, EPD_DASH_STATUS_OK, NULL, 0);
        } break;

#if 0  // Removed from the dedicated fixed-hardware dashboard build.
        case EPD_CMD_SET_PINS:
            if (length < 8) return;

            p_epd->config.mosi_pin = p_data[1];
            p_epd->config.sclk_pin = p_data[2];
            p_epd->config.cs_pin = p_data[3];
            p_epd->config.dc_pin = p_data[4];
            p_epd->config.rst_pin = p_data[5];
            p_epd->config.busy_pin = p_data[6];
            p_epd->config.bs_pin = p_data[7];
            if (length > 8) p_epd->config.en_pin = p_data[8];
            epd_config_write(&p_epd->config);

            EPD_GPIO_Uninit();
            EPD_GPIO_Load(&p_epd->config);
            EPD_GPIO_Init();
            break;
#endif

        case EPD_CMD_INIT:
            p_epd->epd = epd_init((epd_model_id_t)(length > 1 ? p_data[1] : p_epd->config.model_id));
            p_epd->config.model_id = p_epd->epd->id;
            epd_send_mtu(p_epd);
            break;

#if 0  // Legacy clear/raw/full-image/configuration commands are intentionally excluded.
        case EPD_CMD_CLEAR:
            epd_update_display_mode(p_epd, MODE_PICTURE);
            if (p_epd->epd) {
                p_epd->epd->drv->init(p_epd->epd);
                p_epd->epd->drv->clear(p_epd->epd, length > 1 ? p_data[1] : true);
            }
            break;

        case EPD_CMD_SEND_COMMAND:
            if (length < 2) return;
            EPD_WriteCmd(p_data[1]);
            break;

        case EPD_CMD_SEND_DATA:
            EPD_WriteData(&p_data[1], length - 1);
            break;

        case EPD_CMD_REFRESH:
            epd_update_display_mode(p_epd, MODE_PICTURE);
            if (p_epd->epd) p_epd->epd->drv->refresh(p_epd->epd);
            break;

        case EPD_CMD_SLEEP:
            if (p_epd->epd) p_epd->epd->drv->sleep(p_epd->epd);
            break;

        case EPD_CMD_SET_TIME: {
            if (length < 5) return;

            NRF_LOG_DEBUG("time: %02x %02x %02x %02x\n", p_data[1], p_data[2], p_data[3], p_data[4]);
            if (length > 5) NRF_LOG_DEBUG("timezone: %d\n", (int8_t)p_data[5]);

            uint32_t timestamp = (p_data[1] << 24) | (p_data[2] << 16) | (p_data[3] << 8) | p_data[4];
            timestamp += (length > 5 ? (int8_t)p_data[5] : 8) * 60 * 60;  // timezone
            set_timestamp(timestamp);
            epd_update_display_mode(p_epd, length > 6 ? (display_mode_t)p_data[6] : MODE_CALENDAR);
            ble_epd_on_timer(p_epd, timestamp, true);
        } break;

        case EPD_CMD_SET_WEEK_START:
            if (length < 2) return;
            if (p_data[1] < 7 && p_data[1] != p_epd->config.week_start) {
                p_epd->config.week_start = p_data[1];
                epd_config_write(&p_epd->config);
            }
            break;

        case EPD_CMD_WRITE_IMAGE:  // MSB=0000: ram begin, LSB=1111: black
            if (length < 3) return;
            if (p_epd->epd) p_epd->epd->drv->write_ram(p_epd->epd, p_data[1], &p_data[2], length - 2);
            break;

        case EPD_CMD_SET_CONFIG:
            if (length < 2) return;
            memcpy(&p_epd->config, &p_data[1], (length - 1 > EPD_CONFIG_SIZE) ? EPD_CONFIG_SIZE : length - 1);
            epd_config_write(&p_epd->config);
            break;
#endif

        case EPD_CMD_SYS_SLEEP:
            sleep_mode_enter();
            break;

        case EPD_CMD_SYS_RESET:
#if defined(S112)
            nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_RESET);
#else
            NVIC_SystemReset();
#endif
            break;

        default:
            break;
    }
}

/**@brief Function for handling the @ref BLE_GATTS_EVT_WRITE event from the S110 SoftDevice.
 *
 * @param[in] p_epd     EPD Service structure.
 * @param[in] p_ble_evt Pointer to the event received from BLE stack.
 */
static void on_write(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    ble_gatts_evt_write_t* p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;

    if ((p_evt_write->handle == p_epd->char_handles.cccd_handle) && (p_evt_write->len == 2)) {
        if (ble_srv_is_notification_enabled(p_evt_write->data)) {
            NRF_LOG_DEBUG("notification enabled\n");
            p_epd->is_notification_enabled = true;
            static uint16_t length = sizeof(epd_config_t);
            NRF_LOG_DEBUG("send epd config\n");
            uint32_t err_code = ble_epd_string_send(p_epd, (uint8_t*)&p_epd->config, length);
            if (err_code != NRF_ERROR_INVALID_STATE) APP_ERROR_CHECK(err_code);
        } else {
            p_epd->is_notification_enabled = false;
        }
    } else if (p_evt_write->handle == p_epd->char_handles.value_handle) {
        epd_service_on_write(p_epd, p_evt_write->data, p_evt_write->len);
    } else {
        // Do Nothing. This event is not relevant for this service.
    }
}

#if defined(S112)
void ble_epd_evt_handler(ble_evt_t const* p_ble_evt, void* p_context) {
    if (p_context == NULL || p_ble_evt == NULL) return;

    ble_epd_t* p_epd = (ble_epd_t*)p_context;
    ble_epd_on_ble_evt(p_epd, (ble_evt_t*)p_ble_evt);
}
#endif

void ble_epd_on_ble_evt(ble_epd_t* p_epd, ble_evt_t* p_ble_evt) {
    if ((p_epd == NULL) || (p_ble_evt == NULL)) {
        return;
    }

    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED:
            on_connect(p_epd, p_ble_evt);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            on_disconnect(p_epd, p_ble_evt);
            break;

        case BLE_GATTS_EVT_WRITE:
            on_write(p_epd, p_ble_evt);
            break;

        default:
            // No implementation needed.
            break;
    }
}

static uint32_t epd_service_init(ble_epd_t* p_epd) {
    ble_uuid_t ble_uuid = {0};
    ble_uuid128_t base_uuid = BLE_UUID_EPD_SVC_BASE;
    ble_add_char_params_t add_char_params;
    uint8_t app_version = APP_VERSION;

    VERIFY_SUCCESS(sd_ble_uuid_vs_add(&base_uuid, &ble_uuid.type));

    ble_uuid.type = ble_uuid.type;
    ble_uuid.uuid = BLE_UUID_EPD_SVC;
    VERIFY_SUCCESS(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid, &p_epd->service_handle));

    memset(&add_char_params, 0, sizeof(add_char_params));
    add_char_params.uuid = BLE_UUID_EPD_CHAR;
    add_char_params.uuid_type = ble_uuid.type;
    add_char_params.max_len = BLE_EPD_MAX_DATA_LEN;
    add_char_params.init_len = sizeof(uint8_t);
    add_char_params.is_var_len = true;
    add_char_params.char_props.notify = 1;
    add_char_params.char_props.write = 1;
    add_char_params.char_props.write_wo_resp = 1;
    add_char_params.read_access = SEC_OPEN;
    add_char_params.write_access = SEC_OPEN;
    add_char_params.cccd_write_access = SEC_OPEN;

    VERIFY_SUCCESS(characteristic_add(p_epd->service_handle, &add_char_params, &p_epd->char_handles));

    memset(&add_char_params, 0, sizeof(add_char_params));
    add_char_params.uuid = BLE_UUID_APP_VER;
    add_char_params.uuid_type = ble_uuid.type;
    add_char_params.max_len = sizeof(uint8_t);
    add_char_params.init_len = sizeof(uint8_t);
    add_char_params.p_init_value = &app_version;
    add_char_params.char_props.read = 1;
    add_char_params.read_access = SEC_OPEN;

    return characteristic_add(p_epd->service_handle, &add_char_params, &p_epd->app_ver_handles);
}

void ble_epd_sleep_prepare(ble_epd_t* p_epd) {
    // Turn off led
    EPD_LED_OFF();
    // Prepare wakeup pin
    if (p_epd->config.wakeup_pin != 0xFF) {
        nrf_gpio_cfg_sense_input(p_epd->config.wakeup_pin, NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_SENSE_HIGH);
    }
}

uint32_t ble_epd_init(ble_epd_t* p_epd) {
    if (p_epd == NULL) return NRF_ERROR_NULL;

    // Initialize the service structure.
    p_epd->max_data_len = BLE_EPD_MAX_DATA_LEN;
    p_epd->conn_handle = BLE_CONN_HANDLE_INVALID;
    p_epd->is_notification_enabled = false;

    // Dedicated nRF52811 + 7.5-inch UC8179 BWR hardware configuration.
    const epd_config_t fixed_config = {
        .mosi_pin = 0x14,
        .sclk_pin = 0x13,
        .cs_pin = 0x06,
        .dc_pin = 0x05,
        .rst_pin = 0x04,
        .busy_pin = 0x03,
        .bs_pin = 0x02,
        .model_id = UC8179_750_BWR,
        .wakeup_pin = 0xFF,
        .led_pin = 0x12,
        .en_pin = 0x07,
        .display_mode = MODE_CALENDAR,
        .week_start = 1,
    };
    p_epd->config = fixed_config;

    // load config
    EPD_GPIO_Load(&p_epd->config);

    // blink LED on start
    EPD_LED_BLINK();

    // Add the service.
    return epd_service_init(p_epd);
}

uint32_t ble_epd_string_send(ble_epd_t* p_epd, uint8_t* p_string, uint16_t length) {
    if ((p_epd->conn_handle == BLE_CONN_HANDLE_INVALID) || (!p_epd->is_notification_enabled))
        return NRF_ERROR_INVALID_STATE;
    if (length > p_epd->max_data_len) return NRF_ERROR_INVALID_PARAM;

    ble_gatts_hvx_params_t hvx_params;

    memset(&hvx_params, 0, sizeof(hvx_params));

    hvx_params.handle = p_epd->char_handles.value_handle;
    hvx_params.p_data = p_string;
    hvx_params.p_len = &length;
    hvx_params.type = BLE_GATT_HVX_NOTIFICATION;

    return sd_ble_gatts_hvx(p_epd->conn_handle, &hvx_params);
}

void ble_epd_on_timer(ble_epd_t* p_epd, uint32_t timestamp, bool force_update) {
    // Update calendar on 00:00:00, clock on every minute
    if (force_update || (p_epd->config.display_mode == MODE_CALENDAR && timestamp % 86400 == 0) ||
        (p_epd->config.display_mode == MODE_CLOCK && timestamp % 60 == 0)) {
        epd_gui_update_event_t event = {p_epd, timestamp};
        app_sched_event_put(&event, sizeof(epd_gui_update_event_t), epd_gui_update);
    }
}
