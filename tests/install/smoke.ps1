param([Parameter(Mandatory = $true)][string]$Binary)

$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot '..\..\install\install.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("llmgate-smoke-" + [Guid]::NewGuid().ToString('N'))
$release = Join-Path $work 'release'
$prefix = Join-Path $work 'prefix'
$failures = 0

function Check([string]$Label, [bool]$Condition) {
    if ($Condition) { Write-Host "ok   $Label" } else { Write-Host "FAIL $Label"; $script:failures++ }
}

New-Item -ItemType Directory -Force -Path $release | Out-Null
$version = ((& $Binary --version) -join '') -replace '^llmgate ', ''
$asset = "llmgate-$version-windows-x86_64.zip"
$folder = Join-Path $work "llmgate-$version-windows-x86_64"
New-Item -ItemType Directory -Force -Path $folder | Out-Null
Copy-Item -LiteralPath $Binary -Destination (Join-Path $folder 'llmgate.exe')
Compress-Archive -LiteralPath $folder -DestinationPath (Join-Path $release $asset)
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $release $asset)).Hash.ToLowerInvariant()
"$hash  $asset" | Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt')

$common = @{ Version = $version; BaseUrl = $release; Prefix = $prefix; NoInit = $true; NoService = $true; Yes = $true }
$bin = Join-Path $prefix 'llmgate.exe'

& $installer @common -DryRun | Out-Null
Check 'dry run installs nothing' (-not (Test-Path -LiteralPath $bin))

& $installer @common | Out-Null
Check 'install puts llmgate.exe in the prefix' (Test-Path -LiteralPath $bin)
Check 'the installed binary reports the version' (((& $bin --version) -join '') -eq "llmgate $version")
Check 'the prefix is on the user PATH' ([Environment]::GetEnvironmentVariable('Path', 'User') -like "*$prefix*")

$again = (& $installer @common 6>&1) -join "`n"
Check 'a second run says it is already installed' ($again -match 'already installed')

& $installer @common -Force | Out-Null
Check 'force keeps the old binary as .previous' (Test-Path -LiteralPath "$bin.previous")

Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Value ('0' * 64 + "  $asset")
$bad = Start-Process -FilePath powershell.exe -Wait -PassThru -NoNewWindow -ArgumentList @('-NoProfile', '-File', $installer, '-Version', $version, '-BaseUrl', $release, '-Prefix', $prefix, '-NoInit', '-NoService', '-Yes', '-Force')
Check 'a wrong checksum is refused' ($bad.ExitCode -ne 0)

& $installer -Prefix $prefix -Uninstall -Yes | Out-Null
Check 'uninstall removes the binary' (-not (Test-Path -LiteralPath $bin))
Check 'uninstall removes the prefix from the user PATH' ([Environment]::GetEnvironmentVariable('Path', 'User') -notlike "*$prefix*")

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failures -gt 0) { exit 1 }
