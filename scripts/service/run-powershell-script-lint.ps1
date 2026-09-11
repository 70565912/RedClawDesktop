param(
    [string]$SettingsPath = "PSScriptAnalyzerSettings.psd1",
    [string]$ScriptsRoot = "scripts",
    [ValidateSet("Error", "Warning", "Information")]
    [string]$FailOnSeverity = "Warning"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$resolvedSettingsPath = Join-Path $repoRoot $SettingsPath
$resolvedScriptsRoot = Join-Path $repoRoot $ScriptsRoot

if (-not (Test-Path $resolvedSettingsPath)) {
    throw "PSScriptAnalyzer settings file not found: $resolvedSettingsPath"
}

if (-not (Test-Path $resolvedScriptsRoot)) {
    throw "Scripts root not found: $resolvedScriptsRoot"
}

if (-not (Get-Module -ListAvailable -Name PSScriptAnalyzer)) {
    throw "PSScriptAnalyzer is not installed. Run: Install-Module PSScriptAnalyzer -Scope CurrentUser"
}

$scriptFiles = Get-ChildItem -Path $resolvedScriptsRoot -Recurse -File -Filter "*.ps1"
if ($scriptFiles.Count -eq 0) {
    Write-Host "No PowerShell scripts found under $resolvedScriptsRoot"
    exit 0
}

$results = @()
foreach ($file in $scriptFiles) {
    $results += Invoke-ScriptAnalyzer -Path $file.FullName -Settings $resolvedSettingsPath
}

if ($results.Count -eq 0) {
    Write-Host "PSScriptAnalyzer: no issues found."
    exit 0
}

$severityOrder = @{
    Information = 1
    Warning = 2
    Error = 3
}
$threshold = $severityOrder[$FailOnSeverity]
$blocking = $results | Where-Object {
    $severityValue = [string]$_.Severity
    $severityOrder[$severityValue] -ge $threshold
}

$results |
    Sort-Object ScriptName, Line, Severity |
    Select-Object ScriptName, Line, Severity, RuleName, Message |
    Format-Table -AutoSize | Out-String | Write-Host

if ($blocking.Count -gt 0) {
    Write-Host "PSScriptAnalyzer found $($blocking.Count) blocking issue(s) at severity >= $FailOnSeverity." -ForegroundColor Red
    exit 1
}

Write-Host "PSScriptAnalyzer found issues, but none at severity >= $FailOnSeverity."
exit 0
