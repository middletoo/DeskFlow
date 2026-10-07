param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'artifacts/validation/installed-ocr'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$package=Get-AppxPackage -Name 'DeskFlow.Desktop' | Where-Object {$_.Publisher -eq 'CN=DeskFlow Development'} | Sort-Object Version -Descending | Select-Object -First 1
if(!$package){throw 'DeskFlow is not installed for this user.'}
Add-Type -AssemblyName System.Drawing
$image=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'synthetic-ocr.png'
$report=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) 'result.json'
$bitmap=[Drawing.Bitmap]::new(900,240);$graphics=[Drawing.Graphics]::FromImage($bitmap);$font=[Drawing.Font]::new('Microsoft YaHei',38,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
try{$graphics.Clear([Drawing.Color]::White);$graphics.DrawString('Hello DeskFlow 2026',$font,[Drawing.Brushes]::Black,30,30);$graphics.DrawString([string]([char]0x4F60)+[char]0x597D+[char]0x4E16+[char]0x754C,$font,[Drawing.Brushes]::Black,30,110);$bitmap.Save($image,[Drawing.Imaging.ImageFormat]::Png)}finally{$font.Dispose();$graphics.Dispose();$bitmap.Dispose()}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport,Guid("2e941141-7f97-4756-ba1d-9decde894a3d"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IDeskActivation {
    int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string app,[MarshalAs(UnmanagedType.LPWStr)] string args,uint options,out uint pid);
    int ActivateForFile(IntPtr items,[MarshalAs(UnmanagedType.LPWStr)] string verb,out uint pid);
    int ActivateForProtocol(IntPtr items,out uint pid);
}
[ComImport,Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]public class DeskActivationManager {}
public static class DeskOcrActivation {
    public static uint Start(string app,string args){var activation=(IDeskActivation)new DeskActivationManager();uint pid;int result=activation.ActivateApplication(app,args,0,out pid);if(result<0)Marshal.ThrowExceptionForHR(result);return pid;}
}
'@
$arguments='--ocr-verify-image "'+$image+'" --ocr-verify-report "'+$report+'"'
$pidValue=[DeskOcrActivation]::Start($package.PackageFamilyName+'!DeskFlow',$arguments)
$deadline=(Get-Date).AddSeconds(45)
while(!(Test-Path -LiteralPath $report) -and (Get-Date) -lt $deadline){Start-Sleep -Milliseconds 200}
if(!(Test-Path -LiteralPath $report)){throw 'Installed OCR verification did not produce a result.'}
$result=Get-Content -LiteralPath $report -Raw -Encoding utf8 | ConvertFrom-Json
if(!$result.ok){throw "Installed OCR failed: $($result.error)"}
if($result.text -notmatch 'Hello' -or $result.text -notmatch 'DeskFlow'){throw 'OCR did not recognize the synthetic English sample.'}
$result | ConvertTo-Json -Depth 3
Write-Host 'PASS installed package identity and real OCR on synthetic text.'
