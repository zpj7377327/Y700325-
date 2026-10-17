# OPPO OPD2101 (SM8250) 内核

OPPO OPD2101 平板 (骁龙 870 / kona) 自编译内核, 基于 ColorOS 16 内核源码, 含性能调优与内存扩展支持。**纯净版, 不含 KernelSU。**

## 一键编译

```bash
./build_kernel.sh --all        # 全流程: 检查工具链 -> 编译 -> 打包 boot.img
./build_kernel.sh -b           # 已有工具链, 直接编译
./build_kernel.sh -b -i        # 增量编译 (保留 out/, 只重编改动文件, 日常开发推荐)
./build_kernel.sh -p           # 只打包 (编译产物 out/arch/arm64/boot/Image -> boot.img)
```

产物: `out/arch/arm64/boot/Image` 与仓库根目录的 `oppo_kona_<时间戳>.img` (可直接 fastboot 刷写)。

## 打包说明 (make_boot.sh)

打包采用 **magiskboot 换内核** 方案: 解包基础 boot 镜像 → 替换 kernel → 重打包, 不使用 AnyKernel3。

基础镜像 `boot_clean.img` (含 permissive cmdline 的原厂 boot) 为二进制大文件, **不放入 git**, 托管在 [Gitee Release v1.0](https://gitee.com/univille/kernel_sm8250_oppo_opd2101/releases) 附件 (`boot_clean.img.gz`, 约 34MB)。

查找顺序 (任选其一):
1. `BOOT_BASE=/path/to/boot.img` 环境变量
2. 仓库同级目录 `../boot_pack_clean/boot_clean.img` (开发者本机)
3. `cache/boot_clean.img` — **不存在时会自动从 Release 下载** (需联网, 一次性)

## 环境要求

- Linux x86_64, `clang` + `ld.lld` (脚本 `--get` 可自动拉取工具链)
- `magiskboot` (PATH 中可用): `apt install magiskboot` 或从 [Magisk](https://github.com/topjohnwu/Magisk/releases) 单独取二进制
- `curl` (仅自动下载基础镜像时需要)

## 刷写

```bash
adb reboot bootloader          # 或 fastbootd 流程, 视机型而定
fastboot flash boot oppo_kona_xxx.img
fastboot reboot
```

> OPD2101 需解锁 BL。刷机风险自担, 请保留可回退的原厂 boot 备份。

## 主要改动

- `arch/arm64/configs/oppo.config`: fq_codel/fq 队列 (BBR + 抗 bufferbloat)、关闭 DEBUG_LIST
- 移除内嵌 KernelSU (纯净内核); 打包链路不依赖 AnyKernel3
- 配合 ColorOS 内存扩展 (nandswap) 的设备侧脚本另行分发
