# ViewGP HMI 畫面與動作流程匯入指南 (Raspberry Pi Zero W)

本專案支援將 `InfoSys_ViewGP_Web` 編輯器規劃的 HMI 專案匯出為 `project.json`，直接載入到 Raspberry Pi Zero W 的裸機 µC/OS-II + LVGL 系統中執行，無需重新編譯程式！

---

## 1. 運作架構

1. **檔案匯出**：在 `InfoSys_ViewGP_Web` 網頁編輯器中完成畫面、元件與變數動作流程設定，點擊「匯出專案」儲存為 `project.json`。
2. **免驅動載入**：將 `project.json` 複製到 MicroSD 卡根目錄（與 `kernel.img`、`config.txt` 同一層）。
3. **GPU 韌體預載**：`config.txt` 中的 `initramfs project.json,wifi.txt 0x02000000` 會在開機前由 VideoCore GPU 韌體將檔案自動載入到 RAM `0x02000000`。核心透過 ATAGS 取得位置直接解析，不需要 SD/FAT 驅動。
4. **自動縮放與置中**：
   - 核心自動比對 `config.txt` 設定的實體解析度（如 640×480、800×600、1024×768、1920×1080）與專案畫面解析度（如 960×600）。
   - 採用**等比例縮放並置中**，元件座標、大小與文字大小自動等比計算。
5. **中文字型渲染**：
   - 內建 LVGL Source Han Sans 14/16 CJK 中文字型，元件中的繁簡中文標籤皆可正常渲染。

---

## 2. 支援元件與動作 (Widgets & Actions)

| 類別 | 元件 Kind | 對應 LVGL 9 元件與功能 |
|---|---|---|
| **繪圖/形狀** | `rectangle`, `circle`, `line` | 圓角矩形、圓形、格線、背景面板 |
| **文字與數值** | `text`, `number`, `datetime` | 中英文字串標題、即時數值格式化、系統時間/日期 |
| **互動開關/按鈕** | `button`, `switch`, `input` | 動作按鈕、狀態開關、數值按鈕輸入（支援 USB 鍵盤 Tab/Enter 切換） |
| **狀態指示** | `indicator`, `multistate` | 運轉指示燈（綠/灰）、多狀態指示燈（依數值變換文字與顏色） |
| **儀表/進度/圖表** | `bar`, `gauge`, `trend` | 進度條、圓弧儀表盤、歷史即時折線趨勢圖 |
| **表格/警報** | `table`, `alarm` | 多欄位資料表、警報事件清單 |
| **對話盒/快顯** | `popup`, `dialog` | 彈出式半透明遮罩對話盒（如參數設定面板） |

### 支援動作 Action：
- `toggle`：切換布林變數（true ↔ false）。
- `set`：寫入指定數值/文字到變數。
- `increment`：數值變數遞增（可設定負值遞減）。
- `reset`：重設變數為初始值。
- `screen`：切換至指定畫面（自動記錄瀏覽歷史）。
- `previousScreen`：返回上一畫面。
- `popup` / `openDialog`：彈出對話盒畫面。
- `closePopup` / `closeDialog`：關閉當前對話盒。
- `conditionId`（顯示條件）與 `enabledId`（啟用條件）：依綁定變數動態顯示/隱藏或啟用/停用元件。

---

## 3. 使用步驟

1. **準備檔案**：
   - 將專案根目錄下的 `project.json.example`（取自 `InfoSys_ViewGP_Web` 範例）複製或將你自己編輯好的專案儲存為 `project.json`。
   - 放入 MicroSD 卡根目錄。
2. **開機確認**：
   - 插入 Pi Zero W 開機。
   - UART（115200 bps）會輸出：
     ```text
     [GUI] ViewGP Project running: 泵浦教學
     ```
   - 若 SD 卡沒有 `project.json`，系統會自動退回內建的即時系統監控儀表板。

