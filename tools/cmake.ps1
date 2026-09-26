# Normalizes environment key casing for hosts that supply both Path and PATH.
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$CmakeArguments)
$ErrorActionPreference='Stop'
$start=[Diagnostics.ProcessStartInfo]::new()
$start.FileName=(Get-Command cmake.exe).Source
$start.WorkingDirectory=Split-Path -Parent $PSScriptRoot
$start.UseShellExecute=$false
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$start.Environment.Clear()
$normalized=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()){$normalized[[string]$entry.Key]=[string]$entry.Value}
foreach($entry in $normalized.GetEnumerator()){$start.Environment[$entry.Key]=$entry.Value}
foreach($arg in $CmakeArguments){$start.ArgumentList.Add($arg)}
$process=[Diagnostics.Process]::Start($start)
$stdout=$process.StandardOutput.ReadToEndAsync()
$stderr=$process.StandardError.ReadToEndAsync()
$process.WaitForExit()
Write-Output $stdout.GetAwaiter().GetResult()
if($stderr.GetAwaiter().GetResult()){Write-Output $stderr.GetAwaiter().GetResult()}
exit $process.ExitCode
