param([int]$Runs=20)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'dist\Viewer\Viewer.exe'
$bench=Join-Path $root 'dist\Viewer\ViewerBench.exe'
$results=Join-Path $root ('build\performance-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $results | Out-Null
$summary=@()
foreach($mode in @('stored','deflate')){
 $source=Join-Path $root ('tests\fixtures\generated\1000-4mp-'+$mode+'.zip')
 if(!(Test-Path -LiteralPath $source)){throw 'Generate the benchmark fixtures with tests/make-fixtures.ps1 -Benchmark'}
 $env:VIEWER_DATA_DIR=Join-Path $results ('core-'+$mode)
 $core=(& $bench $source $Runs | ConvertFrom-Json)
 if($LASTEXITCODE){throw 'Core benchmark failed'}
 $samples=@()
 for($i=0;$i -lt $Runs;$i++){
  $output=Join-Path $results ($mode+'-'+$i)
  $info=[Diagnostics.ProcessStartInfo]::new($exe)
  $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
  $info.Arguments='"'+$source+'" --test-output "'+$output+'"'
  $info.EnvironmentVariables['VIEWER_DATA_DIR']=Join-Path $output 'state'
  $process=[Diagnostics.Process]::Start($info)
  if(!$process.WaitForExit(35000)){throw 'Application benchmark timed out'}
  $metrics=Join-Path $output 'metrics.json'
  if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $metrics)){throw ('Application failed: '+$output)}
  $samples+=Get-Content -LiteralPath $metrics -Raw | ConvertFrom-Json
  $process.Dispose()
 }
 $sorted=@($samples.first_frame_ms | Sort-Object)
 $entry=@{mode=$mode;runs=$Runs;app_first_render_p95_ms=$sorted[[Math]::Ceiling($Runs*0.95)-1];app_first_render_initial_ms=$samples[0].first_frame_ms;core=$core;samples=$samples;os_cache='uncontrolled; warm after initial run';app_cache='empty directory per launch';render='hidden HWND, Direct2D EndDraw + DwmFlush; timer starts in App constructor'}
 $summary+=$entry
 $entry | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $results ($mode+'.json')) -Encoding UTF8
 Write-Output ($entry | Select-Object mode,runs,app_first_render_p95_ms,app_first_render_initial_ms | ConvertTo-Json -Compress)
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $results 'summary.json') -Encoding UTF8
Write-Output $results
