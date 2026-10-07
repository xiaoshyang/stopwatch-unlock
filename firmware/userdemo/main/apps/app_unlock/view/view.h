#pragma once
#include <memory>
#include <smooth_lvgl.hpp>
#include <uitk/short_namespace.hpp>

namespace view {

class UnlockView {
public:
    void init(lv_obj_t* parent);
    void update();  /* 每帧调用（官方 view 惯例），当前无动画，保留接口 */
    /* usb/ble: 对应通道是否连上 */
    void updateStatus(bool hasPassword, bool usbConnected, bool bleConnected);
    /* 屏幕诊断：显示两个通道的初始化结果（0=ok，其他=错误码，-1=未执行） */
    void setDiag(int usbResult, int bleResult, int bleStep, int advResult, int enterResult);
    void setTriggering(bool triggering);
    bool isUsbTriggerRequested() const { return _usb_trigger_requested; }
    bool isBleTriggerRequested() const { return _ble_trigger_requested; }
    void clearTrigger()
    {
        _usb_trigger_requested = false;
        _ble_trigger_requested = false;
    }

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _title;
    std::unique_ptr<uitk::lvgl_cpp::Label> _usb_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _ble_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _pwd_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _diag_label;
    std::unique_ptr<uitk::lvgl_cpp::Button> _trigger_button;  /* USB 通道 */
    std::unique_ptr<uitk::lvgl_cpp::Button> _ble_button;      /* BLE 通道 */
    std::unique_ptr<uitk::lvgl_cpp::Label> _hint;
    bool _usb_trigger_requested = false;
    bool _ble_trigger_requested = false;
};

}  // namespace view
