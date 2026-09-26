param([string]$RarExe='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$generated=Join-Path $root 'tests\fixtures\generated'
$images=Join-Path $generated 'psd'
& (Join-Path $root 'build\Release\ViewerPsdTests.exe') $images
if($LASTEXITCODE){throw 'PSD decoder tests failed'}
$zip=Join-Path $generated 'psd-images.zip'
Compress-Archive -Path (Join-Path $images '*.psd') -DestinationPath $zip -Force
$sources=@($images,$zip)
if($RarExe){
 $rar=Join-Path $generated 'psd-images.rar'
 & $RarExe a -ma5 -s -ep -idq $rar (Join-Path $images '*.psd')
 if($LASTEXITCODE){throw 'RAR fixture failed'}
 $sources+=$rar
}
$previous=$env:VIEWER_DATA_DIR
try {
 $env:VIEWER_DATA_DIR=Join-Path $root ('build\psd-archives-'+[Guid]::NewGuid().ToString('N'))
 foreach($source in $sources){
  Write-Output $source
  & (Join-Path $root 'build\Release\ViewerBench.exe') $source 2
  if($LASTEXITCODE){throw 'PSD source pipeline failed'}
 }
} finally {$env:VIEWER_DATA_DIR=$previous}
