[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('host', 'controller')]
    [string]$Role,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Security.SecureString]$Password
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Connection credential storage requires Windows DPAPI.' }
Add-Type -AssemblyName System.Security
if ($null -eq $Password) {
    $Password = Read-Host 'Connection password (Host password for Controller)' -AsSecureString
}
$secretPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
$passwordBytes = $null
$salted = $null
$clientKey = $null
$serverKey = $null
$plaintextBytes = $null
try {
    $passwordText = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($secretPointer)
    $passwordBytes = [Text.Encoding]::UTF8.GetBytes($passwordText)
    if ($passwordBytes.Length -eq 0 -or $passwordBytes.Length -gt 1024 -or $passwordText.Contains([string][char]0)) {
        throw 'Connection password must be nonempty and at most 1024 UTF-8 bytes.'
    }
    $credentialSecret = $passwordText
    if ($Role -eq 'host') {
        $salt = New-Object byte[] 16
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($salt) } finally { $rng.Dispose() }
        $derivation = [Security.Cryptography.Rfc2898DeriveBytes]::new(
            $passwordBytes, $salt, 120000, [Security.Cryptography.HashAlgorithmName]::SHA256)
        try { $salted = $derivation.GetBytes(32) } finally { $derivation.Dispose() }
        $hmac = [Security.Cryptography.HMACSHA256]::new($salted)
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $clientKey = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes('Client Key'))
            $serverKey = $hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes('Server Key'))
            $credentialSecret = [ordered]@{
                version = 1; salt = [Convert]::ToBase64String($salt); iterations = 120000
                stored_key = [Convert]::ToBase64String($sha.ComputeHash($clientKey))
                server_key = [Convert]::ToBase64String($serverKey)
            } | ConvertTo-Json -Compress
        } finally { $hmac.Dispose(); $sha.Dispose() }
    }
    $payload = [ordered]@{version = 1; role = $Role; secret = $credentialSecret} | ConvertTo-Json -Compress
    $plaintextBytes = [Text.Encoding]::UTF8.GetBytes("RCD-LOCAL-AUTH-V1 $payload`n")
    $protectedBytes = [Security.Cryptography.ProtectedData]::Protect(
        $plaintextBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
    $destination = [IO.Path]::GetFullPath($OutputPath)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.File]::WriteAllText($destination, [Convert]::ToBase64String($protectedBytes), [Text.UTF8Encoding]::new($false))
    Write-Output $destination
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($secretPointer)
    foreach ($bytes in @($passwordBytes, $salted, $clientKey, $serverKey, $plaintextBytes)) {
        if ($null -ne $bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
    }
    $passwordText = $null; $credentialSecret = $null; $payload = $null
}
