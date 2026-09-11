param(
  [Parameter(Mandatory=$true)]
  [string]$RemoteUrl,

  [string]$DefaultBranch = "main"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path ".git")) {
  git init | Out-Null
}

git add .

try {
  git commit -m "chore: initialize project docs and github governance" | Out-Null
} catch {
  Write-Host "Initial commit may have already been created or git user identity is missing."
}

$currentBranch = git rev-parse --abbrev-ref HEAD
if ($currentBranch -eq "HEAD") {
  git checkout -b $DefaultBranch | Out-Null
} elseif ($currentBranch -ne $DefaultBranch) {
  git branch -M $DefaultBranch | Out-Null
}

$hasOrigin = git remote | Select-String -Pattern "^origin$" -Quiet
if ($hasOrigin) {
  git remote set-url origin $RemoteUrl
} else {
  git remote add origin $RemoteUrl
}

git push -u origin $DefaultBranch
Write-Host "Remote bootstrap complete: $RemoteUrl"
