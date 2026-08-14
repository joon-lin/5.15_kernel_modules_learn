CC ?= cc
CFLAGS ?= -Wall -Wextra -O2

TARGET := si_ioctl16_test
MODULE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))..

.PHONY: all clean

all: $(TARGET)

$(TARGET): si_ioctl16_test.c $(MODULE_DIR)/si_ioctl16_uapi.h
	$(CC) $(CFLAGS) -I$(MODULE_DIR) -o $@ si_ioctl16_test.c

clean:
	rm -f $(TARGET)
