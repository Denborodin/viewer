$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$portable=Join-Path $root 'dist\Viewer'
if(!(Test-Path -LiteralPath (Join-Path $portable 'Viewer.exe'))){throw 'Build and install Release first'}
$source=Join-Path $root ('build\source-package-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $source,(Join-Path $source 'third_party') | Out-Null
foreach($name in @('src','tools','packaging','docs','README.md','THIRD_PARTY.md','CMakeLists.txt','CMakePresets.json','.gitignore','.gitattributes','.clang-format')){
 Copy-Item -LiteralPath (Join-Path $root $name) -Destination $source -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $source 'tests\fixtures') | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'tests') -File | Copy-Item -Destination (Join-Path $source 'tests') -Force
Get-ChildItem -LiteralPath (Join-Path $root 'tests\fixtures') -File | Copy-Item -Destination (Join-Path $source 'tests\fixtures') -Force
foreach($name in @('dependencies.json','7zip-src.tar.xz','7zip-x64.exe','libwebp.tar.gz','libjpeg-turbo.tar.gz','libjpeg-turbo-x64.exe','zlib.tar.gz')){
 Copy-Item -LiteralPath (Join-Path $root ('third_party\'+$name)) -Destination (Join-Path $source 'third_party') -Force
}
Compress-Archive -LiteralPath $portable -DestinationPath (Join-Path $root 'dist\Viewer-0.1.0-win-x64.zip') -Force
Compress-Archive -Path (Join-Path $source '*') -DestinationPath (Join-Path $root 'dist\Viewer-0.1.0-source.zip') -Force
Get-ChildItem -LiteralPath (Join-Path $root 'dist') -Filter '*.zip' | Get-FileHash -Algorithm SHA256 | ForEach-Object {$_.Hash.ToLower()+'  '+[IO.Path]::GetFileName($_.Path)} | Set-Content -LiteralPath (Join-Path $root 'dist\SHA256SUMS.txt') -Encoding ASCII
Write-Output 'Portable and source archives created in dist.'
