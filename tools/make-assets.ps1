$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
$root=Split-Path -Parent $PSScriptRoot
$assets=Join-Path $root 'packaging\Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
function Make-Logo([int]$size,[string]$file){
 $bitmap=[Drawing.Bitmap]::new($size,$size)
 $g=[Drawing.Graphics]::FromImage($bitmap)
 $g.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
 $g.Clear([Drawing.Color]::FromArgb(255,22,28,39))
 $scale=$size/256.0
 $g.ScaleTransform($scale,$scale)
 $pen=[Drawing.Pen]::new([Drawing.Color]::FromArgb(255,91,178,255),13)
 $g.DrawRectangle($pen,38,46,180,164)
 $brush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,107,206,179))
 $points=[Drawing.PointF[]]@([Drawing.PointF]::new(51,191),[Drawing.PointF]::new(106,115),[Drawing.PointF]::new(144,156),[Drawing.PointF]::new(174,125),[Drawing.PointF]::new(207,191))
 $g.FillPolygon($brush,$points)
 $g.FillEllipse([Drawing.Brushes]::White,164,73,26,26)
 $bitmap.Save($file,[Drawing.Imaging.ImageFormat]::Png)
 $g.Dispose();$pen.Dispose();$brush.Dispose();$bitmap.Dispose()
}
Make-Logo 256 (Join-Path $assets 'Icon256.png')
Make-Logo 150 (Join-Path $assets 'Logo150.png')
Make-Logo 44 (Join-Path $assets 'Logo44.png')
Make-Logo 50 (Join-Path $assets 'StoreLogo.png')
$png=[IO.File]::ReadAllBytes((Join-Path $assets 'Icon256.png'))
$stream=[IO.File]::Create((Join-Path $root 'src\viewer.ico'))
$writer=[IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]1)
$writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([byte]0)
$writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$png.Length);$writer.Write([uint32]22);$writer.Write($png)
$writer.Dispose()
