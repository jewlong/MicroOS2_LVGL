# Raspberry Pi Zero HMI 實機測試矩陣與支援聲明

本文件明確區分目前**已實機驗證**與**僅完成編譯／靜態／契約測試、待實機最終驗收**的硬體與功能項目，避免現場導入時對支援範圍產生誤解。

狀態定義：
- **✅ 已實機驗證**：已於實體硬體完成上電開機與功能驗證。
- **🧪 待實機驗收**：已完成 ARM 交叉編譯（`-Werror` 0 警告）、連結符號檢查、單元／契約測試，尚需連接實體周邊／PLC 進行最終實機簽核。
- **❌ 不支援**：目前架構明確不支援或屬規劃外項目。

---

## 1. 硬體平台與周邊支援矩陣

| 類別 | 項目 / 型號 | 狀態 | 備註與已知限制 |
|---|---|:---:|---|
| 主機板 | Raspberry Pi Zero W (BCM2835, ARM1176JZF-S, 512 MiB) | ✅ 已實機驗證 | 主要開發與驗證目標，`kernel.img` 預設建置對象 |
| 主機板 | Raspberry Pi Zero 2 W (BCM2710A1) | 🧪 待實機驗收 | 理論相容（需確認 `config.txt` 與 CYW43436s 韌體差異），**目前未實機簽核** |
| 顯示輸出 | HDMI 螢幕 (640×480 / 800×600 / 1024×768 / 1280×720 / 1920×1080) | ✅ 已實機驗證 | 透過 GPU Mailbox Framebuffer 32bpp 輸出，畫面自動等比縮放置中 |
| 輸入裝置 | USB HID Boot Keyboard（直連 OTG） | ✅ 已實機驗證 | `Tab`/`Shift+Tab`/`Enter`/`Esc`/方向鍵/`F12` 診斷頁 |
| 輸入裝置 | USB HID Boot Mouse（直連 / 複合裝置 / 1 層 Hub） | 🧪 待實機驗收 | 驅動與 LVGL Pointer 已整合並通過編譯與符號檢查，待實體滑鼠驗證移動/點擊/滾輪 |
| 輸入裝置 | USB 1 層 Hub（同時接鍵盤＋滑鼠） | 🧪 待實機驗收 | 支援 1 層 Hub 列舉與雙端點輪詢；不支援多層串接 Hub |
| 觸控螢幕 | USB HID Touch / I2C Touch | ❌ 不支援 | 目前僅實作 HID Boot Protocol Keyboard/Mouse |
| 無線網路 | 板載 Wi-Fi CYW43438 (2.4 GHz, WPA2-PSK) | 🧪 待實機驗收 | SDIO/WHC 與 lwIP 2.2.1 整合已完成，待實機環境長時間連線與重連測試 |
| 序列通訊 | PL011 UART0 (GPIO14/15) 除錯輸出 (115200 8N1) | ✅ 已實機驗證 | 開機日誌與錯誤診斷預設輸出通道 |
| 序列通訊 | PL011 UART0 + 外接 RS-485 收發器 (Modbus RTU) | 🧪 待實機驗收 | `modbus_rtu.c` 已實作幀計時與 CRC16，啟用時與除錯輸出共用 UART0（建議搭配外接自動流向控制模組） |

---

## 2. 功能與非功能驗收矩陣

| 測試項目 | 狀態 | 驗收方法與通過標準 |
|---|:---:|---|
| 建置與連結檢查 (`build-and-stage.ps1`, `verify-build.ps1`, `verify-sdcard.ps1`) | ✅ 已通過 | 0 warnings / 0 errors，RAM 靜態佔用 `11.36 MiB / 16 MiB`，`kernel.img` 大小 `~2.01 MiB` |
| 設計器 ↔ 執行端 JSON 契約測試 (`npm test` + `tools/test-contract.ps1`) | ✅ 已通過 | 41/41 單元測試通過；`contract_full.json`（全 22 元件）與 `sdcard/project.json` 雙向驗證通過 |
| CJK 字型渲染 (16/22/36/48px 微軟正黑體) | ✅ 已通過 | 繁體中文標籤、按鈕、大數值與常用單位無亂碼或破圖 |
| 解析錯誤提示與 `F12` 本機診斷頁 | 🧪 待實機驗收 | 刻意放入錯誤 JSON 與 `schemaVersion: 2`，確認畫面顯示錯誤摘要而非當機；按 `F12` 正常開關診斷面板 |
| Modbus TCP 斷線／逾時／重連／寫入失敗 | 🧪 待實機驗收 | 拔除網路或停止 PLC 模擬器：3 秒內元件顯示 `---`/紅框、既有警報凍結不誤觸發；恢復連線後自動復原 |
| Modbus RTU 實體 RS-485 讀寫 | 🧪 待實機驗收 | 連接實體 RS-485 設備，驗證 9600/19200/115200 bps 輪詢與寫入回讀 |
| SNTP 對時與時區偏移 (`tz_offset=8`) | 🧪 待實機驗收 | 連網後 30 秒內 `datetime` 元件與 `F12` 顯示正確台灣時間 |
| HTTP 8080 認證與原子熱重載 | 🧪 待實機驗收 | 未帶 `X-Ops-Token` 回傳 `401`；送出損毀 JSON 回傳 `400` 且既有畫面不中斷 |
| 容量與長時間穩定度壓力測試 (Soak Test) | 🧪 待實機驗收 | 執行下列壓力測試指令連續重載 20 次 + 持續運行 24 小時，確認 `lvgl_mem_used_pct` 穩定不洩漏、無重啟 |

---

## 3. 實機驗收自動化腳本

在 Pi Zero W 連上 Wi-Fi 並取得 IP 後，可於開發機直接執行契約與連續熱重載壓力測試：

```powershell
# 1. 執行靜態契約檢查（不需連線 Pi Zero）
.\tools\test-contract.ps1

# 2. 對實機執行 /api/status 檢查 + 20 次連續原子熱重載壓力測試
.\tools\test-contract.ps1 -PiHost 192.168.1.50 -OpsToken "你的ops_token" -SoakRuns 20
```

通過標準：
1. 每次 `POST /api/project` 皆回傳 HTTP `200 OK` 且 `reloads` 遞增。
2. 測試前後 `/api/status` 的 LVGL 記憶體使用率波動低於 `5%`，無記憶體洩漏。
3. 刻意送出不合法 JSON（例如 `{"schemaVersion":99}`）時回傳 HTTP `400`，且畫面維持正常運作。
