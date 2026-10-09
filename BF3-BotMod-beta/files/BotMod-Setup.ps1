# BF3 BotMod installer / uninstaller - run through Install-BotMod.bat / Uninstall-BotMod.bat.
# Works with the bf4play / ALP layout (...\Battlefield 3 Server\Battlefield_3_Server) and BF-Portable
# (...\BF-Portable\Battlefield_3_Server). BotMod's settings folder is always "BotMod" next to Battlefield_3_Server.
param(
    [ValidateSet('install', 'uninstall')] [string]$Mode = 'install',
    [string]$ServerDir = ''
)
$ErrorActionPreference = 'Stop'

$Pkg        = $PSScriptRoot                       # ...\files
$PkgRoot    = Split-Path $Pkg -Parent             # folder holding Install-BotMod.bat
$ExeName    = 'bf3_server_Final.exe'
$ExeSha256  = 'F39F7623063CFDE9C449A0BDC9A57448D2D137D6DEF00B17EA04800EDA955470'
$StockXinputSha256 = '8E3534E38725C51EA9F08340C580374F0EBA8CFD4E6A83CF5BB741F518CD02ED'
# Files BotMod learns / users edit: only copied when missing, never overwritten.
$KeepIfPresent = @('BotMod_loadouts.txt', 'BotMod_kitextra.txt', 'BotMod_hq_*.txt', 'admins.txt', 'botcount.txt', 'kitskip.txt')

function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Ask([string]$q) { (Read-Host "$q [Y/N]") -match '^\s*y' }
function Fail([string]$m) { Say ""; Say "ERROR: $m" Red; exit 1 }

function Test-ServerDir([string]$d) { $d -and (Test-Path -LiteralPath (Join-Path $d $ExeName)) }

# Accepts the server folder itself or a folder that contains Battlefield_3_Server.
function Resolve-ServerDir([string]$d) {
    if (-not $d) { return $null }
    $d = $d.Trim().Trim('"').TrimEnd('\')
    if (Test-ServerDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    $sub = Join-Path $d 'Battlefield_3_Server'
    if (Test-ServerDir $sub) { return (Resolve-Path -LiteralPath $sub).Path }
    return $null
}

function Find-ServerDir {
    $cands = New-Object System.Collections.Generic.List[string]
    $p = $PkgRoot
    for ($i = 0; $i -lt 4 -and $p; $i++) { $cands.Add($p); $p = Split-Path $p -Parent }   # the zip was extracted in/under the server
    $cands.Add('C:\BF-Portable')
    $cands.Add('C:\ALPLauncher\server\Battlefield 3 Server')
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $cands.Add((Join-Path $drv.Root 'BF-Portable'))
        $cands.Add((Join-Path $drv.Root 'ALPLauncher\server\Battlefield 3 Server'))
    }
    foreach ($c in $cands) { $r = Resolve-ServerDir $c; if ($r) { return $r } }
    return $null
}

function Pick-ServerDir {
    Say "Select your BF3 server folder (the one containing $ExeName, or the folder above it)." Yellow
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "Select the BF3 server folder (Battlefield_3_Server, containing $ExeName)"
        $dlg.ShowNewFolderButton = $false
        if ($dlg.ShowDialog() -eq 'OK') { return (Resolve-ServerDir $dlg.SelectedPath) }
    } catch { }
    return (Resolve-ServerDir (Read-Host 'Or type / paste the folder path'))
}

function Get-ServerDir {
    if ($ServerDir) {
        $r = Resolve-ServerDir $ServerDir
        if (-not $r) { Fail "$ExeName not found in '$ServerDir'." }
        return $r
    }
    $r = Find-ServerDir
    if ($r) {
        Say "Found BF3 server: $r" Cyan
        if (Ask 'Use this folder?') { return $r }
    }
    $r = Pick-ServerDir
    if (-not $r) { Fail "No folder containing $ExeName was selected." }
    return $r
}

function Assert-ServerStopped([string]$srv) {
    while ($true) {
        $running = @(Get-Process -Name 'bf3_server_Final' -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and $_.Path.StartsWith($srv, [StringComparison]::OrdinalIgnoreCase) } catch { $true } })
        if (-not $running) { return }
        Say "The BF3 server is running ($($running.Count) instance(s)). Shut it down from your launcher first." Yellow
        if (-not (Ask 'Retry now that it is stopped?')) { Fail 'Cancelled - the server is still running.' }
    }
}

# Re-launch elevated when the server folder is not writable (e.g. under Program Files).
function Assert-Writable([string]$srv) {
    $probe = Join-Path $srv ('.botmod_write_test_' + [guid]::NewGuid().ToString('N'))
    try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force; return } catch { }
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Fail "Cannot write to $srv." }
    Say 'The server folder needs administrator rights - asking for them...' Yellow
    try { Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -NoExit -File `"$PSCommandPath`" -Mode $Mode -ServerDir `"$srv`"" } catch { Fail 'Administrator rights were refused.' }
    exit 0
}

function Is-BotModLoader([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($path)
    return ([Text.Encoding]::ASCII.GetString($bytes)).Contains('BotModCore.dll')
}

function Same-File([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $b)) { return $false }
    if ((Get-Item -LiteralPath $a).Length -ne (Get-Item -LiteralPath $b).Length) { return $false }
    return (Get-FileHash -LiteralPath $a).Hash -eq (Get-FileHash -LiteralPath $b).Hash
}

$script:BackupDir = $null
$script:BackedUp = 0
function Backup([string]$path, [string]$relName) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    if (-not (Test-Path -LiteralPath $script:BackupDir)) { New-Item -ItemType Directory -Path $script:BackupDir | Out-Null }
    $dst = Join-Path $script:BackupDir $relName
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir | Out-Null }
    Copy-Item -LiteralPath $path -Destination $dst -Force
    $script:BackedUp++
}

function Is-KeepFile([string]$name) { foreach ($k in $KeepIfPresent) { if ($name -like $k) { return $true } }; return $false }

# Copies every file of $srcDir into $dstDir: user/learned files only when missing, the rest replaced (old copy backed up).
function Copy-Set([string]$srcDir, [string]$dstDir, [string]$backupSub, [string[]]$skip) {
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir | Out-Null }
    $n = 0
    foreach ($f in Get-ChildItem -LiteralPath $srcDir -File) {
        if ($skip -contains $f.Name) { continue }
        $dst = Join-Path $dstDir $f.Name
        if (Test-Path -LiteralPath $dst) {
            if ((Is-KeepFile $f.Name) -or (Same-File $f.FullName $dst)) { continue }
            Backup $dst (Join-Path $backupSub $f.Name)
        }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        Unblock-File -LiteralPath $dst -ErrorAction SilentlyContinue
        $n++
    }
    return $n
}

# ---------------------------------------------------------------------------------------------------------------
Say ''
Say "  BF3 BotMod (beta) - $Mode" White
Say '  ======================================' White
$srv = Get-ServerDir
$root = Split-Path $srv -Parent            # ...\Battlefield 3 Server  or  ...\BF-Portable
$botModDir = Join-Path $root 'BotMod'
Assert-ServerStopped $srv
Assert-Writable $srv

$xin   = Join-Path $srv 'xinput9_1_0.dll'
$xorig = Join-Path $srv 'xinput9_1_0_orig.dll'
$stock = Join-Path $Pkg 'server\xinput9_1_0_stock.dll'

if ($Mode -eq 'uninstall') {
    if (Test-Path -LiteralPath $xorig) {
        Move-Item -LiteralPath $xorig -Destination $xin -Force
        Say 'Restored the original xinput9_1_0.dll.' Green
    } elseif (Is-BotModLoader $xin) {
        Copy-Item -LiteralPath $stock -Destination $xin -Force
        Say 'Put a stock xinput9_1_0.dll back (no original was saved).' Green
    } else {
        Say 'BotMod loader is not installed in this server.' Yellow
    }
    Get-ChildItem -LiteralPath $srv -Filter 'BotModCore*.dll' -File | Remove-Item -Force
    Say ''
    Say "BotMod removed - the server runs stock again." Green
    Say "Left in place (harmless, delete them if you like): BotMod_* data / log files in $srv and the folder $botModDir." DarkGray
    exit 0
}

# ---- install ----
$exe = Join-Path $srv $ExeName
$hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($hash -ne $ExeSha256) {
    Say ''
    Say "WARNING: $ExeName in this folder is not the build BotMod was made for." Red
    Say 'BotMod hooks one exact server build (the bf4play / ALP / BF-Portable one); on another build it disables' Red
    Say 'itself at best and crashes the server at worst.' Red
    if (-not (Ask 'Install anyway?')) { Fail 'Cancelled.' }
}

$script:BackupDir = Join-Path $root ('BotMod_backup_' + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))

# 1. The loader (proxy xinput9_1_0.dll) forwards the real XInput calls to xinput9_1_0_orig.dll.
$loader = Join-Path $Pkg 'server\xinput9_1_0.dll'
if (Test-Path -LiteralPath $xin) {
    if (Is-BotModLoader $xin) {
        if (-not (Same-File $loader $xin)) { Backup $xin 'Battlefield_3_Server\xinput9_1_0.dll' }   # older BotMod build
    } else {
        Backup $xin 'Battlefield_3_Server\xinput9_1_0.dll'
        if (Test-Path -LiteralPath $xorig) { Backup $xorig 'Battlefield_3_Server\xinput9_1_0_orig.dll' }
        Copy-Item -LiteralPath $xin -Destination $xorig -Force          # the server's own XInput becomes the forward target
    }
}
if (-not (Test-Path -LiteralPath $xorig) -or (Is-BotModLoader $xorig)) {
    Copy-Item -LiteralPath $stock -Destination $xorig -Force
}
Copy-Item -LiteralPath $loader -Destination $xin -Force
Unblock-File -LiteralPath $xin -ErrorAction SilentlyContinue
Say "Loader installed (xinput9_1_0.dll -> forwards to xinput9_1_0_orig.dll)." Green

# 2. Core, navigation maps and data next to the server exe; per-run copies of the core are recreated on start.
Get-ChildItem -LiteralPath $srv -Filter 'BotModCore.loaded*.dll' -File | Remove-Item -Force -ErrorAction SilentlyContinue
$n = Copy-Set (Join-Path $Pkg 'server') $srv 'Battlefield_3_Server' @('xinput9_1_0.dll', 'xinput9_1_0_stock.dll')
Say "Core + navigation data: $n file(s) copied into $srv" Green

# 3. Settings folder (BotMod next to Battlefield_3_Server) - existing settings are kept.
$n = Copy-Set (Join-Path $Pkg 'BotMod') $botModDir 'BotMod' @()
Say "Settings: $botModDir ($n new file(s); existing settings kept)" Green

Say ''
if ($script:BackedUp) { Say "Replaced files were backed up to: $($script:BackupDir)" Cyan }
Say 'BotMod installed. Start the server from your launcher as usual - a "BF3 BotMod" console opens with it.' Green
Say "Add your in-game name to $botModDir\admins.txt to use the admin chat commands." Yellow
exit 0
