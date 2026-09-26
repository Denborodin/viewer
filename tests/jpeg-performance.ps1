param([int]$Runs=30)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$generated=Join-Path $root 'tests\fixtures\generated'
$inputJpeg=Join-Path $generated '4mp.jpg'
if(!(Test-Path $inputJpeg)){throw 'Run tests/make-fixtures.ps1 -Benchmark first'}
$folder=Join-Path $generated 'jpeg-speed'
New-Item -ItemType Directory -Force $folder | Out-Null
Add-Type -AssemblyName System.Drawing
$source=[Drawing.Image]::FromFile($inputJpeg)
try {
 $large=[Drawing.Bitmap]::new(6000,4000)
 try {
  $graphics=[Drawing.Graphics]::FromImage($large)
  try {$graphics.DrawImage($source,0,0,6000,4000)} finally {$graphics.Dispose()}
  $large.Save((Join-Path $folder '24mp.jpg'),[Drawing.Imaging.ImageFormat]::Jpeg)
 } finally {$large.Dispose()}
} finally {$source.Dispose()}
$bin=Join-Path $root 'third_party\jpeg-bin\bin'
& (Join-Path $bin 'djpeg.exe') -bmp -outfile (Join-Path $folder '4mp.bmp') $inputJpeg
if($LASTEXITCODE){throw 'djpeg failed'}
foreach($kind in @('progressive','grayscale')) {
 & (Join-Path $bin 'cjpeg.exe') -quality 90 ('-'+$kind) -outfile (Join-Path $folder ($kind+'.jpg')) (Join-Path $folder '4mp.bmp')
 if($LASTEXITCODE){throw 'cjpeg failed'}
}
$bench=Join-Path $root 'dist\Viewer\ViewerJpegBench.exe'
$results=@()
foreach($file in @($inputJpeg,(Join-Path $folder '24mp.jpg'),(Join-Path $folder 'progressive.jpg'),(Join-Path $folder 'grayscale.jpg'))) {
 foreach($backend in @('wic','wic-gpu','turbo-gpu')) {
  $sample=& $bench $file 1920 1080 $Runs $backend | ConvertFrom-Json
  if($LASTEXITCODE){throw 'JPEG benchmark failed'}
  $results+=@{file=[IO.Path]::GetFileName($file);backend=$backend;metrics=$sample}
 }
}
$output=Join-Path $root ('build\jpeg-performance-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.json')
$results | ConvertTo-Json -Depth 6 | Set-Content $output -Encoding UTF8
Write-Output $output
