#!/bin/sh

# 板端运行时权限补充脚本。
# 文件清单全部使用板端绝对路径，每次开机重复执行 chmod +x 不会产生副作用。

# 正常板端运行时保持为空；测试临时根文件系统时可以传入目录前缀。
ROOT_PREFIX="${ROOT_PREFIX:-}"

# 需要增加执行权限的文件清单。
# 后续新增 Shell、Python 脚本或二进制工具时，直接在清单末尾追加绝对路径。
EXEC_FILES="
${ROOT_PREFIX}/etc/init.d/S99zzencoder
${ROOT_PREFIX}/root/add_exec.sh
${ROOT_PREFIX}/root/encoder/app_service.sh
${ROOT_PREFIX}/root/encoder/battery.sh
${ROOT_PREFIX}/root/encoder/common.sh
${ROOT_PREFIX}/root/encoder/config.sh
${ROOT_PREFIX}/root/encoder/config_page.sh
${ROOT_PREFIX}/root/encoder/delete_all.sh
${ROOT_PREFIX}/root/encoder/device_id.sh
${ROOT_PREFIX}/root/encoder/encoder_main.sh
${ROOT_PREFIX}/root/encoder/feature_engine.sh
${ROOT_PREFIX}/root/encoder/install.sh
${ROOT_PREFIX}/root/encoder/led.sh
${ROOT_PREFIX}/root/encoder/mqtt.sh
${ROOT_PREFIX}/root/encoder/protocol.sh
${ROOT_PREFIX}/root/encoder/runtime.sh
${ROOT_PREFIX}/root/encoder/start_encoder.sh
${ROOT_PREFIX}/root/encoder/state.sh
${ROOT_PREFIX}/root/encoder/voice.sh
"

updated_count=0

# 逐项处理清单，不扫描目录，避免为不需要执行的文件误加权限。
for file_path in $EXEC_FILES; do
    if [ ! -f "$file_path" ]; then
        echo "add_exec failed: listed file not found: $file_path" >&2
        exit 1
    fi

    # 保留原有读写权限，只补充执行权限，不写死为 755 或 777。
    chmod +x "$file_path" || {
        echo "add_exec failed: chmod error: $file_path" >&2
        exit 1
    }

    echo "add_exec enabled: $file_path"
    updated_count=$((updated_count + 1))
done

echo "add_exec success: enabled $updated_count listed files"
