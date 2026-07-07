# Screenshot des Lumora-Fensters in NATIVER Aufloesung -> JPG.
#   ./capture.ps1 -Out pfad.jpg [-Quality 92]
# Nutzt PrintWindow(PW_RENDERFULLCONTENT): rendert das FENSTER selbst,
# unabhaengig davon, was darueber liegt (kein Abfotografieren des Bildschirms).
param([Parameter(Mandatory=$true)][string]$Out, [int]$Quality = 92)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cap {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[Cap]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Name "Lumora" -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "Lumora-Fenster nicht gefunden" }
$r = New-Object Cap+RECT
[Cap]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { throw "Fenster-Rect ungueltig ($w x $h)" }
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Cap]::PrintWindow($p.MainWindowHandle, $hdc, 2)   # 2 = PW_RENDERFULLCONTENT (DWM/GPU-Inhalte)
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) { throw "PrintWindow fehlgeschlagen" }
$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)
$bmp.Save($Out, $enc, $ep)
$bmp.Dispose()
Write-Host "Gespeichert: $Out ($w x $h)"
