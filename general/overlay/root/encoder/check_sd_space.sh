pt to check SD card storage space for OpenIPC encoder
MOUNT_POINT="/mnt/mmcblk0p1"

echo "==================== SD Card Space Info ===================="
# Check if mount directory exists
if [ ! -d "${MOUNT_POINT}" ];then
    echo "❌ ERROR: Mount path ${MOUNT_POINT} does NOT exist, SD card not detected or mounted!"
    exit 1
fi

# Get disk data and filter target mount point
df_out=$(df -P "${MOUNT_POINT}" | grep -v Filesystem)
total=$(echo $df_out | awk '{print $2}')
used=$(echo $df_out | awk '{print $3}')
free=$(echo $df_out | awk '{print $4}')
use_pct=$(echo $df_out | awk '{print $5}')

# Convert unit from KB to MB / GB
total_mb=$((total / 1024))
used_mb=$((used / 1024))
free_mb=$((free / 1024))
total_gb=$(echo "scale=2; $total_mb / 1024" | bc)
used_gb=$(echo "scale=2; $used_mb / 1024" | bc)
free_gb=$(echo "scale=2; $free_mb / 1024" | bc)

echo "Mount Path: ${MOUNT_POINT}"
echo "Total Size: ${total_mb} MB (${total_gb} GB)"
echo "Used Size: ${used_mb} MB (${used_gb} GB)"
echo "Free Space: ${free_mb} MB (${free_gb} GB)"
echo "Usage Percentage: ${use_pct}"

# Disk space warning logic
pct_num=${use_pct//%/}
if [ $pct_num -ge 90 ];then
    echo -e "\033[31m⚠️ WARNING: Disk usage over 90%, clean record files immediately!\033[0m"
elif [ $pct_num -ge 75 ];then
    echo -e "\033[33m🟡 NOTICE: High disk usage, backup and clean files soon\033[0m"
else
    echo -e "\033[32m✅ Disk space is sufficient\033[0m"
fi

echo "======================================================"
# Count mp4 record files in record folder
REC_PATH="${MOUNT_POINT}/record"
if [ -d "${REC_PATH}" ];then
    file_count=$(ls -l ${REC_PATH}/*.mp4 2>/dev/null | wc -l)
    echo "Total MP4 record files under ${REC_PATH}: ${file_count}"
fi
