# 联想拯救者 Y700 (TB-9707F / Pike) 内核

本项目是为联想拯救者 Y700 平板**第一代**（TB-9707F，骁龙 870 / kona / SM8250）定制移植的 Linux 内核工程（基于 Linux 4.19.325），当前分支 v6p 是经过实机验证的健康基线。

> **注意：ColorOS 16 整机移植已于 2026-11-08 暂缓**（16的适配计划先暂缓）。当前工作聚焦在 13.1 系统上的振动 + 扬声器修复，基线为 v6p。

---

## 硬件（以本机 stock DTB / 实测为准）

- **设备型号**：Lenovo Legion Y700 1st gen (TB-9707F / Pike)
- **SoC**：Qualcomm Snapdragon 870 (kona / SM8250)
- **内核基座**：Linux Kernel 4.19.325（OPPO OPD2101 树移植，见 README_LENOVO_325.md）
- **触控芯片**：**Novatek NT36523，SPI 总线**（novatek@0，qupv3_se14_spi，CS 0，15 MHz，IRQ GPIO 39；面板 qcom,mdss_dsi_nt36523_panel_vid）
  - **不是 Goodix GT738x**（本机 stock 内核只有 SPI nvt_ts 驱动，无 Goodix）
  - **不是 NT36536**（那是 Y700 四代的芯片）
  - **不是 I2C**（dtbo 里另有 novatek@62 I2C 节点，那是其他变体的，不要用）
- **扬声器功放**：Cirrus Logic **CS35L45 ×2**，I2C **bus 1**（1-0030 spk1 / 1-0031 spk2），MI2S 直连 SoC LPAIF（WCD9385 不在扬声器通路）
- **振动马达**：AWINIC **AW8697 ×2**（I2C 6-005a 右 / 7-005b 左，共用复位 GPIO 61）
- **WiFi / 蓝牙**：Qualcomm QCA6391
- **快充**：Qualcomm SMB1395 ×2（双电荷泵）
- **电池**：双电芯 2P，6550 mAh
- **Root 方案**：**APatch**（用户自行用 APatch 给 clean stock boot 打补丁；不是 KernelSU，也不是 Magisk）

---

## 本分支（v6p）的修改点

v6p 提交（157f95eb2）是当前振动手感最好的实机验证基线，包含以下修改：

| 项 | 修改 | 状态 |
|---|---|---|
| 触摸 | 从小米 Pad 5 Pro（elish）树移植 NT36523 SPI 驱动；固件名改为 novatek_ts_fw.bin / novatek_ts_mp.bin；pen-charging no-op stub | 已验证，触控正常 |
| 振动 | AW8697 双马达，CONT 模式，f0=1754，index=1（stock 默认值） | **实机最佳**（“P好很多”“这份振动是最纯的”） |
| 电量计 | FG gen4 NOM_CAP ×2（双电芯修正，fc=7176000） | 已验证，电量不再掉一半 |
| 音频 | kona machine driver（techpack/audio/asoc/kona.c），PRI_MI2S_RX 双 CS35L45 DAI link；CS35L45 固件（.wmfw / .bin）用 1 代原版 | 已验证声卡注册；扬声器仍无声，排查中 |
| 屏幕帧率 | v5z-dd 方向感知 FPS 发送（升频在 kickoff/panel 先发，降频在 DB load/host 先发） | **已验证**，60↔120 双向切换不闪屏，触控正常 |
| 充电 | 双 SMB1395 电荷泵根因已定位（从机 ONLINE / SCOPE 查询返回 -EINVAL，双泵快充不启用） | 待测试（v6h） |
| 待机 | IRQ 412（SLPI smp2p）待机唤醒循环已定位 | 待验证（60 秒灭屏复测未做） |

> **重要：v6p 的振动手感无法通过重新编译复现。** v6v / v6w 用相同 config + 相同驱动重编后手感仍变弱。**只有原始 v6p Image（sha256 d2a1ee4c…67ca8，70,948,872 B）/ boot（c7614af1…9407c5）是可靠的振动基线。** 后续改动请以该二进制为准，不要只信 config 一致。

---

## 源码树主要结构

| 目录 / 文件 | 说明 |
|---|---|
| arch/arm64/configs/vendor/j716f_defconfig | Y700 移植特性核心 defconfig |
| drivers/input/touchscreen/nt36xxx/ | NT36523 SPI 触摸驱动（elish 移植） |
| drivers/input/misc/aw8697.c | AW8697 双马达振动驱动 |
| sound/soc/codecs/cs35l45.c | CS35L45 扬声器功放驱动 |
| techpack/audio/asoc/kona.c | kona machine driver（PRI_MI2S_RX） |
| drivers/power/supply/qcom/fg_gen4.c | 电量计 NOM_CAP×2 修正 |
| drivers/usb/misc/pi2dpx1217.c | Type-C 高速信号重驱动 |
| drivers/input/hall_sensor.c | 霍尔传感器 |

---

## 编译指南

在 InstaBox（gcc 13.3）上编译：

```bash
export ARCH=arm64
make O=out -j4 ARCH=arm64 KCFLAGS='-Wno-error' CC=aarch64-linux-gnu-gcc-13 CROSS_COMPILE=aarch64-linux-gnu- Image
```

产物位于 out/arch/arm64/boot/Image。

> **不要从 defconfig 重新生成 config**（会丢 PCIE + CONFIG_EXTRA_FIRMWARE 等触摸/WiFi 固件选项）。

---

## 打包与刷写说明

> **只用 fastboot boot 临时启动验证，永远不要直接 flash。**

打包：用 Y700 clean stock boot.img 的 header、dtb、ramdisk、dtbo 原字节，只替换内核 Image：

```bash
python3 repack-los-boot.py boot_zui15_stock.img <Image> <out-boot.img>
fastboot boot <out-boot.img>
```

---

## 已知问题（v6p 分支）

- **扬声器仍无声**（声卡注册正常，AFE 正常消费数据，RX1 slot 正确；排查方向：CS35L45 GLOBAL_ENABLES 0x2014 bit0 / DAPM / DSP Protection 固件 / MI2S 时钟）。
- **系统振动仍不触发**（单马达 sysfs 能振，HAL 层不触发；stock 是双马达同时驱动）。
- **待机耗电未修复**（IRQ 412 唤醒循环，60 秒灭屏复测未做）。
- **v6q 偶发灭屏后电源键不亮屏**（adb 仍连着，系统活着；input keyevent 26 可唤醒）。
