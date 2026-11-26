# Included after the normal Makefile, so production objects stay intact.
build/network_test.o: net/network.c tests/net_probe.inc
	"$(CC)" $(CFLAGS) -DNET_TEST -c $< -o $@
build/network-test.elf: $(filter-out build/network.o,$(OBJECTS)) build/network_test.o linker.ld
	$(file >build/test-objects.rsp,$(filter-out build/network.o,$(OBJECTS)) build/network_test.o)
	"$(CC)" $(filter-out -Wl%,$(LDFLAGS)) -Wl,-T,linker.ld,--gc-sections,--build-id=none,--fatal-warnings,-z,noexecstack,-Map,build/network-test.map @build/test-objects.rsp -lc -lgcc -o $@
test-image: build/network-test.elf
