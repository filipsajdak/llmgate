param([Parameter(Mandatory = $true)][string]$Binary)

$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot '..\..\install\install.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("llmgate-smoke-" + [Guid]::NewGuid().ToString('N'))
$release = Join-Path $work 'release'
$prefix = Join-Path $work 'prefix'
$failures = 0
$realLocal = $env:LOCALAPPDATA; $realRoaming = $env:APPDATA; $realProfile = $env:USERPROFILE
$realHome = $env:HOME; $realConfigHome = $env:XDG_CONFIG_HOME; $realStateHome = $env:XDG_STATE_HOME; $realDataHome = $env:XDG_DATA_HOME
$realOrgDir = $env:LLMGATE_ORG_DIR
$env:LOCALAPPDATA = Join-Path $work 'local'
$env:APPDATA = Join-Path $work 'roaming'
$env:USERPROFILE = Join-Path $work 'profile'
$env:HOME = $env:USERPROFILE
Remove-Item Env:\XDG_CONFIG_HOME, Env:\XDG_STATE_HOME, Env:\XDG_DATA_HOME, Env:\LLMGATE_ORG_DIR -ErrorAction SilentlyContinue
$stateDir = Join-Path (Join-Path (Join-Path $env:USERPROFILE '.local') 'state') 'llmgate'
$configDir = Join-Path (Join-Path $env:USERPROFILE '.config') 'llmgate'
New-Item -ItemType Directory -Force -Path $env:LOCALAPPDATA, $env:APPDATA, $env:USERPROFILE | Out-Null
$noTerminal = Join-Path $work 'no-terminal.txt'
New-Item -ItemType File -Force -Path $noTerminal | Out-Null

function Invoke-Installer([string[]]$Arguments) {
    $outFile = Join-Path $work ([Guid]::NewGuid().ToString('N') + '.out')
    $errFile = "$outFile.err"
    $start = @{
        FilePath = 'powershell.exe'
        ArgumentList = (@('-NoProfile', '-File', $installer) + $Arguments)
        RedirectStandardInput = $noTerminal
        RedirectStandardOutput = $outFile
        RedirectStandardError = $errFile
        Wait = $true
        PassThru = $true
        NoNewWindow = $true
    }
    $process = Start-Process @start
    $text = (Get-Content -Raw -LiteralPath $outFile -ErrorAction SilentlyContinue) + (Get-Content -Raw -LiteralPath $errFile -ErrorAction SilentlyContinue)
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Text = $text }
}

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
Check 'a dry run writes no state directory and no log' (-not (Test-Path -LiteralPath $stateDir))

& $installer @common | Out-Null
Check 'install puts llmgate.exe in the prefix' (Test-Path -LiteralPath $bin)
Check 'the installed binary reports the version' (((& $bin --version) -join '') -eq "llmgate $version")
Check 'the prefix is on the user PATH' ([Environment]::GetEnvironmentVariable('Path', 'User') -like "*$prefix*")
Check 'the install keeps a log that records the checksum' ((@(Get-ChildItem -LiteralPath $stateDir -Filter 'install-*.log' -ErrorAction SilentlyContinue)).Count -eq 1 -and ((Get-Content -Raw -LiteralPath (Get-ChildItem -LiteralPath $stateDir -Filter 'install-*.log' | Select-Object -First 1).FullName) -match 'checksum ok'))

$again = (& $installer @common 6>&1) -join "`n"
Check 'a second run says it is already installed' ($again -match 'already installed')

& $installer @common -Force | Out-Null
Check 'force keeps the old binary as .previous' (Test-Path -LiteralPath "$bin.previous")

if ($env:GITHUB_ACTIONS -eq 'true') {
    $savedUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    try {
        [Environment]::SetEnvironmentVariable('Path', $null, 'User')
        $bare = Invoke-Installer @('-Version', $version, '-BaseUrl', $release, '-Prefix', $prefix, '-NoInit', '-NoService', '-Yes', '-Force')
        Check 'an install works when the user has no PATH variable and puts the prefix on it' ($bare.ExitCode -eq 0 -and [Environment]::GetEnvironmentVariable('Path', 'User') -like "*$prefix*")
    } finally {
        [Environment]::SetEnvironmentVariable('Path', $savedUserPath, 'User')
    }
}

Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Value ('0' * 64 + "  $asset")
$bad = Start-Process -FilePath powershell.exe -Wait -PassThru -NoNewWindow -ArgumentList @('-NoProfile', '-File', $installer, '-Version', $version, '-BaseUrl', $release, '-Prefix', $prefix, '-NoInit', '-NoService', '-Yes', '-Force')
Check 'a wrong checksum is refused' ($bad.ExitCode -ne 0)


Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Value "$hash  $asset"
$arguments = @('-BaseUrl', $release, '-Prefix', $prefix, '-NoInit', '-NoService', '-Yes')

$notSemver = Invoke-Installer (@('-Version', '1.2') + $arguments)
Check 'a version that is not MAJOR.MINOR.PATCH is refused' ($notSemver.ExitCode -ne 0 -and $notSemver.Text -match 'MAJOR.MINOR.PATCH')
$badRepo = Invoke-Installer (@('-Version', $version, '-Repo', 'not a repo') + $arguments)
Check 'a repository that is not OWNER/NAME is refused' ($badRepo.ExitCode -ne 0 -and $badRepo.Text -match 'OWNER/NAME')
$realArch = $env:LLMGATE_ARCH
$env:LLMGATE_ARCH = 'arm64'
$badArch = Invoke-Installer (@('-Version', $version) + $arguments)
if ($null -eq $realArch) { Remove-Item Env:\LLMGATE_ARCH -ErrorAction SilentlyContinue } else { $env:LLMGATE_ARCH = $realArch }
Check 'an unsupported architecture is refused' ($badArch.ExitCode -ne 0 -and $badArch.Text -match 'unsupported architecture')

$other = '9.9.9'
$otherAsset = "llmgate-$other-windows-x86_64.zip"
$otherFolder = Join-Path $work "llmgate-$other-windows-x86_64"
New-Item -ItemType Directory -Force -Path $otherFolder | Out-Null
Copy-Item -LiteralPath $Binary -Destination (Join-Path $otherFolder 'llmgate.exe')
Compress-Archive -LiteralPath $otherFolder -DestinationPath (Join-Path $release $otherAsset)
$otherHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $release $otherAsset)).Hash.ToLowerInvariant()
Add-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Value "$otherHash  $otherAsset"
$kept = ((& $bin --version) -join '')
$mismatch = Invoke-Installer (@('-Version', $other, '-Force') + $arguments)
Check 'a binary that reports another version is refused and the installed one stays' ($mismatch.ExitCode -ne 0 -and $mismatch.Text -match 'reports' -and (((& $bin --version) -join '') -eq $kept))

$pack = Join-Path $PSScriptRoot '..\..\examples\org-pack'
$orgDry = Invoke-Installer (@('-Version', $version, '-Force', '-DryRun', '-Org', $pack) + $arguments)
Check 'org with a dry run only says what it would do' ($orgDry.ExitCode -eq 0 -and $orgDry.Text -match 'would: run llmgate org install')
$orgMissing = Invoke-Installer (@('-Version', $version, '-Force', '-Org', (Join-Path $work 'no-such-pack')) + $arguments)
Check 'an organisation pack that cannot be applied stops the install and names the command to retry' ($orgMissing.ExitCode -ne 0 -and $orgMissing.Text -match 'org install did not finish')
$orgApplied = Invoke-Installer (@('-Version', $version, '-Force', '-Org', $pack) + $arguments)
Check 'an organisation pack is applied before init' ($orgApplied.ExitCode -eq 0 -and $orgApplied.Text -match 'checksum ok')

& $installer -Prefix $prefix -Uninstall -Yes | Out-Null
Check 'uninstall removes the binary' (-not (Test-Path -LiteralPath $bin))
Check 'uninstall removes the prefix from the user PATH' ([Environment]::GetEnvironmentVariable('Path', 'User') -notlike "*$prefix*")

New-Item -ItemType Directory -Force -Path $configDir, $stateDir, (Join-Path $env:USERPROFILE '.claude-llmgate'), (Join-Path $env:USERPROFILE '.claude') | Out-Null
Set-Content -LiteralPath (Join-Path $configDir 'config.jsonc') -Value '{}'
Set-Content -LiteralPath (Join-Path $env:USERPROFILE '.claude\settings.json') -Value 'mine'
Check 'uninstall keeps the configuration' (Test-Path -LiteralPath (Join-Path $configDir 'config.jsonc'))

$noYes = Invoke-Installer @('-Prefix', $prefix, '-Uninstall', '-Purge')
Check 'purge without -Yes and without a terminal removes nothing' ($noYes.ExitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $configDir 'config.jsonc')) -and (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.claude-llmgate')))

$yes = Invoke-Installer @('-Prefix', $prefix, '-Uninstall', '-Purge', '-Yes')
Check 'purge with -Yes removes llmgate directories' ($yes.ExitCode -eq 0 -and -not (Test-Path -LiteralPath $configDir) -and -not (Test-Path -LiteralPath $stateDir) -and -not (Test-Path -LiteralPath (Join-Path $env:USERPROFILE '.claude-llmgate')))
Check 'purge never touches the Claude Code profile' ((Get-Content -Raw -LiteralPath (Join-Path $env:USERPROFILE '.claude\settings.json')).Trim() -eq 'mine')

$env:LOCALAPPDATA = $realLocal; $env:APPDATA = $realRoaming; $env:USERPROFILE = $realProfile
$env:HOME = $realHome
foreach ($pair in @(@('XDG_CONFIG_HOME', $realConfigHome), @('XDG_STATE_HOME', $realStateHome), @('XDG_DATA_HOME', $realDataHome))) {
    if ($null -eq $pair[1]) { Remove-Item "Env:\$($pair[0])" -ErrorAction SilentlyContinue } else { Set-Item "Env:\$($pair[0])" $pair[1] }
}
if ($null -eq $realOrgDir) { Remove-Item Env:\LLMGATE_ORG_DIR -ErrorAction SilentlyContinue } else { $env:LLMGATE_ORG_DIR = $realOrgDir }
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
if ($failures -gt 0) { exit 1 }
