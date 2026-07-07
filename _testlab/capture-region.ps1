# Bildschirm-Ausschnitt (echte Pixel) -> JPG. Fuer Captures MIT OSD-Overlay
# (das OSD ist ein eigenes Fenster ueber der App -> PrintWindow reicht nicht).
#   ./capture-region.ps1 -Out pfad.jpg [-X 0 -Y 0 -W 1920 -H 1080] [-Quality 92]
param([Parameter(Mandatory=$true)][string]$Out, [int]$X = 0, [int]$Y = 0, [int]$W = 1920, [int]$H = 1080, [int]$Quality = 92)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cap2 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[Cap2]::SetProcessDPIAware() | Out-Null
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, $bmp.Size)
$g.Dispose()
$enc = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$ep = New-Object System.Drawing.Imaging.EncoderParameters(1)
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, [long]$Quality)
$bmp.Save($Out, $enc, $ep)
$bmp.Dispose()
Write-Host "Gespeichert: $Out ($W x $H @ $X,$Y)"
