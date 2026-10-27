# Useful smoke checks that need no private firmware fixtures or GPU.
param([string]$Gcc='gcc',[string]$Python='python')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$root=Join-Path $repo 'research\tu102'
$out=Join-Path $repo 'build\public-tests'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$env:TEMP=$out;$env:TMP=$out;$env:TMPDIR=$out;$env:PYTHONDONTWRITEBYTECODE='1'
$include=Join-Path $repo 'deps\edk2\MdePkg\Include'
foreach($file in @('test_handoff','test_internal_handoff')){
 $exe=Join-Path $out ($file+'.exe')
 & $Gcc -std=c11 -Wall -Wextra -Werror -fshort-wchar -O1 -g -I $include -I (Join-Path $include 'X64') (Join-Path $root ('transaction/'+$file+'.c')) -o $exe
 if($LASTEXITCODE -ne 0){throw 'Public handoff test compile failed'}
 & $exe
 if($LASTEXITCODE -ne 0){throw 'Public handoff test failed'}
}
& $Python -B (Join-Path $root 'compare_benchmark.py') (Join-Path $repo 'evidence\before.json') (Join-Path $repo 'evidence\after-automatic-restart.json')
if($LASTEXITCODE -ne 0){throw 'Published evidence validation failed'}
