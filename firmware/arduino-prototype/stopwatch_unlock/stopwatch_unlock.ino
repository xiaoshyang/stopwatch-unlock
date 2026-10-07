/*
 * M5Stack StopWatch (C152, ESP32-S3R8) → macOS 锁屏一键解锁
 *
 * 原理：设备以 USB HID 键盘身份接入 Mac。按下黄色按键 KEYA(GPIO1) 后：
 *   1. 轻点一次 Shift —— 唤醒屏幕/让系统就绪，但不产生字符、不提交任何内容
 *   2. 等待 pressDelayMs（默认 800ms，可调）
 *   3. 连发退格清掉密码框可能残留的字符（空框无副作用），仍然不提交
 *   4. 逐字输入密码
 *   5. 回车（唯一一次提交），完成解锁
 *
 * 密码配置（两种方式任选）：
 *   A. 修改下方 DEFAULT_PASSWORD 后重新编译烧录
 *   B. 烧录后打开串口监视器，发送：pwd:你的密码   （存入 NVS，掉电保存）
 *      其他串口命令：delay:900（调整清场等待 ms）/ show（查看配置）/ test（立刻触发一次）/ pwd:clear（恢复默认）
 *
 * 硬件：M5Stack StopWatch，按键 KEYA=GPIO1（低电平按下），KEYB=GPIO2（本版预留）
 *
 * ⚠️ 安全提醒：任何拿到设备的人按一下按键即可解锁你的 Mac。
 *    按键版请仅在家用等可信环境使用；后续接指纹模块后才具备真正的身份验证能力。
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

// ======================= 默认配置 =======================
// 占位符：真实密码不放源码（避免入库）。烧录后通过串口发送 pwd:你的密码 写入设备 NVS
static const char*    DEFAULT_PASSWORD     = "REPLACE_ME";
static const uint32_t DEFAULT_PRESS_DELAY  = 800;  // 唤醒键后等待时间(ms)，解锁失败可调大到 1200
static const uint8_t  KEY_HOLD_MS          = 6;    // V2 实测可用：按键按下保持时间(ms)
static const uint8_t  CHAR_DELAY_MS        = 8;    // 每个字符之间的间隔(ms)
static const uint32_t TRIGGER_LOCKOUT_MS   = 2000; // 触发后忽略重复触发的时长(ms)
static const uint32_t KEYB_DOWNLOAD_HOLD_MS = 5000; // KEYB 长按此时长 → 重启进入下载模式

#define PIN_KEYA 1   // 黄色按键
#define PIN_KEYB 2   // 蓝色按键（长按 = 进下载模式）
// ========================================================

USBHIDKeyboard Keyboard;
Preferences    nvs;

String   password;
uint32_t pressDelayMs  = DEFAULT_PRESS_DELAY;
uint32_t lastTrigger   = 0;
bool     keyaWasDown   = true;  // 上电时若按键已被按住，要求先松开再触发，避免误发

static void logLine(const char* msg) {
  if (Serial) Serial.println(msg);
}

// 软件重启进入 ROM 下载模式：置位 FORCE_DOWNLOAD_BOOT 后复位，
// ROM bootloader 检查该 RTC 寄存器后进入下载模式（与 USB-JTAG 重置到下载同一机制）
static void enterDownloadMode() {
  logLine("[BOOT] KEYB long-press -> rebooting into download mode");
  delay(100);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

static void loadConfig() {
  nvs.begin("unlock", true);
  password     = nvs.getString("pwd", String(DEFAULT_PASSWORD));
  pressDelayMs = nvs.getULong("delay", DEFAULT_PRESS_DELAY);
  nvs.end();
}

static void tapKey(uint8_t key) {
  Keyboard.press(key);
  delay(KEY_HOLD_MS);
  Keyboard.release(key);
  delay(CHAR_DELAY_MS);
}

static void typePasswordNow() {
  // 1) 唤醒键：轻点 Shift，唤醒屏幕/让系统就绪——不产生字符，不提交任何内容
  tapKey(KEY_LEFT_SHIFT);
  delay(pressDelayMs);
  // 2) 清残留：连发退格删除框内可能的残留字符（空框无副作用），不提交
  for (uint8_t i = 0; i < 24; i++) {
    tapKey(KEY_BACKSPACE);
  }
  delay(60);
  // 3) 输入密码（仅支持 ASCII 密码）
  for (size_t i = 0; i < password.length(); i++) {
    Keyboard.write(password[i]);
    delay(CHAR_DELAY_MS);
  }
  delay(60);
  // 4) 回车登录（唯一一次提交）
  tapKey(KEY_RETURN);
  lastTrigger = millis();
}

static void handleSerial() {
  if (!Serial || !Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line.startsWith("pwd:")) {
    String p = line.substring(4);
    nvs.begin("unlock");
    if (p == "clear") {
      nvs.remove("pwd");
      logLine("[OK] password cleared -> using DEFAULT_PASSWORD");
    } else {
      nvs.putString("pwd", p);
      logLine("[OK] password saved to NVS");
    }
    nvs.end();
    loadConfig();
  } else if (line.startsWith("delay:")) {
    long d = line.substring(6).toInt();
    if (d >= 100 && d <= 5000) {
      nvs.begin("unlock");
      nvs.putULong("delay", (uint32_t)d);
      nvs.end();
      loadConfig();
      logLine("[OK] press delay updated");
    } else {
      logLine("[ERR] delay out of range (100-5000)");
    }
  } else if (line == "show") {
    if (Serial) {
      Serial.printf("[CFG] password: %d chars, pressDelay: %lu ms\n",
                    password.length(), (unsigned long)pressDelayMs);
      Serial.printf("[CFG] source: %s\n",
                    nvs.begin("unlock", true) && nvs.isKey("pwd") ? "NVS" : "DEFAULT");
      nvs.end();
    }
  } else if (line == "test") {
    typePasswordNow();
  } else if (line == "pwd:clear") {
    // 已在 pwd: 分支处理，防误配
  } else {
    logLine("[HINT] commands: pwd:xxx | delay:800 | show | test | pwd:clear");
  }
}

void setup() {
  // HID 键盘先于 USB 枚举注册，Mac 眼里就是一把键盘
  Keyboard.begin();
  USB.begin();

  pinMode(PIN_KEYA, INPUT_PULLUP);
  pinMode(PIN_KEYB, INPUT_PULLUP);

  loadConfig();
  lastTrigger = millis(); // 上电 2s 内不响应，避免烧录瞬间触发

  if (Serial) {
    Serial.begin(115200);
    Serial.println("\n=== StopWatch Unlock (USB HID) ===");
    Serial.printf("[CFG] password: %d chars\n", password.length());
    if (password == String(DEFAULT_PASSWORD) && password == "REPLACE_ME") {
      Serial.println("[WARN] DEFAULT_PASSWORD 未修改！串口发送 pwd:你的密码 或修改源码");
    }
  }
}

void loop() {
  handleSerial();

  bool keyaDown = (digitalRead(PIN_KEYA) == LOW);

  if (keyaDown && !keyaWasDown) {            // 下降沿 = 新的一次按压
    if (millis() - lastTrigger > TRIGGER_LOCKOUT_MS) {
      logLine("[TRIGGER] typing password...");
      typePasswordNow();
      logLine("[TRIGGER] done");
    }
  }
  keyaWasDown = keyaDown;

  // KEYB 长按 5s → 重启进入下载模式（免手势重刷的逃生通道）
  static uint32_t keybDownStart = 0;
  static bool     keybWasDown   = true;
  bool keybDown = (digitalRead(PIN_KEYB) == LOW);
  if (keybDown && !keybWasDown) {
    keybDownStart = millis();
  } else if (keybDown && keybWasDown &&
             millis() - keybDownStart > KEYB_DOWNLOAD_HOLD_MS) {
    enterDownloadMode();
  }
  keybWasDown = keybDown;

  delay(10);
}
