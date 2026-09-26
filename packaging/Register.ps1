param([ValidateSet('Register','Unregister','Status')][string]$Action='Status',[Parameter(Mandatory=$true)][string]$AppDirectory)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false)
$packageName='Viewer.Local'
$appRoot=[IO.Path]::GetFullPath($AppDirectory)
$packageFile=Join-Path $appRoot 'integration\Viewer.msix'
$certificateFile=Join-Path $appRoot 'integration\Viewer.Local.cer'
$stateRoot=Join-Path $env:LOCALAPPDATA 'Viewer'
$recordPath=Join-Path $stateRoot 'integration.json'
$record=$null
if(Test-Path -LiteralPath $recordPath){$record=Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json}
$existing=Get-AppxPackage -Name $packageName
if($Action -eq 'Status'){
 if(!$existing){Write-Output 'Интеграция отключена.'}
 elseif($record -and $record.AppDirectory -eq $appRoot){Write-Output ('Пункт «Открыть в Viewer» зарегистрирован. Режим: '+$record.Mode+'.')}
 else {Write-Output 'Нужно восстановить регистрацию для текущей папки.'}
 exit 0
}
if($Action -eq 'Unregister'){
 if($existing){Remove-AppxPackage -Package $existing.PackageFullName}
 if($record -and $record.OwnedCertificate -and $record.Thumbprint){
  $trusted=Join-Path 'Cert:\CurrentUser\TrustedPeople' $record.Thumbprint
  if(Test-Path -LiteralPath $trusted){Remove-Item -LiteralPath $trusted}
 }
 if(Test-Path -LiteralPath $recordPath){Remove-Item -LiteralPath $recordPath}
 Write-Output 'Интеграция удалена. При необходимости повторно войдите в Windows.'
 exit 0
}
if(!(Test-Path -LiteralPath $packageFile)){throw 'Отсутствует integration\Viewer.msix. Сначала соберите пакет интеграции.'}
if(!(Test-Path -LiteralPath (Join-Path $appRoot 'ViewerShell.dll'))){throw 'Отсутствует ViewerShell.dll.'}
$shellHash=(Get-FileHash -LiteralPath (Join-Path $appRoot 'ViewerShell.dll') -Algorithm SHA256).Hash
if($existing -and $record -and $record.AppDirectory -eq $appRoot -and $record.ShellHash -eq $shellHash -and $record.PackageHash -eq (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash){Write-Output 'Интеграция уже включена.';exit 0}
$addedCertificate=$false
$thumbprint=$null
try {
 if(Test-Path -LiteralPath $certificateFile){
  $cert=[System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
  if($cert.Subject -ne 'CN=Viewer Local Development'){throw 'Неожиданный издатель локального сертификата.'}
  $thumbprint=$cert.Thumbprint
  if(!(Test-Path -LiteralPath (Join-Path 'Cert:\CurrentUser\TrustedPeople' $thumbprint))){
   Import-Certificate -FilePath $certificateFile -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople' | Out-Null
   $addedCertificate=$true
  }
 }
 if($existing){Remove-AppxPackage -Package $existing.PackageFullName}
 $mode='Signed'
 try {Add-AppxPackage -Path $packageFile -ExternalLocation $appRoot -ErrorAction Stop}
 catch {
  # Windows App Installer uses machine trust. Development registration is allowed
  # only when the user has ALREADY enabled Developer Mode; never enable it here.
  $developer=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense
  if($developer -ne 1){throw 'Windows не доверяет подписи пакета. Для локальной сборки нужен заранее включённый режим разработчика или доверенная подпись MSIX. Viewer не меняет настройки безопасности Windows.'}
  Add-AppxPackage -Register (Join-Path $appRoot 'integration\AppxManifest.xml') -ExternalLocation $appRoot -ErrorAction Stop
  $mode='Development'
  if($addedCertificate -and $thumbprint){Remove-Item -LiteralPath (Join-Path 'Cert:\CurrentUser\TrustedPeople' $thumbprint);$addedCertificate=$false}
 }
 New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
 $owned=$addedCertificate -or ($record -and $record.OwnedCertificate -and $record.Thumbprint -eq $thumbprint)
 @{AppDirectory=$appRoot;ShellHash=$shellHash;Thumbprint=$thumbprint;OwnedCertificate=[bool]$owned;Mode=$mode;PackageHash=(Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding UTF8
 Write-Output ('Интеграция включена ('+$mode+'). Если пункта ещё нет, повторно войдите в Windows.')
} catch {
 if($record -and $record.AppDirectory){
  $oldPackage=Join-Path $record.AppDirectory 'integration\Viewer.msix'
  if(Test-Path -LiteralPath $oldPackage){try{if($record.Mode -eq 'Development'){Add-AppxPackage -Register (Join-Path $record.AppDirectory 'integration\AppxManifest.xml') -ExternalLocation $record.AppDirectory -ErrorAction Stop}else{Add-AppxPackage -Path $oldPackage -ExternalLocation $record.AppDirectory -ErrorAction Stop}}catch{Write-Output ('Не удалось восстановить прежнюю регистрацию: '+$_.Exception.Message)}}
 }
 if($addedCertificate -and $thumbprint){Remove-Item -LiteralPath (Join-Path 'Cert:\CurrentUser\TrustedPeople' $thumbprint) -ErrorAction SilentlyContinue}
 Write-Error $_
 exit 1
}
