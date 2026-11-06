# Third-party sources

The delivered source lock files identify exact files and their SHA256 hashes.

| Component | Pinned source | License |
| --- | --- | --- |
| LVGL | https://github.com/lvgl/lvgl/tree/c016f72d4c125098287be5e83c0f1abed4706ee5 | MIT; vendor/lvgl/LICENCE.txt and component notices in src |
| uC/OS-II | https://github.com/weston-embedded/uC-OS2/tree/bfc6120f20cb27a927a9fc5dd667f9e455495777 | Apache-2.0; vendor/ucos2 |
| Raspberry Pi firmware | https://github.com/raspberrypi/firmware/tree/12eeaa12865869b07db760f4bbb7507ec6f1976c | sdcard/LICENCE.broadcom |
| GCC libgcc integer runtime | Arm GNU Toolchain 14.3.Rel1, GCC 14.3.1 | GPLv3 with GCC Runtime Library Exception 3.1 |

LVGL's src tree is copied unchanged; unneeded optional drivers/libraries remain as source but are not built. The Makefile selects only the configured built-in renderer, allocator, font and widget sources. lv_conf.h is a modified copy of the upstream configuration template, retaining its MIT license via the upstream notice.

The firmware binary links a small subset of the official toolchain's libgcc.a.
GCC sources are available from the [Arm toolchain release page](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads).
Runtime license: [GCC Runtime Library Exception 3.1](https://www.gnu.org/licenses/gcc-exception-3.1.html).
The toolchain itself is not included.

Python regression dependencies are specified in tests/requirements.txt and are not bundled.

## Wi-Fi extension

| Component | Pinned source | License / notice |
| --- | --- | --- |
| Infineon WHD 3.1.0 | https://github.com/Infineon/wifi-host-driver/tree/974a4bfc0cc0c54f6c317e570bceb3fdaf1a7eb2 | Apache-2.0 file headers; retain vendor/whd/LICENSE.txt and upstream notices |
| 43438A1 firmware + CLM | Same WHD commit, WiFi_Host_Driver/resources | wifi-firmware/LICENSE-permissive-binary-license-1.0.txt; binary bytes unmodified |
| Zero W NVRAM | https://github.com/RPi-Distro/firmware-nonfree/tree/c9d3ae6584ab79d19a4f94ccf701e888f9f87a53 | wifi-firmware/RPi-Distro-copyright, binary-redist-Cypress; original text retained |
| lwIP 2.2.1 | https://github.com/lwip-tcpip/lwip/tree/77dcd25a72509eb83f72b033d219b1d40cd8eb95 | vendor/lwip/COPYING, BSD-style |
| nanoprintf 0.6.0 | https://github.com/charlesnicholson/nanoprintf/tree/e3a1e65c7ae0699d26d25d68f6c3e21babfe31ca | vendor/nanoprintf/LICENSE, 0BSD / Unlicense |
| Newlib string/conversion functions | Arm GNU Toolchain 14.3.Rel1 libc.a | vendor/NEWLIB-LICENSE.txt; upstream notice collection from https://github.com/mirror/newlib-cygwin/blob/master/COPYING.NEWLIB |

Firmware copyright: Copyright 2024, Cypress Semiconductor Corporation (an Infineon company), as stated in the matching generated upstream 43438A1 C resources. The binary license file is copied unchanged from the resources directory. No DEPENDENCIES file was supplied in that pinned upstream tree.

The Zero W alias is debian/config/brcm80211/brcm/brcmfmac43430-sdio.raspberrypi,model-zero-w.txt, pointing to brcmfmac43430-sdio.txt. The original text is preserved as zero-w-nvram.txt. Runtime packing changes MAC/country fields in RAM; it does not edit the shipped source.

network-lock.json records every added vendor/resource file, its provenance and SHA256. WHD, lwIP and nanoprintf source files are unchanged. The BCM2835 board HAL, RTOS adapters and app live in net/. Circle was consulted for board bring-up reference; no Circle source is incorporated.

Technical references:
- Raspberry Pi Zero W device tree: https://github.com/raspberrypi/linux/blob/rpi-6.6.y/arch/arm/boot/dts/broadcom/bcm2835-rpi-zero-w.dts
- BCM2835 ARM peripherals: https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf
- lwIP OS interface: https://www.nongnu.org/lwip/2_1_x/group__sys__layer.html (implementation is pinned 2.2.1)
- ARM unaligned initialization guidance (U bit 22): https://documentation-service.arm.com/static/5eb946b50f1c1e0dae6ee21e

Newlib provides only the linked C helpers; allocation and formatted logging are supplied by net/net_runtime.c. No semihosting/syscall stubs are used. The prebuilt radio image is opaque: its execution is not covered by the ARM host tests.
