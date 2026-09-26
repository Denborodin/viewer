param([Parameter(Mandatory=$true)][string]$RarExe)
$ErrorActionPreference='Stop'
$rar=[IO.Path]::GetFullPath($RarExe)
$generated=Join-Path $PSScriptRoot 'fixtures\generated'
$pages=Join-Path $generated 'large-pages\страницы'
New-Item -ItemType Directory -Force -Path $pages | Out-Null
for($i=1;$i -le 10000;$i++){
 $destination=Join-Path $pages ($i.ToString('D5')+'.png')
 if(!(Test-Path -LiteralPath $destination)){Copy-Item -LiteralPath (Join-Path $generated ('images\'+(1+($i-1)%12)+'.png')) -Destination $destination}
}
foreach($count in @(1000,10000)){
 $list=Join-Path $generated ('rar-list-'+$count+'.txt')
 (1..$count | ForEach-Object {Join-Path $pages ($_.ToString('D5')+'.png')}) | Set-Content -LiteralPath $list -Encoding Unicode
 foreach($version in @(4,5)){foreach($solid in @($false,$true)){
  $name='large-'+$count+'-rar'+$version+$(if($solid){'-solid'}else{''})+'.rar'
  $archive=Join-Path $generated $name
  if(Test-Path -LiteralPath $archive){Remove-Item -LiteralPath $archive}
  $flag=if($solid){'-s'}else{'-s-'}
  & $rar a ('-ma'+$version) $flag '-ep1' '-m3' '-idq' '-scul' $archive ('@'+$list)
  if($LASTEXITCODE){throw ('RAR failed: '+$name)}
 }}
}
