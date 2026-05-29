# 如果 Buildroot 传了 CC 就用系统的，没传就用默认 gcc
CC ?= gcc
CFLAGS ?= -Wall -O2

TARGET = ipc_server
SRCS = server.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean