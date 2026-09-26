param([string]$AppDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\Viewer'))
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$app=[IO.Path]::GetFullPath($AppDirectory)
$relocated=Join-Path $root ('build\portable relocation Юникод & %\Viewer-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $relocated | Out-Null
Copy-Item -Path (Join-Path $app '*') -Destination $relocated -Recurse -Force
$ps=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
function Integrate([string]$Directory,[string]$Action){
 & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Directory 'integration\Register.ps1') -Action $Action -AppDirectory $Directory
 if($LASTEXITCODE){throw ('Integration failed: '+$Action)}
}
function Associations {
 $result=@{}
 foreach($ext in @('.zip','.rar','.jpg','.jpeg','.png','.webp','.bmp','.gif','.tif','.tiff','.psd')){
  $choice=Get-ItemProperty -LiteralPath ('HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\'+$ext+'\UserChoice') -ErrorAction SilentlyContinue
  $result[$ext]=@($choice.ProgId,$choice.Hash)
 }
 return ($result | ConvertTo-Json -Compress)
}
$before=Associations
try {
 Integrate $app 'Register'
 Integrate $app 'Register'
 Integrate $app 'Unregister'
 if(Get-AppxPackage -Name Viewer.Local){throw 'Package still registered after removal'}
 Integrate $relocated 'Register'
 $record=Get-Content (Join-Path $env:LOCALAPPDATA 'Viewer\integration.json') -Raw | ConvertFrom-Json
 if($record.AppDirectory -ne $relocated){throw 'Relocated path not saved'}
 Integrate $relocated 'Status'
}finally{Integrate $app 'Register'}
if((Associations) -ne $before){throw 'Default associations changed'}
Write-Output 'PASS register, idempotence, unregister, Unicode relocation, repair, unchanged default associations'
