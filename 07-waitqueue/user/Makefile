TARGET := si_wait_test
CFLAGS ?= -Wall -Wextra -O2

.PHONY: all clean

all: $(TARGET)

$(TARGET): si_wait_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
