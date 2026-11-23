# ViewGP HMI 系統規格與功能支援矩陣（Raspberry Pi Zero W / Zero 2 W）

本文件為 `InfoSys_ViewGP_Web` 設計器與 Raspberry Pi Zero 裸機執行器（µC/OS-II V2.93.01 + LVGL 9.4.0 + lwIP 2.2.1）之間的**正式契約與功能支援對照表**。所有支援項目皆對應至 `schemaVersion: 1` JSON 模型與執行端 `vgp_model.h` / `vgp_parser.c` / `vgp_runtime.c`。

---

## 1. 系統架構與匯出契約

1. **設計器預檢與一鍵部署**：在 `InfoSys_ViewGP_Web` 中透過「專案 → 部署至 Pi Zero」進行匯出前驗證（容量、欄位長度、圖片格式、不支援屬性），並可直接下載已命名為 `project.json`（且自動扁平化畫面繼承）的部署檔，或透過 Wi-Fi 推送至 Pi Zero 進行熱重載。
2. **開機載入機制**：`config.txt` 透過 GPU 韌體預載 `project.json` 與 `wifi.txt` 至 RAM，核心不需 FAT 檔案系統驅動即可解析。
3. **版本相容與錯誤回報**：執行端嚴格檢查 `schemaVersion == 1`。遇到未知欄位、未知元件型別或超限項目時，不再靜默忽略，而是記錄於解析診斷報告，並可於開機畫面、`F12` 本機診斷頁與 `/api/status` 查看。
4. **自動縮放與中文字型**：自動依 HDMI 解析度與專案畫面解析度等比例縮放並置中；內建 **Noto Sans TC（思源黑體系，SIL OFL 1.1 授權）**點陣字型（16px 2bpp 繁中全字集 + 22px 4bpp 常用字集 + 36px / 48px 4bpp 大數值與常用單位字集，自動退回鏈 `48 → 36 → 22 → 16 → LVGL 內建思源黑體`）。

---

## 2. 容量上限對照表（`PI_ZERO_LIMITS`）

| 項目 | 上限 | 超限行為（設計器 / Pi Zero） |
|---|---:|---|
| `schemaVersion` | 必須為 `1` | 設計器阻擋 / Pi Zero 拒絕載入並顯示錯誤 |
| 畫面數量 (`screens`) | 32 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 變數數量 (`variables`) | 256 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 全專案元件總數 (`elements`) | 768 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| Modbus 通訊設備 (`devices`) | 16 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 圖片資產 (`assets`) | 32 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 趨勢圖元件 (`trend`) | 16 | 設計器阻擋 / Pi Zero 僅前 16 個背景取樣 |
| 全域 Word 警報區 (`alarmAreas`) | 32（每區 16 bits） | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 元件級警報規則 (`alarmRules`) | 128 | 設計器阻擋 / Pi Zero 截斷並記錄警告 |
| 警報事件池 (`alarmEvents`) | 128 筆 | 超過時自動淘汰最舊的已復歸/已確認事件 |
| 字串長度（ID / 名稱 / 文字 / 警報訊息） | 47 / 47 / 127 / 95 bytes (UTF-8) | 設計器警告 / Pi Zero 安全截斷 |
| `project.json` 檔案大小 | SD 卡 8 MiB / Wi-Fi OTA 2 MiB | 設計器警告或阻擋 / Pi Zero 拒絕過大 OTA |
| LVGL 動態記憶體 (`LV_MEM_SIZE`) | 4 MiB | 靜態 SRAM 總佔用約 11.36 MiB / 16 MiB |

---

## 3. 元件支援矩陣（全部 22 種 `kind`）

| 類別 | 元件 `kind` | 主要支援 JSON 欄位 | Pi Zero Runtime 實作 | 資料品質失效 (`BAD`/`WAITING`) 視覺回饋 | 測試狀態 |
|---|---|---|---|---|---|
| 繪圖 | `rectangle` | `x,y,width,height,background,borderColor,borderWidth,borderRadius,shapeFilled` | LVGL `lv_obj` | 不適用（靜態圖形） | ✅ 實機/編譯 |
| 繪圖 | `circle` | 同上（強制 `LV_RADIUS_CIRCLE`） | LVGL `lv_obj` | 不適用 | ✅ 實機/編譯 |
| 繪圖 | `line` | `width,height,background,borderColor,borderWidth,borderStyle` | LVGL `lv_obj` 線段（支援垂直置中與粗細） | 不適用 | ✅ 實機/編譯 |
| 媒體 | `image` | `imageAssetId`, `image`(Base64 PNG/JPEG), `imageFit`(`contain/cover/fill/none`) | `tjpgd` / `lodepng` 預解碼快取 | 不適用 | 🧪 編譯/契約 |
| 文字 | `text` | `text,color,fontSize,textAlign,verticalAlign,wordWrap,lineHeight,underline,textBackground` | LVGL `lv_label` | 綁定變數時顯示紅框警示 | ✅ 實機/編譯 |
| 數值 | `number` | `variableId,decimals,unit,numberFormat,useGrouping,numberLocale,numberPrefix` | 獨立格式化器 + 大字體 | 顯示 `---`（斷線）或 `...`（等待）+ 紅框 | ✅ 實機/編譯 |
| 時間 | `datetime` | `dateTimeFormat` (`yyyy-MM-dd HH:mm:ss` 等) | `sys_clock` SNTP + `tz_offset` | 未對時顯示開機相對基準時間 | 🧪 編譯/契約 |
| 按鈕 | `button` | `action,actionValue,targetScreenId,buttonPressed*,buttonDisabled*` | `lv_button` + 鍵盤/滑鼠事件 | 寫入失敗時外框閃紅警示 | ✅ 實機/編譯 |
| 開關 | `switch` | `variableId,fillColor,trackColor` | `lv_switch` 雙向寫入 | 斷線時顯示紅框 + 降透明度 | ✅ 實機/編譯 |
| 輸入 | `input` | `variableId,inputMin,inputMax,inputStep` | `lv_textarea` 編輯提交緩衝 | 斷線顯示 `---` + 紅框 | 🧪 編譯/契約 |
| 滑桿 | `slider` | `variableId,scaleMin,scaleMax,orientation,fillColor,trackColor,showValue` | 卡片框 + 標題 + `lv_slider` + 數值（預設顯示） | 斷線顯示紅框 + 降透明度 | 🧪 編譯/模擬器 |
| 旋鈕弧 | `arc` | `variableId,scaleMin,scaleMax,fillColor,trackColor,showValue` | 卡片框 + 標題 + `lv_arc` + 中央數值（預設顯示） | 斷線顯示 `---` + 紅框 | 🧪 編譯/模擬器 |
| 微調框 | `spinbox` | `variableId,inputMin,inputMax,inputStep,decimals` | 複合微調框（`-/+` 按鈕 + 數值標籤） | 斷線顯示紅框 + 降透明度 | 🧪 編譯/模擬器 |
| 下拉選單 | `dropdown` | `variableId,multistateItems` | `lv_dropdown`（預設自動/手動/維護模式 + 中文清單字型） | 斷線顯示紅框 + 降透明度 | 🧪 編譯/模擬器 |
| 指示燈 | `indicator` | `variableId,fillColor,trackColor` | 圓形狀態燈（亮/暗） | 斷線變灰 + 紅框警示 | ✅ 實機/編譯 |
| 多狀態 | `multistate` | `variableId,multistateItems` | 依數值切換背景、文字與字色 | 斷線顯示 `斷線` + 紅框 | ✅ 實機/編譯 |
| 長條圖 | `bar` | `variableId,scaleMin,scaleMax,orientation,warningThreshold,criticalThreshold,showValue,showScale` | 卡片框 + 標題 + `lv_bar` + 刻度 + 門檻變色 | 斷線顯示紅框 + 降透明度 | ✅ 實機/模擬器 |
| 儀表 | `gauge` | `variableId,scaleMin,scaleMax,warningThreshold,criticalThreshold,showValue,showScale,unit` | 卡片框 + 標題 + 180° 半圓 `lv_arc` + 刻度 + 數值 | 斷線顯示 `---` + 紅框 | ✅ 實機/模擬器 |
| 刻度尺 | `scale` | `scaleMin,scaleMax,scaleMajorTicks,scaleMinorTicks,scaleVertical,scaleShowAxis,scaleShowLabels` | `lv_scale`（支援隱藏主軸線與字級縮放） | 不適用 | 🧪 編譯/模擬器 |
| 趨勢圖 | `trend` | `variableId,scaleMin,scaleMax,sampleIntervalMs,historyPoints` | `lv_chart` + 背景環形取樣緩衝（換頁不中斷） | 斷線顯示紅框 | 🧪 編譯/模擬器 |
| 表格 | `table` | `tableCols,tableRows,gridColor,background,borderColor` | `lv_table`（空白格線、均分欄寬，預設 3×4） | 不適用 | 🧪 編譯/模擬器 |
| 警報清單 | `alarm` | `alarmSource(global/local/all),alarmGroup,alarmMaxRows,alarmRules` | 即時警報清單 + 單鍵確認 (ACK) | 變數斷線時凍結既有警報狀態（不誤觸發/不誤復歸） | 🧪 編譯/契約 |

---

## 4. 動作支援矩陣（全部 11 種 `action`）

| 動作 `action` | 行為說明 | Pi Zero 支援 |
|---|---|:---:|
| `toggle` | 反轉布林變數並觸發 Modbus 寫入（若有映射） | ✅ |
| `set` | 將 `actionValue` 寫入指定變數 | ✅ |
| `increment` | 依 `actionValue`（正或負）加減數值變數 | ✅ |
| `reset` | 將變數重設為 `initial` 初始值 | ✅ |
| `momentary` | 按下設為 `true`、放開/失焦強制設為 `false`（安全寸動） | ✅ |
| `screen` | 切換至 `targetScreenId` 並記錄瀏覽歷史 | ✅ |
| `previousScreen` | 返回上一個一般畫面 | ✅ |
| `popup` / `openDialog` | 以半透明遮罩開啟彈出對話盒（最多 16 層） | ✅ |
| `closePopup` / `closeDialog` | 關閉最上層彈出對話盒 | ✅ |

動態控制屬性：`visible`（靜態隱藏）、`conditionId`（依變數動態顯示/隱藏）、`enabledId`（依變數動態啟用/停用）皆已支援。

---

## 5. 外觀屬性、繼承與格式化對照

| 功能項目 | 設計器支援 | Pi Zero 支援狀態 | 處理策略 |
|---|:---:|:---:|---|
| 畫面繼承 (`parentScreenId` / `inheritsScreenId`) | ✅ | ✅ **匯出時自動扁平化** | 透過「部署至 Pi Zero」匯出或推送時自動展開父畫面元件（保留 Z 軸順序與唯一 ID） |
| 數值格式 (`decimal`/`integer`/`percent`/`scientific`) | ✅ | ✅ 完全支援 | 支援 0–6 位小數、千分位 `useGrouping`、`en-US`/`zh-TW`/`de-DE`、`numberPrefix` |
| 對齊與排版 (`textAlign`, `verticalAlign`, `wordWrap`, `lineHeight`, `underline`, `textBackground`) | ✅ | ✅ 完全支援 | 直接對應 LVGL 9 樣式 |
| 按鈕互動色 (`buttonPressed*`, `buttonDisabled*`) | ✅ | ✅ 完全支援 | 對應 `LV_STATE_PRESSED` / `LV_STATE_DISABLED` |
| 邊框樣式 (`borderStyle: solid / none`) | ✅ | ✅ 完全支援 | `dashed` / `dotted` 近似以實線繪製（匯出前會提示警告） |
| 圖片格式 (`PNG` / `JPEG` Base64) | ✅ | ✅ 完全支援 | WebP / GIF 不支援解碼（匯出前會提示警告） |
| `fontFamily`、`fontWeight: bold`、`italic` | ✅ | ❌ 不支援 | 統一使用內建 Noto Sans TC（SIL OFL 1.1）點陣字型（匯出前會提示警告） |
| `buttonHover*`、`imagePosition`、`tableDiagonal` | ✅ | ❌ 不支援 | Pi Zero 忽略（匯出前會提示警告） |

---

## 6. 通訊、警報、輸入與現場維運能力

- **Modbus TCP / RTU**：支援 16 台設備、Coil / Discrete Input / Holding Register / Input Register、`bool`/`uint16`/`int16`/`uint32`/`int32`/`float32`、高低字序、線性縮放與偏移、寫入後強制回讀（寫入失敗不自動重試，避免現場重複動作）。
- **USB 輸入**：同時支援 USB HID Boot Keyboard（`Tab`/`Shift+Tab`/`Enter`/`Esc`/方向鍵）與 USB HID Boot Mouse（自動顯示游標），支援直連與 1 層 USB Hub。
- **本機診斷頁 (`F12`)**：任何畫面按下鍵盤 `F12` 即可切換顯示專案名稱、載入狀態、解析警告數、Wi-Fi IP、SNTP 時間、Modbus 設備連線統計與 LVGL 記憶體使用率。
- **HTTP Port 8080 現場維運 API**：提供 `GET /`、`GET /api/status`、`GET /api/alarms`、`POST /api/project`（原子熱重載：失敗時自動保留既有畫面），並支援 `wifi.txt` 設定 `ops_token=` 認證與 `ops_http=0` 關閉開關。

相關文件：
- 硬體實測矩陣與支援聲明：[HARDWARE_TEST_MATRIX.md](file:///d:/Dropbox/Infosystem/MicroOS2LVGL_Proj/outputs/pi-zero-w-ucos2-wifi/docs/HARDWARE_TEST_MATRIX.md)
- 現場部署、備份與回復 SOP：[DEPLOYMENT_AND_RECOVERY_SOP.md](file:///d:/Dropbox/Infosystem/MicroOS2LVGL_Proj/outputs/pi-zero-w-ucos2-wifi/docs/DEPLOYMENT_AND_RECOVERY_SOP.md)
