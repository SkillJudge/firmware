#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>   
#include <sys/types.h> 
#include <time.h>

#define PORT 8086
#define BUF_SIZE 1024

void get_cmd_output(const char *cmd, char *output, int max_len) {
    memset(output, 0, max_len);
    FILE *fp = popen(cmd, "r");
    if (fp != NULL) {
        if (fgets(output, max_len, fp) != NULL) {
            int len = strlen(output);
            while (len > 0 && (output[len - 1] == '\n' || output[len - 1] == '\r')) {
                output[--len] = '\0';
            }
        }
        pclose(fp);
    }
    if (strlen(output) == 0) {
        strcpy(output, "null");
    }
}

int main() {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUF_SIZE];
    char resp_buf[BUF_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sock, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("OpenIPC 动态 C 级服务已启动，正在监听 8086 端口...\n");

    while (1) {
        memset(buffer, 0, BUF_SIZE);
        int n = recvfrom(sock, buffer, BUF_SIZE - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) continue;

        while(n > 0 && (buffer[n-1] == '\n' || buffer[n-1] == '\r')) {
            buffer[--n] = '\0';
        }

        printf("[RECV] -> [%s]\n", buffer);

        if (strcmp(buffer, "DISCOVER") == 0) {
            char ver[64] = {0};
            char ipnum[64] = {0};
            char deviceid[128] = {0};
            char eth_ip[64] = {0};
            char wifi_ip[64] = {0};
            char eth_mac[64] = {0};
            char wifi_mac[64] = {0};

            // 全部初始化为 null
            strcpy(ver, "null");
            strcpy(ipnum, "null");
            strcpy(deviceid, "null");
            strcpy(eth_ip, "null");
            strcpy(wifi_ip, "null");
            strcpy(eth_mac, "null");
            strcpy(wifi_mac, "null");

            // 读取版本
            get_cmd_output("cat /etc/version 2>/dev/null", ver, sizeof(ver));

            // 读取 IPCNUM
            get_cmd_output("ipcinfo -i 2>/dev/null", ipnum, sizeof(ipnum));

            // 读取 DEVICEID
            get_cmd_output("fw_printenv -n DEVICE_ID 2>/dev/null", deviceid, sizeof(deviceid));

            // 读取 eth0 IP
            get_cmd_output("ip addr show eth0 2>/dev/null | awk '/inet /{split($2,a,\"/\");print a[1]}'", eth_ip, sizeof(eth_ip));

            // 读取 eth0 MAC（你的设备能读到）
            get_cmd_output("cat /sys/class/net/eth0/address 2>/dev/null", eth_mac, sizeof(eth_mac));

            // wlan0 读不到 → 保持 null
            // wifi_mac = null
            // wifi_ip = null

            // 拼接协议（严格格式）
            snprintf(resp_buf, sizeof(resp_buf),
                "DEVICEID=%s|VER=%s|IP=%s|WIFI_IP=%s|IPCNUM=%s|MAC=%s|WIFI_MAC=%s",
                deviceid,
                ver,
                eth_ip,
                wifi_ip,
                ipnum,
                eth_mac,
                wifi_mac
            );

            // 发送
            sendto(sock, resp_buf, strlen(resp_buf), 0, (struct sockaddr *)&client_addr, addr_len);
            printf("[SEND] %s\n", resp_buf);
        }
        else if (strncmp(buffer, "SET=DEVICEID:", 13) == 0) {
            char new_id[64];
            strcpy(new_id, buffer + 13);
            if (strlen(new_id) > 0 && strcmp(new_id, "null") != 0) {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "fw_setenv DEVICE_ID '%s' 2>/dev/null", new_id);
                system(cmd);
                sendto(sock, "SET_SUCCESS", 11, 0, (struct sockaddr *)&client_addr, addr_len);
                system("(sleep 2; reboot) &");
            } else {
                sendto(sock, "SET_FAILED", 10, 0, (struct sockaddr *)&client_addr, addr_len);
            }
        } 
        else if (strcmp(buffer, "CAPTURE") == 0) {
            char cmd[256];
            time_t rawtime;
            struct tm *timeinfo;
            char timestamp[32];

            // 1. 获取系统当前时间
            time(&rawtime);
            timeinfo = localtime(&rawtime);

            // 2. 格式化时间戳 (例如: 20260531_071530)
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);

            // 3. 拼接绝对路径的 curl 命令，避开 system() 无法解析 $(date) 的问题
            // 注意：这里显式指定 /usr/bin/curl 或 /bin/curl，防止找不到命令（你可以通过 which curl 确认路径）
            snprintf(cmd, sizeof(cmd), 
                     "/usr/bin/curl -s -o /mnt/mmcblk0p1/snapshot/snap_%s.jpg \"http://127.0.0.1:80/image.jpg\"", 
                     timestamp);

            // 4. 执行命令
            int ret = system(cmd);
            
            // 5. 打印调试日志，方便你从串口或日志里看命令到底长啥样，以及执行结果
            printf("Execute cmd: %s, return code: %d\n", cmd, ret);

            sendto(sock, "SNAP_SUCCESS", 12, 0, (struct sockaddr *)&client_addr, addr_len);
        }
        else if (strstr(buffer, "FIRMWAREUPDATE=") == buffer)
        {
            const char *ftp_url = buffer + strlen("FIRMWAREUPDATE=");
            char cmd[512];
            
            // 拼接后台执行命令
            // 1. 显式调用 /bin/sh 执行脚本
            // 2. 末尾加上 & 让其进入系统后台运行，绝不阻塞主循环
            snprintf(cmd, sizeof(cmd), "/bin/sh /usr/bin/ftp_upgrade \"%s\" &", ftp_url);

            printf("[UPDATE] Triggered background upgrade: %s\n", cmd);
            
            // 执行后立刻返回，主程序不会被卡死
            system(cmd);
        }
    }
    close(sock);
    return 0;
}
