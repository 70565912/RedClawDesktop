param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ValidationArgs
)

$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$validationScriptPath = Join-Path $scriptRoot "run-m07-t04-uac-e2e-validation.ps1"

if (-not (Test-Path $validationScriptPath)) {
    throw "Validation script not found: $validationScriptPath"
}

$quotedValidationScriptPath = '"' + $validationScriptPath + '"'
$argumentList = @(
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    $quotedValidationScriptPath
)

foreach ($arg in $ValidationArgs) {
    if (-not [string]::IsNullOrWhiteSpace($arg)) {
        $argumentList += $arg
    }
}

Start-Process -FilePath "powershell.exe" -ArgumentList $argumentList -Verb RunAs
