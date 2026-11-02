[CmdletBinding()]
param(
    [string]$Version = 'latest',
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\llmgate'),
    [string]$Repo = 'filipsajdak/llmgate',
    [string]$BaseUrl = '',
    [string]$Org = '',
    [string]$OrgRef = '',
    [switch]$NoInit,
    [switch]$NoService,
    [switch]$WithClaude,
    [switch]$Force,
    [switch]$DryRun,
    [switch]$Yes,
    [switch]$Uninstall,
    [switch]$Purge,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if ($Help) {
    @'
Install llmgate from a release.

  powershell -ExecutionPolicy Bypass -File install.ps1 [options]
  irm https://raw.githubusercontent.com/filipsajdak/llmgate/main/install/install.ps1 | iex

Options:
  -Version X.Y.Z   install this version (default: the latest release)
  -Prefix DIR      install into DIR (default: %LOCALAPPDATA%\Programs\llmgate)
  -Repo OWNER/NAME take releases from another repository (a fork)
  -Org SOURCE      apply an organisation pack (a directory or git URL) before init
  -OrgRef REF      the tag or full commit of a git URL pack
  -NoInit          do not run llmgate init (or llmgate setup) afterwards
  -NoService       do not offer the Task Scheduler autostart
  -WithClaude      install Claude Code with its official installer if it is missing
  -Force           reinstall even when the version is already installed
  -DryRun          print every step and change nothing
  -Yes             answer yes to every question
  -Uninstall       stop llmgate and remove the binary
  -Purge           with -Uninstall: also remove llmgate's configuration, state and profiles
  -BaseUrl URL     download from URL or directory instead of GitHub (for tests); needs -Version
  -Help            show this text
'@
    return
}

$stateDir = Join-Path $env:LOCALAPPDATA 'llmgate'
$configDir = Join-Path $env:APPDATA 'llmgate'
$log = ''
$taskName = 'llmgate'

function Stop-Install([string]$Message) {
    [Console]::Error.WriteLine("install: $Message")
    exit 1
}

function Say([string]$Message) {
    Write-Host $Message
    if ($script:log) {
        Add-Content -LiteralPath $script:log -Value $Message
    }
}

function Test-Step([string]$What) {
    if ($DryRun) {
        Say "would: $What"
        return $false
    }
    return $true
}

function Invoke-Quiet([scriptblock]$Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Command *> $null } catch { } finally { $ErrorActionPreference = $previous }
}

function Test-Terminal {
    return -not [Console]::IsInputRedirected
}

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    if (-not (Test-Terminal)) { return $false }
    $answer = Read-Host "$Question [y/N]"
    return $answer -match '^(y|yes)$'
}

if ($Repo -notmatch '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$' -or $Repo.Contains('..')) {
    Stop-Install '-Repo must look like OWNER/NAME'
}

$bin = Join-Path $Prefix 'llmgate.exe'

function Get-UserPath {
    return [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Test-OnUserPath([string]$Directory) {
    $entries = (Get-UserPath) -split ';' | Where-Object { $_ }
    return [bool]($entries | Where-Object { $_.TrimEnd('\') -ieq $Directory.TrimEnd('\') })
}

if ($Uninstall -or $Purge) {
    if ((Test-Path -LiteralPath $bin) -and (Test-Step "$bin stop")) {
        Invoke-Quiet { & $bin stop }
    }
    if (Test-Step "schtasks /Delete /TN $taskName /F") {
        Invoke-Quiet { schtasks.exe /End /TN $taskName }
        Invoke-Quiet { schtasks.exe /Delete /TN $taskName /F }
    }
    foreach ($file in @($bin, "$bin.previous")) {
        if ((Test-Path -LiteralPath $file) -and (Test-Step "remove $file")) {
            Remove-Item -LiteralPath $file -Force
            Say "removed $file"
        }
    }
    if ((Test-OnUserPath $Prefix) -and (Test-Step "remove $Prefix from the user PATH")) {
        $kept = ((Get-UserPath) -split ';' | Where-Object { $_ -and $_.TrimEnd('\') -ine $Prefix.TrimEnd('\') }) -join ';'
        [Environment]::SetEnvironmentVariable('Path', $kept, 'User')
        Say "removed $Prefix from the user PATH"
    }
    if ($Purge) {
        $found = @($configDir, $stateDir) + @(Get-ChildItem -LiteralPath $env:USERPROFILE -Directory -Filter '.claude-llmgate*' -ErrorAction SilentlyContinue | ForEach-Object FullName)
        $found = @($found | Where-Object { Test-Path -LiteralPath $_ })
        if ($found.Count -eq 0) {
            Say 'nothing to purge'
        } else {
            Say 'purge would remove:'
            $found | ForEach-Object { Say "  $_" }
            if ((Test-Step 'remove the directories above') -and (Confirm-Step 'Remove them (this deletes secrets and the usage ledger)?')) {
                $found | ForEach-Object { Remove-Item -LiteralPath $_ -Recurse -Force }
                Say 'purged'
            }
        }
    }
    return
}

if ($Version -ne 'latest' -and $Version -notmatch '^\d+\.\d+\.\d+$') {
    Stop-Install "version $Version is not MAJOR.MINOR.PATCH"
}

$machine = if ($env:LLMGATE_ARCH) { $env:LLMGATE_ARCH } else { $env:PROCESSOR_ARCHITECTURE }
switch -Regex ($machine) {
    '^(AMD64|x86_64)$' { $arch = 'x86_64' }
    default { Stop-Install "unsupported architecture $machine" }
}

if (-not $DryRun) {
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    $log = Join-Path $stateDir ("install-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType File -Force -Path $log | Out-Null
}

function Get-Release([string]$From, [string]$To) {
    if ($From -match '^https?://') {
        Invoke-WebRequest -UseBasicParsing -Uri $From -OutFile $To
    } else {
        Copy-Item -LiteralPath (Join-Path ($From -replace '^file:/+','') (Split-Path $To -Leaf)) -Destination $To -ErrorAction Stop
    }
}

if ($Version -eq 'latest') {
    if ($BaseUrl) { Stop-Install '-BaseUrl needs -Version' }
    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/$Repo/releases/latest" -MaximumRedirection 5
        $tag = ($response.BaseResponse.RequestMessage.RequestUri.AbsoluteUri -split '/')[-1]
    } catch {
        Stop-Install 'cannot reach github.com'
    }
    $Version = $tag.TrimStart('v')
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        Stop-Install "the latest release of $Repo has no version tag (got $tag)"
    }
}

$releaseUrl = if ($BaseUrl) { $BaseUrl } else { "https://github.com/$Repo/releases/download/v$Version" }
$asset = "llmgate-$Version-windows-$arch.zip"

Say "llmgate $Version for windows-$arch into $Prefix"

$installed = ''
if (Test-Path -LiteralPath $bin) {
    $installed = (& $bin --version 2>$null) -join ''
}

if ($installed -eq "llmgate $Version" -and -not $Force) {
    Say "llmgate $Version is already installed at $bin"
} elseif (Test-Step "download $releaseUrl/$asset, check it against SHA256SUMS.txt and install it to $bin") {
    $work = Join-Path ([IO.Path]::GetTempPath()) ("llmgate-install-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    try {
        try { Get-Release "$releaseUrl" (Join-Path $work $asset) } catch { Stop-Install "cannot download $releaseUrl/$asset" }
        try { Get-Release "$releaseUrl" (Join-Path $work 'SHA256SUMS.txt') } catch { Stop-Install "cannot download $releaseUrl/SHA256SUMS.txt" }
        $line = Get-Content -LiteralPath (Join-Path $work 'SHA256SUMS.txt') | Where-Object { ($_ -split '\s+')[1] -eq $asset } | Select-Object -First 1
        if (-not $line) { Stop-Install "$asset is not listed in SHA256SUMS.txt" }
        $expected = ($line -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $work $asset)).Hash.ToLowerInvariant()
        if ($expected -ne $actual) { Stop-Install "checksum mismatch for $asset; nothing was installed" }
        Say 'checksum ok'
        Expand-Archive -LiteralPath (Join-Path $work $asset) -DestinationPath $work -Force
        $fresh = Join-Path $work "llmgate-$Version-windows-$arch\llmgate.exe"
        if (-not (Test-Path -LiteralPath $fresh)) { Stop-Install "$asset does not contain llmgate.exe" }
        $reported = (& $fresh --version 2>$null) -join ''
        if ($reported -ne "llmgate $Version") { Stop-Install "the downloaded binary reports '$reported', expected 'llmgate $Version'" }
        New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
        if (Test-Path -LiteralPath $bin) {
            Copy-Item -LiteralPath $bin -Destination "$bin.previous" -Force
            Say "kept the old binary as $bin.previous"
        }
        Copy-Item -LiteralPath $fresh -Destination "$bin.new" -Force
        Move-Item -LiteralPath "$bin.new" -Destination $bin -Force
        Say "installed $bin"
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not (Test-OnUserPath $Prefix)) {
    if ((Confirm-Step "Add $Prefix to your user PATH?") -and (Test-Step "add $Prefix to the user PATH")) {
        [Environment]::SetEnvironmentVariable('Path', ((Get-UserPath).TrimEnd(';') + ";$Prefix").TrimStart(';'), 'User')
        Say "added $Prefix to the user PATH; open a new terminal to use it"
    } else {
        Say "$Prefix is not on your PATH; add it with:"
        Say "  [Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path','User') + ';$Prefix', 'User')"
    }
}

if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
    Say 'Claude Code is not installed.'
    if ($WithClaude -or (Confirm-Step 'Install Claude Code with its official installer (https://claude.ai/install.ps1)?')) {
        if (Test-Step 'irm https://claude.ai/install.ps1 | iex') {
            try { Invoke-RestMethod https://claude.ai/install.ps1 | Invoke-Expression } catch { Say "Claude Code's installer failed; install it yourself and run this script again" }
        }
    } else {
        Say 'Install it from https://claude.ai/install.ps1, then run llmgate again.'
    }
}

if ($Org) {
    $orgArguments = @('org', 'install', $Org)
    if ($OrgRef) { $orgArguments += @('--ref', $OrgRef) }
    if ($Yes) { $orgArguments += '--yes' }
    if ($DryRun) {
        Say "would: run llmgate $($orgArguments -join ' ')"
    } elseif ($Yes -or (Test-Terminal)) {
        & $bin @orgArguments
        if ($LASTEXITCODE -ne 0) { Stop-Install "llmgate org install did not finish; run it again: $bin $($orgArguments -join ' ')" }
    } else {
        Say "no terminal to confirm the organisation pack; run: $bin $($orgArguments -join ' ')"
    }
}

if (-not $NoInit) {
    if ($DryRun) {
        Say 'would: run llmgate init (or llmgate setup)'
    } elseif (Test-Terminal) {
        $usage = (cmd.exe /c "`"$bin`" --no-such-flag 2>&1") -join "`n"
        $command = if ($usage -match '\| init ') { 'init' } else { 'setup' }
        Say "running llmgate $command"
        & $bin $command
        if ($LASTEXITCODE -ne 0) { Say "llmgate $command did not finish; run it again when you are ready" }
    } else {
        Say "no terminal for questions; run: $bin setup"
    }
}

if ((Test-Path -LiteralPath $bin) -and -not $NoService) {
    if ((Confirm-Step 'Start llmgate at logon with Task Scheduler?') -and (Test-Step "$bin setup --service")) {
        & $bin setup --service
        if ($LASTEXITCODE -ne 0) { Say "Task Scheduler setup did not finish; run it again: $bin setup --service" }
    }
}

Say 'done; start Claude Code through the gateway with: llmgate claude'
