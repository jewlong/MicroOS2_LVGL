# Raspberry Pi Zero W：µC/OS-II + HDMI + LVGL + Wi-Fi

本版加入第一代 **Zero W（BCM2835 / ARM1176JZF-S）內建 Wi-Fi** 的 SDIO 主機驅動、Infineon WHD、lwIP，以及 µC/OS-II 適配。保留 HDMI 640×480 與 LVGL 專用 GUI 任務。

**已實際編譯並通過 ARM 執行模型測試；尚未在實體 Zero W 驗證 Wi-Fi 晶片啟動、WPA2 連線或 HDMI 訊號。這是供板上 bring-up 的整合版本，不能把「編譯通過」當成「已經連上你的基地台」。** 精確測試範圍見 [VALIDATION.md](VALIDATION.md)。

## 1. 開機資料夾與預設行為

開機檔案在 **[sdcard/](sdcard/)**。把裡面全部檔案放到 microSD 的 FAT32 **根目錄**：

~~~text
R:\
  bootcode.bin
  start.elf
  fixup.dat
  LICENCE.broadcom
  config.txt
  kernel.img
  SHA256SUMS.txt
~~~

不能放成 R:\sdcard\kernel.img。kernel.img 是裸機程式，不是整張磁碟映像。新卡的 MBR/FAT32 製作見 [WINDOWS_SD.md](WINDOWS_SD.md)。已有能開機的 FAT32 卡不必重新格式化。

預編譯 kernel.img **沒有 SSID 或密碼**。成功啟動 radio 後會掃描一次 2.4 GHz 基地台並在 UART 列出結果，然後等待；不會加入網路。韌體與板級 NVRAM 已嵌入 kernel.img，沒有執行期 SD 檔案系統，也不會讀 Linux 的 wpa_supplicant.conf。

本專案不適用 Zero 2 W。Wi-Fi SDIO 使用板上 GPIO34–39，GPIO41 控制 WL_ON，GPIO43 提供 32.768 kHz 時鐘；不需外接 Wi-Fi 模組或接線。

## 2. 在 Windows 設定自己的網路

先依 [WINDOWS_SD.md](WINDOWS_SD.md) 安裝 arm-none-eabi-gcc 與原生 mingw32-make。已驗證 GCC 14.3.1 / Arm GNU 14.3.Rel1、GNU Make 4.4.1。

在 VS Code 開啟本專案資料夾，於 PowerShell 執行：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\configure-wifi.ps1 -Ssid "你的基地台名稱" -Country TW
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-and-stage.ps1
~~~

密碼由本機遮罩提示輸入，不用貼到聊天。支援 **2.4 GHz、WPA2-Personal / AES、8–63 字元 ASCII passphrase**；SSID 以 UTF-8 儲存，最多 32 bytes。Country 請選實際使用地區；TW 是台灣。這版不包含 WPA3-only、企業 802.1X、網頁登入或 5 GHz。

設定寫入 net/wifi_local.h；該檔及建置產物含帳密，不要發布或分享。密碼在韌體內並未加密。恢復掃描模式：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\configure-wifi.ps1 -ScanOnly -Country TW
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-and-stage.ps1
~~~

build-and-stage 會建置、檢查 ELF、更新 sdcard/kernel.img 與校驗表。只有工具顯示成功後才複製到卡：

~~~powershell
Copy-Item -Path .\sdcard\* -Destination R:\ -Force
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sdcard.ps1 -Root R:\
~~~

把 R: 換成真正的 SD 卡代號，安全退出後，在 Zero W 斷電時插卡再上電。**Wi-Fi 用的是本專案程式；卡上不需要 Raspberry Pi OS。**

## 3. 實體板驗證順序

接 mini-HDMI（Type C）到螢幕，先開螢幕再供電。UART：115200、8N1、無 flow control；使用 3.3V USB-TTL，GPIO14／實體腳8 接 RXD，GPIO15／腳10 接 TXD，腳6 接 GND，不接轉接器 VCC。

依序觀察 UART：

1. `[PASS] HIGH woke by IRQ while LOW was busy`：搶占排程自測。
2. `[NET] lwIP 2.2.1 core running`：TCP/IP 任務建立完成。
3. `[SDIO] selected card RCA=...`：SDIO 探測完成。
4. `[Wi-Fi] radio ready, MAC ...`：WHD 韌體啟動與 MAC 查詢完成。
5. `[SCAN]`、`[AP]`：掃描結果；SSID 非 ASCII 位元組會以 ? 顯示，連線仍使用原始 UTF-8。
6. 設定帳密後才會出現 `[JOIN] associated`、`[IP] 192.168....`。

以上是**預期里程碑，不是本次已觀察到的實機紀錄**。HDMI 會顯示目前 Wi-Fi 狀態／IP；UART 的 +、-、r 控制 GUI，s 顯示排程與網路計數。

電腦連同一個 LAN，先確認基地台沒有 client isolation，再使用 UART 所列出的 IP：

~~~powershell
ping 192.168.1.123
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test-network.ps1 -Address 192.168.1.123
~~~

測試工具會驗證 UDP 5000 與 TCP 5001 完整回傳內容。這兩個服務是區網連線驗證用 echo server，沒有登入或 TLS。

關閉基地台再重開，應看到 LINK lost、重新 JOIN、重新 DHCP，GUI 與任務持續運作。若只是加入失敗，重試間隔為 5、10、15…30 秒；若是 SDIO／韌體初始化失敗，本版保留錯誤並停止網路初始化，UART 與 GUI 繼續，需排除原因後重新上電。

| 停在哪裡 | 需要檢查 |
| --- | --- |
| 完全沒有 UART | 供電、FAT32 根目錄、boot firmware、TTL 接線、是否真為第一代 Zero W |
| SDIO initialization failed | mailbox clock/serial 回覆、WL_ON、32.768 kHz、GPIO mux、EMMC reset/CMD5 |
| radio firmware failed | WHD 原始錯誤碼、晶片 ID、firmware/NVRAM 相容性、HT clock、F2 ready |
| scan 沒結果 | 2.4 GHz、國家碼、訊號強度、WHD 錯誤碼 |
| JOIN failed | SSID bytes、WPA2-AES、密碼、基地台安全模式 |
| associated 無 IP | DHCP 伺服器、收包通知、ARP/DHCP 封包、rx/tx 是否增加 |
| 有 IP 但 echo 失敗 | IP 是否最新、同一 LAN、AP 隔離、防火牆、服務埠 |

若失敗，保留**從上電到失敗的完整 UART 紀錄**。避免只截最後一行；初始化錯誤碼與 SDIO 命令階段最有幫助。

## 4. 四個整合層的分工

| 層 | 本版實作 |
| --- | --- |
| SDIO／Wi-Fi | BCM2835 EMMC、CMD0/5/3/7/52/53、25 MHz 四線 PIO、GPIO 電源時鐘、WHD SDPCM、43438A1 firmware／CLM、Zero W 板級 NVRAM |
| 連線管理 | 一次掃描、WPA2-AES PSK 加入、連線狀態輪詢、退避重連；握手交由 WHD 與其 radio firmware |
| TCP/IP | lwIP 2.2.1、IPv4、ARP、DHCP、ICMP、DNS 模組、TCP、UDP；示範使用 raw callback API |
| µC/OS-II | lwIP sys_arch 任務、信號量、訊息佇列、mutex、短臨界區；WHD HAL／RTOS 適配 |

資料路徑：

~~~text
SDIO -> WHD task -> copy to pbuf -> tcpip_input queue -> lwIP task -> UDP/TCP callback
lwIP task -> copy to WHD buffer -> WHD TX queue -> SDIO -> radio
network manager -> status snapshot -> GUI task -> LVGL -> HDMI framebuffer
~~~

lwIP raw API 僅由 TCPIP 任務或其 callback 呼叫；WHD RX 不直接操作協定堆疊。LVGL 仍只有 GUI 任務能使用。沒有跨任務直接修改 LVGL 物件。

優先權數字越小越高：HIGH=5、WHD=8、UART=10、TCPIP=11、NET=12、GUI=15、LOW=20、IDLE=31。Log mutex ceiling=4；lwIP mutex ceiling pool=1、2、3、6、7。WHD/NET stack 各32 KiB，TCPIP16 KiB，GUI32 KiB。µCOS event pool64、queue pool16。

目前 SDIO 用 **100 Hz tick 讀取 card interrupt 狀態**後喚醒 WHD；不是直接接入 EMMC IRQ。無 DMA、無 OOB 中斷、無 host deep sleep。這是先求可診斷的 PIO bring-up 路徑，吞吐量與無線延遲仍待板測。MMU／D-cache 關閉，ARMv6 非對齊單筆存取 U bit 明確開啟，soft-float。

NVRAM 使用 Raspberry Pi 發行來源中 Zero W symlink 指向的檔案，原檔保留不改；執行期移除註解、轉 NUL 分隔與4-byte對齊，取板上序號衍生 locally administered MAC，替換兩個預設 MAC 與國家碼。這不是寫入 OTP，也不修改 SD 上原檔。

## 5. 專案檔案與重現

- [net/network.c](net/network.c)：網路管理、netif、DHCP、TCP/UDP echo。
- [net/sdio.c](net/sdio.c)：BCM2835 SDIO 與板級 GPIO／時鐘。
- [net/sys_arch.c](net/sys_arch.c)、[net/whd_rtos.c](net/whd_rtos.c)：RTOS 適配。
- [net/whd_resources.c](net/whd_resources.c)、[net/whd_buffers.c](net/whd_buffers.c)：韌體資源與封包所有權。
- [wifi-firmware/](wifi-firmware/)：鎖定的 firmware、CLM、NVRAM 與授權。
- [startup.s](startup.s)、[linker.ld](linker.ld)、[Makefile](Makefile)：ARM1176 啟動與建置。
- [tests/](tests/)：CPU、GUI、合成 SDHCI／Ethernet 測試；測試用邊界不會連入 kernel.img。
- [THIRD_PARTY.md](THIRD_PARTY.md)、各 *-lock.json：來源版本、授權與 SHA256。

~~~powershell
mingw32-make -j4 all
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sources.ps1
~~~

測試安裝與命令見 [VALIDATION.md](VALIDATION.md)。未完成項目：實體 radio／WPA2／DHCP 驗收、實際斷線重連、長時間流量／堆疊餘裕、Wi-Fi 韌體故障後自動重設、吞吐量改善、TLS／應用通訊協定。它已超出第一階段 UART boot，但**尚不能宣稱完整硬體 Wi-Fi 移植已驗收完成**。
