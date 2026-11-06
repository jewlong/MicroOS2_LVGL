# Windows 11 建置與 microSD 製作

適用本 Wi-Fi + LVGL + HDMI 專案；HDMI 接線、畫面及診斷請先讀 README.md。

## 1. Windows 11 重建

請解壓到較短的路徑，例如 C:\pizero-wifi，再用 VS Code 開啟。此次乾淨重建曾在過深路徑遇到 LVGL 的多層相對標頭引用超過 Windows 工具鏈路徑長度，出現 lv_conf_internal.h 找不到；移至較短路徑即可。避免將專案放在多層重複的解壓資料夾。

工具與第一階段相同：

- [Arm 官方 Windows 安裝指南](https://learn.arm.com/install-guides/gcc/arm-gnu/)：選 arm-none-eabi 裸機工具鏈。本次驗證 Arm GNU Toolchain 14.3.Rel1 / GCC 14.3.1。
- [VS Code](https://code.visualstudio.com/)：安裝 Microsoft C/C++ 擴充套件。
- [MSYS2 原生 GNU Make 套件](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-make)：在已更新的 MSYS2 UCRT64 終端執行以下指令。

~~~sh
pacman -S --needed mingw-w64-ucrt-x86_64-make
~~~

將 Arm 工具鏈的 bin 及 C:\msys64\ucrt64\bin 加到使用者 PATH；重開 PowerShell / VS Code。後續在 **Windows PowerShell**、本專案根目錄執行：

~~~powershell
arm-none-eabi-gcc --version
mingw32-make --version
mingw32-make -j4 all
mingw32-make inspect
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sources.ps1
mingw32-make stage
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\update-sd-manifest.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sdcard.ps1
~~~

修改後重建會依 .d header dependencies 更新。需要從零建置時，先單獨執行 mingw32-make clean，再執行 all；不要把 clean 與平行 all 放在同一次命令。

Makefile 使用 Windows cmd.exe、mkdir、copy、rmdir；請用 mingw32-make，不用 MSYS make.exe。專案路徑建議短且不含空白或非 ASCII 字元。以 VS Code「開啟資料夾」開啟本專案，Ctrl+Shift+B 即可建置；也提供 Inspect ELF 與 Stage task。

編譯選項固定 ARM1176、ARM 狀態、little endian、soft-float。連結工具鏈 libgcc 運算支援及 Newlib 字串／數字轉換函式；malloc 與 printf 由網路適配提供，無主機 syscall。LVGL 與網路配置器各使用獨立 512 KiB 記憶體池，標準 C 標頭須完整安裝。所有任務、IRQ 和核心都須使用相同 ABI；不可混入 Cortex-M、Thumb-2 或硬浮點物件。

stage 只更新本專案 sdcard/kernel.img 與 config.txt，**不會寫入實體卡**。程式重建後必須更新 manifest，再複製到卡上。不要用「更新 manifest」掩蓋意外損壞；原始版本可與交付 ZIP 比對。

## 2. 新卡建立 MBR + FAT32

已能跑第一階段的卡可以直接沿用，無須重新格式化。

新卡建議至少 4 GB。以下 clean 會清除所選磁碟的全部分割區，先備份並核對卡的容量、型號與編號。這些指令由你手動執行，本專案沒有自動格式化工具。

以系統管理員開啟 PowerShell：

~~~powershell
Get-Disk | Format-Table Number,FriendlyName,BusType,Size,PartitionStyle
diskpart
~~~

先在 DiskPart 核對目標；N 改為實際卡的編號：

~~~text
list disk
select disk N
detail disk
~~~

確定是預定清除的 microSD 後，逐行執行：

~~~text
clean
convert mbr
create partition primary size=1024
format fs=fat32 quick label=RPIBOOT
assign letter=R
exit
~~~

R 必須尚未使用。這會建立 1 GiB FAT32 第一主分割區，其餘容量保持未配置。回到專案目錄：

~~~powershell
Copy-Item -Path .\sdcard\* -Destination R:\ -Force
Get-Volume -DriveLetter R | Format-List DriveLetter,FileSystem,FileSystemLabel
Get-Partition -DriveLetter R | Get-Disk | Format-List Number,PartitionStyle
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify-sdcard.ps1 -Root R:\
~~~

確認 FAT32、MBR、六個檔案校驗皆 PASS。安全退出、重新插卡，再做一次讀回校驗後安全退出，才放入 Zero W。操作語意可查 Microsoft [clean](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/clean)、[MBR](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/convert-mbr) 與 [主分割區](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/create-partition-primary) 文件。

