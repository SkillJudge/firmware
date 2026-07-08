#!/bin/bash
set -eo pipefail

# ====================== 配置区 ======================
JQ_SRC_DIR="$PWD/jq_src"
JQ_BUILD_OUT="$PWD/jq_build_out"

# 脚本放在 firmware 根目录，当前目录就是固件顶层
FIRMWARE_ROOT="$PWD"
# 交叉工具链固定路径，和你的Makefile统一
CROSS_PREFIX="${FIRMWARE_ROOT}/output/host/bin/arm-openipc-linux-musleabi-"
# 交叉编译host标识（去掉末尾横杠）
HOST_TRIPLET="arm-openipc-linux-musleabi"
# ===================================================================

echo "[CHECK] 固件根目录: $FIRMWARE_ROOT"
echo "[CHECK] 交叉编译器前缀: $CROSS_PREFIX"
echo "[CHECK] 交叉编译HOST: $HOST_TRIPLET"

# 校验交叉编译器是否存在
if [ ! -f "${CROSS_PREFIX}gcc" ]; then
    echo "错误：未找到交叉编译器 ${CROSS_PREFIX}gcc"
    echo "解决：进入firmware目录执行 ./build.sh 完整编译一次固件生成工具链"
    exit 1
fi

# 导出编译工具
export CC="${CROSS_PREFIX}gcc"
export CXX="${CROSS_PREFIX}g++"
export STRIP="${CROSS_PREFIX}strip"

echo "============================================="
echo "OpenIPC mini-jq 裁剪编译脚本"
echo "交叉编译器: $CC"
echo "输出目录: $JQ_BUILD_OUT/bin/jq"
echo "============================================="

# 拉取/更新jq源码
OLD_PWD="$PWD"
if [ ! -d "$JQ_SRC_DIR/.git" ]; then
    echo "[1] 克隆jq官方源码"
    git clone https://github.com/jqlang/jq.git "$JQ_SRC_DIR"
    cd "$JQ_SRC_DIR"
    git submodule update --init --recursive
else
    echo "[1] 更新已有jq源码"
    cd "$JQ_SRC_DIR"
    git pull origin master
    git submodule update --recursive
fi
cd "$OLD_PWD"

# 瘦身编译参数
export CFLAGS="-Os -DNDEBUG -ffunction-sections -fdata-sections -fomit-frame-pointer"
export LDFLAGS="-static -Wl,--gc-sections -Wl,--no-export-dynamic"

rm -rf "$JQ_BUILD_OUT"
mkdir -p "$JQ_BUILD_OUT"

# 交叉编译裁剪版jq
cd "$JQ_SRC_DIR"
autoreconf -fi
./configure \
    --host="$HOST_TRIPLET" \
    --prefix="$JQ_BUILD_OUT" \
    --without-oniguruma \
    --disable-all-builtins \
    --disable-maintainer-mode \
    --disable-shared \
    --enable-static

make clean
make -j$(nproc)
make install
cd "$OLD_PWD"

# strip 压缩二进制
JQ_BIN="${JQ_BUILD_OUT}/bin/jq"
$STRIP --strip-all "$JQ_BIN"
echo "[裁剪完成] jq 文件大小: $(ls -lh $JQ_BIN | awk '{print $5}')"
echo "[产物路径] $JQ_BIN"

echo "============================================="
echo "编译结束，仅输出至jq_build_out，无拷贝固件/上传设备操作"
echo "============================================="
