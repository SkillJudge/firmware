#!/bin/sh

# 将本脚本放在仓库根目录后，可以从任意位置执行。
# 脚本会先切换到自身所在目录，因此下面清单中的文件路径全部使用仓库相对路径。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

# 需要增加执行权限的文件清单。
# 每行填写一个相对于仓库根目录的文件路径，不限制文件扩展名。
# 后续新增 Shell、Python 脚本或二进制工具时，直接在本清单末尾追加一行即可。
EXEC_FILES="
general/overlay/root/encoder/app_service.sh
general/overlay/root/encoder/battery.sh
general/overlay/root/encoder/common.sh
general/overlay/root/encoder/config.sh
general/overlay/root/encoder/config_page.sh
general/overlay/root/encoder/device_id.sh
general/overlay/root/encoder/encoder_main.sh
general/overlay/root/encoder/feature_engine.sh
general/overlay/root/encoder/led.sh
general/overlay/root/encoder/mqtt.sh
general/overlay/root/encoder/protocol.sh
general/overlay/root/encoder/runtime.sh
general/overlay/root/encoder/state.sh
general/overlay/root/encoder/voice.sh
general/overlay/etc/init.d/S40network
general/overlay/etc/init.d/S99zzencoder
general/overlay/root/encoder/start_encoder.sh

general/overlay/etc/init.d/S15_i2c_init
general/overlay/etc/init.d/S99ircut_day
general/overlay/usr/bin/ftp_upgrade
general/overlay/usr/bin/led_test
general/overlay/usr/bin/power_key_test

"

updated_count=0

# 逐项处理清单中的文件，不扫描目录，避免为不需要执行的文件误加权限。
for file_path in $EXEC_FILES; do
    # 只允许仓库内部的相对路径，避免误操作系统目录或仓库外部文件。
    case "$file_path" in
        /*|../*|*/../*|*/..|-*)
            printf 'add_exec failed: path must stay relative to repository root: %s\n' "$file_path" >&2
            exit 1
            ;;
    esac

    # 清单项写错或文件尚未加入仓库时立即报错，便于及时发现遗漏。
    if [ ! -f "$file_path" ]; then
        printf 'add_exec failed: listed file not found: %s\n' "$file_path" >&2
        exit 1
    fi

    # 保留文件原有的读写权限，只补充可执行权限，不写死为 755 或 777。
    chmod +x "$file_path" || exit 1
    printf 'add_exec enabled: %s\n' "$file_path"
    updated_count=$((updated_count + 1))
done

printf 'add_exec success: enabled %s listed files\n' "$updated_count"
