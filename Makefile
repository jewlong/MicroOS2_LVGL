# Native Windows GNU Make. No POSIX shell required.
SHELL := cmd.exe
.SHELLFLAGS := /D /C
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
CROSS ?= arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf
NM := $(CROSS)nm
SIZE := $(CROSS)size
CPU := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -mlittle-endian
CFLAGS := $(CPU) -std=c11 -O2 -g3 -ffreestanding -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -fno-unwind-tables \
    -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
    -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes \
    -Wa,--noexecstack -MMD -MP -I. -Iport -Ivendor/ucos2/Source -Ivendor/lvgl -DLV_CONF_INCLUDE_SIMPLE -DLV_DISABLE_API_MAPPING
ASFLAGS := $(CPU) -g3 -x assembler-with-cpp -Wa,--noexecstack -MMD -MP
LDFLAGS := $(CPU) -nostdlib -nostartfiles -static \
    -Wl,-T,linker.ld,--gc-sections,--build-id=none,--fatal-warnings,-z,noexecstack,-Map,build/kernel.map
OBJECTS := build/startup.o build/os_cpu_a.o build/os_cpu_c.o \
    build/main.o build/uart.o build/bsp.o build/mailbox.o build/framebuffer.o build/gui.o build/runtime.o \
    build/os_core.o build/os_task.o build/os_time.o build/os_sem.o \
    build/os_mutex.o build/os_q.o

# Software renderer, built-in allocator/string/printf, selected widgets.
LVROOT := vendor/lvgl/src
LVGL_SOURCES := $(wildcard $(LVROOT)/*.c $(LVROOT)/core/*.c $(LVROOT)/display/*.c \
    $(LVROOT)/draw/*.c $(LVROOT)/draw/convert/*.c $(LVROOT)/draw/sw/*.c $(LVROOT)/draw/sw/blend/*.c \
    $(LVROOT)/misc/*.c $(LVROOT)/misc/cache/*.c $(LVROOT)/misc/cache/class/*.c $(LVROOT)/misc/cache/instance/*.c \
    $(LVROOT)/font/*.c $(LVROOT)/indev/*.c $(LVROOT)/osal/lv_os.c \
    $(LVROOT)/stdlib/*.c $(LVROOT)/stdlib/builtin/*.c $(LVROOT)/tick/*.c \
    $(LVROOT)/themes/lv_theme.c $(LVROOT)/layouts/lv_layout.c \
    $(LVROOT)/libs/bin_decoder/*.c \
    $(LVROOT)/widgets/bar/*.c $(LVROOT)/widgets/button/*.c $(LVROOT)/widgets/label/*.c)
LVGL_OBJECTS := $(addprefix build/lvgl_,$(notdir $(LVGL_SOURCES:.c=.o)))
OBJECTS += $(LVGL_OBJECTS)
vpath %.c $(sort $(dir $(LVGL_SOURCES)))
# Upstream code is unchanged; app-only prototype warnings do not apply to it.
# TLSF logging is disabled; upstream default_walker has unused parameters.
build/lvgl_lv_tlsf.o: CFLAGS += -Wno-unused-parameter
build/lvgl_%.o: %.c Makefile lv_conf.h | build
	"$(CC)" $(filter-out -Wstrict-prototypes -Wmissing-prototypes,$(CFLAGS)) -c $< -o $@


# Wi-Fi/lwIP port. Raw APIs run only in tcpip_thread.
CFLAGS += -Inet -Ivendor/lwip/src/include -Ivendor/nanoprintf \
    -Ivendor/whd/src -Ivendor/whd/inc -Ivendor/whd/src/include -Ivendor/whd/src/bus_protocols \
    -Ivendor/whd/External/rtos -Ivendor/whd/External/hal -Ivendor/whd/External/bsp \
    -DCYBSP_WIFI_INTERFACE_TYPE=CYBSP_SDIO_INTERFACE
LWIP_SOURCES := $(wildcard vendor/lwip/src/core/*.c vendor/lwip/src/core/ipv4/*.c) \
    vendor/lwip/src/api/tcpip.c vendor/lwip/src/netif/ethernet.c
WHD_SOURCES := $(filter-out %/whd_msgbuf_txrx.c %/whd_commonring.c %/whd_flowring.c %/whd_ring.c,$(wildcard vendor/whd/src/*.c)) \
    vendor/whd/src/bus_protocols/whd_bus.c vendor/whd/src/bus_protocols/whd_bus_common.c vendor/whd/src/bus_protocols/whd_bus_sdio_protocol.c
OBJECTS += $(addprefix build/lwip_,$(notdir $(LWIP_SOURCES:.c=.o))) \
    $(addprefix build/radio_,$(notdir $(WHD_SOURCES:.c=.o))) \
    build/net_runtime.o build/sys_arch.o build/whd_rtos.o build/sdio.o \
    build/whd_resources.o build/whd_buffers.o build/network.o build/resources.o
vpath %.c net $(sort $(dir $(LWIP_SOURCES) $(WHD_SOURCES)))
build/lwip_%.o: %.c Makefile | build
	"$(CC)" $(filter-out -Wmissing-prototypes,$(CFLAGS)) -c $< -o $@
build/radio_%.o: %.c Makefile | build
	"$(CC)" $(filter-out -Wmissing-prototypes -Wstrict-prototypes,$(CFLAGS)) -Wno-unused-parameter -c $< -o $@
build/resources.o: net/resources.S wifi-firmware/wifi.bin wifi-firmware/wifi.clm_blob wifi-firmware/zero-w-nvram.txt | build
	"$(CC)" $(ASFLAGS) -c $< -o $@
build/network.o build/whd_resources.o: net/wifi_config.h $(wildcard net/wifi_local.h)

.PHONY: all clean inspect stage size
all: build/kernel.img build/kernel.list build/kernel.symbols.txt
build:
	@if not exist "build" mkdir "build"
build/startup.o: startup.s Makefile | build
	"$(CC)" $(ASFLAGS) -c $< -o $@
build/os_cpu_a.o: port/os_cpu_a.s Makefile | build
	"$(CC)" $(ASFLAGS) -c $< -o $@
build/os_cpu_c.o: port/os_cpu_c.c Makefile | build
	"$(CC)" $(CFLAGS) -c $< -o $@
build/%.o: %.c Makefile | build
	"$(CC)" $(CFLAGS) -c $< -o $@
build/%.o: vendor/ucos2/Source/%.c Makefile | build
	"$(CC)" $(CFLAGS) -c $< -o $@
build/kernel.elf: $(OBJECTS) linker.ld
	$(file >build/objects.rsp,$(OBJECTS))
	"$(CC)" $(LDFLAGS) @build/objects.rsp -lc -lgcc -o $@
build/kernel.img: build/kernel.elf
	"$(OBJCOPY)" -O binary $< $@
build/kernel.list: build/kernel.elf
	"$(OBJDUMP)" -d -S $< > $@
build/kernel.symbols.txt: build/kernel.elf
	"$(NM)" -n $< > $@
size: build/kernel.elf
	"$(SIZE)" $<
inspect: all
	"$(READELF)" -h -A -l -S build/kernel.elf
	"$(NM)" -u build/kernel.elf
	"$(SIZE)" build/kernel.elf
stage: all
	copy /Y "build\kernel.img" "sdcard\kernel.img" >NUL
	copy /Y "config.txt" "sdcard\config.txt" >NUL
clean:
	@if exist "build" rmdir /S /Q "build"
-include $(OBJECTS:.o=.d)
