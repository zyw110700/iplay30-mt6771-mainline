# iplay30-mt6771-mainline

Teclast iPlay 30（联发科 MT6771 / Helio P60，Wi-Fi 与蓝牙由 MT6631 提供）的主线 Linux 内核。
仓库是完整的 Linux 7.2 源码树：第一个提交为上游 v7.2 原样导入，第二个提交是这台平板的移植改动。

## 基线

- 上游：`linux.git`（清华镜像 `mirrors.tuna.tsinghua.edu.cn/git/linux.git`）
- 版本：`v7.2`（`8d3ae5928`）

## 进度

整机已经可以日常启动和使用：

| 项目 | 状态 |
|---|---|
| CPU | 8 核 SMP（4×A73 + 4×A53），调频正常 |
| GPU | Mali-G72 MP3，Panfrost，OpenGL 3.1 / Vulkan 1.3 |
| 显示 | 1200×1920 MIPI-DSI，横屏 1920×1200 |
| 触摸 | Goodix GT9886，支持压感笔 |
| 存储 | eMMC HS400 |
| USB | device（CDC-ACM / CDC-NCM）与 USB2 host（USB-C 拓展坞） |
| Wi-Fi | MT6631，2.4/5 GHz |
| 蓝牙 | BR/EDR + BLE |
| 电源 | MT6358 + MT6370，充电、电量计、背光、6 路温度 |
| 音频 | 未实现 |
| 摄像头 / microSD / 指纹 / 传感器 | 未实现 |

## 已知限制

- 没有声音，摄像头、microSD、指纹、环境光/距离传感器尚未接入。
- SPM 深睡不可用，idle 只到 WFI，无 RTC。
- USB host 仅 USB2，角色需要手动切换。
- PanVK 只到 Vulkan 1.3，DXVK/vkd3d 不可用。

## 相关仓库

- `iplay30-mt6771-init`：stage-1 init 源码
- `iplay30-mt6771-dts`：实际启动用设备树

## 许可

内核改动与新增驱动按 **GPL-2.0** 发布。
`vendor-connectivity`、`drivers/net/wireless/mediatek/mt6631` 等厂商代码
来自联发科/OPPO 的 GPL 内核发布，版权归原作者。
