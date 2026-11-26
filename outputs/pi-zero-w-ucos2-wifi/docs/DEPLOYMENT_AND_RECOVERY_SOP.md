# Raspberry Pi Zero HMI 現場部署、備份與回復作業程序 (SOP)

本文件規範現場人員準備 MicroSD 卡、設定網路與安全參數、更新 HMI 專案，以及發生異常時快速診斷與回復的標準流程。

---

## 1. MicroSD 卡初次部署

### 1.1 準備檔案
1. 準備一張 **FAT32** 格式的 MicroSD 卡（建議 8 GB–32 GB，Class 10）。
2. 於開發機執行建置與打包腳本：
   ```powershell
   .\tools\build-and-stage.ps1
   ```
   此腳本會自動驗證符號表、將所有開機檔案整理至 `sdcard/` 目錄，並產生 `SHA256SUMS.txt` 校驗檔。
3. 將 `sdcard/` 內的檔案完整複製至 MicroSD 卡根目錄，確認包含：
   - `bootcode.bin`、`start.elf`、`fixup.dat`（Raspberry Pi GPU 韌體）
   - `config.txt`、`cmdline.txt`
   - `kernel.img`（HMI 執行核心）
   - `project.json`（HMI 專案檔）
   - `wifi.txt`（從 `wifi.txt.example` 複製並修改）

### 1.2 設定 `wifi.txt`
請以純文字編輯器建立或修改 MicroSD 卡根目錄的 `wifi.txt`（每行 `key=value`，`#` 為註解）：

```ini
ssid=Factory_HMI_WiFi
password=YourStrongPassword
country=TW

# 時區與 SNTP 對時（選填）
tz_offset=8
ntp_server=pool.ntp.org

# 遠端維運 HTTP 8080 安全設定（強烈建議現場設定 ops_token）
ops_token=ChangeMe_32CharRandomSecret
# 若現場完全不需要遠端熱重載與狀態 API，可改為 0 直接關閉 Port 8080
ops_http=1
```

> [!IMPORTANT]
> **安全提醒**：Port 8080 目前為純 HTTP（無 TLS）。現場部署時請務必：
> 1. 將 Pi Zero 置於**獨立工控內網 / VLAN**，切勿直接暴露至網際網路。
> 2. 務必設定 `ops_token=`，未設定時僅限封閉測試環境使用。
> 3. 若無遠端更新需求，請設定 `ops_http=0` 關閉 HTTP 伺服器。

---

## 2. 標準專案更新流程（推薦兩階段法）

為避免錯誤設定導致現場停機，建議採用「**先熱重載試跑 → 確認無誤再寫入 SD 卡**」流程：

1. **設計器匯出前檢查**：
   - 在 `InfoSys_ViewGP_Web` 點選 **「專案 → 部署至 Pi Zero」**。
   - 確認「阻擋錯誤 (Errors)」為 **0**；檢視「警告 (Warnings)」確認無非預期的截斷或樣式差異。
2. **階段一：Wi-Fi 原子熱重載試跑（不改動 SD 卡）**：
   - 輸入 Pi Zero 的 IP（如 `192.168.1.50`）與 `ops_token`，點選「**立即推送熱重載**」。
   - Pi Zero 會先於隔離暫存區 (`g_proj_staging`) 解析與驗證；**若失敗會自動放棄新檔並保留原運作畫面**，不影響現場監控。
   - 推送成功後，現場確認所有畫面、Modbus 數值讀寫與警報運作正常。
3. **階段二：固化至 MicroSD 卡（斷電重開永久生效）**：
   - 點選「**下載 project.json (SD 卡部署)**」。
   - 正常斷電取出 MicroSD 卡，先將原 `project.json` 備份為 `project.json.bak`（或加上日期如 `project.20261119.json.bak`）。
   - 將新的 `project.json` 複製到根目錄，重新插卡開機並確認畫面正常。

---

## 3. 備份與回復程序

### 3.1 定期備份建議
每次現場更新前，請於維護筆電或 MicroSD 卡內保留前一版穩定檔案：
- `project.json.bak`（上一版已驗證的 HMI 專案）
- `kernel.img.bak`（上一版已驗證的核心檔）
- `wifi.txt`（網路與安全設定）

### 3.2 異常回復方式
- **情境 A：Wi-Fi 熱重載失敗或畫面異常**
  - 若 JSON 格式錯誤或超限，系統已自動原子回滾，原專案不受影響。
  - 若新畫面邏輯有誤但已載入，只需**重新上電（斷電重開）**，系統即會自動載回 MicroSD 卡內原本的穩定版 `project.json`。
- **情境 B：更新 MicroSD 卡後無法載入專案或畫面報錯**
  - 斷電取出 MicroSD 卡，刪除有問題的 `project.json`，將 `project.json.bak` 改名為 `project.json`（若為核心更新問題則回復 `kernel.img.bak`），插回重開即可。
- **情境 C：完全移除 `project.json` 進行硬體自檢**
  - 若需排除專案問題，暫時將 `project.json` 移出根目錄後開機，核心會自動退回內建系統狀態畫面，可用於驗證 HDMI、Wi-Fi 與 USB 鍵盤是否正常。

---

## 4. 現場快速除錯指南

| 異常現象 | 檢查方式 | 常見原因與排除步驟 |
|---|---|---|
| 開機顯示「專案載入失敗」紅色錯誤框 | 直接閱讀畫面上的錯誤原因與元件 ID，或接 UART 115200 bps | `schemaVersion` 不為 1、JSON 語法損毀、或檔案超過 8 MiB。請回到設計器透過「部署至 Pi Zero」重新驗證匯出。 |
| 畫面正常但數值顯示 `---` 或紅框 | 按鍵盤 `F12` 開啟本機診斷頁，或瀏覽器查詢 `http://<IP>:8080/api/status` | Modbus 設備斷線、IP/Port/Unit ID 錯誤或逾時。檢查網路連線與 PLC 狀態；恢復連線後數值會自動解除紅框。 |
| 按鈕寫入後外框閃紅 | 檢查變數 `access` 權限與 PLC 暫存器是否可寫入 | Pi Zero 寫入後會強制回讀確認，失敗時不自動重試。請確認 Modbus 位址、型別與 `readWrite` 設定。 |
| Wi-Fi 無法推送（回傳 `401` / `403`） | 確認 `wifi.txt` 中的 `ops_token` 與 `ops_http` | `401` 表示 `X-Ops-Token` 不符；無法連線則確認 `ops_http` 未被設為 `0` 且防火牆允許 TCP 8080。 |
| 畫面部分文字被截斷或樣式略異 | 按 `F12` 檢查 `Parse warnings` 數量 | 超過欄位字節上限（文字 127 B / 警報 95 B）或使用了不支援外觀屬性（如斜體、WebP）。請參考設計器警告修正。 |
