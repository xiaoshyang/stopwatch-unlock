/*
 * "Unlock" app — 锁屏一键解锁（官方 UserDemo 风格）
 * 两个大按钮：USB 通道 / BLE 通道，分别触发对应 HID 键盘序列；
 * 后台 A 键长按手势保留（USB 连接时）。
 *
 * ⚠️ GUI 锁 xGuiSemaphore 是非递归互斥量：同一调用链内禁止重入，
 *    所有锁都在本文件内部取/放，绝不嵌套。
 */
#include "app_unlock.h"

#include <hal/hal.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include "unlock_module.h"

using namespace mooncake;

AppUnlock::AppUnlock()
{
    setAppInfo().name = "Unlock";
    setAppInfo().icon = (void*)&icon_setup;
}

void AppUnlock::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppUnlock::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    {
        LvglLockGuard lock;
        _view = std::make_unique<view::UnlockView>();
        _view->init(lv_screen_active());
        _view->updateStatus(unlock_has_password(), unlock_usb_connected(), unlock_ble_connected());
        _view->setDiag(unlock_usb_init_result(), unlock_ble_init_result(), unlock_ble_init_step(), unlock_ble_adv_result(), unlock_enter_result());
    }
}

void AppUnlock::onRunning()
{
    GetHAL().updateButtonStates();

    input::KeyEvent event = input::KeyEvent::None;
    if (_key_manager) {
        event = _key_manager->update();
    }

    if (event == input::KeyEvent::GoHome) {
        close();
        return;
    }

    if (_view) {
        if (_view->isUsbTriggerRequested()) {
            _view->clearTrigger();
            {
                LvglLockGuard lock;
                _view->setTriggering(true);
            }
            /* 阻塞输入（~1.5s），期间不持有 LVGL 锁 */
            unlock_module_trigger();
            {
                LvglLockGuard lock;
                _view->setTriggering(false);
                _view->updateStatus(unlock_has_password(), unlock_usb_connected(), unlock_ble_connected());
            }
        }
        if (_view->isBleTriggerRequested()) {
            _view->clearTrigger();
            {
                LvglLockGuard lock;
                _view->setTriggering(true);
            }
            /* 阻塞输入（~10s），期间不持有 LVGL 锁 */
            unlock_module_trigger_ble();
            {
                LvglLockGuard lock;
                _view->setTriggering(false);
                _view->updateStatus(unlock_has_password(), unlock_usb_connected(), unlock_ble_connected());
            }
        }
    }

    if (_view) {
        {
            LvglLockGuard lock;
            _view->update();
        }
        /* 周期刷新通道状态 */
        if (GetHAL().millis() - _last_status_tick >= 500) {
            _last_status_tick = GetHAL().millis();
            LvglLockGuard lock;
            _view->updateStatus(unlock_has_password(), unlock_usb_connected(), unlock_ble_connected());
            _view->setDiag(unlock_usb_init_result(), unlock_ble_init_result(), unlock_ble_init_step(), unlock_ble_adv_result(), unlock_enter_result());
        }
    }
}

void AppUnlock::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();

    LvglLockGuard lock;
    _view.reset();
}
