$ErrorActionPreference='Stop'
$out=Join-Path $PSScriptRoot 'fixtures\generated'
$seven=Join-Path $env:ProgramFiles '7-Zip\7z.exe'
if(!(Test-Path -LiteralPath (Join-Path $out 'images\bitmap.bmp'))){throw 'Run make-fixtures.ps1 first'}
foreach($spec in @(@('plain.7z','-ms=off'),@('solid.7z','-ms=on'),@('split.7z','-v64k'),@('split.zip','-v64k'))){
 $target=Join-Path $out $spec[0]
 Get-ChildItem -LiteralPath $out -File | Where-Object {$_.Name -eq $spec[0] -or $_.Name.StartsWith($spec[0]+'.')} | ForEach-Object {Remove-Item -LiteralPath $_.FullName}
 $compression=if($spec[0] -eq 'solid.7z'){'-mx=1'}else{'-mx=0'}
 & $seven a $target (Join-Path $out 'images\*.png') (Join-Path $out 'images\bitmap.bmp') $compression $spec[1] '-bso0' '-bsp0'
 if($LASTEXITCODE){throw '7z fixture creation failed'}
}
# Genuine ZIP disk volumes (.z01 ... .zip): central directory disk fields and
# local-header offset are relative to their respective disks (APPNOTE layout).
$inputZip=Join-Path $out 'zip-volume-input.zip'
if(Test-Path -LiteralPath $inputZip){Remove-Item -LiteralPath $inputZip}
& $seven a $inputZip (Join-Path $out 'images\bitmap.bmp') '-mx=0' '-bso0' '-bsp0'
if($LASTEXITCODE){throw 'ZIP fixture creation failed'}
[byte[]]$bytes=[IO.File]::ReadAllBytes($inputZip)
$eocd=$bytes.Length-22
$central=[BitConverter]::ToUInt32($bytes,$eocd+16)
$capacity=65536
# Put the central directory and EOCD together on a final disk.
$parts=[int][Math]::Ceiling($central / [double]$capacity)
[BitConverter]::GetBytes([uint16]$parts).CopyTo($bytes,$eocd+4)
[BitConverter]::GetBytes([uint16]$parts).CopyTo($bytes,$eocd+6)
[BitConverter]::GetBytes([uint32]0).CopyTo($bytes,$eocd+16)
for($i=0;$i -lt $parts;$i++){
 $offset=$i*$capacity
 $length=[Math]::Min($capacity,$central-$offset)
 $chunk=[byte[]]::new($length)
 [Array]::Copy($bytes,$offset,$chunk,0,$length)
 [IO.File]::WriteAllBytes((Join-Path $out ('disk.z'+($i+1).ToString('00'))),$chunk)
}
$last=[byte[]]::new($bytes.Length-$central)
[Array]::Copy($bytes,$central,$last,0,$last.Length)
[IO.File]::WriteAllBytes((Join-Path $out 'disk.zip'),$last)
