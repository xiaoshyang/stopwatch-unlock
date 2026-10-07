/*
 * M5Stack StopWatch → macOS 蓝牙（BLE）一键解锁 —— 无线版
 *
 * 与 USB 版（stopwatch_unlock）同构，但按键输入走 BLE HID 键盘通道：
 *   - Mac 蓝牙设置里配对一次（设备名 M5StopWatchUnlock），之后自动重连
 *   - 不插数据线也能解锁：戴着或放在桌上都行
 *   - 闲置 5 分钟自动断开 BLE 省电；触发时若未连接先尝试唤醒连接
 *
 * 时序（与 v2.0.0 封板版一致）：Shift 轻点唤醒(不提交) → 等待 → 退格清残留(不提交)
 * → 逐字输入密码 → 回车（唯一一次提交）
 *
 * 密码与 USB 版共享同一份 NVS 配置（命名空间 "unlock"），刷机自动继承已存密码
 *
 * 按键：KEYA(GPIO1) = 触发解锁；KEYB(GPIO2) 长按 5s = 重启进下载模式
 * 串口命令（插线时）：pwd:xxx / show / test / hold:N(分钟) / pwd:clear
 */

#include <M5Unified.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include <Preferences.h>

// ======================= 配置 =======================
static const char*    DEFAULT_BLE_NAME   = "M5StopWatchUnlock";
static const uint32_t DEFAULT_PRESS_DELAY  = 800;   // 唤醒后等待(ms)
static const uint8_t  BLE_CHAR_DELAY_MS        = 15; // BLE 包间隔
static const uint32_t DEFAULT_IDLE_HOLD_MS     = 5*60*1000; // 闲置断连
static const uint8_t  KEYA_PIN = 1;  // 黄色按键（低电平按下）
static const uint8_t  KEYB_PIN = 2;  // 蓝色按键（长按 5s 进下载模式）
// ====================================================

USBHIDKeyboard UsbKeyboard;
BLEServer* bleServer = nullptr;
BLEHIDDevice* hidDevice = nullptr;
BLECharacteristic* hidInput = nullptr;

Preferences nvs;
String password;
uint32_t pressDelayMs = DEFAULT_PRESS_DELAY;
uint32_t idleHoldMs   = DEFAULT_IDLE_HOLD_MS;

uint32_t lastTrigger = 0;
uint32_t lastActivity = 0;
bool keyaWasDown = true;
bool keybWasDown = true;
uint32_t keybDownAt = 0;

/* 标准 HID 键盘报告描述符（report id = 1） */
static const uint8_t s_hid_report_map[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,   /* Usage Page Keyboard, Usage Keyboard, Collection, Report ID 1 */
  0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02, /* modifiers */
  0x75,0x08,0x95,0x01,0x81,0x01,             /* reserved */
  0x05,0x07,0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x75,0x08,0x95,0x06,0x81,0x00, /* 6 keys */
  0xC0
};

/* ASCII → HID 键码 + 修饰键（US 布局），与 V2 固件同一张表 */
static const uint16_t SHIFT_MASK = 0x0100;
static const uint16_t s_ascii_map[128] = {
  0x00,0,0,0,0,0,0,0, 0x2A, 0x2B, 0x28, 0x28, 0,0,0,
  0,0,0,0,0,0,0,0, 0,0,0,0, 0x2C, 0x1E|SHIFT_MASK,0x34|SHIFT_MASK,0x20|SHIFT_MASK,
  0x21|SHIFT_MASK,0x22|SHIFT_MASK,0x24|SHIFT_MASK,0x34, 0x26|SHIFT_MASK,0x27|SHIFT_MASK,0x25|SHIFT_MASK,0x2E|SHIFT_MASK,
  0x36, 0x2D, 0x37, 0x38, 0x27, 0x1E, 0x1F, 0x20,
  0x21, 0x22, 0x23, 0x33|SHIFT_MASK, 0x33, 0x2E, 0x37|SHIFT_MASK, 0x38|SHIFT_MASK,
  0x1F|SHIFT_MASK, 0x04|SHIFT_MASK, 0x05|SHIFT_MASK, 0x06|SHIFT_MASK, 0x07|SHIFT_MASK, 0x08|SHIFT_MASK, 0x09|SHIFT_MASK, 0x0A|SHIFT_MASK,
  0x0B|SHIFT_MASK, 0x0C|SHIFT_MASK, 0x0D|SHIFT_MASK, 0x0E|SHIFT_MASK, 0x0F|SHIFT_MASK, 0x10|SHIFT_MASK, 0x11|SHIFT_MASK, 0x12|SHIFT_MASK,
  0x13|SHIFT_MASK, 0x14|SHIFT_MASK, 0x15|SHIFT_MASK, 0x16|SHIFT_MASK, 0x17|SHIFT_MASK, 0x18|SHIFT_MASK, 0x19|SHIFT_MASK, 0x1D|SHIFT_MASK,
  0x2F, 0x31, 0x30, 0x23|SHIFT_MASK, 0x2D|SHIFT_MASK, 0x35, 0x24|SHIFT_MASK, 0,
  0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B, 0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,
  0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B, 0x1C,0x2F|SHIFT_MASK,0x31|SHIFT_MASK,0x30|SHIFT_MASK,0x35|SHIFT_MASK, 0,0,0,
  0x2A /* DEL */
};

static void logLine(const char* msg) { if (Serial) Serial.println(msg); }

static bool bleConnected() {
  return bleServer && bleServer->getConnectedCount() > 0;
}

static void sendBleKey(uint16_t vk) {
  uint8_t mods = (vk & SHIFT_MASK) ? 0x02 : 0;  /* bit1 = Left Shift */
  uint8_t key  = vk & 0xFF;
  uint8_t report[8] = {0};
  report[0] = mods;
  report[2] = key;
  hidInput->setValue(report, 8);
  hidInput->notify();
  delay(BLE_CHAR_DELAY_MS);
  report[0] = 0;
  report[2] = 0;
  hidInput->setValue(report, 8);
  hidInput->notify();
}

static void tapUsbKey(uint8_t key) {
  UsbKeyboard.press(key);
  delay(6);
  UsbKeyboard.release(key);
  delay(8);
}

static void typeUsbSequence() {
  tapUsbKey(KEY_LEFT_SHIFT);
  delay(pressDelayMs);
  for (uint8_t i = 0; i < 24; i++) tapUsbKey(KEY_BACKSPACE);
  delay(60);
  for (size_t i = 0; i < password.length(); i++) {
    UsbKeyboard.write(password[i]);
    delay(8);
  }
  delay(60);
  tapUsbKey(KEY_RETURN);
  lastTrigger = millis();
  lastActivity = millis();
}

static void typeBleSequence() {
  if (!bleConnected()) {
    /* 尝试重连：断开后重新广播让 Mac 拉回 */
    logLine("[BLE] not connected, re-advertising...");
    if (bleServer) bleServer->startAdvertising();
    for (int i = 0; i < 10 && !bleConnected(); i++) delay(500);
  }
  if (!bleConnected()) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setCursor(233, 250);
    M5.Display.print("BLE 未连接，无法解锁");
    M5.Display.display();
    delay(1500);
    return;
  }
  /* 1) Shift 轻点：唤醒，不产生字符、不提交 */
  {
    uint8_t report[8] = {0};
    report[0] = 0x02;
    hidInput->setValue(report, 8); hidInput->notify();
    delay(6);
    report[0] = 0;
    hidInput->setValue(report, 8); hidInput->notify();
    delay(pressDelayMs);
  }
  /* 2) 退格清残留（不提交） */
  for (uint8_t i = 0; i < 24; i++) { sendBleKey(0x2E); }
  delay(80);
  /* 3) 输入密码 */
  for (size_t i = 0; i < password.length(); i++) {
    uint8_t c = password[i];
    if (c >= 0x20 && c < 0x7F) {
      uint16_t vk = s_ascii_map[c];
      if (vk) sendBleKey(vk);
    }
  }
  delay(80);
  /* 4) 回车：唯一一次提交 */
  sendBleKey(0x28);
  lastTrigger = millis();
  lastActivity = millis();
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.setCursor(233, 250);
  M5.Display.print("已输入密码+回车");
  M5.Display.display();
  delay(2000);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setCursor(233, 245);
  M5.Display.print("M5 USB+BLE UNLOCK");
  M5.Display.display();
}

static void typeSequence() {
  if (bleConnected()) typeBleSequence();
  else typeUsbSequence();
}

static void loadConfig() {
  nvs.begin("unlock", true);
  password = nvs.getString("pwd", String(""));
  pressDelayMs = nvs.getULong("delay", DEFAULT_PRESS_DELAY);
  idleHoldMs = nvs.getULong("held", DEFAULT_IDLE_HOLD_MS);
  nvs.end();
}

static void enterDownloadMode() {
  logLine("[BOOT] KEYB 5s -> download mode");
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

static void handleSerial() {
  if (!Serial || !Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.startsWith("pwd:")) {
    String p = line.substring(4);
    nvs.begin("unlock");
    if (p == "clear") { nvs.remove("pwd"); logLine("[OK] pwd cleared"); }
    else { nvs.putString("pwd", p); logLine("[OK] pwd saved"); }
    nvs.end();
    loadConfig();
  } else if (line.startsWith("hold:")) {
    long min = line.substring(5).toInt();
    if (min >= 1 && min <= 120) {
      idleHoldMs = min * 60000;
      nvs.begin("unlock"); nvs.putULong("held", idleHoldMs); nvs.end();
      logLine("[OK] idle hold set");
    }
  } else if (line == "show") {
    String info = "[CFG] pwd " + String(password.length()) + " chars, delay " + String(pressDelayMs)
                 + "ms, hold " + String(idleHoldMs / 60000) + "min, BLE conn=" + (bleConnected() ? "yes" : "no");
    logLine(info.c_str());
  } else if (line == "test") {
    typeSequence();
  } else {
    logLine("[HINT] pwd:xxx | hold:N | show | test | pwd:clear");
  }
}

void setup() {
  M5.begin();
  M5.Display.setRotation(0);
  UsbKeyboard.begin();
  USB.begin();
  pinMode(KEYA_PIN, INPUT_PULLUP);
  pinMode(KEYB_PIN, INPUT_PULLUP);
  loadConfig();

  /* ---- BLE HID 键盘 ---- */
  BLEDevice::init(DEFAULT_BLE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->advertiseOnDisconnect(true);  /* 断开自动重新广播，Mac 自动回连 */
  hidDevice = new BLEHIDDevice(bleServer);
  hidDevice->reportMap(const_cast<uint8_t*>(s_hid_report_map), sizeof(s_hid_report_map));
  hidDevice->hidInfo(0x33, 0x00);
  hidDevice->manufacturer();
  hidDevice->manufacturer("M5Stack");
  hidInput = hidDevice->inputReport(1);
  hidDevice->startServices();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hidDevice->hidService()->getUUID());
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setCursor(233, 230); M5.Display.print("M5 USB+BLE UNLOCK");
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setCursor(233, 250); M5.Display.print("Mac 蓝牙里配对一次：");
  M5.Display.print(DEFAULT_BLE_NAME);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(233, 270); M5.Display.print("按黄色 A 键 = 解锁 Mac");
  M5.Display.display();

  if (password.length() == 0) {
    logLine("[WARN] 无密码！插线后串口发送 pwd:你的密码（与 USB 版共享 NVS）");
  }

  if (Serial) {
    Serial.begin(115200);
    delay(300);
    logLine("\n=== StopWatch BLE Unlock ===");
  }
  lastActivity = millis();
}

void loop() {
  M5.update();
  handleSerial();

  /* KEYA：触发 */
  bool keyaDown = digitalRead(KEYA_PIN) == LOW;
  if (keyaDown && !keyaWasDown) {
    if (millis() - lastTrigger > 2000) {
      lastActivity = millis();
      typeSequence();
    }
  }
  keyaWasDown = keyaDown;

  /* KEYB：长按 5s 进下载模式（逃生通道） */
  bool keybDown = digitalRead(KEYB_PIN) == LOW;
  if (keybDown && !keybWasDown) keybDownAt = millis();
  else if (!keybDown && keybWasDown) keybDownAt = 0;
  keybWasDown = keybDown;
  if (keybDown && keybDownAt && millis() - keybDownAt > 5000) enterDownloadMode();

  /* 闲置自动断连省电（保留可再连状态：重新广播） */
  if (bleConnected() && millis() - lastActivity > idleHoldMs) {
    if (bleServer) {
      auto peers = bleServer->getPeerDevices(false);
      for (auto &kv : peers) {
        logLine("[BLE] idle -> disconnect, re-advertising");
        bleServer->disconnect(kv.first);
      }
    }
    lastActivity = millis();
  }
  if (millis() - lastActivity > 60000) lastActivity = millis(); /* 防溢出 */
  delay(20);
}
