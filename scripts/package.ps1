<#
.SYNOPSIS
Build a DeskFlow development MSIX without installing it or changing trust.
.EXAMPLE
pwsh -File scripts/package.ps1 -Unsigned
.EXAMPLE
pwsh -File scripts/package.ps1 -CreateDevelopmentCertificate
.EXAMPLE
pwsh -File scripts/package.ps1 -PfxPath C:\private\publisher.pfx -PfxPassword (Read-Host -AsSecureString)
.NOTES
A created certificate is placed in CurrentUser\My (personal certificates).
It is NOT added to TrustedPeople/TrustedRoot. Installing a development MSIX
requires an explicit, separate trust decision and package registration.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$BinaryDirectory = '',
    [string]$OutputDirectory = '',
    [string]$Publisher = 'CN=DeskFlow Development',
    [string]$Version = '0.3.2.0',
    [ValidateSet('x64','x86','ARM64')][string]$Architecture = 'x64',
    [string]$PfxPath = '',
    [string]$CertificateThumbprint = '',
    [Security.SecureString]$PfxPassword,
    [switch]$CreateDevelopmentCertificate,
    [switch]$Unsigned,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$deskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$BinaryDirectory) { $deskBuildName=if($Architecture -eq 'x64'){'build'}else{"build-$Architecture"}; $BinaryDirectory = Join-Path $deskRoot "$deskBuildName/$Configuration" }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $deskRoot 'artifacts/msix' }
$BinaryDirectory = [IO.Path]::GetFullPath($BinaryDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Version must contain four numeric components.' }
if ($Unsigned -and ($PfxPath -or $CreateDevelopmentCertificate)) { throw 'Choose unsigned packaging or one signing method.' }
if ($PfxPath -and $CreateDevelopmentCertificate) { throw 'Choose one signing method.' }
if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit $LASTEXITCODE." }
}
foreach ($deskExecutable in @('DeskFlow.exe','DeskOCR.exe','DeskIndex.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $BinaryDirectory $deskExecutable) -PathType Leaf)) { throw "Build output is missing $deskExecutable in $BinaryDirectory." }
}
$deskKitsRoot = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
if (!$deskKitsRoot) { $deskKitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10' }
$deskSdkDirectory = Get-ChildItem -LiteralPath (Join-Path $deskKitsRoot 'bin') -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'x64/makeappx.exe')) } |
    Sort-Object { [Version]$_.Name } -Descending | Select-Object -First 1
if (!$deskSdkDirectory) { throw 'Windows SDK makeappx.exe was not found.' }
$deskMakeAppx = Join-Path $deskSdkDirectory.FullName 'x64/makeappx.exe'
$deskSignTool = Join-Path $deskSdkDirectory.FullName 'x64/signtool.exe'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# Every run gets its own layout. No recursive cleanup can touch another run.
$deskLayout = Join-Path $OutputDirectory ('layout-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $deskLayout 'Assets') -Force | Out-Null
foreach ($deskExecutable in @('DeskFlow.exe','DeskOCR.exe','DeskIndex.exe')) { Copy-Item -LiteralPath (Join-Path $BinaryDirectory $deskExecutable) -Destination $deskLayout }
# Include adjacent runtime DLLs supplied by the build, if present.
Get-ChildItem -LiteralPath $BinaryDirectory -Filter '*.dll' -File | Copy-Item -Destination $deskLayout
[xml]$deskManifest = Get-Content -LiteralPath (Join-Path $deskRoot 'packaging/AppxManifest.xml') -Raw -Encoding utf8
$deskManifest.Package.Identity.Publisher = $Publisher
$deskManifest.Package.Identity.Version = $Version
$deskManifest.Package.Identity.ProcessorArchitecture = $Architecture.ToLowerInvariant()
$deskManifest.Save((Join-Path $deskLayout 'AppxManifest.xml'))
Add-Type -AssemblyName System.Drawing
foreach ($deskAsset in @(@('StoreLogo.png',50),@('Square44x44Logo.png',44),@('Square150x150Logo.png',150))) {
    $deskSize = [int]$deskAsset[1]
    $deskSource = Join-Path $deskRoot "assets/DeskFlow-$deskSize.png"
    if (!(Test-Path -LiteralPath $deskSource -PathType Leaf)) { throw "Logo asset missing: $deskSource. Run scripts/build-logo-assets.ps1 first." }
    Copy-Item -LiteralPath $deskSource -Destination (Join-Path $deskLayout "Assets/$($deskAsset[0])")
}
$deskPackage = Join-Path $OutputDirectory "DeskFlow-$Version-$($Architecture.ToLowerInvariant()).msix"
& $deskMakeAppx pack /d $deskLayout /p $deskPackage /o
if ($LASTEXITCODE -ne 0) { throw "MSIX validation/packaging failed with exit $LASTEXITCODE." }
$deskCertificatePath = Join-Path $OutputDirectory 'DeskFlow-Development.cer'
if ($CreateDevelopmentCertificate) {
    Write-Host 'Creating a development signing certificate in CurrentUser\My. This does not trust the certificate.'
    $deskCertificate = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyUsage DigitalSignature -CertStoreLocation 'Cert:\CurrentUser\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}') -NotAfter (Get-Date).AddYears(1)
    Export-Certificate -Cert $deskCertificate -FilePath $deskCertificatePath -Force | Out-Null
    & $deskSignTool sign /fd SHA256 /sha1 $deskCertificate.Thumbprint /s My $deskPackage
    if ($LASTEXITCODE -ne 0) { throw "Development signing failed with exit $LASTEXITCODE." }
} elseif($CertificateThumbprint){
    $deskCertificate=Get-Item -LiteralPath ("Cert:\CurrentUser\My\"+$CertificateThumbprint) -ErrorAction Stop
    if($deskCertificate.Subject -ne $Publisher -or !$deskCertificate.HasPrivateKey){throw 'Signing certificate does not match the package publisher or has no private key.'}
    Export-Certificate -Cert $deskCertificate -FilePath $deskCertificatePath -Force | Out-Null
    & $deskSignTool sign /fd SHA256 /sha1 $CertificateThumbprint /s My $deskPackage
    if($LASTEXITCODE -ne 0){throw 'Signing with existing development certificate failed.'}
} elseif ($PfxPath) {
    $PfxPath = [IO.Path]::GetFullPath($PfxPath)
    if (!(Test-Path -LiteralPath $PfxPath -PathType Leaf)) { throw 'PFX certificate file was not found.' }
    $deskPasswordPointer = [IntPtr]::Zero
    try {
        $deskSignArguments = @('sign','/fd','SHA256','/f',$PfxPath)
        if ($PfxPassword) {
            $deskPasswordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($PfxPassword)
            $deskSignArguments += @('/p',[Runtime.InteropServices.Marshal]::PtrToStringBSTR($deskPasswordPointer))
        }
        $deskSignArguments += $deskPackage
        & $deskSignTool @deskSignArguments
        if ($LASTEXITCODE -ne 0) { throw "Signing failed with exit $LASTEXITCODE." }
    } finally {
        $deskSignArguments = $null
        if ($deskPasswordPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($deskPasswordPointer) }
    }
} else {
    Write-Warning 'This development MSIX is unsigned. Sign it before MSIX installation, or explicitly register the layout with Windows Developer Mode enabled.'
}
$deskInstructions = @"
DeskFlow development package
Package: $deskPackage
Layout: $deskLayout

This script did not install/register an app, enable Developer Mode, change UAC,
or add any certificate to TrustedPeople/TrustedRoot.

Signed MSIX: inspect the certificate subject ($Publisher) first. For the
development certificate ($deskCertificatePath), explicitly use Windows
certificate UI to install it into Local Machine > Trusted People (may need UAC).
Personal-store certificate creation does not establish trust. Then run:
  scripts/Register-DevelopmentPackage.ps1 -MsixPath '$deskPackage'

Unsigned layout: explicitly enable Windows Developer Mode if permitted, then:
  scripts/Register-DevelopmentPackage.ps1 -LayoutDirectory '$deskLayout'

Launch DeskFlow from Start after registration. Raw build EXEs remain unpackaged.
MakeAppx validates structure only. Current-user Start activation, local OCR and
DeskFlowStartup enable/disable still require testing with the registered package.
Startup is disabled by default; enable it in DeskFlow settings when wanted.
OCR needs Windows Chinese/English OCR language features. Elevated indexing asks
for UAC separately. Production distribution needs a trusted publisher.
"@
$deskInstructions | Set-Content -LiteralPath (Join-Path $OutputDirectory 'INSTALL-DEVELOPMENT.txt') -Encoding utf8
Write-Host "Development package: $deskPackage"
Write-Host "Explicit install steps: $(Join-Path $OutputDirectory 'INSTALL-DEVELOPMENT.txt')"
Write-Warning 'Package structure was validated; current-user Start activation, OCR and StartupTask remain unverified until explicit registration and testing.'
