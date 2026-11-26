# Project Handoff — Raspberry Pi Zero W / Zero 2 W (µC/OS-II + LVGL 9.4 + ViewGP HMI + USB + Wi-Fi)

本文件整理了本專案目前的完整架構、已完成功能、關鍵硬體修正紀錄與建置／模擬流程，供專案搬移至新目錄（例如 `D:\Dropbox\Infosystem\MicroOS2LVGL_Proj\outputs\pi-zero-w-ucos2-wifi`）後在新對話中快速銜接開發。

---

## 1. 專案目標與支援平台

同一份 `kernel.img` 與 SD 卡開機檔已同時支援：
- **Raspberry Pi Zero W**（BCM2835 / 單核 ARM1176JZF-S，`PERIPHERAL_BASE = 0x20000000`，`DMA_BUS_ALIAS = 0x40000000`）
- **Raspberry Pi Zero 2 W**（RP3A0 / BCM2837 / 四核 Cortex-A53 32-bit AArch32 模式，`PERIPHERAL_BASE = 0x3F000000`，`DMA_BUS_ALIAS = 0xC0000000`）

核心元件：
- **RTOS**：µC/OS-II V2.93.01（100 Hz 搶佔式排程，ARM SYS/SVC/IRQ 模式切換）
- **GUI**：LVGL 9.4.0（32-bit XRGB8888 軟體渲染 + 完整繁體中文字庫）
- **HMI Runtime**：ViewGP JSON 動態載入引擎（相容 `InfoSys_ViewGP_Web` 匯出之 `project.json`）
- **USB Host**：Synopsys DWC2 Internal DMA 驅動 + HID Boot Keyboard（支援直連 OTG 與一階 USB Hub）
- **Wi-Fi / TCP-IP**：CYW4343x SDIO (WHD) + lwIP 堆疊，從 SD 卡 `wifi.txt` 讀取連線設定

---

## 2. 已完成的主要功能與關鍵修改

### A. Pi Zero W & Pi Zero 2 W 雙平台自動偵測與相容
- `startup.s`：
  - 讀取 CP15 `MIDR` 區分 ARM1176 (`0xB76`) 與 Cortex-A53 (`0xD03`)。
  - 在 Cortex-A53 上將 Core 1~3 導入 `wfi` 休眠；若 Core 0 處於 HYP 模式 (`0x1A`)，自動透過 `ELR_hyp` + `eret` 降權至 SVC 模式 (`0x13`)。
  - 啟用 `SCTLR.CP15BEN` (bit 5)、L1 Instruction Cache (bit 12) 與 Branch Prediction (bit 11)，同時維持 D-Cache 關閉以保證 DMA 一致性。
- `platform.h` / `bsp.c`：
  - `soc_detect_init()` 自動設定 `soc_peripheral_base`、`soc_dma_bus_alias`、`soc_is_bcm2837`。
  - 在 Pi Zero 2 W 上建立 1:1 實體位址對映表 (`mmu_l1_table_raw` 對齊至 16KB)，將 RAM (`0x000..0x3EF` MB) 設為 **Normal Non-Cacheable (`TEX=001, C=0, B=0`)**，解決 Cortex-A53 在 MMU 關閉時非對齊存取引發 Data Abort 的問題，同時維持 DMA 免清快取。
  - 在 Pi Zero 2 W 上寫入 `0x4000000C = 0` 將 GPU 中斷（含 System Timer Match 1）路由至 Core 0 IRQ。
- `config.txt` / `sdcard/config.txt`：
  - 加入 `arm_64bit=0`，確保 Zero 2 W 以 32-bit AArch32 啟動 `kernel.img`。

### B. 動態 HDMI 解析度查詢（免重新編譯）
- `framebuffer.h` / `framebuffer.c`：
  - 開機時透過 GPU Mailbox (`0x40003` / `0x40004`) 查詢實際解析度（支援 `640×480` 至 `1920×1200`）。
  - 直接修改 SD 卡根目錄 `config.txt` 的 `hdmi_group`、`hdmi_mode`、`framebuffer_width`、`framebuffer_height` 即可生效，不需重編 `kernel.img`。

### C. SD 卡設定檔載入（`wifi.txt` + `project.json`）
- `config.txt` 使用 `initramfs wifi.txt,project.json 0x02000000`。
- `bootcfg.h` / `bootcfg.c`：
  - 不分檔案順序解析 `wifi.txt`（`ssid=`、`password=`、`country=`）與 `project.json`。
  - 若 Pi Zero 2 W 韌體未透過 `0x100` 傳遞 `ATAG_INITRD2`，會自動檢查 `0x02000000` 讀取設定。
- `net/network.c`：
  - 支援 WPA2 AES / Mixed 自動回退與即時連線狀態回報至畫面底部狀態列。

### D. ViewGP HMI JSON 執行引擎與完整繁體中文字庫
- `vgp_model.h` / `vgp_parser.h` / `vgp_parser.c`：無動態配置（Freestanding `-nostdlib`）的 JSON 解析器，解析 `screens`、`elements`、`variables`、`parentScreenId`、`multistateItems` 等。
- `vgp_runtime.h` / `vgp_runtime.c`：
  - 支援元件：`rectangle`、`circle`、`line`、`text`、`number`、`button`、`indicator`、`switch`、`bar`、`gauge`、`trend`、`alarm`、`table`、`datetime`、`input`、`multistate`、`scale`。
  - 支援動作：`toggle`、`set`、`increment`、`reset`、`screen`、`previousScreen`、`popup`/`openDialog`、`closePopup`/`closeDialog`。
  - 內建鍵盤焦點外框（黃藍雙層邊框）、`rebuild_kbd_group()` 畫面／彈窗切換群組管理、`vgp_runtime_handle_key()`（支援 `Esc` 關彈窗、數字鍵／`+`／`-`／`Backspace` 編輯 `input` 欄位）。
  - 底部狀態列即時顯示：`[ZeroW/Zero2W] [USB:...] [CFG: ...] Wi-Fi ... | RX:... TX:...`。
  - 透過 `set_label_if_changed()` 與狀態快取避免重複無效重繪。
- `tools/generate-cjk-font.ps1` → `vgp_font_cjk.h` / `vgp_font_cjk.c`：
  - 純 ASCII PowerShell + C# 腳本，利用 Windows 內建 **微軟正黑體 (`Microsoft JhengHei`)** 產生 LVGL 9 原生字庫：
    - `vgp_font_zh_16` (16px, 2bpp)：包含 **Big5 第一字面全部 5,401 個常用繁體中文字** + 工業符號（`°`、`℃`、`／`、`–`、`｜` 等）+ `sdcard/project.json` 全部字元，並以 `lv_font_source_han_sans_sc_16_cjk` 為 fallback。
    - `vgp_font_zh_22` (22px, 4bpp)：常用字與 `project.json` 字元，供大標題／數值使用，以 `vgp_font_zh_16` 為 fallback。

### E. USB 鍵盤驅動實機修正（已在 Pi Zero 2 W 實機驗證成功）
- `usb/dwc2.c`：
  - Core Soft Reset 後重新寫入 `ForceHstMode (1<<29)`，等待時間拉長至 100ms + 250ms。
  - **關鍵實機修正**：DWC2 Internal DMA 在 `OUT` / `SETUP` 傳輸完成 (`XferCompl = 1`) 時，`HCTSIZ.XferSize` 不會遞減為 0；修正為 `in == false` 時直接回報 `*actual = length`。
  - 區分 `type == 0`（Control 讓硬體自動重試 NAK）與 `type == 3`（Interrupt IN 遇 NAK 立即停止通道），並加入微秒級快速輪詢（`wait_reg` 與 `packet`）降低按鍵延遲。
  - `HCDMA` 使用動態 `DMA_BUS_ALIAS`（Zero W 為 `0x40000000`，Zero 2 W 為 `0xC0000000`）。
- `usb/usb_keyboard.c` / `usb/descriptors.c`：
  - 支援直接 OTG 連接與一階 USB Hub (`enumerate_via_hub`)。
  - 容忍 `SET_PROTOCOL(BOOT)` 回傳 `USB_STALL`，放寬 `bInterfaceSubClass == 0` 鍵盤檢查。
  - 列舉失敗自動重試；鍵盤輪詢固定為 `OSTimeDly(1)` (10ms / 100Hz)。
- `lv_conf.h` / `gui.c`：
  - `LV_DEF_REFR_PERIOD` 降為 `10ms`，`gui_task` 最大休眠降至 `10ms`，並加上 `keyboard_label != NULL` 防護避免 ViewGP 模式空指標當機。

### F. Windows 互動式 ARM1176 模擬器
- `tests/sim_window.py` + `tools/run-simulator.ps1`：
  - 使用 Unicorn Engine 直接執行編譯出的 ARM `build/kernel.elf`，自動讀取 `sdcard/config.txt` 解析度、`sdcard/project.json` 與 `sdcard/wifi.txt`。
  - 開機 50ms 搶佔自我測試通過後自動移除 `UC_HOOK_BLOCK` 切換至純 C JIT 高速模式。
  - 開啟 Tkinter 視窗即時顯示 HDMI 畫面，並將電腦鍵盤按鍵轉成 USB HID 封包送入模擬器。

---

## 3. 常用建置與測試指令

### 編譯並產出 SD 卡檔案
```powershell
.\tools\build-and-stage.ps1
```
成功後將 `sdcard\` 資料夾內的檔案複製到 FAT32 MicroSD 卡根目錄即可開機。

### 啟動 Windows 互動式模擬器
```powershell
.\tools\run-simulator.ps1
# 或指定解析度：
.\tools\run-simulator.ps1 --width 800 --height 600
```

---

## 4. 搬移專案目錄時的注意事項

1. **ARM 工具鏈路徑**：
   `tools/build-and-stage.ps1` 預設會往上兩層尋找 `..\..\work\tools\arm\bin`。請確認新目錄的相對位置也有 `work\tools\arm\bin`，或將該路徑加入 Windows 系統 `PATH`。
2. **清理舊的 `.venv` 與 `build\`**：
   搬到新目錄後，請先刪除舊的 `.venv` 與 `build\` 資料夾，再執行 `.\tools\build-and-stage.ps1` 與 `.\tools\run-simulator.ps1` 重新產生，避免舊絕對路徑殘留。
3. **編譯規範**：
   本專案採嚴格 freestanding C11 編譯（`-nostdlib -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -DLV_DISABLE_API_MAPPING`），修改 C 檔時需注意不可引入 libc syscall（如 `strtod`、`_sbrk`）或已停用的 LVGL 舊巨集（如 `lv_obj_move_foreground`，應改用 `lv_obj_move_to_index(obj, -1)`）。
