# 联想拯救者 Y700 (TB-9707F / J716F) 内核

本项目是为联想拯救者 Y700 平板（第一代，骁龙 870 / `kona`）定制移植的 Linux 内核工程（基于 Linux 4.19.325 LTS），主要用于驱动与适配 ColorOS 16 / 高版本 Android 系统，包含硬件驱动移植、性能调优与 KernelSU 支持。

---

## 硬件与特性支持

- **设备型号**：Lenovo Legion Y700 (TB-9707F / J716F)
- **SoC**：Qualcomm Snapdragon 870 (`kona` / `sm8250`)
- **内核基座**：Linux Kernel 4.19.325
- **触控芯片**：Goodix GT738x (`drivers/input/touchscreen/goodix/`)，支持 Y700 原生触控面板及触控手势
- **手写笔**：联想二代灵感手写笔驱动与通道切换支持
- **外设与传感器**：
  - Type-C USB Redriver (`pi2dpx1217`)
  - 霍尔传感器 (`hall_sensor`)
  - 音频 Codec（`es7210_adc`、`as33970`）
  - EROFS / F2FS 文件系统完整支持
- **Root 方案**：内置 KernelSU (0.9.5，版本号 pin 至 11071)

---

## 源码树主要结构

| 目录 / 文件 | 说明 |
|---|---|
| `arch/arm64/configs/vendor/j716f_defconfig` | Y700 官方及移植特性核心 defconfig |
| `drivers/input/touchscreen/goodix/` | Y700 GT738x 触摸驱动源码 |
| `drivers/input/touchscreen/nt36xxx/` | 备用 / 补充触摸固件支持 |
| `drivers/usb/misc/pi2dpx1217.c` | Type-C 高速信号重驱动 |
| `drivers/kernelsu/` | 内嵌 KernelSU 源码与钩子 |

---

## 编译指南

使用匹配的 Clang 工具链（如 ZyC Clang / AOSP Clang 10+）：

```bash
export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-

# 生成配置文件
make O=out vendor/j716f_defconfig

# 编译内核镜像
make O=out -j$(nproc)
```

产物位于 `out/arch/arm64/boot/Image`。

---

## 打包与刷写说明

为保证与 Y700 引导加载程序完全兼容：
1. **打包建议**：使用原厂 Y700 引导镜像的 header、dtb 与 ramdisk，将内核 Image 替换为编译产物后重新生成 `boot.img`。
2. **刷写命令**：
   ```bash
   fastboot flash boot_a boot.img
   fastboot flash boot_b boot.img
   fastboot reboot
   ```
