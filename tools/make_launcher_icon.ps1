# RetroBug — make the launcher icon densities from the source PNG.
#
# `tools/build_launcher_apk.sh` has no image tooling and this machine has no ImageMagick or Python, so the
# resizing is done with .NET's System.Drawing, which every Windows image already has. Run from the repo
# root:  powershell -ExecutionPolicy Bypass -File tools/make_launcher_icon.ps1 [-Source tools/icon/retrobug-icon.png]
#
# The default source is the RetroBug bug (tools/icon/retrobug-icon.png). The previous icon — the Android
# robot — is still at the repo root and can be regenerated with `-Source file_0000000033448211bbdb620eed523634.png`.
#
# Two things about a source that decide the output, both measured by the script and printed:
#   * a 24-bit RGB source with **no alpha channel** and an opaque near-black background (the RetroBug icon's
#     corner pixel is 11,15,20) would ship as a black square on the launcher;
#   * the figure itself keeps clear of "near-black", so the key is safe: no part of it is that dark.
# The key is therefore written as transparency, and the scaled copies are drawn in premultiplied-alpha
# format so the downscale does not leave a grey halo where the black met the outline.
param([string]$Source = 'tools/icon/retrobug-icon.png')
Add-Type -AssemblyName System.Drawing

$src = Join-Path (Get-Location) $Source
if (-not (Test-Path $src)) { Write-Error "no source image at $src"; exit 1 }
$KeyBelow = 16   # every channel below this counts as background

$flat = New-Object System.Drawing.Bitmap($src)
Write-Host "source: $($flat.Width)x$($flat.Height) format=$($flat.PixelFormat)"
$corner = $flat.GetPixel(0, 0)
Write-Host "corner pixel: A=$($corner.A) R=$($corner.R) G=$($corner.G) B=$($corner.B)"

# --- key the background out, once, at full resolution
$keyed = New-Object System.Drawing.Bitmap($flat.Width, $flat.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $flat.Width, $flat.Height)
$data = $keyed.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $keyed.PixelFormat)
$stride = $data.Stride
$bytes = New-Object byte[] ($stride * $flat.Height)
for ($y = 0; $y -lt $flat.Height; $y++) {
  for ($x = 0; $x -lt $flat.Width; $x++) {
    $p = $flat.GetPixel($x, $y)
    $i = $y * $stride + $x * 4
    if ($p.R -lt $KeyBelow -and $p.G -lt $KeyBelow -and $p.B -lt $KeyBelow) {
      $bytes[$i] = 0; $bytes[$i + 1] = 0; $bytes[$i + 2] = 0; $bytes[$i + 3] = 0
    } else {
      $bytes[$i] = $p.B; $bytes[$i + 1] = $p.G; $bytes[$i + 2] = $p.R; $bytes[$i + 3] = 255
    }
  }
}
[System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
$keyed.UnlockBits($data)
$flat.Dispose()

$opaque = 0
for ($y = 0; $y -lt $keyed.Height; $y += 8) {
  for ($x = 0; $x -lt $keyed.Width; $x += 8) { if ($keyed.GetPixel($x, $y).A -ne 0) { $opaque++ } }
}
Write-Host "sampled non-transparent pixels: $opaque (of $([int]($keyed.Width*$keyed.Height/64)))"

# The five densities Android picks between. Sizes are the platform's own for a legacy launcher icon.
$densities = @{ 'mdpi' = 48; 'hdpi' = 72; 'xhdpi' = 96; 'xxhdpi' = 144; 'xxxhdpi' = 192 }
foreach ($name in $densities.Keys) {
  $size = $densities[$name]
  $dir = Join-Path (Get-Location) "android/res/mipmap-$name"
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  # Premultiplied alpha for the destination: it is what makes a bicubic downscale of a hard-edged
  # transparent image not produce dark fringes.
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)
  $g.DrawImage($keyed, 0, 0, $size, $size)
  $g.Dispose()
  $out = Join-Path $dir 'ic_launcher.png'
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ("wrote {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
}
$keyed.Dispose()
