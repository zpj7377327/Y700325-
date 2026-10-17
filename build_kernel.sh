#!/usr/bin/env bash
#=========================================#
#                                         #
#       ColorOS Kernel Build Kernel       #
#                                         #
#=========================================#

set -e

# ---------- Colorful Output ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color
info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# ---------- Help----------
usage() {
    cat << EOF
Usage: $0 [Option]

Option Descriptions (Choose one of the three; options cannot be used simultaneously):
  -g, --get      Get Toolchain
  -b, --build    Skipped get toolchain and build kernel directly
  -p, --package  Pack AnyKernel3
  -a, --all      All steps have been completed
  -i, --incremental Keep out/ and only rebuild changed files (faster)
  -h, --help     Show this help message

Example:
  $0 --get
  $0 -b
  $0 --package
EOF
    exit 0
}

PARSED_ARGS=$(getopt -o gbphi --long get,build,package,all,help,incremental -n "$0" -- "$@")
if [ $? -ne 0 ]; then
    exit 1
fi

eval set -- "$PARSED_ARGS"

ACTION=""

while true; do
    case "$1" in
        -g|--get)
            ACTION="get"
            shift
            ;;
        -b|--build)
            ACTION="build"
            shift
            ;;
        -p|--package)
            ACTION="package"
            shift
            ;;
        -a|--all)
            ACTION="all"
            shift
            ;;
        -i|--incremental)
            INC=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        --)
            shift
            break
            ;;
        *)
            error "Unknown Option: $1"
            ;;
    esac
done

if [ $# -gt 0 ]; then
    error "Unrecognized raw parameter detected: $1 (This script does not accept additional positional parameters.)"
fi

if [ -z "$ACTION" ]; then
    error "Exactly one operation must be specified: -g, -b, -p or -a"
fi

do_get() {
    info "=== Execution Mode: Get Toolchain ==="
    if [ -d "build_toolchain" ]; then
        info "Detect folder exsited, Skip."
    else
        mkdir -p build_toolchain
        info "Created folder."
    fi

    missing=()
    for cmd in curl git ccache 7za pahole; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            missing+=("$cmd")
        fi
    done

    if [ ${#missing[@]} -ne 0 ]; then
        error "Error the package not installed：${missing[*]}"
        exit 1
    fi
    info "All packages installed, Skip."

    # 实际使用的工具链在 ../clang/bin (见 do_build 中 TOOLCHAIN_BIN), 存在则跳过全部下载
    if [ -f "../clang/bin/clang" ] && [ -f "../clang/bin/aarch64-linux-gnu-ar" ]; then
        info "Detect toolchain at ../clang/bin, Skip all downloads."
        return 0
    fi

    if [ -f "build_toolchain/clang_ZyC_23/bin/clang" ]; then
        info "Detect clang existed, Skip."
    else
        curl -C - --progress-bar -L "https://github.com/ZyCromerZ/Clang/releases/download/23.0.0git-20260130-release/Clang-23.0.0git-20260130.tar.gz" -o "clang.tar.gz"
        mkdir clang_ZyC_23
        tar -C clang_ZyC_23/ -zxf clang.tar.gz
        rm -f clang.tar.gz
        info "Get clang successfully."
    fi

    if [ -f "build_toolchain/gcc_32/bin/arm-linux-androideabi-ar" ]; then
        info "Detect GCC 32, Skip."
    else
        git clone -b arm32 https://github.com/JackA1ltman/Google-GCC-Android-4.9.git build_toolchain/gcc_32 --depth 1
        info "Get GCC 32 successfully."
    fi

    if [ -f "build_toolchain/gcc_64/bin/aarch64-linux-android-ar" ]; then
        info "Detect GCC 64, Skip."
    else
        git clone -b aarch64 https://github.com/JackA1ltman/Google-GCC-Android-4.9.git build_toolchain/gcc_64 --depth 1
        info "Get GCC 64 successfully."
    fi
}

do_build() {
    info "=== Execution Mode: Build Kernel ==="
    BUILD_DEVICE_NAME="oppo" # oneplus/fx2series/oppo

    if [ "$BUILD_DEVICE_NAME" == "oppo" ] || [ "$BUILD_DEVICE_NAME" == "fx2series" ]; then
        BUILD_DEVICE="oppo"
    else
        BUILD_DEVICE="oneplus"
    fi

    # 工具链在 ../clang/bin 下
    TOOLCHAIN_BIN="$PWD/../clang/bin"
    export PATH=$TOOLCHAIN_BIN:$PATH

    if [ ! -f "$TOOLCHAIN_BIN/clang" ] || [ ! -f "$TOOLCHAIN_BIN/aarch64-linux-gnu-ar" ]; then
        error "Build toolchain not found at $TOOLCHAIN_BIN"
        exit 1
    fi

    # 增量编译: INC=1 时保留 out/, 只重编受影响的文件(秒级~分钟级)
    # 默认清空重编(安全, 但耗时十几分钟)
    if [ -d "out" ]; then
        if [ "$INC" = "1" ]; then
            info "=== 增量编译模式: 保留 out/ ==="
        else
            info "=== 全量编译模式: 清空 out/ ==="
            rm -rf out
        fi
    fi

    CROSS_FLAGS="CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-"

    if [ "$BUILD_DEVICE_NAME" == "oppo" ]; then
        info "=== Selected: OPPO ==="

        make ARCH=arm64 O=out CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS vendor/kona-perf_defconfig $BUILD_DEVICE.config

        ./scripts/config --file out/.config --disable STMVL53L1

        # Unicode bypass fix
        ./scripts/config --file out/.config --enable CONFIG_UNICODE


        make ARCH=arm64 O=out CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS -j$(nproc --all)
    elif [ "$BUILD_DEVICE_NAME" == "fx2series" ]; then
        info "=== Selected: OPPO Find X2 Series ==="

        make ARCH=arm64 O=out CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS vendor/kona-perf_defconfig $BUILD_DEVICE.config

        # Unicode bypass fix
        ./scripts/config --file out/.config --enable CONFIG_UNICODE


        make ARCH=arm64 O=out CC=clang LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS -j$(nproc --all)
    else
        info "=== Selected: OnePlus ==="

        make ARCH=arm64 O=out CC=clang BRAND_SHOW_FLAG=oneplus LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS vendor/kona-perf_defconfig $BUILD_DEVICE.config

        # Unicode bypass fix
        ./scripts/config --file out/.config --enable CONFIG_UNICODE


        make ARCH=arm64 O=out CC=clang BRAND_SHOW_FLAG=oneplus LD=ld.lld CLANG_TRIPLE=aarch64-linux-gnu- $CROSS_FLAGS -j$(nproc --all)
    fi
}

do_package() {
    info "=== Execution Mode: AnyKernel3 ==="
    # 单跑 -p 时 do_build 不执行, 这里兜底设置设备名, 保证包名带前缀
    BUILD_DEVICE="${BUILD_DEVICE:-oppo}"
    if [ -f "out/arch/arm64/boot/Image" ] && [ -f "out/arch/arm64/boot/dtbo.img" ] && [ -f "out/arch/arm64/boot/dtb" ]; then
        BUILD_TIMESTAMP=$(date +%s)

        if [ -d "AnyKernel3" ]; then
            info "Detect AnyKernel3, Skip."

            if [ -f "AnyKernel3/Image" ] || [ -f "AnyKernel3/dtbo.img" ] || [ -f "AnyKernel3/dtb" ]; then
                rm -f AnyKernel3/Image
                rm -f AnyKernel3/dtbo.img
                rm -f AnyKernel3/dtb
                info "Cleared older Image files."
            fi
        else
            git clone https://github.com/osm0sis/AnyKernel3.git -b master
            cd AnyKernel3
            git checkout dca9dc370838d919d56c1f59ec78b27a14a72c68
            cd ..
        fi

        cp -fp out/arch/arm64/boot/Image AnyKernel3/Image
        cp -fp out/arch/arm64/boot/dtbo.img AnyKernel3/dtbo.img
        cp -fp out/arch/arm64/boot/dtb AnyKernel3/dtb

        sed -i 's/do.devicecheck=1/do.devicecheck=0/g' AnyKernel3/anykernel.sh
        sed -i 's!BLOCK=/dev/block/platform/omap/omap_hsmmc.0/by-name/boot;!BLOCK=auto;!g' AnyKernel3/anykernel.sh
        sed -i 's/IS_SLOT_DEVICE=0;/is_slot_device=auto;/g' AnyKernel3/anykernel.sh

        7za a -mx9 ${BUILD_DEVICE}_kona_${BUILD_TIMESTAMP}.zip AnyKernel3/*
        info "Packed AnyKernel3, filename: ${BUILD_DEVICE}_kona_${BUILD_TIMESTAMP}.zip"
    else
        error "Could not found Image, dtbo.img and dtb. Please build kernel."
        exit 1
    fi
}

case "$ACTION" in
    get)
        do_get
        ;;
    build)
        do_build
        ;;
    package)
        do_package
        ;;
    all)
        do_get
        do_build
        do_package
        ;;
    *)
        error "Internal Error: unknown operation $ACTION"
        ;;
esac

info "The operation has been completed！"
exit 0
