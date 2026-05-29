#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

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
            char ver[64], ipnum[64], deviceid[128], eth_ip[64], wifi_ip[64];

            get_cmd_output("cat /etc/version 2>/dev/null", ver, sizeof(ver));
            get_cmd_output("ipcinfo -i 2>/dev/null", ipnum, sizeof(ipnum));
            get_cmd_output("fw_printenv -n DEVICE_ID 2>/dev/null", deviceid, sizeof(deviceid));
            get_cmd_output("ip addr show eth0 2>/dev/null | awk '/inet /{split($2,a,\"/\");print a[1]}'", eth_ip, sizeof(eth_ip));
            get_cmd_output("ip addr show wlan0 2>/dev/null | awk '/inet /{split($2,a,\"/\");print a[1]}'", wifi_ip, sizeof(wifi_ip));

            if (strcmp(ver, "null") == 0) strcpy(ver, "2.1.3");
            if (strcmp(ipnum, "null") == 0) strcpy(ipnum, "23456091");
            if (strcmp(wifi_ip, "null") == 0) strcpy(wifi_ip, "192.168.0.1");

            snprintf(resp_buf, sizeof(resp_buf), 
                     "DEVICEID=%s|VER=%s|IP=%s|WIFI_IP=%s|IPCNUM=%s", 
                     deviceid, ver, eth_ip, wifi_ip, ipnum);

            sendto(sock, resp_buf, strlen(resp_buf), 0, (struct sockaddr *)&client_addr, addr_len);
            printf("[SEND] 回复: %s\n", resp_buf);
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
            system("ffmpeg -i rtsp://127.0.0.1:554/stream=0 -vframes 1 -q:v 2 /tmp/snap.jpg >/dev/null 2>&1");
            sendto(sock, "SNAP_SUCCESS", 12, 0, (struct sockaddr *)&client_addr, addr_len);
        }
    }
    close(sock);
    return 0;
}
