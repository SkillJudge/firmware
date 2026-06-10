#!/bin/sh

# 删除脚本。
# 用于清理指定版本安装包和当前安装目录；会先停止相关进程，再删除文件。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"
. "$SCRIPT_DIR/led.sh"

normalize_version() {
    printf '%s\n' "$1" | sed 's/^V-//'
}

delete_package_archives() {
    # 在常见目录中删除指定版本的安装包，避免旧包被误安装。
    target_version="$1"
    [ -n "$target_version" ] || return 0
    normalized_version=$(normalize_version "$target_version")
    package_version="V-${normalized_version}"

    for base_dir in "$PWD" "$APP_HOME" "$(dirname "$APP_HOME")" "/root" "/tmp"; do
        [ -d "$base_dir" ] || continue
        find "$base_dir" -maxdepth 2 -type f \( \
            -name "${target_version}.tar.gz" -o \
            -name "${target_version}.tgz" -o \
            -name "${target_version}.tar" -o \
            -name "${target_version}.zip" -o \
            -name "${package_version}.tar.gz" -o \
            -name "${package_version}.tgz" -o \
            -name "${package_version}.tar" -o \
            -name "${package_version}.zip" \
        \) 2>/dev/null | while IFS= read -r archive_path; do
            [ -n "$archive_path" ] || continue
            log_info "delete archive file=$archive_path"
            rm -f "$archive_path"
        done
    done
}

delete_installed_files() {
    # 只有目标版本等于 /etc/version 中的板子版本时才删除 APP_HOME 内容。
    target_version="$1"
    normalized_target_version=$(normalize_version "$target_version")
    normalized_device_version=$(normalize_version "$DEVICE_VERSION")
    [ "$normalized_target_version" = "$normalized_device_version" ] || {
        log_info "skip installed file purge because target_version=$target_version device_version=$DEVICE_VERSION"
        return 0
    }

    find "$APP_HOME" -mindepth 1 -maxdepth 1 ! -name 'delete_all.sh' 2>/dev/null | while IFS= read -r item; do
        [ -n "$item" ] || continue
        log_info "delete installed item=$item"
        rm -rf "$item"
    done

    (
        # 当前脚本也在安装目录内，延迟一秒自删除，保证前面的清理流程先完成。
        sleep 1
        rm -f "$APP_HOME/delete_all.sh"
    ) >/dev/null 2>&1 &
}

# 主删除流程：初始化状态 -> 停止服务 -> 删除安装包 -> 删除安装目录内容和运行数据。
ensure_layout
state_init
print_title "$DELETE_PAGE_TITLE"

TARGET_VERSION="${1:-$DEVICE_VERSION}"

log_info "delete_all start target_version=$TARGET_VERSION device_version=$DEVICE_VERSION app_home=$APP_HOME"
stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
stop_pidfile_process "$VOICE_PLAYER_PID_FILE"
stop_pidfile_process "$LISTENER_PID_FILE"
stop_pidfile_process "$HEARTBEAT_PID_FILE"
stop_pidfile_process "$MAIN_PID_FILE"
led_runtime_stop
kill_processes_under_app_home
delete_package_archives "$TARGET_VERSION"
delete_installed_files "$TARGET_VERSION"
safe_remove_path "$WORKDIR"
safe_remove_path "$CAPTURE_LOCAL_DIR"
safe_remove_path "$AUDIO_LOCAL_DIR"
