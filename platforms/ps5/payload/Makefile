# ScriptHook VI PS5 loader payload (also built by CMake: shvi_add_ps5_loader).
# Builds build/shvi_loader.elf with the ps5-payload-sdk toolchain.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK is undefined)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

BUILD := build
ELF   := $(BUILD)/shvi_loader.elf

CFLAGS := -Wall -Wextra -g -O2

SRCS := main.c proc.c proc_select.c inject.c

all: $(ELF)

$(ELF): $(SRCS) proc.h inject.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)

# Send to the console's ELF loader. Ask the user before every run.
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

.PHONY: all clean test
