# StopWatch 一键解锁固件 — 烧录与使用说明

固件：`stopwatch_unlock/stopwatch_unlock.ino`（纯 ESP32 官方库，无需第三方库）
已在 2026-10-06 首次烧录成功并通过验证（设备枚举为 HID 键盘 + CDC 串口）。

## 〇、当前状态速查

| 事项 | 值 |
|---|---|
| 硬件 | M5Stack StopWatch（C152，ESP32-S3R8，16MB Flash，8MB OPI PSRAM） |
| 串口（固件运行时） | `/dev/cu.usbmodem28848543A1CC2`（后缀 28848543A1CC = 芯片 MAC，稳定不变） |
| 串口（下载模式/出厂固件） | `/dev/cu.usbmodem21101` 这类短名字（USB JTAG/serial debug unit） |
| ⚠️ 另一块板 | 机器上还插着一块 ESP32-C3，占用 `/dev/cu.usbmodem21201`，别弄混 |
| 编译/烧录 | PlatformIO（pioarduino espressif32 55.3.311，arduino-esp32 3.3.11）+ `brew install esptool` |
| 密码来源 | 设备 NVS（真实密码不入库；源码里只有占位符 `REPLACE_ME`）。首次烧录后需串口发 `pwd:你的密码` 配置 |

## 一、编译与烧录（PlatformIO，已验证）

```bash
# 编译
pio run -d ~/Documents/claude-code-project/oppen-screen/firmware/stopwatch_unlock -e stopwatch

# 烧录（固件运行中时需先进入下载模式，见下）
pio run -d ~/Documents/claude-code-project/oppen-screen/firmware/stopwatch_unlock -e stopwatch -t upload
```

关键配置都在 `platformio.ini`：`ARDUINO_USB_MODE=0`（TinyUSB，HID 键盘必需）、`ARDUINO_USB_CDC_ON_BOOT=1`（串口日志）、16MB Flash、`qio_opi` PSRAM、`src_dir = .`。

### 进入下载模式（重新烧录时）

**首选：长按蓝色 KEYB 5 秒**（2026-10-06 版固件起内置）——置位 `RTC_CNTL_OPTION1` 的 FORCE_DOWNLOAD_BOOT 位后自动重启进下载模式，端口变回 `cu.usbmodem211XX`。

**官方手势（实测自定义固件下依然有效，应为硬件/PMIC 级实现）**：插着 USB 长按电源键 2~5 秒直到绿灯亮。⚠️ 注意：走这个手势进下载模式并烧录后，**软复位无法退出下载模式**（RTS 复位被忽略），必须物理开关机（长按电源 6~10 秒关机 → 短按开机）才会加载新固件。

硬件兜底（对任何固件都有效）：断开 USB → 短接背面扩展排针 **BOOT(G0)** 与 **GND** → 插回 USB → 出现 `cu.usbmodem211XX` 后断开短接线。

```bash
pio run -d <项目目录> -e stopwatch -t upload --upload-port /dev/cu.usbmodem211XX
```

## 二、串口命令（配置密码/参数）

```bash
pio device monitor -p /dev/cu.usbmodem28848543A1CC2
# 或免装工具：打开两个终端，一个 cat /dev/cu.usbmodem28848543A1CC2，另一个 echo 命令 > 同路径
```

| 命令 | 作用 |
|---|---|
| `pwd:你的密码` | 密码写入 NVS 掉电保存（⚠️ 首次烧录后必须执行一次，否则源码占位符 `REPLACE_ME` 会原样打出），仅支持 ASCII |
| `pwd:clear` | 清除 NVS 密码，回到源码占位符 |
| `delay:900` | 调整清场回车后的等待（100–5000ms），解锁失败时调大 |
| `show` | 查看当前配置（密码只显示长度） |
| `test` | 立即触发一次输入（⚠️ 会打进当前聚焦的窗口） |

## 三、使用

1. USB-C 数据线接 Mac（线必须支持数据）
2. 锁屏：合盖再开盖，或 `Ctrl+Cmd+Q`
3. 按 **黄色按键 KEYA** → 约 1 秒内自动解锁
4. 固件动作序列：轻点 Shift（唤醒屏幕，不产生字符、不提交）→ 等 800ms → 退格清残留（不提交）→ 输入密码 → 回车（唯一一次提交）
5. 参数调整：串口发 `delay:1200`（睡眠唤醒后解锁失败时调大等待）

⚠️ 解锁流程在任何时候按 KEYA 都会执行——桌面未锁定时按了，密码会打进当前焦点窗口，注意别在敏感界面误按。

## 四、故障排查

| 现象 | 处理 |
|---|---|
| 解锁失败、密码框有残留 | 串口发 `delay:900` 调大等待 |
| 睡眠唤醒后第一次没反应 | USB 重新枚举需一点时间，再按一次 |
| 识别不到设备/找不到串口 | 换数据线；确认看的是 28848543A1CC 那个口，不是 C3 板的 21201 |
| 进不了下载模式 | 重插 USB 后再长按电源键试；仍不行用 M5Burner 刷回出厂固件恢复 |
| 密码含非 ASCII 字符 | 不支持，HID 键盘只能打 ASCII |

## 五、安全边界（务必知悉）

1. **密码明文存在设备 flash 里**，用 esptool 可被读出——拿到设备 ≈ 拿到密码。
2. 真实密码已从源码移除（入库的只有占位符），改密码用串口 `pwd:` 命令，不要写回源码提交。
3. 按键触发版 = 任何碰到设备的人都能解锁 Mac，仅适合家用等可信环境。
4. 终极形态：接 AS608 指纹模块（UART），指纹匹配成功才输出密码——见需求文档 M3 里程碑。

## 六、后续路线

- KEYB（GPIO2，蓝色按键）已预留
- StopWatch 板载 1.75" 圆形 AMOLED（466×466）与振动马达：可做屏显状态/振动反馈
- Grove 口（G10/G11）可接 UART 指纹模块（AS608/R307）
