<#
This explicit administrator helper trusts only the reviewed DeskFlow development
certificate. Run only after the user approves the system trust change.
#>
param([Parameter(Mandatory)][string]$CertificatePath,[Parameter(Mandatory)][string]$ExpectedThumbprint,[Parameter(Mandatory)][string]$ResultPath)
$ErrorActionPreference='Stop'
try {
    $deskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.Path]::GetFullPath($CertificatePath))
    $allowedThumbprint='5E73D2B83E4733BB81072695B845044028283DA4'
    if($ExpectedThumbprint -ne $allowedThumbprint -or $deskCertificate.Subject -ne 'CN=DeskFlow Development' -or $deskCertificate.Thumbprint -ne $allowedThumbprint) { throw 'The development certificate does not match the reviewed publisher and fingerprint.' }
    $store=[Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople',[Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
    try{$store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite);$store.Add($deskCertificate)}finally{$store.Dispose()}
    @{ok=$true;thumbprint=$deskCertificate.Thumbprint;store='LocalMachine/TrustedPeople'} | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding utf8
} catch {
    @{ok=$false;error=$_.Exception.Message} | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding utf8
    exit 1
}
