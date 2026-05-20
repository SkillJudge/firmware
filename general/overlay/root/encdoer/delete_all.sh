#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"

delete_package_archives() {
    target_version="$1"
    [ -n "$target_version" ] || return 0

    for base_dir in "$PWD" "$APP_HOME" "$(dirname "$APP_HOME")" "/root" "/tmp"; do
        [ -d "$base_dir" ] || continue
        find "$base_dir" -maxdepth 2 -type f \( \
            -name "${target_version}.tar.gz" -o \
            -name "${target_version}.tgz" -o \
            -name "${target_version}.tar" -o \
            -name "${target_version}.zip" \
        \) 2>/dev/null | while IFS= read -r archive_path; do
            [ -n "$archive_path" ] || continue
            log_info "delete archive file=$archive_path"
            rm -f "$archive_path"
        done
    done
}

delete_installed_files() {
    target_version="$1"
    [ "$target_version" = "$DEVICE_VERSION" ] || {
        log_info "skip installed file purge because target_version=$target_version current_version=$DEVICE_VERSION"
        return 0
    }

    find "$APP_HOME" -mindepth 1 -maxdepth 1 ! -name 'delete_all.sh' 2>/dev/null | while IFS= read -r item; do
        [ -n "$item" ] || continue
        log_info "delete installed item=$item"
        rm -rf "$item"
    done

    (
        sleep 1
        rm -f "$APP_HOME/delete_all.sh"
    ) >/dev/null 2>&1 &
}

ensure_layout
state_init
print_title "$DELETE_PAGE_TITLE"

TARGET_VERSION="${1:-$DEVICE_VERSION}"

log_info "delete_all start target_version=$TARGET_VERSION app_home=$APP_HOME"
stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
stop_pidfile_process "$LISTENER_PID_FILE"
stop_pidfile_process "$HEARTBEAT_PID_FILE"
stop_pidfile_process "$MAIN_PID_FILE"
kill_processes_under_app_home
delete_package_archives "$TARGET_VERSION"
delete_installed_files "$TARGET_VERSION"
safe_remove_path "$WORKDIR"
safe_remove_path "$CAPTURE_LOCAL_DIR"
safe_remove_path "$AUDIO_LOCAL_DIR"
