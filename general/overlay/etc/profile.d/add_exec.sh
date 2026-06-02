#!/bin/sh

# 本脚本与 encoder.sh、start_encoder.sh 一起放在板子的 /etc/profile.d 中。
# 文件清单全部使用板端绝对路径，不依赖调用脚本时所在的当前目录。
# /etc/profile 自动读取本文件时不执行权限操作，只有 encoder.sh 传入 --run 时才执行。
[ "${1:-}" = "--run" ] || return 0 2>/dev/null || exit 0

# 需要增加执行权限的文件清单。
# 每行填写一个板端绝对路径，不限制文件扩展名。
# 后续新增 Shell、Python 脚本或二进制工具时，直接在清单末尾追加一行。
EXEC_FILES="
/etc/profile.d/encoder.sh
/etc/profile.d/add_exec.sh
/etc/profile.d/start_encoder.sh
/root/encoder/app_service.sh
/root/encoder/battery.sh
/root/encoder/common.sh
/root/encoder/config.sh
/root/encoder/config_page.sh
/root/encoder/delete_all.sh
/root/encoder/device_id.sh
/root/encoder/encoder_main.sh
/root/encoder/feature_engine.sh
/root/encoder/install.sh
/root/encoder/led.sh
/root/encoder/mqtt.sh
/root/encoder/protocol.sh
/root/encoder/runtime.sh
/root/encoder/state.sh
/root/encoder/voice.sh
"

updated_count=0

# 逐项处理清单，不扫描目录，避免为不需要执行的文件误加权限。
for file_path in $EXEC_FILES; do
    # 只补充执行权限，不写死为 755 或 777。
    if [ ! -f "$file_path" ]; then
        echo "add_exec failed: listed file not found: $file_path" >&2
        exit 1
    fi

    chmod +x "$file_path" || exit 1
    echo "add_exec enabled: $file_path"
    updated_count=$((updated_count + 1))
done

echo "add_exec success: enabled $updated_count listed files"
