$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$third=Join-Path $root 'third_party'
$lock=Get-Content (Join-Path $third 'dependencies.json') -Raw | ConvertFrom-Json
foreach($item in @(@('7zipSource','7zip-src.tar.xz'),@('7zipBinary','7zip-x64.exe'),@('libwebp','libwebp.tar.gz'),@('zlib','zlib.tar.gz'),@('jpegSource','libjpeg-turbo.tar.gz'),@('jpegBinary','libjpeg-turbo-x64.exe'))){
 $dep=$lock.($item[0]);$file=Join-Path $third $item[1]
 if(!(Test-Path -LiteralPath $file)){Invoke-WebRequest -Uri $dep.url -OutFile $file}
 if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $dep.sha256){throw ('Checksum mismatch: '+$file)}
}
New-Item -ItemType Directory -Force -Path (Join-Path $third '7zip'),(Join-Path $third '7zip-bin') | Out-Null
tar -xf (Join-Path $third '7zip-src.tar.xz') -C (Join-Path $third '7zip')
if($LASTEXITCODE){throw 'Source extraction failed'}
tar -xf (Join-Path $third 'libwebp.tar.gz') -C $third
if($LASTEXITCODE){throw 'WebP extraction failed'}
tar -xf (Join-Path $third 'libjpeg-turbo.tar.gz') -C $third
if($LASTEXITCODE){throw 'JPEG source extraction failed'}
tar -xf (Join-Path $third 'zlib.tar.gz') -C $third
if($LASTEXITCODE){throw 'zlib extraction failed'}
$seven=Get-Command 7z.exe -ErrorAction SilentlyContinue
if($seven){$seven=$seven.Source}else{$seven='C:\Program Files\7-Zip\7z.exe'}
if(!(Test-Path -LiteralPath $seven)){throw 'Install 7-Zip or add 7z.exe to PATH to extract the pinned runtime.'}
& $seven x (Join-Path $third '7zip-x64.exe') ('-o'+(Join-Path $third '7zip-bin')) 7z.dll License.txt -y
if($LASTEXITCODE){throw '7-Zip runtime extraction failed'}
& $seven x (Join-Path $third 'libjpeg-turbo-x64.exe') ('-o'+(Join-Path $third 'jpeg-bin')) -y
if($LASTEXITCODE){throw 'JPEG runtime extraction failed'}
Copy-Item -LiteralPath (Join-Path $third 'jpeg-bin\$SYSDIR\turbojpeg.dll') -Destination (Join-Path $third 'jpeg-bin\bin\turbojpeg.dll') -Force
& (Join-Path $PSScriptRoot 'make-assets.ps1')
