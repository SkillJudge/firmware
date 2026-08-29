#
# encodermain 构建脚本
#   - buildroot 打包模式：由 encodermain.mk 以 CC="$(TARGET_CC)" 调用本 Makefile
#   - 本机快速验证：   make -f Makefile.local（交叉编译）或 make（本地 gcc）
#

TARGET := encodermain
SRCS   := $(wildcard *.c)
OBJS   := $(SRCS:.c=.o)

CC      ?= gcc
# _GNU_SOURCE 由各 .c 文件自行 #define，命令行不再重复定义
# -std=gnu99：proto_internal/mini_json 使用部分 GNU 扩展语义
CFLAGS  ?= -Os -Wall -Wextra -std=gnu99
LDLIBS  ?= -lpthread

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c common.h proto_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o
