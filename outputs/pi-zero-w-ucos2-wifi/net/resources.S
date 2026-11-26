.section .rodata.wifi,"a",%progbits
.balign 4
.global wifi_fw_start, wifi_fw_end, wifi_clm_start, wifi_clm_end
.global wifi_nv_start, wifi_nv_end
wifi_fw_start:
.incbin "wifi-firmware/wifi.bin"
wifi_fw_end:
.balign 4
wifi_clm_start:
.incbin "wifi-firmware/wifi.clm_blob"
wifi_clm_end:
.balign 4
wifi_nv_start:
.incbin "wifi-firmware/zero-w-nvram.txt"
wifi_nv_end:
.section .note.GNU-stack,"",%progbits
