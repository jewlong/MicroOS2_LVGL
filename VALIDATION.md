# 驗證紀錄

日期：2026-11-03。Windows、Arm GNU Toolchain 14.3.Rel1 / GCC 14.3.1、GNU Make 4.4.1。

**實體 Zero W、RF、WPA2 握手、microSD 開機與 HDMI 電氣訊號尚未測試。** ARM 模型執行真正的核心、CPU 移植、GUI、協定堆疊與 HAL 指令，使用合成硬體／封包邊界；不會執行 Wi-Fi 晶片內部 firmware。

## 建置與來源

- 預設掃描版完整編譯連結成功，-Wall -Wextra -Werror。廠商程式停用不適用的 prototype／unused parameter 警告，應用與適配程式保留嚴格檢查。
- 使用虛構 SSID／密碼另外編譯過連線版本，確認 whd_wifi_join、DHCP 啟停、link up/down 與重連程式可連結。測試設定已移除，不在交付檔內。
- ARM ELF32 little endian、entry 0x8000、ARM1176、soft-float，無未解析符號；向量表、BSS、例外堆疊與 flat image 長度檢查通過。
- WHD 87、lwIP 310、nanoprintf 2 個檔案逐檔與鎖定的 upstream checkout 比對相同。
- µC/OS-II 18、LVGL 1153 個既有來源檔與原鎖定雜湊相符。
- SD 開機檔案六項 SHA256 驗證通過。
- 最終映像大小與 SHA256 見 delivery.json 與 sdcard/SHA256SUMS.txt。

## ARM 執行測試

| 測試 | 條件與通過標準 |
| --- | --- |
| GUI + Wi-Fi 初始化故障 | 執行4000個 OS ticks；mailbox clock request 不回覆，使網路進入 error 0xe100；lwIP task 已啟動，GUI繼續，七次按鍵事件得到51→50→53，沒有佇列丟棄；檢查像素、row padding、framebuffer guard |
| RTOS／buffer／resource | mailbox滿／空／NULL訊息、阻塞逾時、信號量逾時／通知、mutex生命週期、WHD二元通知飽和、封包前端移動及長度拒絕、NVRAM打包／資源block越界拒絕 |
| SDIO 合成暫存器 | 實際HAL執行CMD0/5/3/7/52/53，R5錯誤處理，7-byte讀取、70-byte跨兩個64-byte block讀寫、TX補零、RX不越界、方向及byte-mode長度錯誤拒絕；共119項檢查 |
| 合成 Ethernet + 真正 lwIP | DHCP Discover→Offer→Request→ACK、位址設定與ARP、UDP echo、TCP三向握手與echo、錯誤IPv4 checksum封包丟棄；TCPIP/NET/GUI任務底部stack guard保持不變 |

測試對 IRQ／SVC 返回時的暫存器進行核對。合成時間不是板上效能，GUI刷新次數、吞吐量與毫秒延遲不能當作實機測量。

network-test.elf 透過測試專用入口對封包邊界注入／擷取資料；正式 kernel.img 不含該入口、測試帳密或合成網路。SDIO測試只模擬主機暫存器與傳輸時序，**不包含**SDPCM事件、radio firmware執行或WPA2驗證。故障GUI測試使用正式kernel.elf。

實際輸出保存在 [tests/results/](tests/results/)。若提供framebuffer截圖，來源是執行模型，不是實機照片。

## 重跑

安裝 Python 3，於專案目錄使用隔離環境：

~~~powershell
py -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tests\requirements.txt
mingw32-make -j4 all
mingw32-make -j4 -f Makefile -f tests/build-network.mk test-image
.\.venv\Scripts\python.exe tests\test_gui.py --case rgb --ticks 4000
.\.venv\Scripts\python.exe tests\test_network.py --mode sdio
.\.venv\Scripts\python.exe tests\test_network.py --mode ethernet
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sources.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sdcard.ps1
~~~

請勿分享含私人帳密的設定檔或ELF。封裝後另由ZIP解壓至乾淨目錄建置，與預編譯kernel.img逐位元組比對；結果記於delivery.json。

## 尚需實機驗收

1. 板級clock/GPIO、SDIO電氣時序、43438A1 firmware與Zero W NVRAM配對。
2. 正確／錯誤WPA2密碼、掃描、DHCP、ping、區網TCP/UDP。
3. AP重啟、訊號消失恢復、DHCP更新、不同AP相容性。
4. GUI與Wi-Fi同時高流量、長時間heap／stack餘裕、供電穩定。
5. radio故障／SDIO錯誤後自動重新初始化尚未實作；目前初始化失敗保留錯誤，網路管理任務休眠，GUI與UART繼續。

不能因模型測試通過就宣稱已完成實體無線驗收。
