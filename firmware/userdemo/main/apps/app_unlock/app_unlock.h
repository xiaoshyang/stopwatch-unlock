#pragma once
#include "view/view.h"
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <memory>

/**
 * @brief "Unlock Mac" 应用：锁屏一键解锁（USB + BLE 双通道 HID 键盘）
 */
class AppUnlock : public mooncake::AppAbility {
public:
    AppUnlock();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void refreshStatus();

    std::unique_ptr<view::UnlockView> _view;
    std::unique_ptr<input::KeyManager> _key_manager;
    uint32_t _last_status_tick = 0;
};
