<#
.SYNOPSIS
    Publish redclaw_desktop artifacts to release\<Configuration>\.
    Convenience wrapper for scripts/service/publish-desktop.ps1.

.PARAMETER Configuration
    Debug (default) or Release.

.PARAMETER PublishDirectory
    Override destination. Defaults to release\<Configuration>\.

.PARAMETER Qt6Dir
    Qt6 cmake directory. Falls back to Qt6_DIR env-var and CMake cache.

.EXAMPLE
    .\publish.ps1
    .\publish.ps1 -Configuration Release
    .\publish.ps1 -Configuration Debug -PublishDirectory C:\Staging\redclaw
#>
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$PublishDirectory = "",

    [string]$Qt6Dir = ""
)

$ErrorActionPreference = "Stop"

$impl = Join-Path $PSScriptRoot "scripts\service\publish-desktop.ps1"
if (-not (Test-Path $impl)) {
    throw "Implementation script not found: $impl"
}

$passArgs = @{ Configuration = $Configuration }
if (-not [string]::IsNullOrWhiteSpace($PublishDirectory)) { $passArgs['PublishDirectory'] = $PublishDirectory }
if (-not [string]::IsNullOrWhiteSpace($Qt6Dir))           { $passArgs['Qt6Dir']           = $Qt6Dir           }

& $impl @passArgs
