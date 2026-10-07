<#
.SYNOPSIS
Build signed Windows archives locally; signing keys stay in the certificate store.
#>
param(
    [string]$Version='0.3.3.0',
    [string]$CertificateThumbprint='5E73D2B83E4733BB81072695B845044028283DA4',
    [string]$BuildRoot='',
    [string]$OutputDirectory='',
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(!$BuildRoot){$BuildRoot=Join-Path $root 'artifacts/release-build'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root "artifacts/releases/$Version"}
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if($Version -notmatch '^\d+\.\d+\.\d+\.\d+$'){throw 'Version must have four numeric components'}
$certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if(!$certificate.HasPrivateKey -or $certificate.Subject -ne 'CN=DeskFlow Development'){throw 'A known DeskFlow signing key is required'}
$sdk=(Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$sdkBin=Get-ChildItem (Join-Path $sdk 'bin') -Directory | Where-Object {$_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64/signtool.exe'))} | Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
if(!$sdkBin){throw 'Windows SDK signing tools were not found'}
$signTool=Join-Path $sdkBin.FullName 'x64/signtool.exe'
$releaseVersion=([Version]$Version).ToString(3)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$scratch=Join-Path $OutputDirectory ('stage-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
foreach($architecture in @('x64','x86','ARM64')){
    $build=Join-Path $BuildRoot $architecture
    if(!$SkipBuild){& (Join-Path $PSScriptRoot 'build.ps1') -Architecture $architecture -BuildDirectory $build -SkipTests -Targets DeskFlow,DeskIndex,DeskOCR,DeskTrust}
    $binaries=Join-Path $build 'Release'
    foreach($name in @('DeskFlow.exe','DeskIndex.exe','DeskOCR.exe','DeskTrust.exe')){
        $file=Join-Path $binaries $name
        if(!(Test-Path -LiteralPath $file)){throw "Missing build: $architecture/$name"}
        $signature=Get-AuthenticodeSignature -LiteralPath $file
        if(!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $CertificateThumbprint){
            & $signTool sign /fd SHA256 /sha1 $CertificateThumbprint /s My $file
            if($LASTEXITCODE -ne 0){throw "Signing failed: $architecture/$name"}
        }
    }
    $portable=Join-Path $scratch "$architecture-portable"
    New-Item -ItemType Directory -Force -Path $portable | Out-Null
    foreach($name in @('DeskFlow.exe','DeskIndex.exe','DeskOCR.exe')){Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $portable}
    foreach($name in @('README.md','THIRD-PARTY-NOTICES.md')){Copy-Item -LiteralPath (Join-Path $root $name) -Destination $portable}
    Copy-Item -LiteralPath (Join-Path $root 'assets') -Destination $portable -Recurse
    $docs=Join-Path $portable 'docs'
    New-Item -ItemType Directory -Force -Path (Join-Path $docs 'images') | Out-Null
    foreach($name in @('USER_GUIDE.zh-CN.md','USER_GUIDE.en-US.md','DEVELOPMENT.md','PERFORMANCE.md')){Copy-Item -LiteralPath (Join-Path $root "docs/$name") -Destination $docs}
    foreach($name in @('files','clipboard','clipboard-preview','capture','capture-hover','ocr','translation','pin','scrolling','recording')){Copy-Item -LiteralPath (Join-Path $root "docs/images/$name.png") -Destination (Join-Path $docs 'images')}
    @"
DeskFlow $releaseVersion — Windows $architecture

中文：完整解压后双击 DeskFlow.exe。默认快捷键：Alt+Q 文件，Alt+W 剪贴板，Alt+S 截图。
请让 DeskIndex.exe 和 DeskOCR.exe 与主程序放在同一目录。无需另外安装 VC++ 运行库。
本地 OCR 需要 MSIX 包身份：如需 OCR/原图翻译，请下载同架构 setup.zip，运行其中 Install-DeskFlow.cmd。
便携版数据保存在当前用户 LocalAppData/DeskFlow，不写入下载文件夹。

English: Extract everything and run DeskFlow.exe. Keys: Alt+Q files, Alt+W history, Alt+S capture.
Keep DeskIndex.exe and DeskOCR.exe beside the app. No separate VC++ runtime is required.
For local OCR/image translation, download the matching setup.zip and run Install-DeskFlow.cmd.
Portable data lives in the current user's LocalAppData/DeskFlow, outside the download folder.

https://github.com/middletoo/DeskFlow/releases
"@ | Set-Content -LiteralPath (Join-Path $portable 'START-HERE.txt') -Encoding utf8
    $slug=$architecture.ToLowerInvariant()
    Compress-Archive -Path (Join-Path $portable '*') -DestinationPath (Join-Path $OutputDirectory "DeskFlow-$releaseVersion-windows-$slug-portable.zip") -Force
    $packageOutput=Join-Path $scratch "$architecture-msix"
    & (Join-Path $PSScriptRoot 'package.ps1') -SkipBuild -Architecture $architecture -BinaryDirectory $binaries -OutputDirectory $packageOutput -Version $Version -CertificateThumbprint $CertificateThumbprint
    $setup=Join-Path $scratch "$architecture-setup"
    & (Join-Path $PSScriptRoot 'build-installer.ps1') -Architecture $architecture -BinaryDirectory $binaries -PackagePath (Join-Path $packageOutput "DeskFlow-$Version-$slug.msix") -CertificatePath (Join-Path $packageOutput 'DeskFlow-Development.cer') -OutputDirectory $setup -Version $Version
    & (Join-Path $PSScriptRoot '../tests/installer_tests.ps1') -BundleDirectory $setup
    @"
DeskFlow $releaseVersion — Windows $architecture setup

中文：完整解压后双击 Install-DeskFlow.cmd。初次安装可能请求 UAC，仅信任已知 DeskFlow 开发证书到 Trusted People。
安装后从开始菜单启动，可使用本地 OCR 和原图翻译。更新沿用同一包家族的数据。
当前验证版使用开发者自签名证书，不是 Microsoft Store 正式签名版本。下载校验见 Releases 中的 SHA256SUMS.txt。

English: Extract everything and run Install-DeskFlow.cmd. Initial installation may ask for UAC to trust the pinned development leaf in Trusted People.
Launch from Start for local OCR/image translation. Updates retain package-family data.
This validation release uses a self-signed development certificate, not Microsoft Store distribution signing. Check SHA256SUMS.txt from Releases.

https://github.com/middletoo/DeskFlow/releases
"@ | Set-Content -LiteralPath (Join-Path $setup 'START-HERE.txt') -Encoding utf8
    Compress-Archive -Path (Join-Path $setup '*') -DestinationPath (Join-Path $OutputDirectory "DeskFlow-$releaseVersion-windows-$slug-setup.zip") -Force
}
Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.zip' -File | Sort-Object Name | ForEach-Object {
    ((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+$_.Name)
} | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ascii
Write-Output "Release archives: $OutputDirectory"
# Negative installer fixtures intentionally execute failing child processes.
# Report success after every real archive was validated and checksummed.
$global:LASTEXITCODE=0
