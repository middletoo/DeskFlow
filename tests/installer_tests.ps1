param([string]$BundleDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$script=Join-Path $root 'scripts/Install-DeskFlow.ps1'
$engine=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
& $engine -NoProfile -ExecutionPolicy Bypass -File $script -BundleDirectory $BundleDirectory -ValidateOnly
if($LASTEXITCODE -ne 0){throw 'Verified package must pass validation without modifying Windows trust.'}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('DeskFlow-Installer-Tests-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
Copy-Item -Path (Join-Path $BundleDirectory '*') -Destination $fixture
$manifestPath=Join-Path $fixture 'manifest.json'
$original=Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 | ConvertFrom-Json
$changed=$original.PSObject.Copy();$changed.certificateThumbprint='0000000000000000000000000000000000000000'
$changed | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8
$ErrorActionPreference='Continue'
& $engine -NoProfile -ExecutionPolicy Bypass -File $script -BundleDirectory $fixture -ValidateOnly *> (Join-Path $fixture 'wrong-thumbprint.log')
$ErrorActionPreference='Stop'
if($LASTEXITCODE -eq 0){throw 'Unrecognized certificate must be rejected before requesting administrator permission.'}
$original | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8
$payload=Join-Path $fixture $original.packageFile
$stream=[IO.File]::Open($payload,[IO.FileMode]::Append,[IO.FileAccess]::Write);$stream.WriteByte(0);$stream.Dispose()
$ErrorActionPreference='Continue'
& $engine -NoProfile -ExecutionPolicy Bypass -File $script -BundleDirectory $fixture -ValidateOnly *> (Join-Path $fixture 'changed-payload.log')
$ErrorActionPreference='Stop'
if($LASTEXITCODE -eq 0){throw 'Modified payload must fail before requesting administrator permission.'}
Copy-Item -LiteralPath (Join-Path $BundleDirectory $original.packageFile) -Destination $payload -Force
$helper=Join-Path $fixture 'DeskTrust.exe';$stream=[IO.File]::Open($helper,[IO.FileMode]::Append,[IO.FileAccess]::Write);$stream.WriteByte(0);$stream.Dispose()
$original.trustHelperSha256=(Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash
$original | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8
$ErrorActionPreference='Continue'
& $engine -NoProfile -ExecutionPolicy Bypass -File $script -BundleDirectory $fixture -ValidateOnly *> (Join-Path $fixture 'changed-helper.log')
$ErrorActionPreference='Stop'
if($LASTEXITCODE -eq 0){throw 'Modified elevated helper must be rejected even if the mutable manifest digest is changed.'}
Write-Output 'PASS installer verified package, pinned certificate and modified-payload rejection; no trust changes made.'
