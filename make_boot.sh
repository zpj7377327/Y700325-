#!/usr/bin/env bash
# 用法: ./make_boot.sh <新内核Image> [基础boot镜像] [输出boot镜像]
# 作用: 把新内核换进 boot 镜像 (magiskboot unpack -> 替换 kernel -> repack)
# 基础镜像默认查找顺序:
#   1) $BOOT_BASE 环境变量
#   2) 仓库外同级 kernel_build/boot_pack_clean/boot_clean.img (本机开发环境)
# 基础镜像为二进制大文件, 不入库; 可从 Gitee Release 下载或自行从原厂 boot.img 制作
set -e

ORIG_PWD="$PWD"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KB="${KERNEL_BUILD_DIR:-$(dirname "$SCRIPT_DIR")}"

if [ -z "$1" ] || [ ! -f "$1" ]; then
    echo "用法: $0 <新内核Image> [基础boot镜像] [输出boot镜像]"
    echo "例:   $0 out/arch/arm64/boot/Image"
    exit 1
fi

IMG="$(readlink -f "$1")"
BASE="$(readlink -f "${2:-${BOOT_BASE:-$KB/boot_pack_clean/boot_clean.img}}")"
OUT="${3:-$IMG.boot.img}"
case "$OUT" in /*) ;; *) OUT="$ORIG_PWD/$OUT" ;; esac

[ -f "$BASE" ] || { echo "基础 boot 镜像不存在: $BASE"; echo "可设置 BOOT_BASE=/path/to/boot.img 指定"; exit 1; }
command -v magiskboot >/dev/null || { echo "缺少 magiskboot, 请先安装"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "[1/3] 解包基础镜像: $BASE"
magiskboot unpack "$BASE" >/dev/null

echo "[2/3] 替换内核: $(basename "$IMG")"
cp -f "$IMG" kernel

echo "[3/3] 重打包 -> $OUT"
magiskboot repack "$BASE" "$OUT" >/dev/null

echo
echo "完成: $OUT"
ls -la "$OUT" | awk '{print "  大小:", $5, "字节"}'
echo "刷入: fastboot flash boot \"$OUT\""
