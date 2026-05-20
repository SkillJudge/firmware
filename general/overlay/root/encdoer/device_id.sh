#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

usage() {
    cat <<EOF
usage:
  sh $APP_HOME/device_id.sh show
  sh $APP_HOME/device_id.sh check

examples:
  export ENCODER_DEVICE_ID=ENC_20260420_001
  sh $APP_HOME/device_id.sh check
EOF
}

show_device_id_env() {
    print_title "$CONFIG_PAGE_TITLE Device"
    cat <<EOF
DEVICE_ID=$DEVICE_ID
ENCODER_DEVICE_ID=$ENCODER_DEVICE_ID
DEVICE_ID_SOURCE=$DEVICE_ID_SOURCE
DEVICE_ID_CONFIGURED=$DEVICE_ID_CONFIGURED
EOF
}

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
