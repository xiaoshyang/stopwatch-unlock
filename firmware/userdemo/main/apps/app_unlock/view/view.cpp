/*
 * "Unlock Mac" 应用界面：显示解锁状态（USB / BLE / 密码）+ 双触发按钮
 */
#include "view.h"

#include <assets/assets.h>

namespace view {

using namespace uitk::lvgl_cpp;

namespace {
constexpr uint32_t _bg      = 0x000000;
constexpr uint32_t _accent  = 0x4AD78C;
constexpr uint32_t _warn    = 0xE8B449;
constexpr uint32_t _blue    = 0x5B8DEF;
constexpr uint32_t _text    = 0xFFFFFF;
constexpr uint32_t _dim     = 0x9A9A9A;
}  // namespace

void UnlockView::init(lv_obj_t* parent)
{
    _panel = std::make_unique<Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(466, 466);
    _panel->setBgColor(lv_color_hex(_bg));
    _panel->setBgOpa(LV_OPA_COVER);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _title = std::make_unique<Label>(_panel->get());
    _title->setText("Unlock Mac");
    _title->setTextFont(&MontserratSemiBold26);
    _title->setTextColor(lv_color_hex(_text));
    _title->align(LV_ALIGN_CENTER, 0, -165);

    _usb_label = std::make_unique<Label>(_panel->get());
    _usb_label->setTextFont(&lv_font_montserrat_20);
    _usb_label->setTextColor(lv_color_hex(_dim));
    _usb_label->align(LV_ALIGN_CENTER, 0, -120);

    _ble_label = std::make_unique<Label>(_panel->get());
    _ble_label->setTextFont(&lv_font_montserrat_20);
    _ble_label->setTextColor(lv_color_hex(_dim));
    _ble_label->align(LV_ALIGN_CENTER, 0, -95);

    _pwd_label = std::make_unique<Label>(_panel->get());
    _pwd_label->setTextFont(&lv_font_montserrat_20);
    _pwd_label->setTextColor(lv_color_hex(_dim));
    _pwd_label->align(LV_ALIGN_CENTER, 0, -70);

    _trigger_button = std::make_unique<Button>(_panel->get());
    _trigger_button->setSize(150, 150);
    _trigger_button->align(LV_ALIGN_CENTER, -80, 40);
    _trigger_button->setRadius(LV_RADIUS_CIRCLE);
    _trigger_button->setBorderWidth(0);
    _trigger_button->setShadowWidth(0);
    _trigger_button->setBgColor(lv_color_hex(_accent));
    _trigger_button->label().setText("USB");
    _trigger_button->label().setTextFont(&MontserratSemiBold26);
    _trigger_button->label().setTextColor(lv_color_hex(0x0F5831));
    _trigger_button->label().align(LV_ALIGN_CENTER, 0, 0);
    _trigger_button->onClick().connect([this]() { _usb_trigger_requested = true; });

    _ble_button = std::make_unique<Button>(_panel->get());
    _ble_button->setSize(150, 150);
    _ble_button->align(LV_ALIGN_CENTER, 80, 40);
    _ble_button->setRadius(LV_RADIUS_CIRCLE);
    _ble_button->setBorderWidth(0);
    _ble_button->setShadowWidth(0);
    _ble_button->setBgColor(lv_color_hex(_blue));
    _ble_button->label().setText("BLE");
    _ble_button->label().setTextFont(&MontserratSemiBold26);
    _ble_button->label().setTextColor(lv_color_hex(0xFFFFFF));
    _ble_button->label().align(LV_ALIGN_CENTER, 0, 0);
    _ble_button->onClick().connect([this]() { _ble_trigger_requested = true; });

    _diag_label = std::make_unique<Label>(_panel->get());
    _diag_label->setTextFont(&lv_font_montserrat_14);
    _diag_label->setTextColor(lv_color_hex(0xFF9E9E));
    _diag_label->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _diag_label->setWidth(400);
    _diag_label->align(LV_ALIGN_CENTER, 0, 185);
    _diag_label->setText("diag: -");

    _hint = std::make_unique<Label>(_panel->get());
    _hint->setText("USB: plug cable.  BLE: pair M5StopWatchUnlock in Mac Bluetooth.");
    _hint->setTextFont(&lv_font_montserrat_14);
    _hint->setTextColor(lv_color_hex(_dim));
    _hint->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _hint->setWidth(360);
    _hint->align(LV_ALIGN_CENTER, 0, 150);
}

void UnlockView::updateStatus(bool hasPassword, bool usbConnected, bool bleConnected)
{
    _usb_label->setText(usbConnected ? "USB : connected" : "USB : not connected");
    _usb_label->setTextColor(lv_color_hex(usbConnected ? _accent : _dim));

    _ble_label->setText(bleConnected ? "BLE : connected" : "BLE : not paired/connected");
    _ble_label->setTextColor(lv_color_hex(bleConnected ? _accent : _dim));

    _pwd_label->setText(hasPassword ? "Password : stored" : "Password : MISSING");
    _pwd_label->setTextColor(lv_color_hex(hasPassword ? _accent : _warn));

    /* 按钮按通道可用性置灰/隐藏 */
    _trigger_button->setHidden(!usbConnected);
    _ble_button->setHidden(!bleConnected);
}

static const char* diag_str(int r)
{
    if (r == 0) return "ok";
    if (r == -1) return "n/a";
    static char buf[16];
    snprintf(buf, sizeof(buf), "err 0x%x", r);
    return buf;
}

void UnlockView::setDiag(int usbResult, int bleResult, int bleStep, int advResult, int enterResult)
{
    static char buf[64];
    if (usbResult == 0 && bleResult == 0) {
        snprintf(buf, sizeof(buf), "diag: USB ok, BLE ok, adv %s, E %s", diag_str(advResult), enterResult == 0 ? "ok" : (enterResult == 1 ? "fail" : "n/a"));
        _diag_label->setText(buf);
    } else {
        snprintf(buf, sizeof(buf), "diag: USB %s | BLE %s (step %d) E %s",
                 diag_str(usbResult), diag_str(bleResult), bleStep,
                 enterResult == 0 ? "ok" : (enterResult == 1 ? "fail" : "n/a"));
        _diag_label->setText(buf);
    }
}

void UnlockView::update()
{
    /* 无逐帧动画，保留官方 view 接口 */
}

void UnlockView::setTriggering(bool triggering)
{
    /* 触发期间两个按钮都藏起来，避免连点；触发结束后 updateStatus 会按通道可用性复位 */
    _trigger_button->setHidden(triggering);
    _ble_button->setHidden(triggering);
}

}  // namespace view
