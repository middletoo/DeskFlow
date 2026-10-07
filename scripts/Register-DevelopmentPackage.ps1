<#
.SYNOPSIS
Explicitly register a DeskFlow development package for the current user.
.NOTES
This script never imports certificates or enables Developer Mode. Use a signed
MSIX whose publisher you have explicitly trusted, or an unsigned layout after
you explicitly enable Windows Developer Mode. Run DeskFlow from Start afterward.
#>
[CmdletBinding(DefaultParameterSetName='Msix')]
param(
    [Parameter(Mandatory,ParameterSetName='Msix')][string]$MsixPath,
    [Parameter(Mandatory,ParameterSetName='Layout')][string]$LayoutDirectory
)
$ErrorActionPreference = 'Stop'
try {
    if ($PSCmdlet.ParameterSetName -eq 'Msix') {
        $deskPackagePath = [IO.Path]::GetFullPath($MsixPath)
        if (!(Test-Path -LiteralPath $deskPackagePath -PathType Leaf) -or [IO.Path]::GetExtension($deskPackagePath) -ne '.msix') { throw 'Specify an existing DeskFlow .msix file.' }
        Write-Host "Registering this development MSIX for the current user: $deskPackagePath"
        Add-AppxPackage -Path $deskPackagePath -ErrorAction Stop
    } else {
        $deskManifestPath = Join-Path ([IO.Path]::GetFullPath($LayoutDirectory)) 'AppxManifest.xml'
        if (!(Test-Path -LiteralPath $deskManifestPath -PathType Leaf)) { throw 'The layout AppxManifest.xml was not found.' }
        [xml]$deskManifest = Get-Content -LiteralPath $deskManifestPath -Raw
        if ($deskManifest.Package.Identity.Name -ne 'DeskFlow.Desktop') { throw 'The layout does not identify DeskFlow.Desktop.' }
        Write-Host "Registering this explicit development layout for the current user: $deskManifestPath"
        Add-AppxPackage -Register $deskManifestPath -ErrorAction Stop
    }
} catch {
    throw "DeskFlow registration failed. Certificate trust and Windows Developer Mode must be configured explicitly outside this script. Windows reported: $($_.Exception.Message)"
}
Write-Host 'DeskFlow is registered. Launch it from Start to give local OCR package identity.'
