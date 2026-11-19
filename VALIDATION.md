# USB 鍵盤整合驗證

日期：2026-11-04。Windows、GCC14.3.1（Arm GNU14.3.Rel1）、GNU Make4.4.1。所有host C程式使用ARM1176、ARM state、soft-float。

**實體USB／OTG／鍵盤、Wi-Fi RF與HDMI訊號尚未驗證。** 測試執行實際ARM核心、驅動、解析器與LVGL指令；DWC2暫存器、DMA完成與鍵盤回覆由模型提供。測試不代表實機相容性或效能。

## 測試項目

- 嚴格ARM GCC建置、連結、ELF entry0x8000、向量／BSS／stack檢查、無未解析符號。
- HID／描述符／佇列：156項檢查，涵蓋Shift/Caps、六鍵、重複usage／report、rollover、短報告、非法descriptor長度、端點大小、溢位復原與按值複製。
- 正式kernel的Low Speed、Full Speed USB模型：GET_DESCRIPTOR分包、SET_ADDRESS時機、非零interface、SET_CONFIGURATION、SET_PROTOCOL、NAK重試、DATA0/1切換、Caps LED及SET_IDLE STALL容忍。
- 真正LVGL group/keypad：Tab、Shift+Tab、方向鍵、Enter、文字與Backspace；確認文字ABd+-，按鈕結果51，按住Enter拔除不多點擊，重新插入成功。
- 無裝置、錯誤描述符及通道逾時：GUI與排程持續運作，傳輸停止後才允許重用DMA buffer。
- USB不可用時，既有UART+/-/R、framebuffer像素／padding／guard及GUI stack回歸。
- lwIP回歸：合成Ethernet上的DHCP、ARP、UDP/TCP echo、錯誤IPv4 checksum丟棄。這不是無線握手測試。
- 固定vendor來源、SD開機檔案SHA256，以及ZIP解壓至乾淨短路徑後重建逐位元組比對。

測試紀錄保存在 [tests/results/](tests/results/)，映像大小、SHA256和乾淨重建結果見 [delivery.json](delivery.json)。USB單元測試另編入usb-unit.elf；正式kernel.img不包含測試入口或假USB裝置。

## 重跑

~~~powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tests\requirements.txt
mingw32-make -j4 all
mingw32-make -j4 -f Makefile -f tests/build-usb.mk usb-unit-image
mingw32-make -j4 -f Makefile -f tests/build-network.mk test-image
.\.venv\Scripts\python.exe tests/test_gui.py --elf build/usb-unit.elf
.\.venv\Scripts\python.exe tests/test_usb.py --mode low
.\.venv\Scripts\python.exe tests/test_usb.py --mode full
.\.venv\Scripts\python.exe tests/test_usb.py --mode absent --ticks 3000
.\.venv\Scripts\python.exe tests/test_usb.py --mode malformed --ticks 3000
.\.venv\Scripts\python.exe tests/test_usb.py --mode timeout --ticks 3000
.\.venv\Scripts\python.exe tests/test_network.py --mode ethernet
~~~

測試時間是合成時間，不能用log裡的frame數或tick間距推算板上效能。模型未涵蓋Hub、split transaction、高速USB、訊號品質、供電、真實鍵盤韌體差異或Wi-Fi與USB同時的實體壓力測試。

下一步板測：直接接一把標準Boot鍵盤，核對UART中的VID/PID、按鍵、Caps LED、Tab/Enter、拔除重插，再與Wi-Fi傳輸共同測試。若發生controller halt失敗，保持原始紀錄後重新上電。
