param([string]$RarExe='', [switch]$Benchmark)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=Join-Path $PSScriptRoot 'fixtures\generated'
$images=Join-Path $out 'images'
New-Item -ItemType Directory -Force -Path $images | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
for($n=1;$n -le 12;$n++){
 $bitmap=[Drawing.Bitmap]::new(640,400)
 $g=[Drawing.Graphics]::FromImage($bitmap)
 $g.Clear([Drawing.Color]::FromArgb(255,15+$n*7,28+$n*5,50+$n*6))
 $brush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,70,180,230))
 $g.FillEllipse($brush,40+$n*8,40,190,190)
 $font=[Drawing.Font]::new('Segoe UI',42)
 $g.DrawString(('Viewer '+$n),$font,[Drawing.Brushes]::White,45,285)
 $bitmap.Save((Join-Path $images ($n.ToString()+'.png')),[Drawing.Imaging.ImageFormat]::Png)
 if($n -eq 1){foreach($format in @(@('photo.jpg',[Drawing.Imaging.ImageFormat]::Jpeg),@('bitmap.bmp',[Drawing.Imaging.ImageFormat]::Bmp),@('first.gif',[Drawing.Imaging.ImageFormat]::Gif),@('first.tiff',[Drawing.Imaging.ImageFormat]::Tiff))){$bitmap.Save((Join-Path $images $format[0]),$format[1])}}
 $brush.Dispose();$font.Dispose();$g.Dispose();$bitmap.Dispose()
}
function Create-Zip([string]$name,[IO.Compression.CompressionLevel]$level,[int]$count=12,[switch]$Zip64){
 $file=Join-Path $out $name
 $stream=[IO.File]::Open($file,[IO.FileMode]::Create)
 $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
 for($i=$count;$i -ge 1;$i--){$entry=$zip.CreateEntry(('страницы/'+$i+'.png'),$level);$target=$entry.Open();$bytes=[IO.File]::ReadAllBytes((Join-Path $images ((1+($i-1)%12).ToString()+'.png')));$target.Write($bytes,0,$bytes.Length);$target.Dispose()}
 foreach($index in @(1,2)){$entry=$zip.CreateEntry('повтор.png',$level);$target=$entry.Open();$bytes=[IO.File]::ReadAllBytes((Join-Path $images ($index.ToString()+'.png')));$target.Write($bytes,0,$bytes.Length);$target.Dispose()}
 if($Zip64){for($i=0;$i -lt 65536;$i++){$null=$zip.CreateEntry(('metadata/'+$i+'.txt'))}}
 $zip.Dispose();$stream.Dispose()
}
Create-Zip 'stored.zip' ([IO.Compression.CompressionLevel]::NoCompression)
Create-Zip 'deflate.zip' ([IO.Compression.CompressionLevel]::Optimal)
Create-Zip 'zip64.zip' ([IO.Compression.CompressionLevel]::NoCompression) -Zip64
Create-Zip '10000.zip' ([IO.Compression.CompressionLevel]::NoCompression) 10000
[IO.File]::WriteAllBytes((Join-Path $images 'broken.png'),[byte[]]@(1,2,3,4,5))
[IO.File]::WriteAllBytes((Join-Path $out 'broken.zip'),[byte[]]@(80,75,3,4,0,0,0,0))
if($RarExe){
 $rarPath=[IO.Path]::GetFullPath($RarExe)
 foreach($version in @(4,5)){foreach($solid in @($false,$true)){
  $name='rar'+$version+$(if($solid){'-solid'}else{''})+'.rar'
  $archive=Join-Path $out $name
  if(Test-Path -LiteralPath $archive){Remove-Item -LiteralPath $archive}
  $solidFlag=if($solid){'-s'}else{'-s-'}
  & $rarPath a ('-ma'+$version) $solidFlag '-m3' '-ep1' '-idq' $archive (Join-Path $images '*.png')
  if($LASTEXITCODE){throw ('RAR fixture failed: '+$name)}
 }}
 foreach($suffix in @('password','volumes')){
 $archive=Join-Path $out ($suffix+'.rar')
  Get-ChildItem -LiteralPath $out -Filter ($suffix+'*.rar') | ForEach-Object {Remove-Item -LiteralPath $_.FullName}
  $extra=if($suffix -eq 'password'){'-hpviewer'}else{'-v256k'}
  $inputFile=if($suffix -eq 'password'){'1.png'}else{'bitmap.bmp'}
  & $rarPath a '-ma5' '-m0' '-ep1' '-idq' $extra $archive (Join-Path $images $inputFile)
  if($LASTEXITCODE){throw 'RAR unsupported fixture failed'}
 }
}else{Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'fixtures') -Filter '*.rar' | Copy-Item -Destination $out -Force}
if($Benchmark){
 $bitmap=[Drawing.Bitmap]::new(2560,1600)
 $g=[Drawing.Graphics]::FromImage($bitmap)
 $g.Clear([Drawing.Color]::FromArgb(25,40,70))
 $random=[Random]::new(714)
 for($i=0;$i -lt 30000;$i++){$brush=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,$random.Next(256),$random.Next(256),$random.Next(256)));$g.FillRectangle($brush,$random.Next(2560),$random.Next(1600),$random.Next(4,40),$random.Next(4,40));$brush.Dispose()}
 $sample=Join-Path $out '4mp.jpg';$bitmap.Save($sample,[Drawing.Imaging.ImageFormat]::Jpeg);$g.Dispose();$bitmap.Dispose()
 $bytes=[IO.File]::ReadAllBytes($sample)
 foreach($mode in @('stored','deflate')){
  $stream=[IO.File]::Open((Join-Path $out ('1000-4mp-'+$mode+'.zip')),[IO.FileMode]::Create)
  $zip=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create)
  $level=if($mode -eq 'stored'){[IO.Compression.CompressionLevel]::NoCompression}else{[IO.Compression.CompressionLevel]::Optimal}
  for($i=1;$i -le 1000;$i++){$entry=$zip.CreateEntry(($i.ToString()+'.jpg'),$level);$target=$entry.Open();$target.Write($bytes,0,$bytes.Length);$target.Dispose()}
  $zip.Dispose();$stream.Dispose()
 }
}
Write-Output $out
