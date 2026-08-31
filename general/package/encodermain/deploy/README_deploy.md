# encodermain/deploy/ — SD 卡一键部署（无需重新刷固件）

## 目录位置
```
firmware/general/package/encodermain/deploy/
├── deploy.sh            编译虚机 192.168.0.104 上执行：git pull → make br-encodermain → 经跳板机上传到编码器 → 远端 runmain
├── runmain              编码器 SD 卡侧执行：停固件 encodermain + 覆盖配置 + 启新 encodermain
└── README_deploy.md     本文件
```

## 先决条件
### 一次性（在编译虚机 192.168.0.104 上）
```bash
sudo apt install -y sshpass git make build-essential file
```
**必须**：编译虚机里已经 **至少完整跑过一次** `cd firmware && ./build.sh` 或者
`make BOARD=gk7205v300-nor-ultimate all`，确保 buildroot 下载/解包/defconfig/toolchain 全就绪。
deploy.sh 只做单包增量 `br-encodermain-dirclean + br-encodermain-rebuild`（~几十秒），
不负责整包构建。

### 跳板机 58.198.176.157 上
```bash
sudo apt install -y sshpass openssh-client
```
（deploy.sh 会用跳板机做"二跳 sshpass"：先跳板机密码再编码器密码，
  所以跳板机里也必须装 sshpass。）

## 使用方法（在编译虚机 192.168.0.104 上执行）
```bash
# 1) 进入 deploy 目录
cd /path/to/your/git/repo/firmware/general/package/encodermain/deploy

# 2) 导出环境变量（所有必导项，按你实际 IP/密码填）
export GIT_DIR=/path/to/your/git/repo          #  repo 根，里面有 firmware/ 子目录
export FIRMWARE_BOARD=gk7205v300-nor-ultimate   #  板级 defconfig

export JUMP_HOST=58.198.176.157
export JUMP_USER=xxx                            #  跳板机用户名
export JUMP_PASS='xxx'                          #  跳板机密码（若已免密可随便填）

export BOARD_IP=192.168.250.116                 #  编码器 01 在实验室里、跳板机能直达的 IP
export BOARD_USER=root
export BOARD_PASS='12345'                       #  编码器 root 密码

# 3) 一键部署（三步全包：git pull → build → copy + 远端 runmain）
bash deploy.sh
```

## 常用 flag
| flag | 作用 |
|---|---|
| (无) | 默认：build + copy + 远端 runmain start |
| `--no-build` | 上次已编译通过，这次只 copy + run |
| `--no-copy`  | 只 build，不部署（想确认 build 先过） |
| `--no-run`   | build + copy 到 SD 卡，但不执行 runmain start（你 SSH 上去手动 ./runmain） |
| `--restore`  | 远端执行 ./runmain restore：恢复固件配置+固件 encodermain（S96encodermain start），用于回退 |

## runmain 在编码器里做了什么
路径：`/mnt/mmcblk0p1/encdeploy/runmain`

| 子命令 | 行为 |
|---|---|
| `runmain` (或 `start`) | 1) `/etc/init.d/S96encodermain stop` + `killall -9 encodermain`（停固件版）<br>2) **若 deploy 包里有 default.encoder / config.sh**：先备份（`.bak.<日期戳>`）到各自目录，再覆盖 `/etc/default/encoder`、`/root/encoder/config.sh`<br>3) 清 PID/lock 文件，cd `/root/encoder/runtime`，后台 nohup 启动 `.../encdeploy/encodermain -b`<br>4) 日志重定向到 `/mnt/mmcblk0p1/encdeploy/logs/encodermain-YYYYmmdd-HHMMSS.log` |
| `runmain stop`    | 只做 stop 所有 encodermain + S96encodermain stop（不启） |
| `runmain restore` | runmain stop + 恢复最近一份 `.bak.<日期戳>` 配置 + `S96encodermain start`（回到固件版进程） |

## 覆盖业务配置（可选）
把你要覆盖的 `config.sh` 放到 `deploy/config.sh.override`，下次 copy 阶段会自动改名传到
远端 `$BOARD_DEPLOYDIR/config.sh`，runmain start 时就会覆盖并备份旧版。

同理 `default.encoder` 直接放在 encodermain package 根目录，每次都会 copy。
不想覆盖？把 `$DEPLOY_SRCDIR/default.encoder` 改个名临时移走即可。

## 常见问题
### Q1. 提示 "缺少依赖命令: sshpass"
```bash
sudo apt install -y sshpass            # 在编译虚机
ssh $JUMP_USER@$JUMP_HOST "sudo apt install -y sshpass"   # 在跳板机
```

### Q2. `make BOARD=... br-encodermain-rebuild` 失败：`buildroot-2024.02.10 不存在`
完整 build 没跑过。回 firmware 目录先 `./build.sh` 或 `make BOARD=gk7205v300-nor-ultimate all`
（通常 30-60 分钟）。

### Q3. 远端 runmain 返回非 0，没起起来
SSH 登 01 号编码器看启动日志 tail：
```bash
ssh -J $JUMP_USER@$JUMP_HOST root@$BOARD_IP
tail -60 /mnt/mmcblk0p1/encdeploy/logs/encodermain-*.log | less
```
通常是 (a) 固件里 /root/encoder/config.sh 变量空 → `DEPLOY_SRCDIR/deploy/config.sh.override` 准备一份正确的覆盖；(b) `DEVICE_ID` 没写进 env 分区 → encodermain 处于 gate-wait（正常，10s 打印一次 "waiting for DEVICE_ID"，等 factoryinit UDP DISCOVER 拿到设备 ID 就自动进入大循环）。

### Q4. 我不想手动改 env，把参数写死进 deploy.sh 可以吗？
可以，在 deploy.sh 顶部 `: "${VAR:=default}"` 一行里替换默认值即可；不推荐（密码进 git 不安全），推荐用 `~/.bashrc` / `direnv` / `*.env` 方案。

### Q5. deploy 成功后，majestic.yaml 的 outgoing.server 还残留 3.1.0 旧值导致 SRS 2051？
runmain start 只是换 encodermain 进程本身，不主动动 yaml。如果 encodermain 还没收到过
一次新的 start_stream 命令（让 `build_stream_url` 重新写 yaml），majestic 就会用旧残留。
第一次 deploy 后建议发一次 `startStream`（通过 encoder_test 或 brain 业务触发），
encodermain 会 `yaml-cli -s .outgoing.server <规范化URL>` + `killall -HUP majestic`。
想立刻验证，也可以在编码器上直接：
```bash
yaml-cli -i /etc/majestic.yaml -s .outgoing.server \
  "rtmp://192.168.250.100:1935/live/stream_ENC_000001"
killall -HUP majestic
sleep 3; awk '$2~/078B$/ {print}' /proc/net/tcp   # 078B=1935
```
