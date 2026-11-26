build/usb_keyboard_unit.o: usb/usb_keyboard.c
	"$(CC)" $(CFLAGS) -DUSB_UNIT -c $< -o $@
build/usb_unit.o: tests/usb_unit.c
	"$(CC)" $(CFLAGS) -c $< -o $@
build/usb-unit.elf: $(filter-out build/usb_keyboard.o,$(OBJECTS)) build/usb_keyboard_unit.o build/usb_unit.o linker.ld
	$(file >build/usb-unit.rsp,$(filter-out build/usb_keyboard.o,$(OBJECTS)) build/usb_keyboard_unit.o build/usb_unit.o)
	"$(CC)" $(filter-out -Wl%,$(LDFLAGS)) -Wl,-T,linker.ld,--gc-sections,--build-id=none,--fatal-warnings,-z,noexecstack @build/usb-unit.rsp -lc -lgcc -o $@
usb-unit-image: build/usb-unit.elf
