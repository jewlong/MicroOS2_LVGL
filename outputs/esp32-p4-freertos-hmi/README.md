# ViewGP HMI — ESP32-P4 (ESP-IDF / FreeRTOS SMP) 執行端專案

本專案與 `../pi-zero-w-ucos2-wifi` 共用上層跨平台核心 [`../../vgp_core`](../../vgp_core)，使同一份 `InfoSys_ViewGP_Web` 匯出的 `project.json` 可同時部署於 **Raspberry Pi Zero HDMI 裸機版**與 **ESP32-P4 觸控面板版**。

---

## 1. 目錄與架構分工

```text
MicroOS2LVGL_Proj/
├── vgp_core/                       # [共用單一真實來源] 跨平台 ViewGP C 核心
│   ├── vgp_platform.h              # 平台抽象介面 + PSRAM 區段屬性 (VGP_BSS_ATTR)
│   ├── vgp_model.h / vgp_parser.*  # Schema v1 JSON 解析與資料模型
│   ├── vgp_runtime.*               # LVGL 9 全部 22 種元件渲染、彈窗與動作引擎
│   ├── vgp_alarm.* / vgp_trend.*   # 警報評估引擎、背景環形趨勢取樣
│   ├── vgp_image.* / vgp_font_cjk.*# Base64 PNG/JPEG 快取與 Noto Sans TC 繁中字庫
│   ├── modbus_codec.* / vgp_modbus.h
│   └── sys_clock.*                 # UTC 時區換算與格式化
└── outputs/
    ├── pi-zero-w-ucos2-wifi/       # 樹莓派裸機版 (GNU Make + µC/OS-II + HDMI + USB)
    └── esp32-p4-freertos-hmi/      # [本目錄] ESP32-P4 版 (ESP-IDF CMake + FreeRTOS SMP)
        ├── CMakeLists.txt          # 透過 EXTRA_COMPONENT_DIRS 掛載 ../../vgp_core
        ├── partitions.csv          # 16MB Flash 分區（雙 OTA + 6MB SPIFFS 永久儲存）
        ├── sdkconfig.defaults      # 啟用 PSRAM (.ext_ram.bss)、PPA 2D 硬體加速與 LVGL 9
        └── main/
            ├── app_main.c          # Core 0 通訊 / Core 1 GUI 雙核排程、SPIFFS、SNTP
            ├── vgp_plat_esp32p4.c  # 實作 vgp_platform.h (SMP Spinlock、esp_timer、IP 狀態列)
            ├── esp_vgp_modbus.c    # FreeRTOS + lwIP Socket Modbus TCP 批次輪詢與寫入佇列
            └── esp_http_ops.*      # Port 8080 熱重載 API（上傳後自動寫入 Flash 斷電不遺失）
```

---

## 2. 建置與燒錄指令（Windows ESP-IDF v5.3+ / v5.4+）

> **重要提示**：由於專案位於 Dropbox 同步目錄且路徑較深，建置時請使用 `-B D:\build\p4` 將編譯暫存檔輸出至本機短目錄，避免 Dropbox 鎖檔與 Windows 路徑長度限制。

在開啟 **ESP-IDF 5.3 / 5.4 PowerShell** 後執行：

```powershell
cd d:\Dropbox\Infosystem\MicroOS2LVGL_Proj\outputs\esp32-p4-freertos-hmi

# 1. 設定目標晶片為 esp32p4（套用 sdkconfig.defaults）
idf.py -B D:\build\p4 set-target esp32p4

# 2. 編譯專案
idf.py -B D:\build\p4 build

# 3. 燒錄並開啟序列埠監看（請將 COMx 替換為實際連接埠）
idf.py -B D:\build\p4 -p COMx flash monitor
```

---

## 3. 實機面板與網路硬體對接位置

拿到具體的 ESP32-P4 開發板（例如官方 `ESP32-P4-Function-EV-Board` 或市售 7" / 10.1" MIPI-DSI 智慧中控屏）後，僅需在 [`main/app_main.c`](main/app_main.c) 填入對應板載腳位：

1. **螢幕輸出 (`display_flush_cb`)**：初始化板載 MIPI-DSI 或 RGB 面板（`esp_lcd_new_panel_dpi`），於 `display_flush_cb()` 呼叫 `esp_lcd_panel_draw_bitmap()`。
2. **電容觸控 (`touch_read_cb`)**：初始化 I2C 觸控晶片（如 GT911 / FT5x06，`esp_lcd_touch`），於 `touch_read_cb()` 回報座標並將 `g_p4_touch_connected` 設為 `true`。
3. **網路連線（二選一或並用）**：
   - **有線網路 (RMII / PoE)**：於 `app_main()` 加入 `esp_eth_driver_install`（例如板載 IP101 PHY）。
   - **Wi-Fi 6（板載 ESP32-C6 協同晶片）**：於 [`main/idf_component.yml`](main/idf_component.yml) 取消註解 `espressif/esp_hosted` 與 `espressif/esp_wifi_remote`，即可直接使用標準 `esp_wifi` API。
