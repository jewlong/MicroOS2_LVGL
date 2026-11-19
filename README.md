# Raspberry Pi Zero W：µC/OS-II + USB 鍵盤 + LVGL + HDMI + Wi-Fi

本版已依序加入 **USB Host、USB HID Boot 鍵盤解析、LVGL 專用 GUI 任務的鍵盤輸入**。對象是第一代 Zero W（BCM2835／ARM1176），使用 Windows 11 + VS Code + arm-none-eabi-gcc。

**已實際編譯及以 ARM 執行模型驗證；尚未在實體 Zero W／USB 鍵盤上測試。** 模型測試不能證明 USB 電氣時序、OTG 轉接線或每款鍵盤相容性。精確範圍見 [VALIDATION.md](VALIDATION.md)。

## 1. 如何接線與開機

1. Zero W 標示 **USB** 的 Micro-USB 插孔 → Micro-USB OTG 轉 USB-A 母座 → 一把標準有線 USB 鍵盤。
2. **PWR IN** 插孔接電源；mini-HDMI 接螢幕。
3. 將 **[sdcard/](sdcard/)** 裡面的檔案全部複製到 microSD 的 FAT32 根目錄，再安全退出、插卡上電。

不能放成 R:\sdcard\kernel.img，應為 R:\kernel.img。FAT32／MBR 與 UART 接線詳見 [WINDOWS_SD.md](WINDOWS_SD.md)、[WIFI.md](WIFI.md)。

本版的鍵盤不需要 SSID／密碼。預編譯 Wi-Fi 功能仍是無帳密掃描模式；需要加入網路時，依 [WIFI.md](WIFI.md) 在本機設定並重建。

## 2. 鍵盤操作

| 輸入 | 畫面行為 |
| --- | --- |
| Tab、右、下 | 焦點移到下一個按鈕 |
| Shift+Tab、左、上 | 焦點移到上一個按鈕 |
| Enter | 按下並放開後啟動有焦點的按鈕 |
| +、-、R | 增加、減少、重設數值；也保留 UART 的同樣操作 |
| 英文字母、數字、符號 | 顯示於 USB text 示範文字列 |
| Backspace | 刪除示範文字列最後一個字元 |
| Shift、Caps Lock | US 英文鍵盤大小寫／符號轉換，更新 Caps Lock LED |
| Num Lock | 切換數字鍵盤數字輸入，更新 Num Lock LED |

文字列最多63字元，超過時移除最前面的字元。它是輸入驗證用標籤，沒有中文輸入法或完整文字編輯器。一般字元每次按下產生一次；焦點導覽的長按重複由 LVGL 處理。Ctrl／Alt／GUI 修飾鍵會辨識，但本版沒有對應應用快捷鍵，不把組合鍵當作文字輸入。

拔下鍵盤會取消目前按住狀態；**按住 Enter 拔除不會誤觸按鈕**。重新插入會重新列舉並恢復輸入。事件佇列滿時會丟棄未處理事件、取消按住狀態並累計 drop，避免留下卡住的按鍵；釋放後再按即可恢復。

## 3. 支援範圍

- 直接連在 Zero W root port 的 **Low Speed 或 Full Speed** 鍵盤。
- HID class=3、subclass=1、protocol=1 的 Boot Keyboard；成功 SET_PROTOCOL(0) 後解析標準8-byte報告。
- 掃描第一個 USB configuration，選第一個符合條件的 alternate-setting 0 鍵盤 interface；複合裝置可包含其他不使用的 interface。
- 最多六個同時按下的普通鍵，加 modifier；重複 report 不重複產生按下事件。
- Control endpoint max packet 8/16/32/64；鍵盤 Interrupt IN max packet 8–64 bytes，低速限8 bytes；configuration總長最多1024 bytes。

**尚未支援：** USB Hub／內建 Hub 的鍵盤、High Speed 裝置、滑鼠、藍牙、需要專用驅動的裝置、非 Boot/NKRO report 格式、任意 HID report descriptor 解析、多把鍵盤。2.4 GHz接收器只有在它直接提供上述 Boot Keyboard interface 時才可能適用，尚未做實機驗證。

## 4. Windows 重建

解壓到較短路徑，例如 C:\pizero-usb。LVGL 多層相對標頭引用在過深路徑會超過 Windows 工具鏈限制。工具安裝見 [WINDOWS_SD.md](WINDOWS_SD.md)。

在專案根目錄執行：

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-and-stage.ps1
Copy-Item -Path .\sdcard\* -Destination R:\ -Force
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sdcard.ps1 -Root R:\
~~~

R: 請替換為實際 SD 卡代號。build-and-stage 會編譯、檢查 ELF 並更新 SD 檔案校驗表。原本 Wi-Fi 版本保留在另一個資料夾，本版包含它的完整來源與功能。

## 5. 實機驗證與診斷

UART 使用115200／8N1。成功時預期依序看到：

~~~text
[USB] host power on.
[USB] waiting for directly attached boot keyboard.
[USB] HID boot keyboard VID=.... PID=.... low-speed EP=1 ...
[KEY] usage=.. mods=.. key=..
~~~

以上是預期輸出，並非本次實機紀錄。先測字母、Shift、Caps Lock與LED，再測Tab／Enter，最後測拔除與重插。按UART的s可查看usb、keys、key_drop計數。

| usb狀態 | 意義 |
| --- | --- |
| 1 | 控制器初始化中 |
| 2 | 等待鍵盤；傳輸故障後也會等待拔除再插入 |
| 3 | 讀取描述符與設定鍵盤 |
| 4 | 鍵盤已可收報告 |
| 254 | 列舉失敗，需拔除再插入 |
| 255 | 電源／控制器不可用，USB任務休眠，其他任務繼續 |

USB錯誤碼：1=NAK、2=STALL、3=移除、4=逾時、5=控制器／傳輸錯誤、6=描述符不合法、7=裝置不在支援範圍。初始化錯誤與持續傳輸錯誤均保留UART紀錄。通道無法停止時會隔離DMA緩衝區並停用USB，需重新上電；不會讓CPU繼續改寫仍可能由DMA使用的記憶體。

若沒有任何鍵盤輸入，先檢查是否接在USB插孔、是否真為OTG轉接線、是否直接接一把Boot鍵盤；請保留從上電開始的UART紀錄及VID/PID。

## 6. 程式結構

| 檔案 | 工作 |
| --- | --- |
| [usb/dwc2.c](usb/dwc2.c) | USB電源、DWC2初始化、root port reset、單packet DMA、control及interrupt IN傳輸 |
| [usb/descriptors.c](usb/descriptors.c) | 有長度邊界檢查的configuration/interface/endpoint選擇 |
| [usb/hid_keyboard.c](usb/hid_keyboard.c) | 按下／放開、Shift/Caps/Num、US鍵碼、rollover及重複報告處理 |
| [usb/key_queue.c](usb/key_queue.c) | 64筆按值複製事件佇列、短IRQ臨界區、溢位取消 |
| [usb/usb_keyboard.c](usb/usb_keyboard.c) | µC/OS-II USB任務、列舉、SET_PROTOCOL、SET_IDLE、LED與熱插拔 |
| [gui.c](gui.c) | GUI任務內的LVGL keypad、group焦點、文字列與操作 |

USB任務優先權13、stack16 KiB；GUI優先權15。USB任務不呼叫LVGL，GUI任務不操作USB硬體。USB電源mailbox在OS啟動前完成，避免與Wi-Fi初始化的mailbox互搶。

USB Host使用控制器內部DMA和固定128-byte bounce buffer（64-byte對齊），每次最多一個64-byte packet；通道完成狀態由任務輪詢，未接入CPU USB IRQ。等待會讓出CPU，100 Hz tick與其他任務繼續工作。實際輸入延遲包含tick粒度、USB傳輸和畫面刷新，未測量實機效能。

此DMA路徑依賴目前的cold boot、ARM MMU／D-cache關閉及VideoCore預設L2設定。SDIO仍沿用原本PIO實作。Wi-Fi整合說明見 [WIFI.md](WIFI.md)，版本與授權見 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 解析度設定

核心開機時向 GPU 查詢實際解析度，不再固定 640×480。修改 `config.txt` 的 `hdmi_group` / `hdmi_mode`，並讓 `framebuffer_width` / `framebuffer_height` 與之相同即可，不需重新編譯。支援 640×480 至 1920×1200；例如 800×600 為 `hdmi_mode=9`、1024×768 為 `hdmi_mode=16`（`hdmi_group=2`）。查詢失敗或超出範圍時退回 640×480。GUI 會依寬度自動調整版面，高度固定從上方排列。

## ViewGP HMI 畫面與動作流程匯出/匯入

支援從 `..\InfoSys_ViewGP_Web` 網頁編輯器匯出的 `project.json`：
- 將 `project.json` 置於 MicroSD 卡根目錄，`config.txt` 透過 `initramfs project.json,wifi.txt 0x02000000` 載入。
- 支援文字、矩形、圓形、按鈕、開關、指示燈、數值顯示/輸入、進度條、儀表盤、折線趨勢圖、表格、多狀態燈與彈出式對話盒。
- 支援變數操作（切換、數值設定、增減、重設）與畫面切換（切換畫面、返回上一頁、開啟/關閉 Popup 對話盒）。
- 自動偵測實體解析度並等比例縮放居中，內建 Source Han Sans CJK 中文字型渲染。詳細說明請參閱 [VIEWGP.md](VIEWGP.md)。
