param([string]$Python='python')
$ErrorActionPreference='Stop'
$root=Join-Path (Split-Path -Parent $PSScriptRoot) 'research\tu102'
$out=Join-Path $root 'build\transaction'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$env:TEMP=$out;$env:TMP=$out;$env:TMPDIR=$out;$env:PYTHONDONTWRITEBYTECODE='1'
$destination=Join-Path $out ('benchmark-'+(Get-Date -Format 'yyyyMMdd-HHmmssfff')+'.json')
& $Python -B (Join-Path $root 'benchmark_tensor.py') --output $destination
if($LASTEXITCODE -ne 0){throw 'GPU identity/correctness/timing check failed'}
