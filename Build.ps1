# Offline Windows/MinGW build only. Never installs, boots or flashes anything.
param([string]$Gcc='gcc',[string]$Ld='ld',[string]$Python='python',[string]$PrivateInputs)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath($PSScriptRoot)
$root=Join-Path $repo 'research\tu102'
$out=Join-Path $root 'build\transaction'
$scratch=Join-Path $out 'tmp'
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$env:TEMP=$scratch;$env:TMP=$scratch;$env:TMPDIR=$scratch;$env:PYTHONDONTWRITEBYTECODE='1'
if(!$PrivateInputs){throw 'Private firmware/platform inputs are intentionally absent. Use -PrivateInputs with your own archived local research/tu102 directory. See docs/BUILD.md. No hardware was accessed.'}
& $Python -B (Join-Path $repo 'tools\Prepare-PrivateInputs.py') --source $PrivateInputs
if($LASTEXITCODE -ne 0){throw 'Private inputs do not match; stopped before compile/boot'}
$headers=Join-Path $repo 'deps\edk2\MdePkg\Include'
$inc=@('-I',$headers,'-I',(Join-Path $headers 'X64'))
$common=@('-std=c11','-Wall','-Wextra','-Werror','-fshort-wchar')
$hostOptions=@($common)+@('-O1','-g','-Wno-unused-function')
$sha='../../unlock/sha256.c'
Push-Location -LiteralPath $root
try{
 $tests=@(
  @{name='core';file='transaction/test_core.c';sha=$false},
  @{name='latch';file='transaction/test_selector_latch.c';sha=$false},
  @{name='efi-dma';file='transaction/test_efi_dma.c';sha=$false},
  @{name='handoff';file='transaction/test_handoff.c';sha=$false},
  @{name='display';file='transaction/test_display.c';sha=$true},
  @{name='full-entry';file='transaction/test_full_entry.c';sha=$true},
  @{name='owner-release';file='transaction/test_owner_release.c';sha=$true},
  @{name='amd-direct';file='transaction/test_amd_direct.c';sha=$true;high=$true},
  @{name='internal-auto';file='transaction/test_internal_auto.c';sha=$true;high=$true},
  @{name='internal-handoff';file='transaction/test_internal_handoff.c';sha=$false},
  @{name='default-entry';file='transaction/test_app.c';sha=$true},
  @{name='manual-entry';file='transaction/test_app.c';sha=$true;armed=$true}
 )
 foreach($test in $tests){
  $args=@($hostOptions)+@($inc)
  if($test.high){$args+=@('-Wl,--image-base,0x180000000','-Wl,--disable-dynamicbase')}
  if($test.armed){$args+='-DTU102_ENABLE_UNVERIFIED_EXECUTION'}
  $args+=$test.file
  if($test.sha){$args+=$sha}
  $exe=Join-Path $out ('test-'+$test.name+'.exe')
  & $Gcc @args -o $exe
  if($LASTEXITCODE -ne 0){throw ('Mock compilation failed: '+$test.name)}
  & $exe | Tee-Object -LiteralPath (Join-Path $out ($test.name+'-tests.txt'))
  if($LASTEXITCODE -ne 0){throw ('Mock test failed: '+$test.name)}
 }
 $free=@($common)+@('-Os','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-asynchronous-unwind-tables','-fno-unwind-tables','-mno-red-zone','-ffunction-sections','-fdata-sections','-Wno-unused-function')
 $name='TU102-UNLOCK-INTERNAL-AUTO'
 & $Gcc @free @inc -DTU102_ENABLE_UNVERIFIED_EXECUTION -DTU102_AMD_DIRECT_PHYSICAL -DTU102_AUTOMATIC_INTERNAL -c transaction/experiment.c -o "$out/$name.obj"
 if($LASTEXITCODE -ne 0){throw 'EFI entry compilation failed'}
 & $Gcc @free @inc -c $sha -o "$out/sha256.obj"
 if($LASTEXITCODE -ne 0){throw 'SHA-256 compilation failed'}
 & $Ld --subsystem 10 --entry efi_main --image-base 0 --file-alignment 512 --section-alignment 4096 --enable-reloc-section --dynamicbase --nxcompat --no-insert-timestamp --gc-sections -Map "$out/$name.map" "$out/$name.obj" "$out/sha256.obj" -o "$out/$name.linked.efi"
 if($LASTEXITCODE -ne 0){throw 'EFI link failed'}
 & $Python -B verify_transaction_pe.py "$out/$name.linked.efi" "$out/$name.EFI" --report "$out/$name.json" --mode $name
 if($LASTEXITCODE -ne 0){throw 'EFI PE verification failed'}
 Write-Output 'Offline build complete. PRIVATE output contains embedded payloads. No installation or boot operation performed. Do not distribute the EFI.'
}finally{Pop-Location}
