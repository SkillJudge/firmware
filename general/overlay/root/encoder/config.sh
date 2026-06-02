#!/bin/sh

# 全局配置文件。
# 这里保存默认配置；注册成功后，FTP/SRS 等运行时参数会优先使用 runtime/state 中的云端下发值。
APP_HOME=$(CDPATH= cd "$(dirname "$0")" && pwd) # 当前脚本安装目录，自动计算。
INSTALL_TARGET="/root/encoder" # 安装脚本写入板子的目标目录。

# 页面和终端标题。
PROJECT_TITLE="GK7205 Encoder Full Script Controller" # 主程序终端标题。
CONFIG_PAGE_TITLE="GK7205 Encoder Config Page" # 配置页终端标题。
DELETE_PAGE_TITLE="GK7205 Encoder Delete Page" # 删除脚本终端标题。

# 设备 ID 只能从当前进程继承的全局环境变量 DEVICE_ID 读取，不提供默认值。
# 现场部署时必须在系统环境或启动脚本里导出 DEVICE_ID，避免设备注册到空 topic 或错误 topic。
DEVICE_ID_SOURCE="missing"
DEVICE_ID_CONFIGURED="false"
CONFIG_PROCESS_DEVICE_ID="${DEVICE_ID:-}"
DEVICE_ID=""

if [ -n "$CONFIG_PROCESS_DEVICE_ID" ]; then
    DEVICE_ID="$CONFIG_PROCESS_DEVICE_ID"
    DEVICE_ID_SOURCE="global-env:DEVICE_ID"
    DEVICE_ID_CONFIGURED="true"
    export DEVICE_ID
fi

unset CONFIG_PROCESS_DEVICE_ID

# 板子版本从系统文件读取，用于注册、心跳和日志。安装包版本单独保存在 .installed_package_version。
DEVICE_VERSION_FILE="/etc/version" # 板子系统版本文件，读取首行作为上报版本。
INSTALLED_PACKAGE_VERSION_FILE="${APP_HOME}/.installed_package_version" # 当前脚本安装包版本记录文件。

get_device_version() {
    device_version_value=$(sed -n '1p' "$DEVICE_VERSION_FILE" 2>/dev/null | tr -d '\r\n')
    [ -n "$device_version_value" ] || device_version_value="unknown"
    printf '%s\n' "$device_version_value"
}

DEVICE_VERSION=$(get_device_version)

# 控制服务的 MQTT 连接参数。
# 订阅 topic 会绑定当前 DEVICE_ID，因此 device_id 变化后 topic 也会自动变化。
MQTT_HOST="123.60.51.11" # MQTT 服务器地址。
MQTT_PORT="1883" # MQTT 服务器端口。
MQTT_USER="mqttadmin" # MQTT 登录用户名。
MQTT_PASS="skilljudge123" # MQTT 登录密码。
MQTT_QOS="2" # MQTT 服务质量等级。

MQTT_SUBSCRIBE_TOPIC="+/+/encoder/${DEVICE_ID}/#" # 接收控制端下发指令的订阅主题。
MQTT_REGISTER_TOPIC="encoder/${DEVICE_ID}/ctrlsrv/0/heartbeat/register" # 设备注册上报主题。
MQTT_REGISTER_ACK_TOPIC="ctrlsrv/0/encoder/${DEVICE_ID}/heartbeat/register_ack" # 设备注册 ACK 主题。
MQTT_HEARTBEAT_TOPIC="encoder/${DEVICE_ID}/ctrlsrv/0/heartbeat/heartbeat" # 设备心跳上报主题。
MQTT_HEARTBEAT_ACK_TOPIC="ctrlsrv/0/encoder/${DEVICE_ID}/heartbeat/heartbeat_ack" # 心跳 ACK 主题。

# 服务循环与超时配置。
HEARTBEAT_INTERVAL_SEC="30" # 心跳上报间隔秒数。
REGISTER_ACK_TIMEOUT_SEC="10" # 等待注册 ACK 的超时时间。
REGISTER_RETRY_INTERVAL_SEC="5" # 注册失败后的重试间隔。
SEGMENT_SCAN_INTERVAL_SEC="10" # 录像分片扫描间隔。
SEGMENT_STABLE_SEC="20" # 文件保持不变达到该时长后，才视为可上传分片。
RECORD_FINALIZE_WAIT_SEC="3" # 停止录像后等待最终文件落盘的时间。
CURL_CONNECT_TIMEOUT_SEC="10" # FTP 上传连接超时时间。
CURL_UPLOAD_MAX_TIME_SEC="60" # 单个文件 FTP 上传最长执行时间。

BATTERY_REFRESH_ENABLED="true" # 是否在每次心跳前读取真实电量与充电状态。
BATTERY_I2C_BUS="1" # 库仑计 I2C 总线编号。
BATTERY_I2C_ADDR="0x36" # 库仑计 I2C 地址。
BATTERY_SOC_REG="0x04" # SOC 电量百分比寄存器。
BATTERY_CRATE_REG="0x16" # CRATE 充放电速率寄存器。
BATTERY_CHARGING_THRESHOLD_RAW="5" # 大于等于该值时判定为正在充电。
BATTERY_DISCHARGING_THRESHOLD_RAW="-5" # 小于等于该值时判定为正在放电。

# LED controller on the I2C GPIO expander. Only bits 3, 4, and 5 are modified.
LED_ENABLED="true" # 是否启用状态灯控制。
LED_I2C_BUS="1" # 状态灯 I2C 总线编号。
LED_I2C_ADDR="0x20" # 状态灯 GPIO 扩展器地址。
LED_MONO_MASK="0x08" # 单色录像灯掩码，对应 bit 3。
LED_STREAM_MASK="0x10" # 双色灯红色推流灯掩码，对应 bit 4。
LED_UPLOAD_MASK="0x20" # 双色灯绿色上传灯掩码，对应 bit 5。
LED_STATUS_DELAY_SEC="1" # 推流和录像业务状态变化后，延迟刷新指示灯的秒数。
LED_UPLOAD_ON_SEC="1" # 上传期间绿灯每次点亮的秒数。
LED_UPLOAD_OFF_SEC="1" # 上传期间绿灯每次熄灭的秒数。
LED_UPLOAD_MIN_BLINK_SEC="8" # 即使上传很快完成，绿灯至少持续快闪的秒数。
LED_IDLE_POLL_SEC="1" # 绿灯闪烁 worker 空闲时的检查间隔。

# 录像文件命名和本地录像配置。
# 远端录像根目录当前按需求设置为 upload，最终路径为 upload/<device_id>/<record_id>/<file_name>。
RECORD_FILE_TIME_FORMAT="%Y%m%d%H%M%S" # 上传文件名中的时间格式。
RECORD_FILE_NAME_TEMPLATE="{device_id}-{task_id}-{timestamp}-{segment_no}.mp4" # 录像上传文件名模板。
RECORD_PATH="/mnt/mmcblk0p1/%F" # Majestic 本地录像目录模板。
RECORD_SPLIT="1" # Majestic 录像分片时长，单位分钟。
RECORD_MAX_USAGE="95" # 本地录像存储空间最大占用百分比。
RECORD_SUBSTREAM="false" # 录像是否使用子码流。
RECORD_REMOTE_ROOT="upload" # FTP 服务器上的录像根目录。

# 主码流/子码流配置。推流或录像开始前会写入 majestic。
MAIN_STREAM_ENABLED="true" # 是否启用主码流。
MAIN_STREAM_CODEC="h264" # 主码流编码格式。
MAIN_STREAM_SIZE="1920x1080" # 主码流分辨率。
MAIN_STREAM_FPS="10" # 主码流帧率。
MAIN_STREAM_BITRATE="" # 主码流码率，留空表示保留板子当前设置。
SUB_STREAM_ENABLED="true" # 是否启用子码流。
SUB_STREAM_CODEC="h264" # 子码流编码格式。
SUB_STREAM_SIZE="640x360" # 子码流分辨率。
SUB_STREAM_FPS="15" # 子码流帧率。
SUB_STREAM_BITRATE="" # 子码流码率，留空表示保留板子当前设置。

# 默认 FTP 配置。registerAck 下发运行时配置后，会优先使用云端值。
FTP_HOST="123.60.51.11" # 默认 FTP 服务器地址，注册 ACK 可覆盖。
FTP_PORT="21" # 默认 FTP 端口，注册 ACK 可覆盖。
FTP_USER="ftpuser" # 默认 FTP 用户名，注册 ACK 可覆盖。
FTP_PASS="skilljudge123" # 默认 FTP 密码，注册 ACK 可覆盖。

# 默认 SRS 配置。没有下发 streamUrl 时，代码会用这些值拼接 rtmp 推流地址。
SRS_HOST="123.60.51.11" # 默认 SRS 推流服务器地址，注册 ACK 可覆盖。
SRS_PORT="1935" # 默认 RTMP 端口，注册 ACK 可覆盖。
SRS_APP="live" # RTMP 应用名称。
SRS_STREAM_PREFIX="stream_${DEVICE_ID}" # 推流名称。
STREAM_PUSH_URL="" # 固定推流地址，留空时按 SRS 配置自动生成。
STREAM_SUBSTREAM="true" # 推流是否使用子码流。

# 抓拍、音频、本地录像搜索目录。
CAPTURE_SNAPSHOT_URL="http://127.0.0.1/image.jpg" # Majestic 本地抓拍接口。
CAPTURE_LOCAL_DIR="${APP_HOME}/media/capture" # 抓拍图片临时保存目录。
AUDIO_LOCAL_DIR="${APP_HOME}/media/audio" # 音频临时保存目录。
RECORD_SEARCH_ROOT="/mnt/mmcblk0p1" # 查找 Majestic 录像文件的根目录。

# 视频服务 Majestic 配置和重载命令。当前通过 yaml-cli 修改配置，再 HUP majestic 生效。
STREAM_CONFIG_FILE="/etc/majestic.yaml" # Majestic 主配置文件。
STREAM_CONFIG_BACKUP="/etc/majestic.yaml.encoder.bak" # Majestic 配置备份路径。
STREAM_SERVICE_PROCESS="majestic" # Majestic 进程名称。
STREAM_SERVICE_PID_FILE="/var/run/majestic.pid" # Majestic 系统服务 PID 文件。
STREAM_SERVICE_START_CMD="/etc/init.d/S95majestic start" # Majestic 异常退出后的恢复命令。
STREAM_RELOAD_CMD="killall -HUP majestic" # 修改 Majestic 配置后的重载命令。
STREAM_RELOAD_WAIT_SEC="2" # 重载后等待进程稳定的时间。
STREAM_START_WAIT_SEC="2" # 异常恢复启动后等待进程就绪的时间。

# 日志开关。默认减少现场输出；排查问题时可以通过 config_page.sh set 打开。
LOG_VERBOSE="false" # 是否输出详细调试日志。
CONFIG_COMMAND_VERBOSE="false" # 是否记录实际执行的 Majestic 配置命令。
MQTT_PAYLOAD_VERBOSE="false" # 是否在日志中打印完整 MQTT payload。
HEARTBEAT_LOG_VERBOSE="false" # 是否记录每次心跳发送与接收日志。

# 运行目录和运行时文件位置。
WORKDIR="${APP_HOME}/runtime" # 运行时数据根目录。
STATE_DIR="${WORKDIR}/state" # 状态文件目录。
LOG_DIR="${WORKDIR}/logs" # 日志目录。
TMP_DIR="${WORKDIR}/tmp" # 临时文件目录。
RECORD_NAMED_LOCAL_DIR="${WORKDIR}/named_records" # 上传前标准命名录像的临时目录。
LOGFILE="${LOG_DIR}/encoder.log" # 主日志文件。
MSGID_FILE="${STATE_DIR}/msgid" # MQTT 消息 ID 持久化文件。
TIME_OFFSET_FILE="${STATE_DIR}/time_offset_ms" # 服务端与板端时间差文件。
SERVER_TIMESTAMP_FILE="${STATE_DIR}/server_timestamp_ms" # 最近一次服务端时间戳文件。
RUNTIME_FTP_HOST_FILE="${STATE_DIR}/runtime_ftp_host" # 注册 ACK 下发的 FTP 地址文件。
RUNTIME_FTP_PORT_FILE="${STATE_DIR}/runtime_ftp_port" # 注册 ACK 下发的 FTP 端口文件。
RUNTIME_FTP_USER_FILE="${STATE_DIR}/runtime_ftp_user" # 注册 ACK 下发的 FTP 用户名文件。
RUNTIME_FTP_PASS_FILE="${STATE_DIR}/runtime_ftp_pass" # 注册 ACK 下发的 FTP 密码文件。
RUNTIME_SRS_HOST_FILE="${STATE_DIR}/runtime_srs_host" # 注册 ACK 下发的 SRS 地址文件。
RUNTIME_SRS_PORT_FILE="${STATE_DIR}/runtime_srs_port" # 注册 ACK 下发的 SRS 端口文件。
RUNTIME_SRS_USER_FILE="${STATE_DIR}/runtime_srs_user" # 注册 ACK 下发的 SRS 用户名文件。
RUNTIME_SRS_PASS_FILE="${STATE_DIR}/runtime_srs_pass" # 注册 ACK 下发的 SRS 密码文件。

# 常驻进程 pidfile。
MAIN_PID_FILE="${STATE_DIR}/encoder_main.pid" # 主控制器进程 PID 文件。
LISTENER_PID_FILE="${STATE_DIR}/listener.pid" # MQTT 监听进程 PID 文件。
HEARTBEAT_PID_FILE="${STATE_DIR}/heartbeat.pid" # 心跳进程 PID 文件。
SEGMENT_WORKER_PID_FILE="${STATE_DIR}/segment_worker.pid" # 录像分片上传 worker PID 文件。
VOICE_PLAYER_PID_FILE="${STATE_DIR}/voice_player.pid" # 语音播报 worker PID 文件。
LED_UPLOAD_BLINK_PID_FILE="${STATE_DIR}/led_upload_blink.pid" # 上传绿灯闪烁 worker PID 文件。
LED_UPLOAD_TOKEN_DIR="${STATE_DIR}/led_upload_tokens" # 当前上传动作的内部 token 目录。
LED_I2C_LOCK_DIR="${STATE_DIR}/led_i2c.lock" # 灯控 I2C 写入锁目录。

# 状态文件。心跳、配置页、业务流程都通过 state.sh 统一访问这些文件。
STATE_IDLE_FILE="${STATE_DIR}/is_idle" # 是否空闲状态文件。
STATE_RECORDING_FILE="${STATE_DIR}/is_recording" # 是否录像状态文件。
STATE_PUBLISHING_FILE="${STATE_DIR}/is_publishing" # 是否推流状态文件。
STATE_CHARGING_FILE="${STATE_DIR}/is_charging" # 是否充电状态文件。
STATE_BATTERY_FILE="${STATE_DIR}/battery" # 电量百分比状态文件。
STATE_SIGNAL_FILE="${STATE_DIR}/signal" # 信号强度状态文件。
STATE_CURRENT_TASK_ID_FILE="${STATE_DIR}/current_task_id" # 当前任务 ID 文件。
STATE_CURRENT_RECORD_ID_FILE="${STATE_DIR}/current_record_id" # 当前录像 ID 文件。
STATE_CURRENT_RECORD_FLOW_FILE="${STATE_DIR}/current_record_flow" # 当前录像流程类型文件。
STATE_CURRENT_STREAM_URL_FILE="${STATE_DIR}/current_stream_url" # 当前推流地址文件。
STATE_RECORD_START_TS_FILE="${STATE_DIR}/record_start_ts" # 当前录像开始时间文件。
STATE_RECORD_SESSION_TIME_FILE="${STATE_DIR}/record_session_time" # 当前录像会话时间文本文件。
STATE_SEGMENT_NO_FILE="${STATE_DIR}/segment_no" # 当前录像分片序号文件。
STATE_SEGMENT_MANIFEST_FILE="${STATE_DIR}/segment_manifest" # 已上传录像分片清单文件。

# 配置页允许修改的字段白名单，避免误改内部路径或派生变量。
CONFIG_EDITABLE_KEYS="
MQTT_HOST
MQTT_PORT
MQTT_USER
MQTT_PASS
MQTT_QOS
HEARTBEAT_INTERVAL_SEC
REGISTER_ACK_TIMEOUT_SEC
REGISTER_RETRY_INTERVAL_SEC
SEGMENT_SCAN_INTERVAL_SEC
SEGMENT_STABLE_SEC
RECORD_FINALIZE_WAIT_SEC
CURL_CONNECT_TIMEOUT_SEC
CURL_UPLOAD_MAX_TIME_SEC
RECORD_FILE_TIME_FORMAT
RECORD_FILE_NAME_TEMPLATE
RECORD_PATH
RECORD_SPLIT
RECORD_MAX_USAGE
RECORD_SUBSTREAM
RECORD_REMOTE_ROOT
MAIN_STREAM_ENABLED
MAIN_STREAM_CODEC
MAIN_STREAM_SIZE
MAIN_STREAM_FPS
MAIN_STREAM_BITRATE
SUB_STREAM_ENABLED
SUB_STREAM_CODEC
SUB_STREAM_SIZE
SUB_STREAM_FPS
SUB_STREAM_BITRATE
FTP_HOST
FTP_PORT
FTP_USER
FTP_PASS
SRS_HOST
SRS_PORT
SRS_APP
SRS_STREAM_PREFIX
STREAM_PUSH_URL
STREAM_SUBSTREAM
CAPTURE_SNAPSHOT_URL
CAPTURE_LOCAL_DIR
AUDIO_LOCAL_DIR
RECORD_SEARCH_ROOT
STREAM_CONFIG_FILE
STREAM_CONFIG_BACKUP
STREAM_SERVICE_START_CMD
STREAM_RELOAD_CMD
STREAM_RELOAD_WAIT_SEC
STREAM_START_WAIT_SEC
LOG_VERBOSE
CONFIG_COMMAND_VERBOSE
MQTT_PAYLOAD_VERBOSE
HEARTBEAT_LOG_VERBOSE
LED_STATUS_DELAY_SEC
LED_UPLOAD_ON_SEC
LED_UPLOAD_OFF_SEC
LED_UPLOAD_MIN_BLINK_SEC
"
