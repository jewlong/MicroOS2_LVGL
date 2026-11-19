$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$outFile = Join-Path $project 'vgp_font_cjk.c'
$jsonFile = Join-Path $project 'sdcard\project.json'

if (Test-Path -LiteralPath $outFile) {
    $outTime = (Get-Item -LiteralPath $outFile).LastWriteTimeUtc
    $scriptTime = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
    $jsonNewer = (Test-Path -LiteralPath $jsonFile) -and ((Get-Item -LiteralPath $jsonFile).LastWriteTimeUtc -gt $outTime)
    if (($outTime -ge $scriptTime) -and (-not $jsonNewer)) {
        Write-Output 'vgp_font_cjk.c is up to date.'
        exit 0
    }
}

Add-Type -AssemblyName System.Drawing

$code = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public class LvglFontGenerator
{
    public class GlyphInfo
    {
        public int Codepoint;
        public int BitmapIndex;
        public int AdvW;
        public int BoxW;
        public int BoxH;
        public int OfsX;
        public int OfsY;
    }

    public static void GenerateFontSection(
        StringBuilder sb,
        string fontVarName,
        string fallbackVarName,
        int pxSize,
        int lineHeight,
        int baseLine,
        int bpp,
        SortedSet<int> codepoints)
    {
        string fontName = "Microsoft JhengHei";
        using (Font testFont = new Font(fontName, pxSize, FontStyle.Regular, GraphicsUnit.Pixel))
        {
            if (testFont.Name != fontName)
            {
                fontName = "PMingLiU";
            }
        }

        int canvasW = pxSize * 2 + 8;
        int canvasH = lineHeight + 8;
        int yBaseline = lineHeight - baseLine;

        List<byte> bitmapBytes = new List<byte>(512 * 1024);
        List<GlyphInfo> glyphs = new List<GlyphInfo>(codepoints.Count + 1);
        glyphs.Add(new GlyphInfo { Codepoint = 0, BitmapIndex = 0, AdvW = 0, BoxW = 0, BoxH = 0, OfsX = 0, OfsY = 0 });

        using (Font font = new Font(fontName, pxSize, FontStyle.Regular, GraphicsUnit.Pixel))
        using (Bitmap bmp = new Bitmap(canvasW, canvasH, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.SmoothingMode = SmoothingMode.HighQuality;
            StringFormat sf = new StringFormat(StringFormat.GenericTypographic);
            sf.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;

            byte[] rawPixels = new byte[canvasW * canvasH * 4];

            foreach (int cp in codepoints)
            {
                string s = char.ConvertFromUtf32(cp);
                g.Clear(Color.FromArgb(0, 0, 0, 0));

                float drawY = 1.0f;
                g.DrawString(s, font, Brushes.White, 1.0f, drawY, sf);

                BitmapData bd = bmp.LockBits(new Rectangle(0, 0, canvasW, canvasH), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                Marshal.Copy(bd.Scan0, rawPixels, 0, rawPixels.Length);
                bmp.UnlockBits(bd);

                int minX = canvasW, maxX = -1, minY = canvasH, maxY = -1;
                for (int y = 0; y < canvasH; y++)
                {
                    int rowStart = y * canvasW * 4;
                    for (int x = 0; x < canvasW; x++)
                    {
                        byte alpha = rawPixels[rowStart + x * 4 + 3];
                        if (alpha >= 16)
                        {
                            if (x < minX) minX = x;
                            if (x > maxX) maxX = x;
                            if (y < minY) minY = y;
                            if (y > maxY) maxY = y;
                        }
                    }
                }

                float measuredW = g.MeasureString(s, font, 1000, sf).Width;
                int advPx;
                if (cp == 32)
                {
                    advPx = Math.Max(4, pxSize / 3);
                }
                else if (cp >= 0x2E80)
                {
                    advPx = pxSize;
                }
                else
                {
                    advPx = (int)Math.Ceiling(measuredW);
                    if (maxX >= minX && (maxX - minX + 2) > advPx)
                        advPx = maxX - minX + 2;
                    if (advPx < 4) advPx = 4;
                }

                if (maxX < minX || maxY < minY)
                {
                    glyphs.Add(new GlyphInfo
                    {
                        Codepoint = cp,
                        BitmapIndex = bitmapBytes.Count,
                        AdvW = advPx * 16,
                        BoxW = 0,
                        BoxH = 0,
                        OfsX = 0,
                        OfsY = 0
                    });
                    continue;
                }

                int boxW = maxX - minX + 1;
                int boxH = maxY - minY + 1;
                int ofsX = Math.Max(0, minX - 1);
                int ofsY = yBaseline - (maxY + 1);
                int bmpIdx = bitmapBytes.Count;

                if (bpp == 2)
                {
                    int bitPos = 0;
                    byte curByte = 0;
                    for (int y = minY; y <= maxY; y++)
                    {
                        int rowStart = y * canvasW * 4;
                        for (int x = minX; x <= maxX; x++)
                        {
                            byte a = rawPixels[rowStart + x * 4 + 3];
                            int v = (a + 42) / 85;
                            if (v > 3) v = 3;
                            curByte = (byte)((curByte << 2) | v);
                            bitPos += 2;
                            if (bitPos == 8)
                            {
                                bitmapBytes.Add(curByte);
                                curByte = 0;
                                bitPos = 0;
                            }
                        }
                    }
                    if (bitPos > 0)
                    {
                        curByte = (byte)(curByte << (8 - bitPos));
                        bitmapBytes.Add(curByte);
                    }
                }
                else // bpp == 4
                {
                    int nibblePos = 0;
                    byte curByte = 0;
                    for (int y = minY; y <= maxY; y++)
                    {
                        int rowStart = y * canvasW * 4;
                        for (int x = minX; x <= maxX; x++)
                        {
                            byte a = rawPixels[rowStart + x * 4 + 3];
                            int v = (a + 8) / 17;
                            if (v > 15) v = 15;
                            if (nibblePos == 0)
                            {
                                curByte = (byte)(v << 4);
                                nibblePos = 1;
                            }
                            else
                            {
                                curByte |= (byte)v;
                                bitmapBytes.Add(curByte);
                                curByte = 0;
                                nibblePos = 0;
                            }
                        }
                    }
                    if (nibblePos > 0)
                    {
                        bitmapBytes.Add(curByte);
                    }
                }

                glyphs.Add(new GlyphInfo
                {
                    Codepoint = cp,
                    BitmapIndex = bmpIdx,
                    AdvW = advPx * 16,
                    BoxW = boxW,
                    BoxH = boxH,
                    OfsX = ofsX,
                    OfsY = ofsY
                });
            }
        }

        sb.AppendLine(string.Format("static LV_ATTRIBUTE_LARGE_CONST const uint8_t {0}_bitmap[] = {{", fontVarName));
        for (int i = 0; i < bitmapBytes.Count; i++)
        {
            if (i % 16 == 0) sb.Append("    ");
            sb.AppendFormat("0x{0:x2},", bitmapBytes[i]);
            if (i % 16 == 15 || i == bitmapBytes.Count - 1) sb.AppendLine();
        }
        sb.AppendLine("};");
        sb.AppendLine();

        sb.AppendLine(string.Format("static const lv_font_fmt_txt_glyph_dsc_t {0}_glyph_dsc[] = {{", fontVarName));
        for (int i = 0; i < glyphs.Count; i++)
        {
            var g = glyphs[i];
            sb.AppendLine(string.Format("    {{.bitmap_index = {0}, .adv_w = {1}, .box_w = {2}, .box_h = {3}, .ofs_x = {4}, .ofs_y = {5}}},",
                g.BitmapIndex, g.AdvW, g.BoxW, g.BoxH, g.OfsX, g.OfsY));
        }
        sb.AppendLine("};");
        sb.AppendLine();

        int firstSparseIdx = 96; // After 95 ASCII chars (32..126)
        int firstSparseCp = glyphs[firstSparseIdx].Codepoint;
        int lastSparseCp = glyphs[glyphs.Count - 1].Codepoint;
        int rangeLen = lastSparseCp - firstSparseCp + 1;
        int sparseCount = glyphs.Count - firstSparseIdx;

        sb.AppendLine(string.Format("static const uint16_t {0}_unicode_list[] = {{", fontVarName));
        for (int i = firstSparseIdx; i < glyphs.Count; i++)
        {
            int idx = i - firstSparseIdx;
            if (idx % 12 == 0) sb.Append("    ");
            sb.AppendFormat("0x{0:x4},", glyphs[i].Codepoint - firstSparseCp);
            if (idx % 12 == 11 || i == glyphs.Count - 1) sb.AppendLine();
        }
        sb.AppendLine("};");
        sb.AppendLine();

        sb.AppendLine(string.Format("static const lv_font_fmt_txt_cmap_t {0}_cmaps[] = {{", fontVarName));
        sb.AppendLine("    {");
        sb.AppendLine("        .range_start = 32, .range_length = 95, .glyph_id_start = 1,");
        sb.AppendLine("        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY");
        sb.AppendLine("    },");
        sb.AppendLine("    {");
        sb.AppendLine(string.Format("        .range_start = {0}, .range_length = {1}, .glyph_id_start = {2},", firstSparseCp, rangeLen, firstSparseIdx));
        sb.AppendLine(string.Format("        .unicode_list = {0}_unicode_list, .glyph_id_ofs_list = NULL, .list_length = {1}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY", fontVarName, sparseCount));
        sb.AppendLine("    }");
        sb.AppendLine("};");
        sb.AppendLine();

        sb.AppendLine(string.Format("static const lv_font_fmt_txt_dsc_t {0}_dsc = {{", fontVarName));
        sb.AppendLine(string.Format("    .glyph_bitmap = {0}_bitmap,", fontVarName));
        sb.AppendLine(string.Format("    .glyph_dsc = {0}_glyph_dsc,", fontVarName));
        sb.AppendLine(string.Format("    .cmaps = {0}_cmaps,", fontVarName));
        sb.AppendLine("    .kern_dsc = NULL,");
        sb.AppendLine("    .kern_scale = 0,");
        sb.AppendLine("    .cmap_num = 2,");
        sb.AppendLine(string.Format("    .bpp = {0},", bpp));
        sb.AppendLine("    .kern_classes = 0,");
        sb.AppendLine("    .bitmap_format = 0");
        sb.AppendLine("};");
        sb.AppendLine();

        sb.AppendLine(string.Format("const lv_font_t {0} = {{", fontVarName));
        sb.AppendLine("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,");
        sb.AppendLine("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,");
        sb.AppendLine(string.Format("    .line_height = {0},", lineHeight));
        sb.AppendLine(string.Format("    .base_line = {0},", baseLine));
        sb.AppendLine("    .subpx = LV_FONT_SUBPX_NONE,");
        sb.AppendLine("    .underline_position = -2,");
        sb.AppendLine("    .underline_thickness = 1,");
        sb.AppendLine(string.Format("    .dsc = &{0}_dsc,", fontVarName));
        sb.AppendLine(string.Format("    .fallback = {0}", fallbackVarName));
        sb.AppendLine("};");
        sb.AppendLine();
    }
}
'@

Add-Type -TypeDefinition $code -ReferencedAssemblies System.Drawing

$set16 = New-Object 'System.Collections.Generic.SortedSet[int]'
$set22 = New-Object 'System.Collections.Generic.SortedSet[int]'

# 1. Printable ASCII (32..126)
for ($cp = 32; $cp -le 126; $cp++) {
    [void]$set16.Add($cp)
    [void]$set22.Add($cp)
}

# 2. Common symbols & units as pure ASCII hex codepoints (no encoding issues on CP950 PowerShell)
$symbolCps = @(
    0x00B0, 0x00B1, 0x00D7, 0x00F7, 0x03BC, 0x03A9,
    0x2013, 0x2014, 0x2026, 0x2030, 0x2103, 0x2109,
    0x2190, 0x2191, 0x2192, 0x2193, 0x221E, 0x2248,
    0x2260, 0x2264, 0x2265, 0x25A0, 0x25A1, 0x25B2,
    0x25BC, 0x25CB, 0x25CF, 0x2605, 0x2606, 0x3001,
    0x3002, 0x3008, 0x3009, 0x300A, 0x300B, 0x300C,
    0x300D, 0x300E, 0x300F, 0x3010, 0x3011, 0xFF01,
    0xFF05, 0xFF08, 0xFF09, 0xFF0C, 0xFF0F, 0xFF1A,
    0xFF1B, 0xFF1F, 0xFF5C
)
foreach ($cp in $symbolCps) {
    [void]$set16.Add($cp)
    [void]$set22.Add($cp)
}

# 3. Extract all characters from sdcard/project.json (if present)
if (Test-Path -LiteralPath $jsonFile) {
    $jsonText = [IO.File]::ReadAllText($jsonFile, [Text.Encoding]::UTF8)
    foreach ($ch in $jsonText.ToCharArray()) {
        $cp = [int][char]$ch
        if ($cp -gt 126 -and $cp -le 0xFFFF) {
            [void]$set16.Add($cp)
            [void]$set22.Add($cp)
        }
    }
}

# 4. Add ALL 5,401 Big5 Level-1 Standard Traditional Chinese characters & symbols to 16px,
#    and Big5 symbols + top high-frequency characters to 22px
$big5 = [System.Text.Encoding]::GetEncoding(950)
$buf = New-Object byte[] 2
for ($lead = 0xA1; $lead -le 0xC6; $lead++) {
    $buf[0] = [byte]$lead
    for ($trail = 0x40; $trail -le 0xFE; $trail++) {
        if ($trail -ge 0x7F -and $trail -le 0xA0) { continue }
        $buf[1] = [byte]$trail
        $str = $big5.GetString($buf)
        if ($str.Length -eq 1) {
            $cp = [int][char]$str[0]
            if ($cp -gt 126 -and $cp -le 0xFAFF -and ($cp -lt 0xE000 -or $cp -gt 0xF8FF)) {
                [void]$set16.Add($cp)
                if ($lead -le 0xA8) {
                    [void]$set22.Add($cp)
                }
            }
        }
    }
}

Write-Output "Generating vgp_font_cjk.c: 16px ($($set16.Count) glyphs), 22px ($($set22.Count) glyphs)..."
$sb = New-Object System.Text.StringBuilder(4 * 1024 * 1024)
[void]$sb.AppendLine("/* Auto-generated Traditional Chinese LVGL 9 Font (Microsoft JhengHei) */")
[void]$sb.AppendLine('#include "vgp_font_cjk.h"')
[void]$sb.AppendLine('#include "src/font/lv_font_fmt_txt.h"')
[void]$sb.AppendLine()

[LvglFontGenerator]::GenerateFontSection($sb, "vgp_font_zh_16", "&lv_font_source_han_sans_sc_16_cjk", 16, 20, 4, 2, $set16)
[LvglFontGenerator]::GenerateFontSection($sb, "vgp_font_zh_22", "&vgp_font_zh_16", 22, 27, 5, 4, $set22)

[IO.File]::WriteAllText($outFile, $sb.ToString(), [Text.Encoding]::ASCII)
Write-Output "Generated $outFile ($(([IO.FileInfo]$outFile).Length) bytes)."
