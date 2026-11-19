# P2 畫面外觀、排版與數值格式化開發規格書（EGM Spec）

| 項目 | 內容 |
| :--- | :--- |
| **文件狀態** | Implemented v1.0（ARM 編譯、字庫產生與靜態驗證通過） |
| **適用專案** | `pi-zero-w-ucos2-wifi`（Raspberry Pi Zero W / Zero 2 W，µC/OS-II V2.93.01 + LVGL 9.4.0） |
| **上游規格來源** | `InfoSys_ViewGP_Web`（`src/model.ts`：`validateAppearance`、`validateDataSettings`） |
| **涵蓋範圍（P2）** | ① 完整數值格式化（`numberFormat`、`useGrouping`、`numberPrefix`、`numberLocale`、`decimals` 0–6）　② 文字／形狀／按鈕外觀樣式（對齊、換行、行高、底線、背景、空心形狀、無邊框、按鈕狀態配色）　③ 大尺寸數值字體級距（36px / 48px `Microsoft JhengHei` + CJK fallback） |
| **編譯與記憶體約束** | Freestanding C11（`-nostdlib -Wall -Wextra -Werror -Wstrict-prototypes -Wmissing-prototypes -DLV_DISABLE_API_MAPPING`）；新增字庫預估約 `25–35 KiB`（`.rodata`），總映像維持 < 6 MiB / 16 MiB 上限 |

---

## 1. 背景、目標與非目標（Scope & Non-Goals）

### 1.1 背景與目標
P0 與 P1 已補齊工業通訊、警報、時間與全部 22 種 HMI 元件。但網頁設計端 `InfoSys_ViewGP_Web` 匯出的外觀與資料格式屬性（例如 `fontSize: 42` 的大數值、千分位逗號、百分比、科學記號、文字置中／靠右、空心外框矩形、按鈕按下／停用配色）目前在 Pi Zero 端尚未套用，導致實機畫面與網頁預覽存在視覺落差。

P2 階段目標為縮小實機與網頁設計端之視覺差異，達成高還原度的工業儀表板呈現。

### 1.2 明確非目標（Non-Goals）
- **不產生 36px/48px 全 5,401 字繁中點陣字庫**：高尺寸全中文字庫會佔用數 MB Flash/RAM；36px 與 48px 字庫僅收錄 ASCII 可印字元（`0x20..0x7E`）與工業單位符號（`°℃℉±×÷µΩ–—／`），若大標題含中文字元則透過 LVGL `fallback` 鏈自動回退至 `vgp_font_zh_22`。
- **不載入動態系統字型（FreeType / TTF）**：維持編譯期靜態 LVGL 點陣字庫，確保 0 外部檔案依賴與即時渲染效能。
- **不自繪斜體（`italic`）與任意虛線矩形向量（`dashed/dotted`）**：LVGL 9 軟體渲染無原生合成斜體與 `lv_obj` 虛線邊框；`borderStyle: "none"` 會精確移除邊框，`dashed/dotted` 以較低透明度邊框（`LV_OPA_60`）近似呈現。

---

## 2. 資料模型與解析器擴充（`vgp_model.h` / `vgp_parser.c`）

### 2.1 新增 Enum 與 `vgp_element_t` 欄位
```c
typedef enum {
    VGP_NUMFMT_DECIMAL = 0,
    VGP_NUMFMT_INTEGER,
    VGP_NUMFMT_PERCENT,
    VGP_NUMFMT_SCIENTIFIC
} vgp_num_format_t;

typedef enum {
    VGP_LOCALE_EN_US = 0, /* 千分位 ','、小數點 '.' */
    VGP_LOCALE_ZH_TW,     /* 千分位 ','、小數點 '.' */
    VGP_LOCALE_DE_DE      /* 千分位 '.'、小數點 ',' */
} vgp_num_locale_t;

typedef enum {
    VGP_ALIGN_LEFT = 0,
    VGP_ALIGN_CENTER,
    VGP_ALIGN_RIGHT
} vgp_text_align_t;

typedef enum {
    VGP_VALIGN_TOP = 0,
    VGP_VALIGN_MIDDLE,
    VGP_VALIGN_BOTTOM
} vgp_vert_align_t;

typedef enum {
    VGP_BORDER_SOLID = 0,
    VGP_BORDER_NONE,
    VGP_BORDER_DASHED,
    VGP_BORDER_DOTTED
} vgp_border_style_t;
```

於 `vgp_element_t` 擴充：
```c
    /* P2 Number Formatting */
    vgp_num_format_t number_format;
    vgp_num_locale_t number_locale;
    bool use_grouping;
    char number_prefix[16];

    /* P2 Typography & Appearance */
    vgp_text_align_t text_align;
    vgp_vert_align_t vertical_align;
    bool word_wrap;
    bool underline;
    bool text_background;
    bool shape_filled;
    uint16_t line_height_x100; /* 100..300 (1.0..3.0), default 120 */
    vgp_border_style_t border_style;

    /* P2 Button state colors */
    uint32_t btn_pressed_bg, btn_pressed_color;
    uint32_t btn_disabled_bg, btn_disabled_color;
    bool has_btn_pressed_bg, has_btn_pressed_color;
    bool has_btn_disabled_bg, has_btn_disabled_color;
```

### 2.2 `vgp_parser.c` 預設值與解析鍵
- 預設值：`shape_filled = true`、`word_wrap = true`、`line_height_x100 = 120`、`border_style = VGP_BORDER_SOLID`。
- `parse_element_obj()` 新增解析：
  - `"numberFormat"`（`"decimal" | "integer" | "percent" | "scientific"`）
  - `"numberLocale"`（`"en-US" | "zh-TW" | "de-DE"`）
  - `"useGrouping"`（boolean）
  - `"numberPrefix"`（string，上限 15 bytes）
  - `"textAlign"`（`"left" | "center" | "right"`）
  - `"verticalAlign"`（`"top" | "middle" | "bottom"`）
  - `"wordWrap"`、`"underline"`、`"textBackground"`、`"shapeFilled"`（boolean）
  - `"lineHeight"`（number `1.0..3.0` → 轉存 `uint16_t` ×100）
  - `"borderStyle"`（`"none" | "solid" | "dashed" | "dotted"`）
  - `"buttonPressedBackground"`、`"buttonPressedColor"`、`"buttonDisabledBackground"`、`"buttonDisabledColor"`

---

## 3. 數值格式化引擎規格（`vgp_runtime.c`：`format_num_val`）

完全 Freestanding 實作（不依賴 libc `%e` / `setlocale`）：

| 模式 (`numberFormat`) | 行為規格 | 範例（`val = 1234.56`, `decimals = 2`） |
| :--- | :--- | :--- |
| `decimal`（預設） | 四捨五入至 `decimals`（0–6 位）；依 `useGrouping` 插入千分位 | `1,234.56`（`de-DE` 為 `1.234,56`） |
| `integer` | 忽略 `decimals`，四捨五入為整數；支援 `useGrouping` | `1,235` |
| `percent` | 數值先 `× 100.0`，依 `decimals` 格式化並附加 `%`（若 `unit` 非空仍保留 `unit`） | `123,456.00%`（`val=0.856, dec=1` → `85.6%`） |
| `scientific` | 正規化為 `m × 10^e`（`1.0 ≤ |m| < 10.0`），以 `decimals` 位輸出尾數 + `e+XX`/`e-XX` | `1.23e+03` |

- **前綴與單位組裝順序**：`[numberPrefix][formatted_number][% if percent][ unit if unit non-empty]`。
- **語系分隔字元**：`en-US` / `zh-TW` → 小數點 `'.'`、千分位 `','`；`de-DE` → 小數點 `','`、千分位 `'.'`。
- **整數溢位保護**：改用 `int64_t` 進行 `decimals` 縮放運算（支援至 6 位小數與大數值不溢位）。

---

## 4. 外觀與排版樣式套用規格（`vgp_runtime.c`）

1. **文字標籤共通樣式（`apply_label_style()`，套用於 `text`、`number`、`datetime`）**：
   - `text_align` → `lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT / CENTER / RIGHT, 0)`。
   - `word_wrap` → `lv_label_set_long_mode(obj, el->word_wrap ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_CLIP)`。
   - `underline` → 若為 `true`，呼叫 `lv_obj_set_style_text_decor(obj, LV_TEXT_DECOR_UNDERLINE, 0)`。
   - `line_height_x100` → 若 `> 100`，設定 `lv_obj_set_style_text_line_space(obj, ((int32_t)(el->line_height_x100 - 100u) * font->line_height) / 100, 0)`。
   - `text_background` → 若為 `true`，設定 `bg_color = el->background`、`bg_opa = LV_OPA_COVER`。
   - `vertical_align` → 若為 `MIDDLE` 計算 `pad_top = max(0, (eh - font->line_height) / 2)`；若為 `BOTTOM` 計算 `pad_top = max(0, eh - font->line_height)`。
2. **形狀填充與邊框（`apply_box_style_ex()`，套用於 `rectangle`、`circle`）**：
   - `shape_filled == false` → `bg_opa = LV_OPA_TRANSP`（繪製空心外框）。
   - `border_style == VGP_BORDER_NONE` → `border_width = 0`。
   - `border_style == VGP_BORDER_DASHED / DOTTED` → `border_opa = LV_OPA_60`。
3. **按鈕狀態樣式（`apply_button_state_styles()`，套用於 `button`）**：
   - 若 `has_btn_pressed_bg` → `lv_obj_set_style_bg_color(btn, ..., LV_STATE_PRESSED)`、`bg_opa = LV_OPA_COVER`。
   - 若 `has_btn_pressed_color` → `lv_obj_set_style_text_color(lbl, ..., LV_STATE_PRESSED)`（因 LVGL label 需跟隨狀態，可由 `lv_obj_set_style_text_color(btn, ..., LV_STATE_PRESSED)` 繼承，並移除子 label 強制覆蓋或在 `PRESSED/DISABLED` 繼承父按鈕文字色）。
   - 若 `has_btn_disabled_bg` / `has_btn_disabled_color` → 套用於 `LV_STATE_DISABLED`。

---

## 5. 大尺寸數值字體規格（`tools/generate-cjk-font.ps1` / `vgp_font_cjk.h`）

### 5.1 新增字庫定義
於 `tools/generate-cjk-font.ps1` 新增兩組字庫（皆使用 `Microsoft JhengHei` 4bpp 抗鋸齒）：
- **`vgp_font_num_36`**：`pxSize = 36`、`lineHeight = 42`、`baseLine = 8`、`bpp = 4`、`fallback = &vgp_font_zh_22`。
- **`vgp_font_num_48`**：`pxSize = 48`、`lineHeight = 56`、`baseLine = 10`、`bpp = 4`、`fallback = &vgp_font_num_36`。
- **字元集**：ASCII `0x20..0x7E`（95 字）+ 工業符號 `°` (`0x00B0`)、`±` (`0x00B1`)、`µ` (`0x00B5`)、`×` (`0x00D7`)、`÷` (`0x00F7`)、`Ω` (`0x03A9`)、`–` (`0x2013`)、`—` (`0x2014`)、`℃` (`0x2103`)、`℉` (`0x2109`)、`／` (`0xFF0F`)（共 106 字）。

### 5.2 字體選取規則（`vgp_runtime.c`：`select_font()`）
```c
static const lv_font_t *select_font(const char *text, int32_t font_size, int32_t scale)
{
    (void)text;
    int32_t scaled_size = (font_size * scale) / 1000;
    if (scaled_size >= 40) return &vgp_font_num_48;
    if (scaled_size >= 29) return &vgp_font_num_36;
    if (scaled_size >= 19) return &vgp_font_zh_22;
    return &vgp_font_zh_16;
}
```
> 效果驗證：範例 `sdcard/project.json` 中 `溫度顯示`（`fontSize: 42`, `unit: "°C"`）在 960×600 以上解析度會自動選用 36px/48px 大字體清晰呈現（例如 `26.5 °C`），不再縮水成 22px。

---

## 6. 檔案變更清單與驗證計畫

| 檔案 | 類型 | 說明 |
| :--- | :---: | :--- |
| `tools/generate-cjk-font.ps1` | 修改 | 新增 `vgp_font_num_36` 與 `vgp_font_num_48` 產生段落 |
| `vgp_font_cjk.h` | 修改 | 宣告 `LV_FONT_DECLARE(vgp_font_num_36)` 與 `LV_FONT_DECLARE(vgp_font_num_48)` |
| `vgp_model.h` | 修改 | 擴充數值格式、文字排版、形狀邊框與按鈕狀態色欄位 |
| `vgp_parser.c` | 修改 | 解析全部 P2 JSON 屬性與預設值 |
| `vgp_runtime.c` | 修改 | 升級 `format_num_val()`、`select_font()`，新增 `apply_label_style()`、`apply_box_style_ex()` 與按鈕狀態色 |

### 驗證標準
1. 執行 `.\tools\build-and-stage.ps1`：`generate-cjk-font.ps1` 自動重新產出 `vgp_font_cjk.c`、0 warnings / 0 errors（`-Werror`）、`verify-build.ps1` 與 `verify-sdcard.ps1` 全數 PASS。
2. 檢視 `sdcard/project.json` 之 `溫度顯示`（`fontSize: 42`, `decimals: 1`, `unit: "°C"`）與 `共用標題`（`fontSize: 24`）字體大小級距正確且不截斷。
3. 確認 `decimal / integer / percent / scientific`、`useGrouping`、`de-DE`、`numberPrefix`、`shapeFilled: false`、`borderStyle: "none"` 與 `textAlign` 邏輯正確無溢位。
