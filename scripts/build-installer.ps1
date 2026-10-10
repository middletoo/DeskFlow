param([string]$PackagePath,[string]$OutputDirectory='',[string]$Version='0.3.5.0',
      [ValidateSet('x64','x86','ARM64')][string]$Architecture='x64',
      [string]$BinaryDirectory='', [string]$CertificatePath='',
      [string]$CertificateThumbprint='5E73D2B83E4733BB81072695B845044028283DA4')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$PackagePath){$PackagePath=Join-Path $root "artifacts/msix/DeskFlow-$Version-$($Architecture.ToLowerInvariant()).msix"}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'artifacts/installer'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$cert=if($CertificatePath){[IO.Path]::GetFullPath($CertificatePath)}else{Join-Path $root 'artifacts/msix/DeskFlow-Development.cer'}
if(!$BinaryDirectory){$deskBuildName=if($Architecture -eq 'x64'){'build'}else{"build-$Architecture"};$BinaryDirectory=Join-Path $root "$deskBuildName/Release"}
Copy-Item -LiteralPath $PackagePath -Destination $OutputDirectory -Force
Copy-Item -LiteralPath $cert -Destination $OutputDirectory -Force
foreach($script in @('Install-DeskFlow.ps1','Install-DeskFlow.cmd')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $OutputDirectory -Force}
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'DeskTrust.exe') -Destination $OutputDirectory -Force
$certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($cert)
$manifest=@{version=$Version;publisher=$certificate.Subject;certificateThumbprint=$certificate.Thumbprint;packageFile=[IO.Path]::GetFileName($PackagePath);certificateFile=[IO.Path]::GetFileName($cert);packageSha256=(Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash;certificateSha256=(Get-FileHash -LiteralPath $cert -Algorithm SHA256).Hash;trustHelperSha256=(Get-FileHash -LiteralPath (Join-Path $OutputDirectory 'DeskTrust.exe') -Algorithm SHA256).Hash}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
$buildDirectory=Split-Path -Parent ([IO.Path]::GetFullPath($BinaryDirectory))
$generated=Join-Path $buildDirectory 'generated'
New-Item -ItemType Directory -Force -Path $generated | Out-Null
$resource=Join-Path $generated 'setup-payload.rc'
$resourceLines=@('#pragma code_page(65001)')
$payloads=@(@(201,'manifest.json'),@(202,[IO.Path]::GetFileName($PackagePath)),@(203,'DeskFlow-Development.cer'),@(204,'DeskTrust.exe'),@(205,'Install-DeskFlow.ps1'))
foreach($payload in $payloads){
    $payloadPath=(Join-Path $OutputDirectory $payload[1]).Replace('\','/')
    $resourceLines+=($payload[0].ToString()+' RCDATA "'+$payloadPath+'"')
}
$resourceLines | Set-Content -LiteralPath $resource -Encoding utf8
cmake -S $root -B $buildDirectory "-DDESKFLOW_SETUP_PAYLOAD_RC=$resource"
if($LASTEXITCODE -ne 0){throw 'Installer resource configuration failed'}
cmake --build $buildDirectory --config Release --target DeskSetup --parallel 4
if($LASTEXITCODE -ne 0){throw 'Native installer build failed'}
$sdk=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdkBin=Get-ChildItem (Join-Path $sdk 'bin') -Directory | Where-Object {$_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64/signtool.exe'))} | Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
$setup=Join-Path $BinaryDirectory 'DeskSetup.exe'
& (Join-Path $sdkBin.FullName 'x64/signtool.exe') sign /fd SHA256 /sha1 $CertificateThumbprint /s My $setup
if($LASTEXITCODE -ne 0){throw 'Native installer signing failed'}
Copy-Item -LiteralPath $setup -Destination $OutputDirectory -Force
Write-Output $OutputDirectory
