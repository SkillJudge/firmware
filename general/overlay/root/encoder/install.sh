#!/bin/sh

# 安装脚本。
# 在板端执行后，会解压 V-* 安装包到 /root/encoder，记录脚本安装包版本，并尽量恢复 majestic 配置。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
TARGET_DIR="/root/encoder"
TMP_DIR="/tmp/encoder_install.$$"
BOARD_MAJESTIC_CFG="/etc/majestic.yaml"
BOARD_MAJESTIC_CFG_BAK_PRIMARY="/etc/majestic.yaml.bak"
BOARD_MAJESTIC_CFG_BAK_SECONDARY="/etc/majestic.yaml.encoder.bak"

print_line() {
    # 安装脚本不依赖 common.sh，保持可以独立运行。
    printf '%s\n' "$*"
}

find_package_file() {
    # 优先使用用户传入的包路径；没有传参时，在当前脚本目录查找第一个 V-* 安装包。
    if [ -n "$1" ] && [ -f "$1" ]; then
        printf '%s\n' "$1"
        return 0
    fi

    for ext in tar.gz tgz tar zip; do
        for candidate in "$SCRIPT_DIR"/V-*."$ext"; do
            [ -f "$candidate" ] || continue
            printf '%s\n' "$candidate"
            return 0
        done
    done

    return 1
}

extract_package() {
    # 支持 tar.gz/tgz/tar/zip 四种包格式。
    package_file="$1"
    output_dir="$2"

    case "$package_file" in
        *.tar.gz|*.tgz)
            tar -xzf "$package_file" -C "$output_dir"
            ;;
        *.tar)
            tar -xf "$package_file" -C "$output_dir"
            ;;
        *.zip)
            unzip -oq "$package_file" -d "$output_dir"
            ;;
        *)
            return 1
            ;;
    esac
}

detect_extract_root() {
    # 如果压缩包里只有一个顶层目录，就把这个目录作为源码根；否则直接使用解压目录。
    base_dir="$1"
    entries=$(find "$base_dir" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l | tr -d '[:space:]')
    if [ "$entries" = "1" ]; then
        only_entry=$(find "$base_dir" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -n 1)
        if [ -n "$only_entry" ]; then
            printf '%s\n' "$only_entry"
            return
        fi
    fi

    printf '%s\n' "$base_dir"
}

extract_version() {
    # 从安装包文件名提取版本号，例如 V-2.5.zip -> V-2.5。
    package_name=$(basename "$1")
    package_name=${package_name%.tar.gz}
    package_name=${package_name%.tgz}
    package_name=${package_name%.tar}
    package_name=${package_name%.zip}
    printf '%s\n' "$package_name"
}

check_device_id_env() {
    # 只做提示，不阻塞安装；真正启动时 encoder_main.sh 会强校验 device_id。
    if [ -n "$DEVICE_ID" ]; then
        print_line "device id detected from global environment: DEVICE_ID=$DEVICE_ID"
        return 0
    fi

    print_line "warning: DEVICE_ID not found in current process environment"
    print_line "encoder_main.sh will require global DEVICE_ID before startup"
    return 0
}

restore_board_majestic_config() {
    # 安装时确认 /etc/majestic.yaml 仍包含 records/outgoing 关键配置。
    # 如果当前文件缺失或明显不完整，就从备份恢复，避免 cli 配置写入失败。
    if [ -f "$BOARD_MAJESTIC_CFG_BAK_PRIMARY" ]; then
        source_bak="$BOARD_MAJESTIC_CFG_BAK_PRIMARY"
    elif [ -f "$BOARD_MAJESTIC_CFG_BAK_SECONDARY" ]; then
        source_bak="$BOARD_MAJESTIC_CFG_BAK_SECONDARY"
    else
        print_line "majestic backup not found: $BOARD_MAJESTIC_CFG_BAK_PRIMARY or $BOARD_MAJESTIC_CFG_BAK_SECONDARY"
        return 0
    fi

    if [ ! -f "$BOARD_MAJESTIC_CFG" ]; then
        cp "$source_bak" "$BOARD_MAJESTIC_CFG" || return 1
        print_line "majestic config restored from backup: missing current file source=$source_bak"
        return 0
    fi

    if grep -q '^[[:space:]]*records:' "$BOARD_MAJESTIC_CFG" && grep -q '^[[:space:]]*outgoing:' "$BOARD_MAJESTIC_CFG"; then
        print_line "majestic config already looks complete, keep current file"
        return 0
    fi

    cp "$source_bak" "$BOARD_MAJESTIC_CFG" || return 1
    print_line "majestic config restored from backup: incomplete current file source=$source_bak"
}

stop_old_processes() {
    # 安装前停止旧版本服务，避免复制文件时仍有脚本在运行。
    ps w 2>/dev/null | grep "$TARGET_DIR" | grep -E 'encoder_main\.sh|app_service\.sh|voice\.sh|led\.sh' | grep -v grep | while read -r pid _; do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null
    done
}

# 主安装流程：找包 -> 解压 -> 停旧进程 -> 清空目标目录 -> 复制 -> 记录脚本安装包版本 -> 检查 majestic。
print_line "===== GK7205 Encoder Installer ====="

PACKAGE_FILE=$(find_package_file "$1") || {
    print_line "install failed: package not found"
    print_line "usage: sh install.sh /path/to/V-2.0.tar.gz"
    exit 1
}

PACKAGE_VERSION=$(extract_version "$PACKAGE_FILE")
print_line "package file: $PACKAGE_FILE"
print_line "package version: $PACKAGE_VERSION"

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR" || exit 1

extract_package "$PACKAGE_FILE" "$TMP_DIR" || {
    print_line "install failed: package extract error"
    rm -rf "$TMP_DIR"
    exit 1
}

SOURCE_DIR=$(detect_extract_root "$TMP_DIR")
print_line "source dir: $SOURCE_DIR"

stop_old_processes
mkdir -p "$TARGET_DIR" || {
    rm -rf "$TMP_DIR"
    exit 1
}

find "$TARGET_DIR" -mindepth 1 -maxdepth 1 2>/dev/null | while IFS= read -r item; do
    [ -n "$item" ] || continue
    rm -rf "$item"
done

cp -R "$SOURCE_DIR"/. "$TARGET_DIR"/ || {
    print_line "install failed: copy error"
    rm -rf "$TMP_DIR"
    exit 1
}

chmod +x "$TARGET_DIR"/*.sh 2>/dev/null
check_device_id_env

restore_board_majestic_config || {
    print_line "install failed: majestic config restore error"
    rm -rf "$TMP_DIR"
    exit 1
}

printf '%s\n' "$PACKAGE_VERSION" > "$TARGET_DIR/.installed_package_version"
rm -rf "$TMP_DIR"

print_line "install success: target=$TARGET_DIR version=$PACKAGE_VERSION"
print_line "next step: sh $TARGET_DIR/encoder_main.sh"
