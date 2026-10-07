/*
 * unlock_module.c — macOS 锁屏一键解锁（ESP-IDF + esp_tinyusb 版）
 * 移植自 oppen-screen v2.0.0（Arduino 版），时序与密码存储完全一致。
 */
#include "unlock_module.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tusb.h"
#include "tinyusb.h"

/* ---- BLE HID 通道（esp_hid + Bluedroid-BLE，官方 esp_hid_device 同款路径）---- */
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_common.h"
#include "esp_gap_ble_api.h"
#include "esp_hid_gap.h"  /* 官方示例 GAP 辅助（本地副本） */

#define TAG "unlock"

/* 触发按键：B1 = GPIO2（该固件里 GPIO1 是 B2 返回/PTT，避免冲突） */
#define UNLOCK_KEY_GPIO   GPIO_NUM_1
#define UNLOCK_OTHER_GPIO GPIO_NUM_2
#define UNLOCK_HOLD_MS    700
#define UNLOCK_LOCKOUT_MS 2500
#define UNLOCK_DEFAULT_DELAY_MS 800
#define UNLOCK_CHAR_DELAY_MS    8
#define UNLOCK_BACKSPACES 24
#define UNLOCK_POLL_MS    20

/* ASCII → USB HID 键码表（us 布局；bit6=需要 Shift），与 Arduino asciimap 一致 */
static const uint8_t s_ascii_map[128] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x2B, 0x28, 0x00, 0x00, 0x28, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x5E, 0x74, 0x60, 0x61, 0x62, 0x64, 0x34,
    0x66, 0x67, 0x65, 0x6E, 0x36, 0x2D, 0x37, 0x38,
    0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x73, 0x33, 0x76, 0x2E, 0x77, 0x78,
    0x5F, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
    0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
    0x5B, 0x5C, 0x5D, 0x2F, 0x31, 0x30, 0x63, 0x6D,
    0x35, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,
    0x1B, 0x1C, 0x1D, 0x6F, 0x71, 0x70, 0x75, 0x00
};

/* ---- USB：完整配置描述符（HID 类启用时必须提供，否则 INVALID_ARG）---- */
/* ---- BLE HID：标准 8 字节键盘报告（Report ID 1，与官方示例一致）---- */
#define UNLOCK_BLE_NAME "M5StopWatchUnlock"
static char s_password[64];
/* 屏幕诊断用：0=成功(ESP_OK)，其他=esp_err 错误码，-1=未执行 */
int g_usb_init_result = -1;
int g_ble_init_result = -1;
int g_ble_init_step   = 0;   /* 1=gap 2=adv 3=gatts 4=dev_init，失败停在那一步 */
int g_ble_adv_result  = -1;  /* 广播启动结果 */
int g_enter_result    = -1;  /* 最后一次回车发送结果：0=ok 1=fail */
static uint32_t s_press_delay_ms = UNLOCK_DEFAULT_DELAY_MS;
static bool s_key_was_down = false;
static bool s_fired_this_hold = false;
static uint32_t s_key_down_at = 0;
static uint32_t s_last_trigger = 0;
static const uint8_t s_kbd_report_map[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
    0x95,0x01,0x75,0x08,0x81,0x03,
    0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,
    0x95,0x01,0x75,0x03,0x91,0x03,
    0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,
    0xC0
};
static esp_hid_raw_report_map_t s_ble_maps[] = {
    { .data = s_kbd_report_map, .len = sizeof(s_kbd_report_map) },
};
static esp_hidd_dev_t *s_ble_hid = NULL;
static bool s_ble_started = false;
static bool s_ble_connected = false;

/* 原始广播数据（30 字节 ≤ 31）：flags + 键盘外观 + HID 服务 0x1812 + 完整名字 */
static uint8_t s_adv_raw[] = {
    0x02, 0x01, 0x06,
    0x03, 0x19, 0xC1, 0x03,
    0x03, 0x03, 0x12, 0x18,
    0x12, 0x09,
    'M', '5', 'S', 't', 'o', 'p', 'W', 'a', 't', 'c', 'h', 'U', 'n', 'l', 'o', 'c', 'k',
};

static bool ble_hid_connected(void) {
    return s_ble_started && s_ble_connected && s_ble_hid != NULL && esp_hidd_dev_connected(s_ble_hid);
}

static void ble_send_key(uint8_t modifier, uint8_t keycode) {
    uint8_t buf[8] = {0};
    buf[0] = modifier;
    buf[2] = keycode;
    esp_hidd_dev_input_set(s_ble_hid, 0, 1, buf, 8);
    vTaskDelay(pdMS_TO_TICKS(50));      /* BLE 包间隔：保守 50ms */
    memset(buf, 0, sizeof(buf));
    esp_hidd_dev_input_set(s_ble_hid, 0, 1, buf, 8);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void ble_send_ascii(uint8_t v) {
    ble_send_key((v & 0x40) ? 0x02 : 0, v & 0x3F);
}

static bool type_sequence_ble(void) {
    if (!ble_hid_connected()) return false;
    /* 1) Shift 轻点：与 USB 路径一致，唤醒但不输入字符 */
    ble_send_key(0x02, HID_KEY_SHIFT_LEFT);
    vTaskDelay(pdMS_TO_TICKS(s_press_delay_ms));
    /* 2) 退格清残留：与 USB 路径一致 */
    for (int i = 0; i < UNLOCK_BACKSPACES; i++) {
        ble_send_key(0, HID_KEY_BACKSPACE);
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    /* 3) 逐字输入密码 */
    for (const char *q = s_password; *q; q++) {
        uint8_t v = s_ascii_map[(uint8_t)*q];
        if (v) {
            ble_send_ascii(v);
            vTaskDelay(pdMS_TO_TICKS(UNLOCK_CHAR_DELAY_MS));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    /* 4) 回车（唯一提交） */
    ble_send_key(0, HID_KEY_ENTER);
    return true;
}

/* gap.c（官方副本）extern 的两个演示符号，这里给空实现 */
void ble_hid_task_start_up(void) {}
void ble_hid_task_shut_down(void) {}

static void ble_hidd_event_cb(void *args, esp_event_base_t base, int32_t id, void *edata) {
    (void)args; (void)base; (void)edata;
    esp_hidd_event_t ev = (esp_hidd_event_t)id;
    switch (ev) {
    case ESP_HIDD_START_EVENT:
        s_ble_started = true;
        g_ble_adv_result = (int)esp_hid_ble_gap_adv_start();
        ESP_LOGI(TAG, "BLE HID stack started, adv result=0x%x", g_ble_adv_result);
        break;
    case ESP_HIDD_CONNECT_EVENT:
        s_ble_connected = true;
        ESP_LOGI(TAG, "BLE HID connected (Mac paired)");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_ble_connected = false;
        g_ble_adv_result = (int)esp_hid_ble_gap_adv_start();
        ESP_LOGI(TAG, "BLE HID disconnected, re-advertise 0x%x", g_ble_adv_result);
        break;
    default:
        break;
    }
}

static void load_config(void) {
    nvs_handle_t h;
    if (nvs_open("unlock", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_password);
        if (nvs_get_str(h, "pwd", s_password, &len) != ESP_OK) {
            s_password[0] = '\0';
        }
        uint32_t d = 0;
        if (nvs_get_u32(h, "delay", &d) == ESP_OK && d >= 100 && d <= 5000) {
            s_press_delay_ms = d;
        }
        nvs_close(h);
    }
}

/* 键盘专用 HID 报告描述符（tinyusb 标准宏，无 Report ID） */
static const uint8_t s_hid_report_desc[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

/* USB：完整配置描述符（HID 类启用时必须提供，否则 INVALID_ARG） */
#define USB_TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, USB_TUSB_DESC_TOTAL_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 0, false, sizeof(s_hid_report_desc), 0x81, 16, 1),
};

/* tinyusb HID 类要求的三个回调（仅键盘输入，get/set 给空实现） */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

static bool hid_wait_ready(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (!tud_hid_ready()) {
        if (waited >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }
    return true;
}

static bool hid_press_key(uint8_t modifier, uint8_t keycode) {
    uint8_t report[6] = {0};
    if (!hid_wait_ready(100)) return false;
    report[0] = keycode;
    tud_hid_keyboard_report(0, modifier, report);
    return true;
}

static void hid_release_key(void) {
    uint8_t report[6] = {0};
    if (!hid_wait_ready(100)) return;
    tud_hid_keyboard_report(0, 0, report);
}

/* 敲一个键：按下 → 保持 6ms → 释放（V2 已验证时序） */
static void hid_send_key(uint8_t modifier, uint8_t keycode) {
    if (!hid_press_key(modifier, keycode)) return;
    vTaskDelay(pdMS_TO_TICKS(6));
    hid_release_key();
}

static bool type_sequence(void) {
    if (!tud_connected()) return false;
    /* 1) Shift 轻点：唤醒屏幕，不产生字符、不提交 */
    hid_send_key(KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_SHIFT_LEFT);
    vTaskDelay(pdMS_TO_TICKS(s_press_delay_ms));
    /* 2) 退格清残留：不提交 */
    for (int i = 0; i < UNLOCK_BACKSPACES; i++) {
        hid_send_key(0, HID_KEY_BACKSPACE);
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    /* 3) 逐字输入密码（全部输完才碰回车） */
    for (const char *p = s_password; *p; p++) {
        uint8_t v = s_ascii_map[(uint8_t)*p];
        if (v == 0x00) continue;
        uint8_t mod = (v & 0x40) ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
        hid_send_key(mod, v & 0x3F);
        vTaskDelay(pdMS_TO_TICKS(UNLOCK_CHAR_DELAY_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(60));
    /* 4) 全部输入完成 → 回车：唯一一次提交（V2 已验证时序） */
    g_enter_result = 1;
    if (hid_press_key(0, HID_KEY_ENTER)) {
        vTaskDelay(pdMS_TO_TICKS(6));
        hid_release_key();
        g_enter_result = 0;
    }
    ESP_LOGI(TAG, "enter send result: %d", g_enter_result);
    return true;
}

static void unlock_task(void *arg) {
    s_last_trigger = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    for (;;) {
        if (s_password[0] != '\0') {
            bool down = gpio_get_level(UNLOCK_KEY_GPIO) == 0;
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (down && !s_key_was_down) {
                s_key_down_at = now;
                s_fired_this_hold = false;
            } else if (down && s_key_was_down) {
                if (!s_fired_this_hold &&
                    now - s_key_down_at >= UNLOCK_HOLD_MS &&
                    now - s_last_trigger > UNLOCK_LOCKOUT_MS &&
                    gpio_get_level(UNLOCK_OTHER_GPIO) != 0 && /* 另一键未按住 */
                    (tud_connected() || ble_hid_connected())) { /* USB 优先，拔线自动走 BLE */
                    s_fired_this_hold = true;
                    if (tud_connected()) {
                        ESP_LOGI(TAG, "trigger (USB): typing password");
                        type_sequence();
                    } else {
                        ESP_LOGI(TAG, "trigger (BLE): typing password");
                        type_sequence_ble();
                    }
                    s_last_trigger = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                }
            }
            s_key_was_down = down;
        }
        vTaskDelay(pdMS_TO_TICKS(UNLOCK_POLL_MS));
    }
}

static void start_ble_channel(void) {
    esp_hid_device_config_t cfg = {
        .vendor_id        = 0x16C0,
        .product_id       = 0x0100,
        .version          = 0x0100,
        .device_name      = UNLOCK_BLE_NAME,
        .manufacturer_name = "Espressif",
        .serial_number    = NULL,
        .report_maps      = s_ble_maps,
        .report_maps_len  = 1,
    };
    esp_err_t ret = esp_hid_gap_init(ESP_BT_MODE_BLE);
    g_ble_init_step = 1; g_ble_init_result = (int)ret;
    if (ret != ESP_OK) { ESP_LOGE(TAG, "gap init: %s", esp_err_to_name(ret)); return; }

    /* StopWatch 没有配对码输入/确认界面：必须走 Just Works，否则 Mac 会弹码但设备无法确认。 */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_device_name(UNLOCK_BLE_NAME);

    /* 原始广播数据：绕开 esp_hid 广播辅助函数（IDF 5.5 上该路径返回 INVALID_ARG） */
    ret = esp_ble_gap_config_adv_data_raw(s_adv_raw, sizeof(s_adv_raw));
    g_ble_init_step = 2; g_ble_init_result = (int)ret;
    if (ret != ESP_OK) { ESP_LOGE(TAG, "adv raw: %s", esp_err_to_name(ret)); return; }
    /* Bluedroid GATTS 事件必须挂上，否则连接不上 */
    ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    g_ble_init_step = 3; g_ble_init_result = (int)ret;
    if (ret != ESP_OK) { ESP_LOGE(TAG, "gatts cb: %s", esp_err_to_name(ret)); return; }
    ret = esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, ble_hidd_event_cb, &s_ble_hid);
    g_ble_init_step = 4; g_ble_init_result = (int)ret;
    if (ret != ESP_OK) { ESP_LOGE(TAG, "hidd init: %s", esp_err_to_name(ret)); return; }
    ESP_LOGI(TAG, "BLE HID channel ready, name=%s", UNLOCK_BLE_NAME);
}

void unlock_module_init(void) {
    load_config();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << UNLOCK_KEY_GPIO) | (1ULL << UNLOCK_OTHER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    static const char *string_desc[] = {
        "",              /* 0: 语言（由栈填充） */
        "Espressif",     /* 1: 厂商 */
        "StopWatch Unlock", /* 2: 产品 */
        "",              /* 3: 序列号（用默认） */
    };
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,          /* 用组件默认设备描述符（免 qualifier）*/
        .configuration_descriptor = s_desc_configuration,
        .string_descriptor = string_desc,
        .external_phy = false,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    g_usb_init_result = (int)err;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "tinyusb HID ready, password %d chars", (int)strlen(s_password));
    } else {
        /* 失败不致命：BLE 通道仍须启动（usb_serial_jtag/VFS 可能占着 USB 外设） */
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s — BLE 通道仍可用", esp_err_to_name(err));
    }
    if (s_password[0] == '\0') {
        ESP_LOGW(TAG, "no password in NVS (unlock namespace) — triggers stay inert");
    }

    if (xTaskCreate(unlock_task, "unlock", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create unlock task failed");
    }

    /* BLE HID 通道（无线解锁）：任何状态下都启动，与 USB 并存 */
    start_ble_channel();
}

bool unlock_module_trigger_ble(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - s_last_trigger < UNLOCK_LOCKOUT_MS) return false;
    if (s_password[0] == '\0') return false;
    if (!type_sequence_ble()) return false;
    s_last_trigger = now;
    return true;
}

bool unlock_ble_connected(void) { return ble_hid_connected(); }

bool unlock_usb_connected(void) { return tud_connected(); }

bool unlock_module_trigger(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - s_last_trigger < UNLOCK_LOCKOUT_MS) return false;
    if (s_password[0] == '\0' || !tud_connected()) return false;
    ESP_LOGI(TAG, "manual trigger: typing password");
    type_sequence();
    s_last_trigger = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return true;
}

bool unlock_has_password(void) { return s_password[0] != '\0'; }

bool unlock_set_password(const char *pwd) {
    if (pwd == NULL || pwd[0] == '\0' || strlen(pwd) >= sizeof(s_password)) return false;
    nvs_handle_t h;
    if (nvs_open("unlock", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "pwd", pwd) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) strlcpy(s_password, pwd, sizeof(s_password));
    return ok;
}

bool unlock_clear_password(void) {
    nvs_handle_t h;
    if (nvs_open("unlock", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_erase_key(h, "pwd");
    nvs_commit(h);
    nvs_close(h);
    s_password[0] = '\0';
    return true;
}

int unlock_usb_init_result(void) { return g_usb_init_result; }
int unlock_ble_init_result(void) { return g_ble_init_result; }
int unlock_ble_init_step(void)   { return g_ble_init_step; }
int unlock_ble_adv_result(void) { return g_ble_adv_result; }
int unlock_enter_result(void) { return g_enter_result; }
