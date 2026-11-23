# Y700 1st gen (TB-9707F) — 4.19.325 触控修复代码

基线：Gitee `univille/kernel_sm8250_lenovo_y700`，4.19.325，OPPO OPD2101 底子。
本分支是为 Y700 一代自己的 stock 固件/DT 做的触控修复，不是通用树。

## 这台机器的触控事实（来自用户自己的 stock  dump，不是猜的）

- 触控只走 SPI：`novatek@0`，`compatible = "novatek,NVT-ts-spi"`，
  总线 `qupv3_se14_spi`，CS 0，15 MHz，IRQ GPIO 39，**没有复位 GPIO**
- `novatek,swrst-n8-addr = 0x03F0FE`，`novatek,spi-rd-fast-addr = 0x03F310`
- 面板 `qcom,mdss_dsi_nt36523_panel_vid`，面板名
  `nt36523 lcd vid mode dsi panel`
- stock 固件名：`novatek_ts_fw.bin` / `novatek_ts_mp.bin`
- stock DT 里没有 `novatek,config-array-size`，也没有 I2C `novatek@62`
  那个变体（同 dtbo 里给别的机型准备的，不要用）

## 改了什么

### 1. 通用 nt36xxx 驱动的 DRM 门（v1，`drivers/input/touchscreen/nt36xxx/`）

原驱动 SPI probe 里 `nvt_ts_check_dt()` 要找 DRM panel，
Y700 是 MDSS 面板，永远找不到，于是 probe 一直 `-EPROBE_DEFER`：
屏亮、触摸设备都不出现。改成 `CONFIG_DRM_PANEL` 守卫、
不 defer，直接进 late probe，让芯片 trim 校验决定。

### 2. elish nt36523 驱动完整移植（v2，`drivers/input/touchscreen/nt36523/`）

关键：同样 trim ID `{*,*,*,0x23,0x65,0x03}`，elish 驱动映射到
NT36523 memmap（EVENT_BUF `0x2FE00`），而通用驱动映射到 NT36525
（`0x11A00`），所以通用驱动读固件信息永远是错的，`fw_ver` 一直 0。
移植时做的 Y700 适配：

- `CONFIG_TOUCHSCREEN_NT36XXX` 关，`CONFIG_TOUCHSCREEN_NT36523_HOSTDL_SPI` 开
- stock DT 没有 config-array 时不再报错退出，fallback 到
  `novatek_ts_fw.bin` / `novatek_ts_mp.bin`（见 `nvt_parse_dt`）
- elish 60 秒 lockdown 等待改成单次检查 + stub（返回 `-ENODEV`）
- `touch_irq_boost`、`lpm_disable`、pen-charge、双击唤醒改成空 stub
- Xiaomi touchfeature（`drivers/input/touchscreen/xiaomi/`）一起带上，
  并修了 Y700 没有 `xiaomi-touch` 节点时的空指针
- `CTP_SPI_READ/WRITE` 改名 `NVT36523_SPI_*`，避开 OPPO 触控符号冲突
- `NVT_TOUCH_SUPPORT_HW_RST=0`，不假设复位脚，复位走软件复位 + 面板 RESX

### 3. MSM DRM 屏灭通知（v3，`nt36523/nt36xxx.c`）

v2 注册的是小米 mi_drm 通知，但这棵 OPPO 显示驱动根本不发 mi_drm，
它发 MSM DRM 通知，而且亮屏时只对面板名正好是
`nt36523 lcd vid mode dsi panel` 的屏发 `EVENT UNBLANK`
（`techpack/display/msm/dsi/dsi_display.c` 里注释写着
“add for NT36523 resume touch”）。所以 v2 灭屏一次就丢触摸，
因为没人调 resume、没人重新下载固件。

v3 改成注册 `msm_drm_register_client`：
`EARLY POWERDOWN` / `EVENT POWERDOWN` → suspend，
`EVENT UNBLANK` → resume_work（重新下载固件）。

坑：不要直接 include `include/linux/msm_drm_notify.h`，
主配置没开 `CONFIG_DRM_MSM` 时它会把注册函数 inline 成空操作，
哪怕显示驱动里真的有这个导出函数。v3 是在驱动里自己
`extern` 声明的。

另外加了 `/proc/nvt_tp_control`：`echo resume > /proc/nvt_tp_control`
手动触发 resume，`echo suspend` 手动挂起，两个函数都是幂等的。

### 4. USB 充电模式写禁用（v4，诊断版，`nt36523/nt36xxx.c`）

v3 之后：正常触摸和灭屏恢复都好了，但拔插数据线还是丢触摸，
再灭屏亮屏一次又回来。驱动里唯一“恰好在 USB 插拔时才跑”的
是 `nvt_power_supply_work` 写充电模式命令（插上 `0x53`、拔掉 `0x51`）。
v4 把这条 SPI 写改成只记日志：

```
Y700 v4: USB charger write skipped, cmd=0x%02X
```

如果 v4 下插拔不丢触摸了，就是这条写把 Y700 固件写挂了，
可以正式关掉；如果还是丢，就是 USB 插拔本身让芯片复位/掉电，
驱动写无辜。

## 其他编译修复（为 gcc 13.3 编这棵 clang-10 时代的树）

`bpf_trace` inline、`adfr`/`ion` inline、`sy6529`/`nu1619` 括号、
`qcom_pmicwd` 缺 return、`y700_hw_info.c` 里 `get_cached_platform_id` 的
KONA=1 shim、`CONFIG_OPLUS_FINGERPRINT` 开（opticalfp）、
`CONFIG_STMVL53L1` 关、fp_common 缺的头文件。这些只是为了让树
在 gcc 13.3 下链接通过，不改触控逻辑。

## 编译和打包（只 fastboot boot，不要直接刷）

```
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-
make O=out -j$(nproc) KCFLAGS="-Wno-error" Image
```

打包只换 Image，stock boot 的头、cmdline、ramdisk、DTB 原样保留，
重新算 boot ID。已验证过的 boot（v3/v4）都在另外的发布目录，
不在本仓库里——仓库里是源码，不是刷机包。

## 不适用的东西

- Y700 四代的 NT36536 编号不要往一代上套
- 别的平板的 Novatek 固件不要直接拷过来，固件是面板调校过的
- 本仓库的 DT 适配只针对 TB-9707F 这一台的 stock DT，
  别的触控批次（比如 Goodix 版）不一定适用
