# 联想 Y700（TB-9707F / J716F）专用 325 内核基座

- 基座：OPPO OPD2101 (apd) 内核树（4.19.325，wcedla 派生，含 OPPO 侧大量修复与 backport 特征）
- 设备源：ISNing/android_kernel_lenovo_sm8250（android-11 分支，4.19.157，联想内核 dump）
- 本提交 = 325 基座 + 从 157 树移植的 Y700 设备专属件。**后续 backport（BPF LSM / resolve_btfids 等）由人工进行。**

## 已移植内容

| 项 | 文件 | 接线 |
|---|---|---|
| defconfig | `arch/arm64/configs/vendor/j716f_defconfig` | 无需 |
| 触摸 GT738x | `drivers/input/touchscreen/goodix/`（含 brl/、gt738x/） | touchscreen Kconfig source + Makefile obj（GOODIX_BRL / GOODIX_GT738X） |
| USB redriver | `drivers/usb/misc/pi2dpx1217.c` | usb/misc Kconfig `USB_REDRIVER_PI2DPX`（depends USB_PHY）+ Makefile |
| 霍尔传感器 | `drivers/input/hall_sensor.c` | input Kconfig `SENSORS_HALL` + Makefile |
| 音频 codec | `sound/soc/codecs/as33970/`、`dbmdx/`、`es7210_adc.c` | codecs Kconfig（dbmdx source、AS33970、ES7210）+ Makefile（es7210 objs、as33970 obj） |
| 充电 JEITA | `drivers/power/supply/qcom/lenovo-jeita.c/.h` | Kconfig `QPNP_FLOAT_CHG_RECHECK` 已加；**Makefile 未挂**（见下） |
| 校验 | `make olddefconfig` 已过，以上选项全部存活（BPF_LSM 未开，属 backport 范畴） | |

## 待人工 backport（重要）

1. **BPF LSM 依赖链**（A15 卡米修复核心，见资料库《复盘_Y700内核移植与Android15卡米修复》）：
   - `CONFIG_BPF_LSM=y` + `CONFIG_DEBUG_INFO_BTF=y`
   - `kernel/bpf/verifier.c`：`should_fail_alloc_page` 加 `CONFIG_FAIL_PAGE_ALLOC` guard（不开时 `BTF_ID_UNUSED` 留洞）
   - `tools/bpf/resolve_btfids/`：自包含 resolver（~728 行，不依赖 libelf/libbpf/pahole；4.19 kbuild 用 `hostprogs-y` + `always :=`）
   - 顶层 Makefile：prepare0 后挂 `btf_ids` 目标
   - 验证标准：`resolve_btfids: ok (251 slots)`，Image 内 `bpf_lsm_hooks cnt=213` 严格升序全非零
2. **lenovo-jeita 充电**：325 树充电栈已被 OPPO 换成 `OPLUS_FEATURE_CHG_BASIC`（无 smb5-lib.o/qpnp-smb5.o），jeita 钩子在 157 的 `smb5-lib.c` 里。需把 jeita 逻辑迁到 OPPO 充电栈或恢复上游 smb5，再挂 Makefile。
3. **DBMDX**：157 里只有 Kconfig、无 Makefile 挂钩（死配置），本树按原样移植；若 Y700 实际需要 DBMDX DSP 需补 obj 接线。
4. **其他上游差异**：`SLAB_FREELIST_HARDENED`、`STATIC_USERMODEHELPER`、`EXT4_ENCRYPTION`（4.19 中已并入 `FS_ENCRYPTION`）等在 325 树中不存在或已改名，olddefconfig 会静默丢弃，按需补。
5. **DTS 不在内核树**（157 的 `dts/vendor` 是指向 vendor 分区 devicetree-4.19 的软链接）。打包策略沿用复盘文档：**DTB/ramdisk/boot 头全部用 Y700 stock boot.img 原字节，只换内核 Image**。
6. 构建注意：用桌面树的 clang_ZyC_23（325 树按 clang10 语义写就，已降级部分告警）；`link-vmlinux.sh` 的 cleanup trap 会毁现场，调试 resolver 时用 `RESOLVE_BTFIDS=<wrapper>` 包装器存底。

## 上次实机遗留（刷机前先看复盘文档 §7）

Image 54MB 超 stock 14.7MB 是第一嫌疑 → 先 strip `.debug_*` 保留 `.BTF`（~30MB）；随后按 §7.2 做两个正交对照实验；`fastboot boot` 临时启动验证，勿直接 flash。
