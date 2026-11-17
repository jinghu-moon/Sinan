param(
    [Parameter(Mandatory)][string]$CurrentExecutable,
    [Parameter(Mandatory)][string]$NewExecutable,
    [Parameter(Mandatory)][string]$DataDirectory,
    [switch]$ExecutableOnly
)
$ErrorActionPreference = 'Stop'
$currentPath = (Resolve-Path -LiteralPath $CurrentExecutable).Path
$sourcePath = (Resolve-Path -LiteralPath $NewExecutable).Path
$dataPath = [IO.Path]::GetFullPath($DataDirectory)
if ($currentPath -eq $sourcePath) { throw 'Select a separate build executable as the update source.' }
$runtimeNames = @('tabtail-hook.dll', 'tabtail-layout.dll', 'tabtail-features.dll', 'tabtail-style-0.7.dll', 'tabtail-login.exe', 'tabtail-symbols.exe')
foreach ($runtimeName in $runtimeNames) {
    $runtimeSource = Join-Path (Split-Path -Parent $sourcePath) $runtimeName
    $runtimeTarget = Join-Path (Split-Path -Parent $currentPath) $runtimeName
    if ($ExecutableOnly) {
        if (!(Test-Path -LiteralPath $runtimeTarget -PathType Leaf)) { throw "ExecutableOnly requires compatible $runtimeName." }
    } else {
        if (!(Test-Path -LiteralPath $runtimeSource -PathType Leaf)) { throw "New build is missing $runtimeName." }
        if ((Test-Path -LiteralPath $runtimeTarget) -and (Get-FileHash -LiteralPath $runtimeTarget).Hash -ne (Get-FileHash -LiteralPath $runtimeSource).Hash) {
            throw "$runtimeName differs. Exit processes using it before upgrading; no running process was changed."
        }
    }
}

# Both IPC and the loaded DLL validate the canonical executable path.
# Stop through that path, replace only the EXE, then start through the same path.
$stateText = & $currentPath --data-dir $dataPath agent status --json
if ($LASTEXITCODE -ne 0) {
    throw "Cannot contact the existing agent. CurrentExecutable must be the exact path that started it: $currentPath"
}
$state = $stateText | ConvertFrom-Json
$resumeTitles = $state.mode -eq 'customize'
$otherCommands = @(Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -eq $currentPath -and (!$state.running -or $_.ProcessId -ne $state.pid)
})
if ($otherCommands.Count) {
    throw 'Close the TUI and other commands using CurrentExecutable, then retry. The agent was not stopped.'
}
if ($state.running) {
    & $currentPath --data-dir $dataPath agent stop
    if ($LASTEXITCODE -ne 0) { throw 'Agent did not stop. No executable was replaced.' }
}
$remaining = @(Get-CimInstance Win32_Process | Where-Object ExecutablePath -EQ $currentPath)
if ($remaining.Count) {
    throw 'Close the TUI and any other commands using CurrentExecutable, then retry. No executable was replaced.'
}
if ((Get-FileHash -LiteralPath $currentPath).Hash -ne (Get-FileHash -LiteralPath $sourcePath).Hash) {
    $backupPath = $currentPath + '.backup-' + [guid]::NewGuid().ToString('N')
    Copy-Item -LiteralPath $currentPath -Destination $backupPath
    Copy-Item -LiteralPath $sourcePath -Destination $currentPath -Force
    Write-Host "Previous executable saved: $backupPath"
}
& $currentPath --version
if (!$ExecutableOnly) {
    foreach ($runtimeName in $runtimeNames) {
        $runtimeSource = Join-Path (Split-Path -Parent $sourcePath) $runtimeName
        $runtimeTarget = Join-Path (Split-Path -Parent $currentPath) $runtimeName
        if (!(Test-Path -LiteralPath $runtimeTarget)) { Copy-Item -LiteralPath $runtimeSource -Destination $runtimeTarget }
    }
}
if ($resumeTitles) { & $currentPath --data-dir $dataPath start }
else { & $currentPath --data-dir $dataPath agent start }
if ($LASTEXITCODE -ne 0) { throw "Update copied, but startup was not fully successful. Diagnose using: $currentPath --data-dir $dataPath status --json" }
Write-Host "Updated at the original path: $currentPath"
Write-Host 'Saved configuration is retained. Temporary tab overrides were cleared. The hook DLL was not replaced.'
