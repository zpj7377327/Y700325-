#!/usr/bin/env bash
# 用法: ./make_boot.sh <新内核Image> [基础boot镜像] [输出boot镜像]
# 作用: 把新内核换进 boot 镜像 (magiskboot unpack -> 替换 kernel -> repack)
# 基础镜像查找顺序:
#   1) $BOOT_BASE 环境变量
#   2) 仓库外同级 kernel_build/boot_pack_clean/boot_clean.img (本机开发环境)
#   3) 本目录 cache/boot_clean.img, 不存在则自动从 Gitee Release 下载
# 基础镜像为二进制大文件, 不入 git; 托管在 Gitee Release v1.0 附件 (gzip 压缩)
set -e

ORIG_PWD="$PWD"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KB="${KERNEL_BUILD_DIR:-$(dirname "$SCRIPT_DIR")}"
REPO="univille/kernel_sm8250_oppo_opd2101"
BASE_URL="https://gitee.com/${REPO}/releases/download/v1.0/boot_clean.img.gz"

if [ -z "$1" ] || [ ! -f "$1" ]; then
    echo "用法: $0 <新内核Image> [基础boot镜像] [输出boot镜像]"
    echo "例:   $0 out/arch/arm64/boot/Image"
    exit 1
fi

IMG="$(readlink -f "$1")"
OUT="${3:-$IMG.boot.img}"
case "$OUT" in /*) ;; *) OUT="$ORIG_PWD/$OUT" ;; esac

# ---- 定位/获取基础镜像 ----
BASE=""
for cand in "${BOOT_BASE:-}" "$KB/boot_pack_clean/boot_clean.img" "$SCRIPT_DIR/cache/boot_clean.img"; do
    [ -n "$cand" ] && [ -f "$cand" ] && BASE="$(readlink -f "$cand")" && break
done

if [ -z "$BASE" ]; then
    echo "[0/3] 本地无基础镜像, 从 Gitee Release 下载 (约 34MB)..."
    CACHE_DIR="$SCRIPT_DIR/cache"
    mkdir -p "$CACHE_DIR"
    GZ="$CACHE_DIR/boot_clean.img.gz"
    curl -fL --retry 3 -o "$GZ" "$BASE_URL"
    echo "      解压中..."
    gunzip -kf "$GZ"
    BASE="$CACHE_DIR/boot_clean.img"
fi

[ -f "$BASE" ] || { echo "基础 boot 镜像不存在: $BASE"; echo "可设置 BOOT_BASE=/path/to/boot.img 指定"; exit 1; }
command -v magiskboot >/dev/null || { echo "缺少 magiskboot, 请先安装 (apt install magiskboot 或下载 magiskboot 二进制)"; exit 1; }

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
