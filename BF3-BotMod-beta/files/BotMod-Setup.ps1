# BF3 BotMod installer / uninstaller - a small window (Install-BotMod.bat / Uninstall-BotMod.bat open it).
# Point it at bf3_server_Final.exe: that file's folder is the server folder (Battlefield_3_Server or whatever it is
# called), BotMod's settings folder is "BotMod" next to it, and an uninstall.bat for that exact location is written
# into the settings folder. -NoGui -ServerDir <folder> runs it from a console instead.
param(
    [ValidateSet('install', 'uninstall')] [string]$Mode = 'install',
    [string]$ServerDir = '',
    [switch]$NoGui
)
$ErrorActionPreference = 'Stop'

$Pkg        = $PSScriptRoot                       # ...\files
$PkgRoot    = Split-Path $Pkg -Parent             # folder holding Install-BotMod.bat
$PkgServer  = Join-Path $Pkg 'Battlefield_3_Server'   # what goes next to the server exe
$PkgBotMod  = Join-Path $Pkg 'BotMod'                 # the settings folder
$ExeName    = 'bf3_server_Final.exe'
$ExeSha256  = 'F39F7623063CFDE9C449A0BDC9A57448D2D137D6DEF00B17EA04800EDA955470'
# Files BotMod learns / users edit: only copied when missing, never overwritten.
$KeepIfPresent = @('BotMod_loadouts.txt', 'BotMod_kitextra.txt', 'BotMod_hq_*.txt', 'admins.txt', 'botcount*.txt', 'kitskip.txt', 'BotMod_bounds.txt')
# Never copied from the package: logs, per-run core copies, the stock XInput (handled separately).
$SkipServer = @('xinput9_1_0.dll', 'xinput9_1_0_stock.dll', 'BotModCore.loaded*.dll', 'BotMod_*.log', 'BotMod_*.old.log')

$script:Gui = $null      # the log box when the window is up
function Say([string]$m, [string]$c = 'Gray') {
    if ($script:Gui) {
        $box = $script:Gui
        $box.SelectionStart = $box.TextLength; $box.SelectionLength = 0
        $box.SelectionColor = switch ($c) { 'Red' { 'Firebrick' } 'Green' { 'ForestGreen' } 'Yellow' { 'DarkGoldenrod' } 'Cyan' { 'SteelBlue' } 'White' { 'Black' } default { 'DimGray' } }
        $box.AppendText("$m`r`n"); $box.ScrollToCaret()
        [System.Windows.Forms.Application]::DoEvents()
    } else { Write-Host $m -ForegroundColor $c }
}
function Ask([string]$q) {
    if ($script:Gui) { return ([System.Windows.Forms.MessageBox]::Show($q, 'BF3 BotMod', 'YesNo', 'Question') -eq 'Yes') }
    return ((Read-Host "$q [Y/N]") -match '^\s*y')
}
function Fail([string]$m) { throw $m }

function Test-ServerDir([string]$d) { $d -and (Test-Path -LiteralPath (Join-Path $d $ExeName)) }

# Accepts bf3_server_Final.exe itself, its folder, or a folder that contains Battlefield_3_Server.
function Resolve-ServerDir([string]$d) {
    if (-not $d) { return $null }
    $d = $d.Trim().Trim('"').TrimEnd('\')
    if ($d -like "*\$ExeName" -and (Test-Path -LiteralPath $d)) { $d = Split-Path $d -Parent }
    if (Test-ServerDir $d) { return (Resolve-Path -LiteralPath $d).Path }
    $sub = Join-Path $d 'Battlefield_3_Server'
    if (Test-ServerDir $sub) { return (Resolve-Path -LiteralPath $sub).Path }
    return $null
}

# A first guess for the box (the zip extracted in/under the server, or the usual install spots).
function Find-ServerDir {
    $cands = New-Object System.Collections.Generic.List[string]
    $p = $PkgRoot
    for ($i = 0; $i -lt 4 -and $p; $i++) { $cands.Add($p); $p = Split-Path $p -Parent }
    foreach ($drv in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $cands.Add((Join-Path $drv.Root 'BF-Portable'))
        $cands.Add((Join-Path $drv.Root 'ALPLauncher\server\Battlefield 3 Server'))
    }
    foreach ($c in $cands) { $r = Resolve-ServerDir $c; if ($r) { return $r } }
    return $null
}

function Get-RunningServers([string]$srv) {
    @(Get-Process -Name 'bf3_server_Final' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and $_.Path.StartsWith($srv, [StringComparison]::OrdinalIgnoreCase) } catch { $true } })
}
function Assert-ServerStopped([string]$srv) {
    while ($true) {
        $running = Get-RunningServers $srv
        if (-not $running) { return }
        if (-not (Ask "The BF3 server in this folder is running ($($running.Count) instance(s)).`r`nShut it down from your launcher, then press Yes to continue.")) { Fail 'Cancelled - the server is still running.' }
    }
}

function Test-Writable([string]$srv) {
    $probe = Join-Path $srv ('.botmod_write_test_' + [guid]::NewGuid().ToString('N'))
    try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force; return $true } catch { return $false }
}
# Not writable (e.g. under Program Files): the same window again, elevated, with the folder filled in.
function Restart-Elevated([string]$srv) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Fail "Cannot write to $srv." }
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Mode $Mode -ServerDir `"$srv`""
    if ($NoGui) { $argList = "-NoExit $argList -NoGui" } else { $argList = "-WindowStyle Hidden $argList" }
    try { Start-Process powershell.exe -Verb RunAs -ArgumentList $argList } catch { Fail 'Administrator rights were refused.' }
    return $true
}

function Is-BotModLoader([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    return ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))).Contains('BotModCore.dll')
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
    $dst = Join-Path $script:BackupDir $relName
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir -Force | Out-Null }
    Copy-Item -LiteralPath $path -Destination $dst -Force
    $script:BackedUp++
}
function Matches-Any([string]$name, [string[]]$patterns) { foreach ($k in $patterns) { if ($name -like $k) { return $true } }; return $false }

# Copies every file of $srcDir into $dstDir: user/learned files only when missing, the rest replaced (old copy backed up).
function Copy-Set([string]$srcDir, [string]$dstDir, [string]$backupSub, [string[]]$skip) {
    if (-not (Test-Path -LiteralPath $srcDir)) { Fail "The package is missing $srcDir." }
    if (-not (Test-Path -LiteralPath $dstDir)) { New-Item -ItemType Directory -Path $dstDir | Out-Null }
    $n = 0
    foreach ($f in Get-ChildItem -LiteralPath $srcDir -File) {
        if (Matches-Any $f.Name $skip) { continue }
        $dst = Join-Path $dstDir $f.Name
        if (Test-Path -LiteralPath $dst) {
            if ((Matches-Any $f.Name $KeepIfPresent) -or (Same-File $f.FullName $dst)) { continue }
            Backup $dst (Join-Path $backupSub $f.Name)
        }
        Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        Unblock-File -LiteralPath $dst -ErrorAction SilentlyContinue
        $n++
    }
    foreach ($d in Get-ChildItem -LiteralPath $srcDir -Directory) {   # (sub folders: src, tools, out)
        $n += Copy-Set $d.FullName (Join-Path $dstDir $d.Name) (Join-Path $backupSub $d.Name) $skip
    }
    return $n
}

# uninstall.bat in the settings folder, pointing at this exact server folder (absolute - the server folder need not be
# called Battlefield_3_Server, nor sit where the package expected it).
function Write-UninstallBat([string]$srv, [string]$botModDir) {
    $bat = @"
@echo off
:: BF3 BotMod uninstall - written by the installer for this server:
::   $srv
:: Puts the original xinput9_1_0.dll back and removes the BotMod core (shut the server down first).
setlocal
set "SRV=$srv"
tasklist /fi "imagename eq bf3_server_Final.exe" 2>nul | findstr /i "bf3_server_Final.exe" >nul && ( echo Shut the BF3 server down first. & pause & exit /b 1 )
del /q "%SRV%\BotModCore.dll" "%SRV%\BotModCore.loaded*.dll" 2>nul
if not exist "%SRV%\xinput9_1_0_orig.dll" ( echo BotMod's loader is not installed in %SRV%. & pause & exit /b 0 )
move /y "%SRV%\xinput9_1_0_orig.dll" "%SRV%\xinput9_1_0.dll" >nul
echo BotMod removed - the server runs stock again.
echo Left in place (harmless): BotMod_* data / log files in %SRV% and this BotMod folder.
pause
"@
    $path = Join-Path $botModDir 'uninstall.bat'
    [IO.File]::WriteAllText($path, ($bat -replace "`r?`n", "`r`n"), [Text.Encoding]::ASCII)
    return $path
}
# install.bat (developer: copies a fresh build from out\) - the same, pointed at this server folder.
function Set-InstallBatTarget([string]$srv, [string]$botModDir) {
    $path = Join-Path $botModDir 'install.bat'
    if (-not (Test-Path -LiteralPath $path)) { return }
    $t = [IO.File]::ReadAllText($path)
    $t = $t -replace 'set "SRV=[^"]*"', ('set "SRV=' + $srv.Replace('$', '$$') + '"')
    [IO.File]::WriteAllText($path, $t, [Text.Encoding]::ASCII)
}

function Invoke-Uninstall([string]$srv) {
    $root = Split-Path $srv -Parent
    $botModDir = Join-Path $root 'BotMod'
    $xin   = Join-Path $srv 'xinput9_1_0.dll'
    $xorig = Join-Path $srv 'xinput9_1_0_orig.dll'
    $stock = Join-Path $PkgServer 'xinput9_1_0_stock.dll'
    Assert-ServerStopped $srv
    if (-not (Test-Writable $srv)) { if (Restart-Elevated $srv) { return 'elevated' } }
    if (Test-Path -LiteralPath $xorig) {
        Move-Item -LiteralPath $xorig -Destination $xin -Force
        Say 'Restored the original xinput9_1_0.dll.' Green
    } elseif (Is-BotModLoader $xin) {
        Copy-Item -LiteralPath $stock -Destination $xin -Force
        Say 'Put a stock xinput9_1_0.dll back (no original was saved).' Green
    } else {
        Say 'The BotMod loader is not installed in this server.' Yellow
    }
    Get-ChildItem -LiteralPath $srv -Filter 'BotModCore*.dll' -File | Remove-Item -Force
    Say ''
    Say 'BotMod removed - the server runs stock again.' Green
    Say "Left in place (harmless, delete them if you like): BotMod_* data / log files in $srv and the folder $botModDir." DarkGray
    return 'done'
}

function Invoke-Install([string]$srv) {
    $root = Split-Path $srv -Parent            # ...\Battlefield 3 Server  or  ...\BF-Portable
    $botModDir = Join-Path $root 'BotMod'
    Assert-ServerStopped $srv
    if (-not (Test-Writable $srv) -or ((Test-Path -LiteralPath $botModDir) -and -not (Test-Writable $botModDir))) { if (Restart-Elevated $srv) { return 'elevated' } }

    $hash = (Get-FileHash -LiteralPath (Join-Path $srv $ExeName) -Algorithm SHA256).Hash
    if ($hash -ne $ExeSha256) {
        Say "WARNING: $ExeName in this folder is not the build BotMod was made for." Red
        if (-not (Ask "$ExeName in this folder is not the build BotMod was made for (the bf4play / ALP / BF-Portable one).`r`nOn another build BotMod disables itself at best and crashes the server at worst.`r`n`r`nInstall anyway?")) { Fail 'Cancelled.' }
    }

    $xin    = Join-Path $srv 'xinput9_1_0.dll'
    $xorig  = Join-Path $srv 'xinput9_1_0_orig.dll'
    $stock  = Join-Path $PkgServer 'xinput9_1_0_stock.dll'
    $loader = Join-Path $PkgServer 'xinput9_1_0.dll'
    if (-not (Test-Path -LiteralPath $loader) -or -not (Test-Path -LiteralPath $stock)) { Fail "The package is incomplete - $PkgServer must hold xinput9_1_0.dll and xinput9_1_0_stock.dll." }
    $script:BackupDir = Join-Path $root ('BotMod_backup_' + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
    $script:BackedUp = 0
    $srvName = Split-Path $srv -Leaf

    # 1. The loader (proxy xinput9_1_0.dll) forwards the real XInput calls to xinput9_1_0_orig.dll.
    if (Test-Path -LiteralPath $xin) {
        if (Is-BotModLoader $xin) {
            if (-not (Same-File $loader $xin)) { Backup $xin "$srvName\xinput9_1_0.dll" }   # an older BotMod build
        } else {
            Backup $xin "$srvName\xinput9_1_0.dll"
            if (Test-Path -LiteralPath $xorig) { Backup $xorig "$srvName\xinput9_1_0_orig.dll" }
            Copy-Item -LiteralPath $xin -Destination $xorig -Force          # the server's own XInput becomes the forward target
        }
    }
    if (-not (Test-Path -LiteralPath $xorig) -or (Is-BotModLoader $xorig)) { Copy-Item -LiteralPath $stock -Destination $xorig -Force }
    Copy-Item -LiteralPath $loader -Destination $xin -Force
    Unblock-File -LiteralPath $xin -ErrorAction SilentlyContinue
    Say 'Loader installed (xinput9_1_0.dll -> forwards to xinput9_1_0_orig.dll).' Green

    # 2. Core, navigation maps and data next to the server exe; per-run copies of the core are recreated on start.
    Get-ChildItem -LiteralPath $srv -Filter 'BotModCore.loaded*.dll' -File | Remove-Item -Force -ErrorAction SilentlyContinue
    Say 'Copying the core and navigation data (a few hundred MB - give it a moment)...'
    $n = Copy-Set $PkgServer $srv $srvName $SkipServer
    Say "Core + navigation data: $n file(s) copied into $srv" Green

    # 3. Settings folder (BotMod next to the server folder) - existing settings are kept.
    $n = Copy-Set $PkgBotMod $botModDir 'BotMod' @('uninstall.bat')
    Say "Settings: $botModDir ($n new file(s); existing settings kept)" Green
    $u = Write-UninstallBat $srv $botModDir
    Set-InstallBatTarget $srv $botModDir
    Say "Uninstaller for this server written: $u" Green

    Say ''
    if ($script:BackedUp) { Say "Replaced files were backed up to: $($script:BackupDir)" Cyan }
    Say 'BotMod installed. Start the server from your launcher as usual - a "BF3 BotMod" console opens with it.' Green
    Say "Add your in-game name to $botModDir\admins.txt to use the admin chat commands." Yellow
    return 'done'
}

# ---------------------------------------------------------------------------------------------------------------
if ($NoGui) {
    Say ''
    Say "  BF3 BotMod (beta) - $Mode" White
    try {
        $srv = Resolve-ServerDir $ServerDir
        if (-not $srv) { $srv = Find-ServerDir; if ($srv -and -not (Ask "Use the BF3 server in $srv ?")) { $srv = $null } }
        if (-not $srv) { $srv = Resolve-ServerDir (Read-Host "Path to $ExeName (or its folder)") }
        if (-not $srv) { Fail "No folder containing $ExeName was given." }
        if ($Mode -eq 'uninstall') { [void](Invoke-Uninstall $srv) } else { [void](Invoke-Install $srv) }
        exit 0
    } catch { Say ''; Say "ERROR: $($_.Exception.Message)" Red; exit 1 }
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = 'BF3 BotMod (beta) - setup'
$form.Size = New-Object System.Drawing.Size(720, 520)
$form.MinimumSize = New-Object System.Drawing.Size(560, 420)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$lbl = New-Object System.Windows.Forms.Label
$lbl.Text = "Your BF3 server's $ExeName (BotMod installs next to it; its settings go in a ""BotMod"" folder beside that folder):"
$lbl.Location = New-Object System.Drawing.Point(12, 12); $lbl.Size = New-Object System.Drawing.Size(680, 20); $lbl.Anchor = 'Top, Left, Right'
$form.Controls.Add($lbl)

$path = New-Object System.Windows.Forms.TextBox
$path.Location = New-Object System.Drawing.Point(12, 36); $path.Size = New-Object System.Drawing.Size(580, 24); $path.Anchor = 'Top, Left, Right'
$form.Controls.Add($path)

$browse = New-Object System.Windows.Forms.Button
$browse.Text = 'Browse...'; $browse.Location = New-Object System.Drawing.Point(600, 35); $browse.Size = New-Object System.Drawing.Size(92, 26); $browse.Anchor = 'Top, Right'
$form.Controls.Add($browse)

$status = New-Object System.Windows.Forms.Label
$status.Location = New-Object System.Drawing.Point(12, 66); $status.Size = New-Object System.Drawing.Size(680, 36); $status.Anchor = 'Top, Left, Right'
$form.Controls.Add($status)

$btnInstall = New-Object System.Windows.Forms.Button
$btnInstall.Text = 'Install / update'; $btnInstall.Location = New-Object System.Drawing.Point(12, 106); $btnInstall.Size = New-Object System.Drawing.Size(140, 32)
$form.Controls.Add($btnInstall)
$btnUninstall = New-Object System.Windows.Forms.Button
$btnUninstall.Text = 'Uninstall'; $btnUninstall.Location = New-Object System.Drawing.Point(160, 106); $btnUninstall.Size = New-Object System.Drawing.Size(110, 32)
$form.Controls.Add($btnUninstall)
$btnClose = New-Object System.Windows.Forms.Button
$btnClose.Text = 'Close'; $btnClose.Location = New-Object System.Drawing.Point(600, 106); $btnClose.Size = New-Object System.Drawing.Size(92, 32); $btnClose.Anchor = 'Top, Right'
$btnClose.Add_Click({ $form.Close() })
$form.Controls.Add($btnClose)

$log = New-Object System.Windows.Forms.RichTextBox
$log.Location = New-Object System.Drawing.Point(12, 148); $log.Size = New-Object System.Drawing.Size(680, 320); $log.Anchor = 'Top, Bottom, Left, Right'
$log.ReadOnly = $true; $log.BackColor = [System.Drawing.Color]::White; $log.Font = New-Object System.Drawing.Font('Consolas', 9)
$form.Controls.Add($log)
$script:Gui = $log

function Update-Status {
    $srv = Resolve-ServerDir $path.Text
    if (-not $srv) {
        $status.ForeColor = [System.Drawing.Color]::Firebrick
        $status.Text = "No $ExeName there - pick the file with Browse..."
        $btnInstall.Enabled = $false; $btnUninstall.Enabled = $false
        return
    }
    $root = Split-Path $srv -Parent
    $installed = Is-BotModLoader (Join-Path $srv 'xinput9_1_0.dll')
    $running = (Get-RunningServers $srv).Count
    $status.ForeColor = if ($running) { [System.Drawing.Color]::DarkGoldenrod } else { [System.Drawing.Color]::ForestGreen }
    $status.Text = "Server folder: $srv`r`nSettings folder: $(Join-Path $root 'BotMod')   -   BotMod is $(if ($installed) { 'installed' } else { 'not installed' })$(if ($running) { "   -   the server is RUNNING ($running) - shut it down first" })"
    $btnInstall.Enabled = $true; $btnUninstall.Enabled = $installed -or (Test-Path -LiteralPath (Join-Path $srv 'xinput9_1_0_orig.dll'))
}

$browse.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select your BF3 server's $ExeName"
    $dlg.Filter = "BF3 server ($ExeName)|$ExeName|All programs (*.exe)|*.exe"
    $cur = Resolve-ServerDir $path.Text
    if ($cur) { $dlg.InitialDirectory = $cur }
    if ($dlg.ShowDialog($form) -eq 'OK') {
        if ((Split-Path $dlg.FileName -Leaf) -ne $ExeName) { [void][System.Windows.Forms.MessageBox]::Show("That is not $ExeName.", 'BF3 BotMod', 'OK', 'Warning'); return }
        $path.Text = $dlg.FileName
    }
})
$path.Add_TextChanged({ Update-Status })

function Run-Action([string]$what) {
    $srv = Resolve-ServerDir $path.Text
    if (-not $srv) { return }
    $btnInstall.Enabled = $false; $btnUninstall.Enabled = $false; $browse.Enabled = $false
    $form.Cursor = 'WaitCursor'
    Say ''
    Say "---- $what : $srv" White
    try {
        $script:Mode = $what
        $r = if ($what -eq 'uninstall') { Invoke-Uninstall $srv } else { Invoke-Install $srv }
        if ($r -eq 'elevated') { Say 'Continuing in a new window with administrator rights.' Yellow; $form.Close(); return }
    } catch { Say "ERROR: $($_.Exception.Message)" Red }
    finally { $form.Cursor = 'Default'; $browse.Enabled = $true; Update-Status }
}
$btnInstall.Add_Click({ Run-Action 'install' })
$btnUninstall.Add_Click({ if (Ask 'Remove BotMod from this server (the original xinput9_1_0.dll goes back)?') { Run-Action 'uninstall' } })

$start = Resolve-ServerDir $ServerDir
if (-not $start) { $start = Find-ServerDir }
if ($start) { $path.Text = Join-Path $start $ExeName } else { Update-Status }
Say "BF3 BotMod (beta). Pick your server's $ExeName above, then Install / update or Uninstall."
if ($Mode -eq 'uninstall') { $form.AcceptButton = $btnUninstall; $form.Add_Shown({ $btnUninstall.Focus() }) } else { $form.AcceptButton = $btnInstall }
[void]$form.ShowDialog()
