#
# encalertd 构建脚本
#   - buildroot 打包模式：由 encalertd.mk 以 CC="$(TARGET_CC)" 调用本 Makefile
#   - 本机快速验证：   make -f Makefile.local（交叉编译）或 make（本地 gcc）
#

TARGET := encalertd
SRCS   := $(wildcard *.c)
OBJS   := $(SRCS:.c=.o)

CC      ?= gcc
# _GNU_SOURCE 由各 .c 文件自行 #define，命令行不再重复定义
CFLAGS  ?= -Os -Wall -Wextra -std=c99
LDFLAGS ?=

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o
