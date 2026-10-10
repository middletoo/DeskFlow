[CmdletBinding()]
param([string]$BundleDirectory=$PSScriptRoot,[switch]$ValidateOnly,[int]$ParentProcessId=0,[long]$ParentWindowHandle=0,[switch]$NoLaunch,[string]$ReceiptPath="",
      [string]$DataDirectory='', [string]$SourceDataDirectory='', [string]$SetupExecutablePath='')
$ErrorActionPreference='Stop'
trap {
    if(!$ValidateOnly){
        $failure=@{ok=$false;error=$_.Exception.Message}
        if($ReceiptPath){$failure | ConvertTo-Json | Set-Content -LiteralPath $ReceiptPath -Encoding utf8}
        if($ParentProcessId -gt 0 -and !(Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue)){
            try{$notice=New-Object -ComObject WScript.Shell;$notice.Popup(('DeskFlow installation did not finish: '+$_.Exception.Message),0,'DeskFlow',16) | Out-Null}catch{}
        }
    }
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
$allowedPublisher='CN=DeskFlow Development'
$allowedThumbprint='5E73D2B83E4733BB81072695B845044028283DA4'
$bundle=[IO.Path]::GetFullPath($BundleDirectory)
$manifestPath=Join-Path $bundle 'manifest.json'
if(!(Test-Path -LiteralPath $manifestPath -PathType Leaf)){throw 'Installer manifest is missing.'}
$manifest=Get-Content -LiteralPath $manifestPath -Raw -Encoding utf8 | ConvertFrom-Json
if($manifest.publisher -ne $allowedPublisher -or $manifest.certificateThumbprint -ne $allowedThumbprint){throw 'Installer publisher or certificate does not match DeskFlow.'}
foreach($name in @($manifest.packageFile,$manifest.certificateFile)){
    if([IO.Path]::GetFileName($name) -ne $name -or [string]::IsNullOrWhiteSpace($name)){throw 'Payload filenames must stay inside the installer directory.'}
}
$packagePath=Join-Path $bundle $manifest.packageFile
$certificatePath=Join-Path $bundle $manifest.certificateFile
if((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $manifest.packageSha256){throw 'The installation package failed its SHA256 check.'}
if((Get-FileHash -LiteralPath $certificatePath -Algorithm SHA256).Hash -ne $manifest.certificateSha256){throw 'The signing certificate failed its SHA256 check.'}
$certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
if($certificate.Subject -ne $allowedPublisher -or $certificate.Thumbprint -ne $allowedThumbprint){throw 'The certificate is not the reviewed DeskFlow certificate.'}
$signature=Get-AuthenticodeSignature -LiteralPath $packagePath
if(!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $allowedThumbprint){throw 'The package is not signed with the reviewed DeskFlow certificate.'}
$untrustedOwnChain=$false
if($signature.Status -eq 'UnknownError'){
    $chain=[Security.Cryptography.X509Certificates.X509Chain]::new()
    $chain.ChainPolicy.RevocationMode=[Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
    $chain.Build($certificate) | Out-Null
    $allowedFlag=[int][Security.Cryptography.X509Certificates.X509ChainStatusFlags]::UntrustedRoot
    $flags=0;foreach($state in $chain.ChainStatus){$flags=$flags -bor [int]$state.Status}
    $untrustedOwnChain=($flags -eq $allowedFlag);$chain.Dispose()
}
if($signature.Status -notin @('Valid','NotTrusted') -and !$untrustedOwnChain){throw "The package signature is invalid: $($signature.Status)."}
$trustHelper=Join-Path $bundle 'DeskTrust.exe'
# Exact signed maintainer helpers: original x64 and the 0.3.1 x64/x86/ARM64 builds.
$allowedHelperSha256=@(
    '8FF5D8E44955C8B88FDA9DDA51438C981E01C98E4B199802B86F5D89CE911A3F',
    'E0CAC9D31EF19060D5AD4A1768193461C7EE509499452E717BB89D5CABE1AA99',
    'BB8BC54E8AFBD5CBE2F0AED11E0DB4369AEED926501AE09927BB1D64A0C8D7C2',
    '4F2DBB62CEB3754A28EC409B3364C20D00AD3D44D99B75A10AAD3494CA4263DF'
)
if((Get-FileHash -LiteralPath $trustHelper -Algorithm SHA256).Hash -notin $allowedHelperSha256){throw 'Certificate helper failed its SHA256 check.'}
if($ValidateOnly){
    @{validated=$true;publisher=$certificate.Subject;thumbprint=$certificate.Thumbprint;package=$packagePath;signatureStatus=[string]$signature.Status;changesMade=$false} | ConvertTo-Json -Compress
    exit 0
}
if($DataDirectory){
    if(!$SourceDataDirectory -or !$SetupExecutablePath){throw 'The data-folder choice requires the reviewed setup wizard.'}
    $setupSignature=Get-AuthenticodeSignature -LiteralPath $SetupExecutablePath
    $setupOwnChain=$false
    if($setupSignature.Status -eq 'UnknownError' -and $setupSignature.SignerCertificate){
        $setupChain=[Security.Cryptography.X509Certificates.X509Chain]::new()
        $setupChain.ChainPolicy.RevocationMode=[Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
        $setupChain.Build($setupSignature.SignerCertificate) | Out-Null
        $setupFlags=0;foreach($state in $setupChain.ChainStatus){$setupFlags=$setupFlags -bor [int]$state.Status}
        $setupOwnChain=($setupFlags -eq [int][Security.Cryptography.X509Certificates.X509ChainStatusFlags]::UntrustedRoot);$setupChain.Dispose()
    }
    if(!$setupSignature.SignerCertificate -or $setupSignature.SignerCertificate.Thumbprint -ne $allowedThumbprint -or
       ($setupSignature.Status -notin @('Valid','NotTrusted') -and !$setupOwnChain)){throw 'The data migration helper is not signed by the reviewed publisher.'}
    if(![IO.Path]::IsPathRooted($DataDirectory) -or ![IO.Path]::IsPathRooted($SourceDataDirectory)){throw 'Data folders require full local paths.'}
    $setupLock=[IO.File]::Open($SetupExecutablePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
}
Write-Host 'DeskFlow: Windows may ask for administrator approval once.'
Write-Host 'The verified DeskFlow signing certificate will be trusted in LocalMachine/TrustedPeople.'
$receiptRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'DeskFlow-Setup'
New-Item -ItemType Directory -Force -Path $receiptRoot | Out-Null
$payloadLock=[IO.File]::Open($packagePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
$helperLock=[IO.File]::Open($trustHelper,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
if((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $manifest.packageSha256 -or (Get-FileHash -LiteralPath $trustHelper -Algorithm SHA256).Hash -notin $allowedHelperSha256){throw 'Payload changed before administrator confirmation.'}
$alreadyTrusted=Test-Path -LiteralPath ("Cert:\LocalMachine\TrustedPeople\"+$allowedThumbprint)
if(!$alreadyTrusted){
    $principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    try {
        if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
            $elevated=Start-Process -FilePath $trustHelper -WindowStyle Hidden -PassThru
        } else {
            $elevated=Start-Process -FilePath $trustHelper -Verb RunAs -WindowStyle Hidden -PassThru
        }
    } catch {throw 'Windows administrator approval was cancelled. Installation did not proceed.'}
    if(!$elevated.WaitForExit(120000)){throw 'Waiting for administrator action timed out.'}
    if($elevated.ExitCode -ne 0){throw "Certificate helper failed with code $($elevated.ExitCode)."}
}
if(!(Test-Path -LiteralPath ("Cert:\LocalMachine\TrustedPeople\"+$allowedThumbprint))){throw 'Windows has not trusted the development certificate.'}
if((Get-AuthenticodeSignature -LiteralPath $packagePath).Status -ne 'Valid'){throw 'Windows did not validate the package signature after certificate trust.'}
if($ParentProcessId -gt 0){
    $parent=Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue
    if($parent){
        if($parent.ProcessName -ne 'DeskFlow'){throw 'Installer may only restart its DeskFlow parent.'}
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DeskFlowSetupWindow {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd,uint message,IntPtr wp,IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd,System.Text.StringBuilder name,int count);
}
'@
        $parent.Refresh()
        $window=if($ParentWindowHandle -gt 0){[IntPtr]$ParentWindowHandle}else{$parent.MainWindowHandle}
        if($window -eq [IntPtr]::Zero){throw 'DeskFlow parent window could not be found.'}
        $windowPid=[uint32]0;[DeskFlowSetupWindow]::GetWindowThreadProcessId($window,[ref]$windowPid) | Out-Null
        $class=[Text.StringBuilder]::new(128);[DeskFlowSetupWindow]::GetClassName($window,$class,128) | Out-Null
        if($windowPid -ne $ParentProcessId -or $class.ToString() -ne 'DeskFlowPanel'){throw 'Installer parent window does not belong to DeskFlow.'}
        [DeskFlowSetupWindow]::PostMessage($window,0x8005,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
        if(!$parent.WaitForExit(45000)){throw 'DeskFlow is still finishing a task. Please exit it and run the installer again.'}
    }
}
if($DataDirectory){
    $migrationReceipt=Join-Path $receiptRoot ('migration-'+[Guid]::NewGuid().ToString('N')+'.json')
    $sourceArgument='"'+[IO.Path]::GetFullPath($SourceDataDirectory).TrimEnd('\')+'"'
    $targetArgument='"'+[IO.Path]::GetFullPath($DataDirectory).TrimEnd('\')+'"'
    $receiptArgument='"'+$migrationReceipt+'"'
    $migrationProcess=Start-Process -FilePath $SetupExecutablePath -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--migrate','--source',$sourceArgument,'--target',$targetArgument,'--receipt',$receiptArgument)
    if(!(Test-Path -LiteralPath $migrationReceipt)){throw 'Data migration did not finish. The source folder and previous storage setting remain available.'}
    $migration=Get-Content -LiteralPath $migrationReceipt -Raw -Encoding utf8 | ConvertFrom-Json
    if(!$migration.ok){throw $migration.error}
    if($migrationProcess.ExitCode -ne 0){throw 'Data migration returned an error. The source folder remains available.'}
    $setupLock.Dispose()
}
if((Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne $manifest.packageSha256){throw 'Package changed during administrator confirmation.'}
$finalSignature=Get-AuthenticodeSignature -LiteralPath $packagePath
if($finalSignature.Status -ne 'Valid' -or $finalSignature.SignerCertificate.Thumbprint -ne $allowedThumbprint){throw 'Package signer or signature changed before deployment.'}
Add-AppxPackage -Path $packagePath -ErrorAction Stop
$installed=Get-AppxPackage -Name 'DeskFlow.Desktop' | Where-Object { $_.Publisher -eq $allowedPublisher } | Sort-Object Version -Descending | Select-Object -First 1
$payloadLock.Dispose();$helperLock.Dispose()
if(!$installed){throw 'Windows did not register DeskFlow for the current user.'}

$receipt=@{ok=$true;installedAt=(Get-Date).ToString('o');packageFamilyName=$installed.PackageFamilyName;version=[string]$installed.Version;installLocation=$installed.InstallLocation;certificateThumbprint=$allowedThumbprint;trustStore='LocalMachine/TrustedPeople'}
if($DataDirectory){$receipt.dataDirectory=$DataDirectory;$receipt.sourceDataRetained=$true}
$receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $receiptRoot 'installed.json') -Encoding utf8
if($ReceiptPath){$receipt | ConvertTo-Json | Set-Content -LiteralPath $ReceiptPath -Encoding utf8}
if(!$NoLaunch){Start-Process -FilePath explorer.exe -WindowStyle Hidden -ArgumentList ('shell:AppsFolder\'+$installed.PackageFamilyName+'!DeskFlow')}
Write-Host 'DeskFlow installed for the current Windows user. Local OCR now has package identity.'
