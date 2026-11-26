# P1 遺漏 HMI 元件與互動流程開發規格書（EGM Spec）

| 項目 | 內容 |
| :--- | :--- |
| **文件狀態** | Implemented v1.0（ARM 編譯與靜態驗證通過，待模擬器／實機驗證） |
| **適用專案** | `pi-zero-w-ucos2-wifi`（Raspberry Pi Zero W / Zero 2 W，µC/OS-II V2.93.01 + LVGL 9.4.0） |
| **上游規格來源** | `InfoSys_ViewGP_Web`（`src/model.ts`、`sdcard/project.json`） |
| **涵蓋範圍（P1）** | ① 補齊 5 種遺漏元件（`image`、`slider`、`arc`、`spinbox`、`dropdown`）　② 補強 6 種既有元件（背景 `trend`、`input` 編輯提交、`bar`、`gauge`、`scale`、`table`）　③ 補齊 `momentary` 點動按鈕與 `visible: false`　④ USB HID Boot Mouse（`LV_INDEV_TYPE_POINTER`） |
| **編譯與記憶體約束** | Freestanding C11（`-nostdlib -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -DLV_DISABLE_API_MAPPING`）；總 RAM 映像低於 `linker.ld` 之 16 MiB（目前約 3.8 MiB，P1 預估增加約 2.0 MiB，總計 < 6 MiB） |

---

## 1. 背景、目標與非目標（Scope & Non-Goals）

### 1.1 背景與目標
P0 已補齊 Modbus TCP/RTU、警報引擎與 SNTP 真實時間。但對照網頁設計端 `InfoSys_ViewGP_Web` 的 22 種 HMI 元件，目前 Pi Zero 端尚缺 5 種元件、6 種既有元件僅有雛形、`momentary` 與 `visible: false` 尚未生效，且缺少滑鼠指標輸入。

P1 階段目標為讓 `InfoSys_ViewGP_Web` 匯出的 **全部 22 種元件與標準互動流程** 皆能在 Pi Zero 上正確顯示與操作，並支援 USB 滑鼠點擊／拖曳，使鍵盤與滑鼠雙輸入無縫運作。

### 1.2 明確非目標（Non-Goals）
- **不支援 WebP / 動態 GIF 解碼**：圖片資產（`assets[]` / `element.image`）在 P1 支援 **JPEG**（複用 LVGL 內建 `tjpgd`）與 **PNG**（複用 LVGL 內建 `lodepng`），涵蓋 99% 工業 HMI 標誌與底圖（含範例 `sdcard/project.json` 之 `infosystem` JPEG 標誌）。
- **不處理 P2 細部排版樣式**（`numberLocale`、`scientific` 科學記號、48px 大字體、虛線邊框、hover 配色等留待 P2）。
- **不實作多點觸控 HID Digitizer**（P1 先實作標準 USB HID Boot Mouse `bInterfaceProtocol == 2`，涵蓋所有 USB 滑鼠與模擬滑鼠模式之電阻／電容觸控螢幕）。

---

## 2. LVGL 設定與既有函式庫複用（Reuse & Memory Budget）

### 2.1 `lv_conf.h` 與 `Makefile` 變更
為避免重複造輪子，直接啟用專案中已存在於 `vendor/lvgl/src/` 的元件與解碼器：

| 巨集 / 模組 | 現況 | P1 變更 | 用途 |
| :--- | :---: | :---: | :--- |
| `LV_MEM_SIZE` | `512 KiB` | **`2048 KiB`（2 MiB）** | 容納解碼後的靜態圖片像素緩衝區與 `dropdown` / `scale` / `slider` 物件 |
| `LV_USE_IMAGE` | `0` | **`1`** | `VGP_KIND_IMAGE` 圖片顯示（`lv_image_create`） |
| `LV_USE_SLIDER` | `0` | **`1`** | `VGP_KIND_SLIDER` 滑桿（`lv_slider_create`） |
| `LV_USE_DROPDOWN` | `0` | **`1`** | `VGP_KIND_DROPDOWN` 下拉選單（`lv_dropdown_create`） |
| `LV_USE_SCALE` | `0` | **`1`** | `VGP_KIND_SCALE` 刻度尺（`lv_scale_create`） |
| `LV_USE_TJPGD` | `0` | **`1`**（直呼 `tjpgd.c`） | 解碼 `data:image/jpeg;base64,...` 靜態圖片 |
| `LV_USE_LODEPNG` | `0` | **`1`**（直呼 `lodepng.c`） | 解碼 `data:image/png;base64,...` 靜態圖片 |

`Makefile` 之 `LVGL_SOURCES` 補加入：
- `$(LVROOT)/widgets/slider/*.c`
- `$(LVROOT)/widgets/dropdown/*.c`
- `$(LVROOT)/libs/tjpgd/*.c`
- `$(LVROOT)/libs/lodepng/*.c`

> **設計取捨（`spinbox`）**：LVGL 原生 `lv_spinbox` 依賴 `LV_USE_TEXTAREA` 且偏向逐位數字游標操作，與 ViewGP 網頁端「`[-]` 數值顯示 `[+]`（依 `inputStep` 增減並支援 `inputMin/inputMax/decimals`）」的工業操作習慣不同。因此 `VGP_KIND_SPINBOX` 採用輕量複合元件（容器 + 減號按鈕 + 數值標籤 + 加號按鈕，並支援鍵盤 `+`/`-` 與滑鼠點擊），操作直覺且不需啟用 `LV_USE_TEXTAREA`。

---

## 3. 資料模型與解析器擴充（`vgp_model.h` / `vgp_parser.c`）

### 3.1 `vgp_kind_t` 擴充
補齊其餘 4 種未定義 enum（`VGP_KIND_IMAGE` 已存在）：
```c
    VGP_KIND_SLIDER,
    VGP_KIND_ARC,
    VGP_KIND_SPINBOX,
    VGP_KIND_DROPDOWN
```

### 3.2 圖片資產索引（Zero-Copy 指向 `initramfs` 原始 JSON）
為避免在 `.bss` 複製數百 KB Base64 字串，`vgp_asset_ref_t` 僅記錄 `project.json` 緩衝區內（常駐於 `0x02000000`）之指標與長度：
```c
#define VGP_MAX_ASSETS 16

typedef enum {
    VGP_IMG_FIT_CONTAIN = 0,
    VGP_IMG_FIT_COVER,
    VGP_IMG_FIT_FILL,
    VGP_IMG_FIT_NONE
} vgp_img_fit_t;

typedef struct {
    char id[VGP_MAX_ID_LEN];
    const char *b64_ptr;   /* 指向 "base64," 之後的原始 JSON 字串 */
    size_t b64_len;
    bool is_png;           /* true=PNG, false=JPEG */
} vgp_asset_ref_t;
```

### 3.3 `vgp_element_t` 擴充欄位
```c
    /* Image */
    char image_asset_id[VGP_MAX_ID_LEN];
    const char *inline_b64_ptr;
    size_t inline_b64_len;
    bool inline_is_png;
    vgp_img_fit_t image_fit;

    /* Thresholds, Input & Trend & Scale & Table */
    double input_min, input_max, input_step;
    bool has_input_min, has_input_max;
    double warning_threshold, critical_threshold;
    bool has_warning_threshold, has_critical_threshold;
    bool show_value;
    bool show_scale;
    uint32_t sample_interval_ms; /* trend: default 1000 */
    uint16_t history_points;     /* trend: default 60, max 120 */
    bool scale_vertical;         /* scaleOrientation */
    bool scale_show_axis;
    bool scale_show_labels;
    uint32_t grid_color;         /* table gridColor */

    /* Runtime secondary label (for bar/gauge/slider/arc showValue, spinbox +/-) */
    lv_obj_t *lv_aux;
```

### 3.4 `vgp_parser.c` 解析擴充
1. 頂層新增解析 `"assets"` 陣列：提取 `id` 並定位 `"data"` 中 `"data:image/png;base64,"` 或 `"data:image/jpeg;base64,"`（不複製整段 Base64，僅記錄 `s + start` 指標與長度）。
2. `parse_kind_str()` 新增 `"slider"`、`"arc"`、`"spinbox"`、`"dropdown"`。
3. `parse_element_obj()` 新增解析：
   - `"imageAssetId"`、`"image"`（inline Base64 指標定位）、`"imageFit"`
   - `"inputMin"`、`"inputMax"`、`"inputStep"`
   - `"warningThreshold"`、`"criticalThreshold"`
   - `"showValue"`、`"showScale"`
   - `"sampleInterval"`、`"historyPoints"`
   - `"scaleOrientation"`、`"scaleShowAxis"`、`"scaleShowLabels"`
   - `"gridColor"`

---

## 4. 遺漏與補強元件規格（`vgp_image.c` / `vgp_trend.c` / `vgp_runtime.c`）

### 4.1 圖片解碼與快取（新增 `vgp_image.h` / `vgp_image.c`）
- **解碼流程（載入專案時執行一次，渲染期零解碼開銷）**：
  1. 依序對 `proj->assets[]`（最多 16 張）與元件 inline `image` 進行 Base64 解碼至暫存緩衝（`lv_malloc`）。
  2. JPEG：呼叫 `jd_prepare()` + `jd_decomp()`（來自 `vendor/lvgl/src/libs/tjpgd/tjpgd.c`）輸出至 `LV_COLOR_FORMAT_XRGB8888` 像素緩衝。
  3. PNG：呼叫 `lodepng_decode32()`（來自 `vendor/lvgl/src/libs/lodepng/lodepng.c`）並將 RGBA8888 轉為 LVGL 小端 BGRA（`LV_COLOR_FORMAT_ARGB8888`）。
  4. 封裝為 `lv_image_dsc_t` 快取；單張圖片解碼後像素上限限制為 `640 × 480`（超過則跳過並記錄日誌，防止耗盡 2 MiB LVGL heap）。
- **縮放適配（`imageFit`）**：
  - 根據來源圖寬高 `(src_w, src_h)` 與元件寬高 `(ew, eh)` 計算 LVGL 縮放倍率（`256 = 1.0×`）：
    - `contain`：`min(ew*256/src_w, eh*256/src_h)`，置中顯示。
    - `cover`：`max(ew*256/src_w, eh*256/src_h)`，外框裁切（`overflow hidden`）。
    - `fill`：依 `lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH)` 填滿。
    - `none`：`256`（原尺寸置中）。

### 4.2 背景趨勢採樣引擎（新增 `vgp_trend.h` / `vgp_trend.c`）
解決目前切換畫面後趨勢圖停止採樣與清空資料的問題：
- **資料結構**：
  ```c
  #define VGP_MAX_TRENDS        8
  #define VGP_TREND_MAX_POINTS  120
  typedef struct {
      uint16_t element_idx;
      int32_t  var_idx;
      uint32_t sample_interval_ms;
      uint16_t capacity;       /* min(el->history_points, 120) */
      uint16_t count;          /* 目前已採樣點數 */
      uint16_t head;           /* 環形寫入索引 */
      uint32_t last_sample_ms;
      int32_t  samples[VGP_TREND_MAX_POINTS]; /* 固定點 ×10 儲存以保留 0.1 精度 */
  } vgp_trend_buf_t;
  ```
- **背景採樣**：`vgp_trend_tick(now_ms)` 於 `vgp_runtime_update()` 中每次呼叫執行，遍歷所有 `VGP_KIND_TREND` 元件（**不論是否在當前畫面**）：
  - 若 `now_ms - last_sample_ms >= sample_interval_ms` 且變數品質為 `INTERNAL` 或 `GOOD`（跳過 `BAD/WAITING`），寫入環形緩衝。
  - 若該 `trend` 元件目前在畫面上（`el->lv_obj != NULL`），同步推送至 `lv_chart`。
- **畫面切換還原**：`create_screen_widgets()` 建立 `lv_chart` 時，設定 `lv_chart_set_point_count(obj, capacity)`、`lv_chart_set_axis_range(obj, LV_CHART_AXIS_PRIMARY_Y, scale_min*10, scale_max*10)`，並立即從環形緩衝回填所有歷史點位。

### 4.3 新增與補強互動元件行為表

| 元件 | LVGL 實作方式 | 互動與寫入規則 |
| :--- | :--- | :--- |
| **`slider`** | `lv_slider_create` + 選配數值 `lv_label`（`showValue`） | 支援 `scaleMin..scaleMax`、`fillColor/trackColor`、`orientation_vertical`；滑鼠拖曳放開（`LV_EVENT_RELEASED`）或鍵盤 `+`/`-` 透過 `write_var_num()` 寫入；Modbus 斷線／唯讀時鎖定 `LV_STATE_DISABLED`。 |
| **`arc`** | `lv_arc_create` + 中央數值 `lv_label`（`showValue`） | 保持可互動（相較 `gauge` 鎖定唯讀）；釋放（`LV_EVENT_RELEASED`）或鍵盤 `+`/`-` 寫入變數；同步更新圓弧角度與數值標籤。 |
| **`spinbox`** | 容器 + `[-]` 按鈕 + 數值標籤 + `[+]` 按鈕 | 點擊 `[-]`/`[+]` 或聚焦時按 `-`/`+` 依 `inputStep`（預設 `1`）加減並箝位於 `[inputMin, inputMax]`，經 `write_var_num()` 寫入。 |
| **`dropdown`** | `lv_dropdown_create`（套用 `vgp_font_zh_16`） | 將 `multistateItems[].label` 以 `\n` 串接為選項；`LV_EVENT_VALUE_CHANGED` 時取對應 `multistateItems[sel].value` 寫入變數；變數外部變更時反向同步選中索引。 |
| **`input`**（補強） | 編輯狀態機（`g_edit_el` + `char g_edit_buf[32]`） | 聚焦按數字鍵 / `.` / `-` / `Backspace` 時僅修改 `g_edit_buf` 並顯示游標 `_`（不立即送 Modbus）；按 **`Enter` 或離焦（`LV_EVENT_DEFOCUSED`）** 才解析數值、依 `inputMin/inputMax` 箝位並呼叫 `write_var_num()`；按 `Esc` 取消編輯恢復原值。 |
| **`bar`**（補強） | `lv_bar_create` + 選配數值 `lv_label`（`showValue`） | 支援垂直條（高 > 寬自動為垂直，LVGL `lv_bar` 依尺寸自動判定方向）；依 `warningThreshold`（琥珀 `#f59e0b`）與 `criticalThreshold`（紅 `#ef4444`）動態變更 `LV_PART_INDICATOR` 顏色。 |
| **`gauge`**（補強） | `lv_arc_create`（移除 `LV_OBJ_FLAG_CLICKABLE` 唯讀）+ 中央數值標籤 | 鎖定唯讀、隱藏旋鈕（`LV_PART_KNOB` 透明）、依門檻變色並顯示 `showValue` 數值與單位。 |
| **`scale`**（補強） | `lv_scale_create` | 設定 `LV_SCALE_MODE_HORIZONTAL_BOTTOM` 或 `VERTICAL_LEFT`、`lv_scale_set_range(min, max)`、`lv_scale_set_total_tick_count`、`lv_scale_set_major_tick_every`、`lv_scale_set_label_show(scale_show_labels)`。 |
| **`table`**（補強） | `lv_table_create` | 套用 `background`、`borderColor`、`borderWidth`、`gridColor`（設於 `LV_PART_ITEMS` 邊框色）。 |

### 4.4 動作與可見性補強
1. **`visible: false`**：於 `create_screen_widgets()` 建立元件後，若 `!el->visible` 立即呼叫 `lv_obj_add_flag(el->lv_obj, LV_OBJ_FLAG_HIDDEN)`。
2. **`momentary`（點動按鈕）**：
   - 綁定 `LV_EVENT_PRESSED` → `write_var_bool(vi, true)`並記錄 `g_momentary_var_idx = vi`。
   - 綁定 `LV_EVENT_RELEASED`、`LV_EVENT_PRESS_LOST`、`LV_EVENT_DEFOCUSED` → 若 `g_momentary_var_idx >= 0` 立即呼叫 `write_var_bool(g_momentary_var_idx, false)` 並重置為 `-1`。
   - 切換畫面（`switch_screen_to()`）與關閉彈窗（`close_active_popup()`）前，亦主動檢查並釋放 `g_momentary_var_idx`，確保安全不卡死。

---

## 5. USB HID Boot Mouse 指標輸入規格（`usb/` & `gui.c`）

### 5.1 USB 裝置列舉擴充（`usb/usb_host.h`、`usb/descriptors.c`、`usb/usb_keyboard.c`）
- **雙介面支援**：
  - `usb_parse_config()` 同時掃描 HID Boot Keyboard（`bInterfaceClass == 3 && bInterfaceProtocol == 1`）與 HID Boot Mouse（`bInterfaceClass == 3 && bInterfaceProtocol == 2`），可分別記錄 `kbd_dev` 與 `mouse_dev`（支援單一滑鼠、單一鍵盤、鍵鼠無線接收器複合裝置，以及 USB Hub 下同時插入鍵盤與滑鼠）。
  - `usb_keyboard_task` 在主輪詢迴圈中依序輪詢已啟用之 `kbd_dev` 與 `mouse_dev`（每 10ms 一次），解析標準 3-byte / 4-byte Boot Mouse 報告：
    - `byte[0]`: buttons（bit 0 = 左鍵, bit 1 = 右鍵, bit 2 = 中鍵）
    - `byte[1]`: `(int8_t)dx`
    - `byte[2]`: `(int8_t)dy`
- **無鎖事件佇列（新增 `usb/mouse_queue.h` / `usb/mouse_queue.c`）**：
  - 累計相對位移 `(acc_dx, acc_dy)` 與最新按鍵狀態 `left_pressed`（在 `OS_CPU_SR_Save` 短臨界區讀寫），避免快速移動滑鼠時掉封包。

### 5.2 LVGL Pointer 註冊（`gui.c`）
- 建立第二個輸入裝置 `lv_indev_t *mouse_indev`：
  - `lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER)`
  - `mouse_read()` 讀取累積 `(dx, dy)` 更新游標座標（箝位於 `[0, fb->width - 1]` × `[0, fb->height - 1]`），回傳 `LV_INDEV_STATE_PRESSED / RELEASED`。
- **可見游標物件**：當偵測到首次滑鼠移動或按鍵時，於 `lv_layer_sys()` 建立高對比小箭頭／圓點游標（12×12 白框黑心），透過 `lv_indev_set_cursor(mouse_indev, cursor_obj)` 由 LVGL 自動跟隨繪製（未接滑鼠時不顯示，不干擾純鍵盤畫面）。
- **底部狀態列診斷**：若偵測到滑鼠，狀態列顯示 `M:OK` 方便現場除錯。

---

## 6. 檔案變更清單與分階段實作計畫

| 檔案 | 類型 | 說明 |
| :--- | :---: | :--- |
| `lv_conf.h` / `Makefile` | 修改 | 調高 `LV_MEM_SIZE` 至 2 MiB，啟用 `IMAGE/SLIDER/DROPDOWN/SCALE/TJPGD/LODEPNG` 並加入對應 `.c` |
| `vgp_model.h` / `vgp_parser.c` | 修改 | 補齊 4 種 `kind` enum、`assets[]` 零拷貝索引、`imageFit/inputMin/Max/Step/Thresholds/showValue/trend/scale/gridColor` 解析 |
| `vgp_image.h` / `vgp_image.c` | 新增 | Base64 解碼 + `tjpgd` JPEG / `lodepng` PNG 解碼與 `lv_image_dsc_t` 快取 |
| `vgp_trend.h` / `vgp_trend.c` | 新增 | 全域背景趨勢環形緩衝與畫面還原 |
| `vgp_runtime.c` | 修改 | 實作 `image/slider/arc/spinbox/dropdown`、補強 `trend/input/bar/gauge/scale/table`、`momentary` 與 `visible: false` |
| `usb/mouse_queue.h` / `.c` | 新增 | 滑鼠位移與左鍵無鎖累加佇列 |
| `usb/usb_host.h` / `descriptors.c` / `usb_keyboard.c` | 修改 | 同時支援 HID Boot Keyboard 與 Boot Mouse（直連／複合／Hub） |
| `gui.c` | 修改 | 註冊 `LV_INDEV_TYPE_POINTER` 與滑鼠游標 |

### 分階段實作順序
- **Phase 1**：`lv_conf.h`、`Makefile`、`vgp_model.h`、`vgp_parser.c`、`vgp_image.c`（圖片解碼）、`vgp_trend.c`（背景趨勢）。
- **Phase 2**：`vgp_runtime.c` 全部新增與補強元件、`input` 編輯提交、`momentary` 與 `visible: false`。
- **Phase 3**：USB HID Boot Mouse（`usb/mouse_queue.c`、`descriptors.c`、`usb_keyboard.c`、`gui.c`）+ 完整編譯與驗證。

---

## 7. 驗證計畫（Verification Plan）

1. **建置與靜態檢查**：執行 `.\tools\build-and-stage.ps1`，確認 0 warnings / 0 errors（`-Werror`）、`verify-build.ps1` 與 `verify-sdcard.ps1` 全數 PASS、總映像低於 16 MiB。
2. **範例專案 `sdcard/project.json` 驗證**：
   - `公司標誌`（`imageAssetId: "infosystem"` JPEG）正確解碼並縮放顯示於右下角。
   - `溫度長條`（`bar`）隨數值超過 60°C 變琥珀色、超過 80°C 變紅色，並顯示數值標籤。
   - `溫度趨勢`（`trend`）停留在主畫面時持續背景取樣，切換至趨勢畫面立即顯示完整歷史曲線。
   - `目標溫度輸入`（`input`）支援小數點輸入、`0..100` 範圍箝位、按 `Enter` 送出、按 `Esc` 取消。
3. **實機 USB 滑鼠測試**：插入 USB 滑鼠（直連 OTG 或經 Hub 與鍵盤並插），確認游標顯示、可點擊按鈕／開關／拖曳滑桿與圓弧旋鈕。
