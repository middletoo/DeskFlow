param([string]$PackagePath,[string]$OutputDirectory='',[string]$Version='0.3.0.0')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$PackagePath){$PackagePath=Join-Path $root "artifacts/msix/DeskFlow-$Version-x64.msix"}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'artifacts/installer'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cert=Join-Path $root 'artifacts/msix/DeskFlow-Development.cer'
Copy-Item -LiteralPath $PackagePath -Destination $OutputDirectory -Force
Copy-Item -LiteralPath $cert -Destination $OutputDirectory -Force
foreach($script in @('Install-DeskFlow.ps1','Install-DeskFlow.cmd')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $OutputDirectory -Force}
Copy-Item -LiteralPath (Join-Path $root 'build/Release/DeskTrust.exe') -Destination $OutputDirectory -Force
$certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($cert)
$manifest=@{version=$Version;publisher=$certificate.Subject;certificateThumbprint=$certificate.Thumbprint;packageFile=[IO.Path]::GetFileName($PackagePath);certificateFile=[IO.Path]::GetFileName($cert);packageSha256=(Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash;certificateSha256=(Get-FileHash -LiteralPath $cert -Algorithm SHA256).Hash;trustHelperSha256=(Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'DeskTrust.exe') -Algorithm SHA256).Hash}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
Write-Output $OutputDirectory
