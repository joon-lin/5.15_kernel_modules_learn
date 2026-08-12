TARGET := si_char_test

.PHONY: all clean info

all: $(TARGET)

$(TARGET): si_char_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

info:
	@echo "Compiler: $(CC)"
	@echo "Target  : $(TARGET)"
	@echo "CFLAGS  : $(CFLAGS)"
