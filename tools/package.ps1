param([switch]$Unsigned)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$sdk=Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory | Where-Object {Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')} | Sort-Object Name -Descending | Select-Object -First 1
if(!$sdk){throw 'Windows SDK tools are required'}
$output=Join-Path $root 'dist\Viewer\integration'
$stage=Join-Path $root 'build\msix'
New-Item -ItemType Directory -Force -Path $stage,$output | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'packaging\AppxManifest.xml') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'packaging\Assets') -Destination $stage -Recurse -Force
& (Join-Path $sdk.FullName 'x64\makeappx.exe') pack /o /nv /d $stage /p (Join-Path $output 'Viewer.msix')
if($LASTEXITCODE){throw 'MakeAppx failed'}
if(!$Unsigned){
 $cert=Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object {$_.Subject -eq 'CN=Viewer Local Development' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30)} | Sort-Object NotAfter -Descending | Select-Object -First 1
 if(!$cert){$cert=New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Viewer Local Development' -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)}
 Export-Certificate -Cert $cert -FilePath (Join-Path $output 'Viewer.Local.cer') -Force | Out-Null
 & (Join-Path $sdk.FullName 'x64\signtool.exe') sign /fd SHA256 /s My /sha1 $cert.Thumbprint (Join-Path $output 'Viewer.msix')
 if($LASTEXITCODE){throw 'SignTool failed'}
}
Write-Output ('Package: '+(Join-Path $output 'Viewer.msix'))
