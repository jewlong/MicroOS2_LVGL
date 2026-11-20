# P4 Web 設計器與 Pi Zero LVGL 元件視覺與預設值對齊修改設計規格書（EGM Spec）

| 項目 | 內容 |
| :--- | :--- |
| **文件狀態** | Implemented v1.0（ARM 編譯、契約檢查與 ARM1176 模擬器全 22 元件驗收通過） |
| **適用專案** | `pi-zero-w-ucos2-wifi`（Raspberry Pi Zero W / Zero 2 W，µC/OS-II V2.93.01 + LVGL 9.4.0） |
| **上游規格來源** | `InfoSys_ViewGP_Web`（`src/components/widgets/{Bar,Gauge,Arc,Slider,Line,Table,Dropdown,Scale}Widget.tsx`、`src/model.ts`） |
| **涵蓋範圍（P4）** | ① `showValue` / `showScale` 預設值對齊　② `line` 顏色與粗細對齊　③ `dropdown` 預設選項對齊　④ `table` 空白格線對齊　⑤ `gauge`/`bar`/`slider`/`arc` 卡片框、標題與刻度對齊　⑥ `scale` 軸線顯示（`scaleShowAxis`）與字體對齊 |
| **編譯與記憶體約束** | Freestanding C11（`-nostdlib -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -DLV_DISABLE_API_MAPPING`）；靜態 RAM 增量 < `2 KiB`（維持 `< 12 MiB / 16 MiB`），LVGL 動態記憶體維持於 `LV_MEM_SIZE = 4 MiB` 內 |

---

## 1. 背景、目標與非目標（Scope & Non-Goals）

### 1.1 背景與目標
Web 設計器（`InfoSys_ViewGP_Web`）與 Pi Zero 執行端（`vgp_runtime.c`）目前均已支援相同的 22 種元件 `kind`，但逐檔比對後發現 6 項實質視覺與預設值落差，導致使用者從設計器拖放元件並匯出 `project.json` 後，在 Pi Zero 實機與 Windows ARM 模擬器上出現「數值不見」、「線條顏色不對」、「表格多出 A/B/C/D」、「下拉選單顯示 Option 1」、「儀表/長條/滑桿/旋鈕缺少卡片框與標題」或「刻度隱藏軸線無效」等問題。

本規格書定義最小侵入、零架構破壞的對齊修改方案，使兩端呈現與行為高度一致。

### 1.2 明確非目標（Non-Goals）
- **不啟用 `LV_USE_SPINBOX` / `LV_USE_TEXTAREA` 或 WebP/GIF/SVG 解碼器**：維持現有 `lv_conf.h` 編譯配置與靜態記憶體水位。
- **不在 `gauge` 內自繪旋轉向量指針**：`gauge` 改採 180° 半圓 `lv_arc`（`180° → 360°`）搭配卡片框、標題、最小/最大刻度與中央數值，與設計器半圓儀表幾何一致，無需引入額外 `lv_line` 旋轉三角函數計算。
- **不新增螢幕虛擬數字鍵盤（`KeypadDialog`）或 `tableDiagonal` 對角斜線**：維持既有鍵盤/滑鼠輸入模型。

---

## 2. 資料模型與解析器預設值對齊（`vgp_model.h` / `vgp_parser.c`）

### 2.1 `vgp_element_t` 最小擴充（`vgp_model.h`）
為區分 `line` 元件是否顯式指定了邊框樣式（對應 `LineWidget.tsx` Lines 5–9 判斷邏輯），於 `vgp_element_t` 新增兩個旗標（共 +2 bytes）：
```c
bool has_border_color;
bool has_border_width;
```
不新增任何額外陣列或 LVGL 物件指標（沿用既有 `lv_obj`、`lv_extra`、`lv_aux` 三指標）。

### 2.2 解析器預設值修正（`vgp_parser.c`：`parse_element_obj`）
修正以下與 Web 設計器相反或不一致的初始值：

| 欄位 | 原 Pi Zero 預設值 | 修正後預設值 | 對齊依據（`InfoSys_ViewGP_Web`） |
| :--- | :--- | :--- | :--- |
| `e->show_value` | `false`（`memset` 歸零） | **`true`** | `BarWidget` / `GaugeWidget` / `ArcWidget` / `SliderWidget` 皆為 `e.showValue !== false` |
| `e->show_scale` | `false`（`memset` 歸零） | **`true`** | `BarWidget` / `GaugeWidget` 皆為 `e.showScale !== false` |
| `e->table_cols` | `0`（runtime 回退 4） | **`3`** | `TableWidget.tsx`：`element.tableCols ?? 3` |
| `e->table_rows` | `0`（runtime 回退 4） | **`4`** | `TableWidget.tsx`：`element.tableRows ?? 4` |
| `e->has_border_color` | 無 | `false`（讀到 `borderColor` 設為 `true`） | `LineWidget.tsx` 判斷分支 |
| `e->has_border_width` | 無 | `false`（讀到 `borderWidth` 設為 `true`） | `LineWidget.tsx` 判斷分支 |

---

## 3. 元件呈現與互動對齊設計（`vgp_runtime.c`）

### 3.1 `lv_obj` / `lv_extra` / `lv_aux` 指標所有權契約（重要不變量）
將 `bar` / `gauge` / `arc` / `slider` 升級為「卡片容器 + 內部 LVGL 控件 + 標題/數值/刻度」時，必須嚴格遵守下列指標分工，確保 `visible`、`conditionId`、`enabledId`、`modbus_bad` 紅框警示、焦點群組與事件回呼完全一致：

| 元件 `kind` | `el->lv_obj`（外層可視與品質容器） | `el->lv_extra`（核心控制／數值控件） | `el->lv_aux`（即時數值文字） |
| :--- | :--- | :--- | :--- |
| `VGP_KIND_BAR` | 卡片 `lv_obj`（背景、邊框、圓角） | 內部 `lv_bar` | 數值 `lv_label`（`show_value` 為真時建立） |
| `VGP_KIND_GAUGE` | 卡片 `lv_obj` | 內部唯讀 `lv_arc`（180°–360°） | 數值 `lv_label`（`show_value` 為真時建立） |
| `VGP_KIND_ARC` | 卡片 `lv_obj` | 內部可互動 `lv_arc`（135°–45°） | 數值 `lv_label`（`show_value` 為真時建立） |
| `VGP_KIND_SLIDER` | 卡片 `lv_obj` | 內部可互動 `lv_slider` | 數值 `lv_label`（`show_value` 為真時建立） |

連動檢查點（必須全數改讀 `lv_obj_t *ctrl = el->lv_extra ? el->lv_extra : el->lv_obj;`）：
1. **`add_screen_to_kbd_group()`**：對 `SLIDER` 與 `ARC` 將 `el->lv_extra` 加入 `g_kbd_group`（而非外層卡片），確保鍵盤焦點框與滑鼠拖曳皆作用在真正可操作控件上。
2. **`slider_release_cb()` / `arc_release_cb()`**：從 `el->lv_extra` 讀取 `lv_slider_get_value()` / `lv_arc_get_value()`。
3. **`vgp_runtime_handle_key()`**：`focused == el->lv_extra || focused == el->lv_obj` 皆可響應 `+` / `-` 鍵微調。
4. **`vgp_runtime_update()`**：
   - `visible` / `conditionId` 隱藏與 `modbus_bad` 降透明度＋紅框仍套用在 `el->lv_obj`（整張卡片同步隱藏或標紅）。
   - `enabledId` 與 `modbus_locked` 的 `LV_STATE_DISABLED` 同時套用到 `el->lv_extra`（若存在），確保斷線時滑桿／旋鈕確實鎖定。
   - 數值更新與門檻變色（`threshold_fill_color`）操作在 `el->lv_extra` 上；`el->lv_aux` 依 `format_num_val()` 更新（斷線顯示 `---`，等待顯示 `...`）。

---

### 3.2 各元件詳細排版與修正規格

#### (1) `VGP_KIND_LINE`（直線）
對齊 `LineWidget.tsx`：
- **顏色決定規則**：
  - 若 `el->has_border_color || el->has_border_width || el->border_style != VGP_BORDER_SOLID`：線條顏色取 `el->has_border_color ? el->border_color : el->background`；線條厚度取 `lw = el->has_border_width ? (el->border_width * scale / 1000) : eh`（最小 `1px`，上限 `eh`），並在垂直方向置中於 `ey + (eh - lw) / 2`。
  - 否則（未設任何邊框屬性）：直接以 `el->background` 填滿 `(ex, ey, ew, eh > 1 ? eh : 2)`。
- **邊框樣式近似**：若 `border_style == VGP_BORDER_NONE` 設 `LV_OPA_TRANSP`；`DASHED` / `DOTTED` 設 `LV_OPA_60`。

#### (2) `VGP_KIND_DROPDOWN`（下拉選單）
對齊 `DropdownWidget.tsx` Lines 4–8：
- 建立 `VGP_KIND_DROPDOWN` 時，若 `el->num_multistate_items == 0u`，自動填入與設計器完全一致的預設 3 組項目至 `el->multistate_items[]`：
  1. `{ value: 0.0, label: "自動模式", color: 0x0038BDF8u, text_color: 0x00FFFFFFu }`
  2. `{ value: 1.0, label: "手動模式", color: 0x0022C55Eu, text_color: 0x00FFFFFFu }`
  3. `{ value: 2.0, label: "維護模式", color: 0x00F59E0Bu, text_color: 0x00FFFFFFu }`
  並設 `el->num_multistate_items = 3u`。
- 同時套用背景色 `el->background`、字色 `el->color`、邊框色 `el->border_color` 與圓角 `el->border_radius ? el->border_radius : 6`，並對展開清單（`lv_dropdown_get_list(el->lv_obj)`）套用 `&vgp_font_zh_16`，確保下拉展開時繁中選項不亂碼。
- 因直接寫入 `el->multistate_items[]`，`dropdown_change_cb()` 與 `vgp_runtime_update()` 的數值寫回與同步選取亦一併生效。

#### (3) `VGP_KIND_TABLE`（表格）
對齊 `TableWidget.tsx`：
- 移除 `VGP_KIND_TABLE` 分支中硬編碼的表頭文字 `"A"`、`"B"`、`"C"`、`"D"`（保留 `VGP_KIND_ALARM` 的 `"時間"`/`"警報訊息"`/`"等級"`/`"狀態"`）。
- `cols = el->table_cols ? el->table_cols : 3u`，`rows = el->table_rows ? el->table_rows : 4u`。
- 均分欄寬：`col_w = (ew - 2) / cols`（最小 `8px`），每一欄透過 `lv_table_set_column_width()` 設定，使格線填滿元件寬度。
- 儲存格背景設為 `el->background`、格線邊框設為 `el->grid_color ? el->grid_color : 0x00334155u`（1px）、字色設為 `el->color`，並將所有儲存格清空為 `""`。

#### (4) `VGP_KIND_BAR`（長條顯示）
對齊 `BarWidget.tsx`：
- 外層卡片 `el->lv_obj`：`apply_box_style(el->lv_obj, el->background, el->border_color, el->border_width ? el->border_width : 1, el->border_radius ? el->border_radius : 8)`，`pad_all = 0`。
- 若 `el->text[0]` 非空且卡片高度足夠（`eh >= 36`），於上方建立標題 `lv_label`（`x = 8, y = 4`，字色 `el->color`，`&vgp_font_zh_16`）。
- 內部 `el->lv_extra = lv_bar_create(el->lv_obj)`：
  - **水平模式（`!el->orientation_vertical`）**：置於卡片中段（`x = 10, y = title_h + 4, w = ew - 20, h = clamp(eh - title_h - footer_h - 10, 8, 22)`）。
  - **垂直模式（`el->orientation_vertical`）**：置於卡片中央（`w = clamp(ew / 3, 10, 24), h = eh - title_h - footer_h - 12`）。
- 若 `el->show_scale` 且高度足夠（`eh >= 48`）：於進度條下方左右建立最小／最大刻度標籤（`scale_min` 左對齊、`scale_max` 右對齊，`&lv_font_montserrat_14`，`0x0094A3B8u`）。
- 若 `el->show_value`：建立 `el->lv_aux` 置於卡片右上角（水平）或底部置中（垂直），顯示 `format_num_val()`。

#### (5) `VGP_KIND_GAUGE`（儀表）
對齊 `GaugeWidget.tsx`：
- 外層卡片 `el->lv_obj`：同上卡片樣式（圓角預設 10）。
- 若 `el->text[0]` 非空，於卡片頂部置中建立標題 `lv_label`（`y = 4`）。
- 內部 `el->lv_extra = lv_arc_create(el->lv_obj)`：
  - 改採 **180° 半圓角度** `lv_arc_set_angles(arc, 180, 360)` 與 `lv_arc_set_bg_angles(arc, 180, 360)`，移除 `LV_OBJ_FLAG_CLICKABLE` 與隱藏旋鈕 `LV_PART_KNOB`。
  - 尺寸取 `arc_sz = min(ew - 16, (eh - title_h) * 14 / 10)`（避免半圓底部截斷），置中對齊於卡片中段。
- 若 `el->show_scale`：於左下角與右下角建立 `scale_min` / `scale_max` 刻度標籤。
- 若 `el->show_value`：建立 `el->lv_aux` 置於卡片中下方（`LV_ALIGN_BOTTOM_MID`, `y = -6`），以 `font` 顯示格式化數值並隨門檻變色。

#### (6) `VGP_KIND_ARC`（圓弧旋鈕）
對齊 `ArcWidget.tsx`：
- 外層卡片 `el->lv_obj`：同上卡片樣式（圓角預設 10）。
- 若 `el->text[0]` 非空，於卡片頂部置中建立標題 `lv_label`（`y = 4`）。
- 內部 `el->lv_extra = lv_arc_create(el->lv_obj)`：保留可拖曳旋鈕（`135° → 45°`），`arc_sz = min(ew - 16, eh - title_h - 10)` 置中於卡片下半部，套用 `apply_focus_style(el->lv_extra)` 並綁定 `arc_release_cb`。
- 若 `el->show_value`：建立 `el->lv_aux` 置於圓弧中央。

#### (7) `VGP_KIND_SLIDER`（滑桿）
對齊 `SliderWidget.tsx`：
- 外層卡片 `el->lv_obj`：同上卡片樣式（圓角預設 8）。
- 上方標題列（若 `eh >= 34`）：左側顯示 `el->text`（`x = 8, y = 4`）；若 `el->show_value`，右側建立 `el->lv_aux`（`LV_ALIGN_TOP_RIGHT`, `x = -8, y = 4`）顯示即時數值。
- 內部 `el->lv_extra = lv_slider_create(el->lv_obj)`：
  - 水平模式：`x = 14, y = eh * 62 / 100 - track_h / 2, w = ew - 28, h = clamp(eh / 5, 8, 14)`。
  - 垂直模式：置中於卡片中段（`w = clamp(ew / 5, 8, 14), h = eh - 36`）。
  - 套用 `apply_focus_style(el->lv_extra)` 並綁定 `slider_release_cb`。

#### (8) `VGP_KIND_SCALE`（刻度尺）
對齊 `ScaleWidget.tsx`：
- 若 `!el->scale_show_axis`：設定 `lv_obj_set_style_line_width(el->lv_obj, 0, LV_PART_MAIN);`，隱藏主軸線而保留刻度線（`LV_PART_ITEMS` / `LV_PART_INDICATOR`）。
- 刻度標籤字型改由 `select_font(NULL, el->font_size > 4 ? el->font_size - 4 : 12, scale)` 決定（最小退回 `&lv_font_montserrat_14`），取代原本寫死 14px。

---

## 4. 記憶體與效能影響評估

| 項目 | 影響 |
| :--- | :--- |
| `.bss` 靜態 RAM | `vgp_element_t` 增加 2 bytes × 768 × 2（`g_proj` + `g_proj_staging`）≈ **`3 KiB`**（總靜態 RAM 仍約 `11.36 MiB / 16 MiB`） |
| LVGL Heap (`LV_MEM_SIZE = 4 MiB`) | 每張 `bar`/`gauge`/`arc`/`slider` 卡片多出 2–4 個輕量 `lv_label`/`lv_obj`（約 200–350 bytes/元件），即使全專案放滿 50 個儀表也僅佔 `< 18 KiB`（< 0.5% Heap） |
| 渲染效能 | 所有標題與刻度標籤為靜態建立、僅在數值變化時呼叫 `set_label_if_changed()`，零多餘重繪 |

---

## 5. 驗收標準與測試計畫

1. **ARM 編譯與靜態驗證**：
   - `.\tools\build-and-stage.ps1`：0 warnings / 0 errors（`-Werror`），`verify-build.ps1` 與 `verify-sdcard.ps1` 全數 PASS。
2. **契約驗證**：
   - `.\tools\test-contract.ps1`：驗證 `sdcard/project.json` 與 `tests/fixtures/contract_full.json` 全數 PASS。
3. **ARM1176 模擬器實測驗證（Unicorn）**：
   - 載入 `contract_full.json`（涵蓋全部 22 種元件）與 `sdcard/project.json`，驗證：
     - `bar`/`gauge`/`arc`/`slider` 預設 `show_value == true` 且正常渲染無 `gui_fault`。
     - `dropdown` 無 `multistateItems` 時自動載入 3 項中文預設值。
     - `table` 第一列儲存格為空字串（不再出現 `A/B/C/D`）。
     - `slider` 與 `arc` 可透過滑鼠點擊／鍵盤 `+`/`-` 更新綁定變數。
     - `JOG 點動` 滑鼠點擊開啟彈窗與 `Esc` 關閉維持正常（無迴歸）。
4. **文件同步**：
   - 實作完成後更新 [VIEWGP.md](file:///d:/Dropbox/Infosystem/MicroOS2LVGL_Proj/outputs/pi-zero-w-ucos2-wifi/VIEWGP.md) 對應條目。
