#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * unlock_module — macOS 锁屏一键解锁（ESP-IDF 版，移植自 oppen-screen v2.0.0）
 *
 * 手势：USB 主机接入时长按 A 键(GPIO1) ≥700ms 触发一次（UserDemo 触摸为主，GPIO1/2 空闲）
 * 时序：Shift 轻点唤醒(不提交) → 等待 → 退格清残留(不提交) → 密码 → 回车(唯一一次提交)
 * 密码：NVS 命名空间 "unlock"/键 "pwd"（与 V2 固件相同），刷机后自动继承已存密码
 * 未配置密码时模块完全惰性
 */

#ifdef __cplusplus
extern "C" {
#endif

void unlock_module_init(void);
/* 手动触发一次解锁输入（供 UI 按钮调用；USB 已连接且已存密码时执行，带 2.5s 互锁）。
 * 阻塞约 1.5s（打字耗时）。未执行返回 false。 */
bool unlock_module_trigger(void);
/* 通过 BLE HID 通道触发（无线解锁；Mac 已配对 M5StopWatchUnlock 且已连接时执行，带互锁）。
 * 阻塞约 8~15s（BLE 包间隔保守 50ms）。未执行返回 false。 */
bool unlock_module_trigger_ble(void);
/* BLE 通道是否可用（已启动 HID 服务） */
bool unlock_ble_connected(void);
/* 屏幕诊断：0=成功，其他=esp_err 错误码，-1=未执行；ble_step=失败停在第几步 */
int unlock_usb_init_result(void);
int unlock_ble_init_result(void);
int unlock_ble_init_step(void);
int unlock_ble_adv_result(void);
int unlock_enter_result(void);
/* 当前 USB 是否连接（Mac 主机接入） */
bool unlock_usb_connected(void);
bool unlock_has_password(void);
bool unlock_set_password(const char *pwd);
bool unlock_clear_password(void);

#ifdef __cplusplus
}
#endif
