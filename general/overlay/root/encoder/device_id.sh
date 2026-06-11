#!/bin/sh

# 设备 ID 检查工具。
# 用于确认当前进程看到的 DEVICE_ID 来源，避免设备注册到错误 topic。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)

# 诊断工具直接运行时，优先展示 U-Boot 中持久化的设备 ID。
if command -v fw_printenv >/dev/null 2>&1; then
    UBOOT_DEVICE_ID=$(fw_printenv -n DEVICE_ID 2>/dev/null | sed -n '1p' | tr -d '\r\n')
    if [ -n "$UBOOT_DEVICE_ID" ]; then
        DEVICE_ID="$UBOOT_DEVICE_ID"
        DEVICE_ID_ORIGIN="uboot-env:DEVICE_ID"
        export DEVICE_ID DEVICE_ID_ORIGIN
    fi
fi

. "$SCRIPT_DIR/common.sh"

usage() {
    # 打印用法和设置 DEVICE_ID 的示例。
    cat <<EOF
usage:
  sh $APP_HOME/device_id.sh show
  sh $APP_HOME/device_id.sh check

examples:
  fw_printenv -n DEVICE_ID
  sh $APP_HOME/device_id.sh check
EOF
}

show_device_id_env() {
    # 展示 device_id 相关变量，方便确认是环境变量还是默认配置生效。
    print_title "$CONFIG_PAGE_TITLE Device"
    cat <<EOF
DEVICE_ID=$DEVICE_ID
DEVICE_ID_SOURCE=$DEVICE_ID_SOURCE
DEVICE_ID_CONFIGURED=$DEVICE_ID_CONFIGURED
EOF
}

# `show` 只展示信息，`check` 会执行合法性校验并在失败时返回非零。
case "$1" in
    show|"")
        show_device_id_env
        ;;
    check)
        ensure_device_id_configured || exit 1
        printf '%s\n' "device id ok: DEVICE_ID=$DEVICE_ID source=$DEVICE_ID_SOURCE"
        ;;
    *)
        usage
        exit 1
        ;;
esac
