# RetroBug — generate the iOS AppIcon set from the same source as the Android launcher icon.
# SPDX-License-Identifier: Apache-2.0
#
# iOS icons have no transparency (transparent pixels render black), and an app icon is a square
# full-bleed image; this scales the source onto its own near-black background and writes every size the
# asset catalog asks for. Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File tools/make_ios_icon.ps1 [-Source <png>]
param([string]$Source = 'file_0000000033448211bbdb620eed523634.png')
Add-Type -AssemblyName System.Drawing

$src = Join-Path (Get-Location) $Source
if (-not (Test-Path $src)) { Write-Error "no source image at $src"; exit 1 }
$outDir = Join-Path (Get-Location) 'ios/RetroBug/Assets.xcassets/AppIcon.appiconset'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$image = [System.Drawing.Image]::FromFile($src)
Write-Host "source: $($image.Width)x$($image.Height)"

# Sizes iOS asks for (pixels). One file per size, referenced by the idioms that need it.
$sizes = @(20, 29, 40, 58, 60, 76, 80, 87, 120, 152, 167, 180, 1024)
foreach ($size in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  # The source's own corner is the background colour, so the icon keeps the Android launcher's look.
  $g.Clear([System.Drawing.Color]::FromArgb(1, 1, 1))
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
  $g.DrawImage($image, 0, 0, $size, $size)
  $g.Dispose()
  $path = Join-Path $outDir ("icon-{0}.png" -f $size)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host "wrote $path"
}
$image.Dispose()
