<#
  CoAPatcher.ps1 - apply mod-coa-playerbots patches and rebuild the CoA bot worldserver.

  Windows PowerShell 5.1 + WinForms only. Nothing to install.
  Start it with "Run CoAPatcher.bat" (it uses -STA, which drag and drop needs).

  What it will do:   back up MySQL databases with mysqldump, run "git apply" on the module,
                     stop/start worldserver (after asking), run "cmake --build".
  What it never does: git reset / clean / checkout, drop or import databases,
                     touch authserver, open any network port.
#>

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths and settings
# ---------------------------------------------------------------------------
$script:AppDir = $PSScriptRoot
if (-not $script:AppDir) { $script:AppDir = (Get-Location).Path }
$script:SettingsFile = Join-Path $script:AppDir 'CoAPatcher.settings.json'

$script:ModuleRel  = 'modules/mod-coa-playerbots'
$script:GitFlags   = @('-p2', ('--directory=' + $script:ModuleRel), '--exclude=*.md')

function New-DefaultSettings {
    [ordered]@{
        ForkRoot      = 'F:\Git\ewoog-azerothcore-wotlk-coa'
        BuildDir      = 'F:\CoA\builds-bot'
        BuildConfig   = 'RelWithDebInfo'
        BuildTarget   = 'worldserver'
        RuntimeDir    = 'F:\CoA\builds-bot\bin\RelWithDebInfo'
        MySqlBin      = 'C:\Program Files\MySQL\MySQL Server 8.4\bin'
        MySqlUser     = 'root'
        MySqlPassword = ''
        DbCharacters  = 'acore_characters_bots'
        DbWorld       = 'acore_world_bots'
        DbAuth        = 'acore_auth_bots'
        BackupChars   = $true
        BackupWorld   = $false
        BackupAuth    = $false
        BackupDir     = 'F:\CoA\backups'
        PatchDir      = 'F:\CoA\patches'
        StartAfter    = $false
        Patches       = @()
        History       = @()
    }
}

function Load-Settings {
    $s = New-DefaultSettings
    if (Test-Path -LiteralPath $script:SettingsFile) {
        try {
            $raw = [System.IO.File]::ReadAllText($script:SettingsFile)
            $j = $raw | ConvertFrom-Json
            foreach ($k in @($s.Keys)) {
                if ($j.PSObject.Properties.Name -contains $k) { $s[$k] = $j.$k }
            }
        } catch {
            Write-Log ('Could not read settings file, using defaults: ' + $_.Exception.Message)
        }
    }
    # ConvertFrom-Json can turn one-element arrays into scalars; force arrays back.
    $s.Patches = @($s.Patches | Where-Object { $_ })
    $s.History = @($s.History | Where-Object { $_ })
    return $s
}

function Save-Settings {
    $out = [ordered]@{}
    foreach ($k in $script:S.Keys) { $out[$k] = $script:S[$k] }
    $out.Patches = @($script:S.Patches)
    $out.History = @($script:S.History)
    $json = ConvertTo-Json -InputObject $out -Depth 6
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($script:SettingsFile, $json, $enc)
}

# ---------------------------------------------------------------------------
# Logging (GUI textbox if present, console otherwise) + per-run log file
# ---------------------------------------------------------------------------
$script:LogBox  = $null
$script:RunLog  = $null
$script:Pump    = { }                         # replaced by DoEvents in GUI mode
$script:AskYesNo = { param($text, $title) 'Yes' } # replaced by MessageBox in GUI mode

function Write-Log([string]$text) {
    $line = (Get-Date).ToString('HH:mm:ss') + '  ' + $text
    if ($script:LogBox) {
        $script:LogBox.AppendText($line + "`r`n")
    } else {
        Write-Host $line
    }
    if ($script:RunLog) {
        try { [System.IO.File]::AppendAllText($script:RunLog, $line + "`r`n") } catch { }
    }
}

function Start-RunLog([string]$name) {
    $script:RunLog = $null
    try {
        if (-not (Test-Path -LiteralPath $script:S.BackupDir)) {
            New-Item -ItemType Directory -Path $script:S.BackupDir | Out-Null
        }
        $ts = (Get-Date).ToString('yyyyMMdd_HHmmss')
        $script:RunLog = Join-Path $script:S.BackupDir ('CoAPatcher_' + $ts + '_' + $name + '.log')
        Write-Log ('Log file: ' + $script:RunLog)
    } catch {
        Write-Log ('(could not create log file in backup dir: ' + $_.Exception.Message + ')')
    }
}

# ---------------------------------------------------------------------------
# Running external programs with live output, without freezing the window
# ---------------------------------------------------------------------------
function ConvertTo-ArgString([string[]]$ArgList) {
    $parts = @()
    foreach ($a in $ArgList) {
        if ($a -eq $null -or $a -eq '') { $parts += '""'; continue }
        if ($a -notmatch '[\s"]') { $parts += $a; continue }
        $q = $a -replace '(\\*)"', '$1$1\"'
        $q = $q -replace '(\\+)$', '$1$1'
        $parts += ('"' + $q + '"')
    }
    return ($parts -join ' ')
}

# Runs Exe with ArgList in WorkDir. Streams stdout+stderr into the log.
# Returns the exit code. -Quiet keeps the output out of the log (still returns the code).
function Invoke-Tool {
    param([string]$Exe, [string[]]$ArgList, [string]$WorkDir, [switch]$Quiet, [string]$Show)
    $argString = ConvertTo-ArgString $ArgList
    if ($Show) { Write-Log ('> ' + $Show) } elseif (-not $Quiet) { Write-Log ('> "' + $Exe + '" ' + $argString) }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $argString
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    try {
        [void]$p.Start()
    } catch {
        Write-Log ('Could not start ' + $Exe + ': ' + $_.Exception.Message)
        return 9999
    }

    # Read both pipes with ReadLineAsync tasks and keep the window alive in between.
    $tOut = $p.StandardOutput.ReadLineAsync()
    $tErr = $p.StandardError.ReadLineAsync()
    $sincePump = 0
    while ($tOut -ne $null -or $tErr -ne $null) {
        $got = $false
        if ($tOut -ne $null -and $tOut.IsCompleted) {
            $l = $tOut.Result
            if ($l -eq $null) { $tOut = $null } else {
                if (-not $Quiet) { Write-Log ('  ' + $l) }
                $tOut = $p.StandardOutput.ReadLineAsync()
            }
            $got = $true
        }
        if ($tErr -ne $null -and $tErr.IsCompleted) {
            $l = $tErr.Result
            if ($l -eq $null) { $tErr = $null } else {
                if (-not $Quiet) { Write-Log ('  ' + $l) }
                $tErr = $p.StandardError.ReadLineAsync()
            }
            $got = $true
        }
        $sincePump++
        if (-not $got -or $sincePump -ge 40) {
            & $script:Pump
            $sincePump = 0
            if (-not $got) { Start-Sleep -Milliseconds 25 }
        }
    }
    $p.WaitForExit()
    $code = $p.ExitCode
    $p.Dispose()
    return $code
}

# ---------------------------------------------------------------------------
# Finding tools
# ---------------------------------------------------------------------------
function Find-Git {
    $c = Get-Command git -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    # GitHub Desktop ships its own git
    if ($env:LOCALAPPDATA) {
        $gd = Join-Path $env:LOCALAPPDATA 'GitHubDesktop'
        if (Test-Path -LiteralPath $gd) {
            $hit = Get-ChildItem -LiteralPath $gd -Directory -Filter 'app-*' -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'resources\app\git\cmd\git.exe' } |
                Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
            if ($hit) { return $hit }
        }
    }
    foreach ($p in @("$env:ProgramFiles\Git\cmd\git.exe", "${env:ProgramFiles(x86)}\Git\cmd\git.exe")) {
        if ($p -and (Test-Path -LiteralPath $p)) { return $p }
    }
    return $null
}

function Find-CMake {
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $paths = & $vswhere -all -products '*' -property installationPath 2>$null
        foreach ($vs in @($paths)) {
            if (-not $vs) { continue }
            $cm = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $cm) { return $cm }
        }
    }
    $pf = Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $pf) { return $pf }
    return $null
}

function Test-Settings([switch]$NeedGit, [switch]$NeedBuild, [switch]$NeedDb) {
    $ok = $true
    if ($NeedGit) {
        if (-not (Test-Path -LiteralPath $script:S.ForkRoot)) { Write-Log ('ERROR: fork root not found: ' + $script:S.ForkRoot); $ok = $false }
        else {
            $mod = Join-Path $script:S.ForkRoot ($script:ModuleRel -replace '/', '\')
            if (-not (Test-Path -LiteralPath $mod)) { Write-Log ('ERROR: module folder not found: ' + $mod); $ok = $false }
        }
    }
    if ($NeedBuild) {
        if (-not (Test-Path -LiteralPath $script:S.BuildDir)) { Write-Log ('ERROR: build dir not found: ' + $script:S.BuildDir); $ok = $false }
    }
    if ($NeedDb) {
        $md = Join-Path $script:S.MySqlBin 'mysqldump.exe'
        if (-not (Test-Path -LiteralPath $md)) { Write-Log ('ERROR: mysqldump.exe not found in ' + $script:S.MySqlBin); $ok = $false }
    }
    return $ok
}

# ---------------------------------------------------------------------------
# Step a: mysqldump backups
# ---------------------------------------------------------------------------
function Invoke-Backup {
    $dbs = @()
    if ($script:S.BackupChars -and $script:S.DbCharacters) { $dbs += $script:S.DbCharacters }
    if ($script:S.BackupWorld -and $script:S.DbWorld)      { $dbs += $script:S.DbWorld }
    if ($script:S.BackupAuth  -and $script:S.DbAuth)       { $dbs += $script:S.DbAuth }
    if ($dbs.Count -eq 0) { Write-Log 'Backup: no databases ticked, skipping.'; return $true }

    $md = Join-Path $script:S.MySqlBin 'mysqldump.exe'
    if (-not (Test-Path -LiteralPath $md)) { Write-Log ('ERROR: mysqldump.exe not found: ' + $md); return $false }
    if (-not (Test-Path -LiteralPath $script:S.BackupDir)) { New-Item -ItemType Directory -Path $script:S.BackupDir | Out-Null }

    # Password goes in a temporary option file, so it is not on the command line or in the log.
    $cnf = Join-Path ([System.IO.Path]::GetTempPath()) ('coapatcher_' + [guid]::NewGuid().ToString('N') + '.cnf')
    $cnfText = "[client]`r`nuser=" + $script:S.MySqlUser + "`r`npassword=`"" + $script:S.MySqlPassword + "`"`r`nhost=127.0.0.1`r`n"
    [System.IO.File]::WriteAllText($cnf, $cnfText, (New-Object System.Text.UTF8Encoding($false)))
    try {
        foreach ($db in $dbs) {
            $ts = (Get-Date).ToString('yyyyMMdd_HHmmss')
            $file = Join-Path $script:S.BackupDir ($db + '_' + $ts + '.sql')
            Write-Log ('Backing up ' + $db + ' -> ' + $file)
            $dumpArgs = @(('--defaults-extra-file=' + $cnf), '--single-transaction', '--routines', '--triggers',
                      '--default-character-set=utf8mb4', ('--result-file=' + $file), $db)
            $code = Invoke-Tool -Exe $md -ArgList $dumpArgs -Show ('mysqldump ' + $db + ' --result-file="' + $file + '"')
            if ($code -ne 0) { Write-Log ('ERROR: mysqldump failed for ' + $db + ' (exit code ' + $code + ').'); return $false }
            if (-not (Test-Path -LiteralPath $file)) { Write-Log ('ERROR: backup file was not created: ' + $file); return $false }
            $len = (Get-Item -LiteralPath $file).Length
            if ($len -lt 1024) { Write-Log ('ERROR: backup file is suspiciously small (' + $len + ' bytes): ' + $file); return $false }
            Write-Log ('  OK, ' + [math]::Round($len / 1MB, 2) + ' MB')
        }
    } finally {
        Remove-Item -LiteralPath $cnf -Force -ErrorAction SilentlyContinue
    }
    return $true
}

# ---------------------------------------------------------------------------
# Step b: git apply
# ---------------------------------------------------------------------------
function Invoke-GitApply([string]$Git, [string]$Root, [string]$Patch, [switch]$Reverse, [switch]$Check, [switch]$Quiet) {
    $a = @('apply')
    if ($Reverse) { $a += '-R' }
    if ($Check)   { $a += '--check' }
    $a += $script:GitFlags
    $a += $Patch
    return (Invoke-Tool -Exe $Git -ArgList $a -WorkDir $Root -Quiet:$Quiet)
}

# Applies the patch list in order inside $Root.
#   forward --check OK      -> apply it
#   else reverse --check OK -> "already applied, skipping"
#   else                    -> fail, and reverse every patch this call applied (newest first)
# Returns @{ Ok = bool; Applied = list of patch paths applied in this call }
function Invoke-PatchChain([string]$Git, [string]$Root, [string[]]$Patches, [string]$Label) {
    $applied = New-Object System.Collections.ArrayList
    foreach ($p in $Patches) {
        $name = Split-Path -Leaf $p
        if (-not (Test-Path -LiteralPath $p)) {
            Write-Log ('[' + $Label + '] ERROR: patch file missing: ' + $p)
            Undo-Chain $Git $Root $applied $Label
            return @{ Ok = $false; Applied = @() }
        }
        Write-Log ('[' + $Label + '] checking ' + $name)
        $fwd = Invoke-GitApply $Git $Root $p -Check -Quiet
        if ($fwd -eq 0) {
            $code = Invoke-GitApply $Git $Root $p
            if ($code -ne 0) {
                Write-Log ('[' + $Label + '] ERROR: apply failed for ' + $name + ' even though the check passed.')
                Undo-Chain $Git $Root $applied $Label
                return @{ Ok = $false; Applied = @() }
            }
            [void]$applied.Add($p)
            Write-Log ('[' + $Label + '] applied ' + $name)
            continue
        }
        $rev = Invoke-GitApply $Git $Root $p -Reverse -Check -Quiet
        if ($rev -eq 0) {
            Write-Log ('[' + $Label + '] ' + $name + ': already applied, skipping')
            continue
        }
        [void](Invoke-GitApply $Git $Root $p -Check)   # run again, this time showing git's error messages
        Write-Log ('[' + $Label + '] ERROR: ' + $name + ' does not apply (see the git messages above).')
        Undo-Chain $Git $Root $applied $Label
        return @{ Ok = $false; Applied = @() }
    }
    return @{ Ok = $true; Applied = @($applied) }
}

function Undo-Chain([string]$Git, [string]$Root, $Applied, [string]$Label) {
    if ($Applied.Count -eq 0) { return }
    Write-Log ('[' + $Label + '] rolling back ' + $Applied.Count + ' patch(es) applied in this run...')
    for ($i = $Applied.Count - 1; $i -ge 0; $i--) {
        $p = $Applied[$i]
        $code = Invoke-GitApply $Git $Root $p -Reverse
        if ($code -eq 0) { Write-Log ('[' + $Label + '] rolled back ' + (Split-Path -Leaf $p)) }
        else { Write-Log ('[' + $Label + '] WARNING: could not roll back ' + $p + ' - check the module folder by hand.') }
    }
}

# Dry run: copy the module into a temp folder laid out the same way and work out the plan
# there, so chained patches are checked properly and the real module is not touched.
#   1. From the bottom of the list upward, any patch that reverses cleanly is "already applied"
#      (this handles A then B applied on top of A, where A alone no longer reverses).
#   2. The rest are run top to bottom: apply if the check passes, skip if already applied, else fail.
# Returns @{ Ok = bool; Plan = patches that still need applying, in order }
function Invoke-DryRun([string]$Git, [string[]]$Patches) {
    $src = Join-Path $script:S.ForkRoot ($script:ModuleRel -replace '/', '\')
    $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('CoAPatcher_dry_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
    $dst = Join-Path $tmpRoot ($script:ModuleRel -replace '/', [System.IO.Path]::DirectorySeparatorChar)
    Write-Log ('Dry run: copying module to ' + $tmpRoot)
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    try {
        $robo = Get-Command robocopy -ErrorAction SilentlyContinue
        if ($robo) {
            $rc = Invoke-Tool -Exe $robo.Source -ArgList @($src, $dst, '/E', '/XD', '.git', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1') -Quiet
            if ($rc -ge 8) { Write-Log ('ERROR: robocopy failed with code ' + $rc); return @{ Ok = $false; Plan = @() } }
        } else {
            Get-ChildItem -LiteralPath $src -Force | Where-Object { $_.Name -ne '.git' } |
                ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dst -Recurse -Force }
        }
        foreach ($p in $Patches) {
            if (-not (Test-Path -LiteralPath $p)) { Write-Log ('ERROR: patch file missing: ' + $p); return @{ Ok = $false; Plan = @() } }
        }

        # 1. peel already-applied patches off the end of the list (temp copy only)
        $k = $Patches.Count
        while ($k -gt 0) {
            $p = $Patches[$k - 1]
            if ((Invoke-GitApply $Git $tmpRoot $p -Reverse -Check -Quiet) -ne 0) { break }
            if ((Invoke-GitApply $Git $tmpRoot $p -Reverse -Quiet) -ne 0) { break }
            Write-Log ('[dry run] ' + (Split-Path -Leaf $p) + ': already applied, skipping')
            $k--
        }
        if ($k -eq 0) { return @{ Ok = $true; Plan = @() } }

        # 2. the remaining patches, top to bottom
        $prefix = @($Patches[0..($k - 1)])
        $r = Invoke-PatchChain $Git $tmpRoot $prefix 'dry run'
        if (-not $r.Ok) { return @{ Ok = $false; Plan = @() } }
        if ($k -lt $Patches.Count -and $r.Applied.Count -gt 0) {
            Write-Log ('[dry run] ERROR: ' + (Split-Path -Leaf $r.Applied[0]) + ' is not applied yet, but ' + (Split-Path -Leaf $Patches[$k]) +
                       ' further down the list already is. Remove the already-applied ones from the list, or undo them first.')
            return @{ Ok = $false; Plan = @() }
        }
        return @{ Ok = $true; Plan = @($r.Applied) }
    } finally {
        Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# Steps c-e: worldserver and build
# ---------------------------------------------------------------------------
function Get-BotWorldservers {
    $runtime = [System.IO.Path]::GetFullPath($script:S.RuntimeDir).TrimEnd('\') + '\'
    $list = @()
    foreach ($p in @(Get-Process -Name worldserver -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch { }
        if (-not $path) { $list += @{ Proc = $p; Path = '(path unknown - may be running as admin)' }; continue }
        if ($path.StartsWith($runtime, [System.StringComparison]::OrdinalIgnoreCase)) {
            $list += @{ Proc = $p; Path = $path }
        } else {
            Write-Log ('Leaving another worldserver alone: ' + $path)
        }
    }
    return $list
}

function Stop-BotWorldserver {
    $procs = @(Get-BotWorldservers)
    if ($procs.Count -eq 0) { Write-Log 'worldserver (bot) is not running.'; return $true }
    $desc = ($procs | ForEach-Object { 'PID ' + $_.Proc.Id + '  ' + $_.Path }) -join "`r`n"
    $ans = & $script:AskYesNo ("worldserver is running:`r`n`r`n" + $desc + "`r`n`r`nStop it now so the build can replace worldserver.exe?`r`n(Log out of the game first. authserver is not touched.)") 'Stop worldserver?'
    if ($ans -ne 'Yes') { Write-Log 'You chose not to stop worldserver. Stopping here (the build cannot replace a running worldserver.exe).'; return $false }
    foreach ($x in $procs) {
        Write-Log ('Stopping worldserver PID ' + $x.Proc.Id)
        try { Stop-Process -Id $x.Proc.Id -ErrorAction Stop } catch { Write-Log ('ERROR: ' + $_.Exception.Message); return $false }
        $deadline = (Get-Date).AddSeconds(30)
        while (-not $x.Proc.HasExited -and (Get-Date) -lt $deadline) { & $script:Pump; Start-Sleep -Milliseconds 200 }
        if (-not $x.Proc.HasExited) { Write-Log 'ERROR: worldserver did not exit within 30 seconds.'; return $false }
    }
    Write-Log 'worldserver stopped.'
    return $true
}

function Invoke-Build {
    $cm = Find-CMake
    if (-not $cm) { Write-Log 'ERROR: cmake not found on PATH or in Visual Studio (vswhere).'; return $false }
    Write-Log ('Using cmake: ' + $cm)
    $code = Invoke-Tool -Exe $cm -ArgList @('--build', $script:S.BuildDir, '--config', $script:S.BuildConfig, '--target', $script:S.BuildTarget) -WorkDir $script:S.BuildDir
    if ($code -ne 0) { Write-Log ('ERROR: build failed (exit code ' + $code + '). Scroll up for the first "error" line.'); return $false }
    Write-Log 'Build succeeded.'
    return $true
}

function Start-BotWorldserver {
    $exe = Join-Path $script:S.RuntimeDir 'worldserver.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Write-Log ('ERROR: not found: ' + $exe); return $false }
    Write-Log ('Starting ' + $exe)
    Start-Process -FilePath $exe -WorkingDirectory $script:S.RuntimeDir | Out-Null
    return $true
}

# ---------------------------------------------------------------------------
# Patch list helpers
# ---------------------------------------------------------------------------
function Get-FileHashHex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($Path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))) } finally { $fs.Dispose(); $sha.Dispose() }
}

# Copies a patch into the patch dir (never overwrites a different file of the same name).
function Import-PatchFile([string]$Path) {
    if ([System.IO.Path]::GetExtension($Path) -ne '.patch') { Write-Log ('Ignored (not a .patch): ' + $Path); return $null }
    $dir = $script:S.PatchDir
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $full = [System.IO.Path]::GetFullPath($Path)
    $target = Join-Path $dir (Split-Path -Leaf $full)
    if ([string]::Equals($full, [System.IO.Path]::GetFullPath($target), [System.StringComparison]::OrdinalIgnoreCase)) { return $full }
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHashHex $target) -eq (Get-FileHashHex $full)) { return $target }
        $base = [System.IO.Path]::GetFileNameWithoutExtension($full)
        $target = Join-Path $dir ($base + '_' + (Get-Date).ToString('yyyyMMdd_HHmmss') + '.patch')
        Write-Log ('A different file with that name is already in the patch dir; saving as ' + (Split-Path -Leaf $target))
    }
    Copy-Item -LiteralPath $full -Destination $target
    Write-Log ('Copied to patch dir: ' + $target)
    return $target
}

# ---------------------------------------------------------------------------
# Top-level actions
# ---------------------------------------------------------------------------
function Do-CheckOnly {
    $patches = @($script:S.Patches)
    if ($patches.Count -eq 0) { Write-Log 'No patches in the list.'; return $false }
    if (-not (Test-Settings -NeedGit)) { return $false }
    $git = Find-Git
    if (-not $git) { Write-Log 'ERROR: git not found (PATH, GitHub Desktop, or Program Files\Git).'; return $false }
    $d = Invoke-DryRun $git $patches
    if ($d.Ok) {
        Write-Log ('CHECK PASSED: ' + $d.Plan.Count + ' patch(es) would be applied, the rest are already in. Nothing was changed.')
    } else { Write-Log 'CHECK FAILED: nothing was changed in your module folder.' }
    return $d.Ok
}

function Do-ApplyAndBuild {
    $patches = @($script:S.Patches)
    if ($patches.Count -eq 0) { Write-Log 'No patches in the list. Use "Build only" if you just want to rebuild.'; return $false }
    if (-not (Test-Settings -NeedGit -NeedBuild)) { return $false }
    $git = Find-Git
    if (-not $git) { Write-Log 'ERROR: git not found (PATH, GitHub Desktop, or Program Files\Git).'; return $false }

    Write-Log '=== Step 1/5: database backup ==='
    if (-not (Invoke-Backup)) { Write-Log 'STOPPED at backup. Nothing was changed.'; return $false }

    Write-Log '=== Step 2/5: check every patch (dry run on a temp copy) ==='
    $d = Invoke-DryRun $git $patches
    if (-not $d.Ok) { Write-Log 'STOPPED: a patch does not apply. Nothing was changed.'; return $false }

    Write-Log '=== Step 3/5: apply patches to the module ==='
    $r = @{ Ok = $true; Applied = @() }
    if ($d.Plan.Count -gt 0) { $r = Invoke-PatchChain $git $script:S.ForkRoot @($d.Plan) 'apply' }
    if (-not $r.Ok) { Write-Log 'STOPPED: apply failed; patches from this run were rolled back.'; return $false }
    foreach ($p in $r.Applied) {
        $script:S.History = @($script:S.History) + @([ordered]@{ Patch = $p; AppliedAt = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss') })
    }
    Save-Settings
    if ($r.Applied.Count -eq 0) { Write-Log 'All patches were already applied; continuing to the build.' }

    Write-Log '=== Step 4/5: stop worldserver and build ==='
    if (-not (Stop-BotWorldserver)) { Write-Log 'STOPPED before the build. Patches are applied; press "Build only" when ready.'; return $false }
    if (-not (Invoke-Build)) { Write-Log 'STOPPED: build failed. Patches stay applied; use "Undo last patch" if you want them out.'; return $false }

    Write-Log '=== Step 5/5: start worldserver ==='
    if ($script:S.StartAfter) { [void](Start-BotWorldserver) } else { Write-Log 'Not starting worldserver (box unticked).' }
    Write-Log 'ALL DONE. worldserver applies any new SQL updates itself when it starts.'
    return $true
}

function Do-BuildOnly {
    if (-not (Test-Settings -NeedBuild)) { return $false }
    if (-not (Stop-BotWorldserver)) { return $false }
    if (-not (Invoke-Build)) { return $false }
    if ($script:S.StartAfter) { [void](Start-BotWorldserver) }
    return $true
}

function Do-BackupOnly {
    if (Invoke-Backup) { Write-Log 'Backup finished.'; return $true }
    return $false
}

function Do-UndoLast {
    $hist = @($script:S.History)
    if ($hist.Count -eq 0) { Write-Log 'No applied patches recorded in the history.'; return $false }
    if (-not (Test-Settings -NeedGit)) { return $false }
    $git = Find-Git
    if (-not $git) { Write-Log 'ERROR: git not found.'; return $false }
    $last = $hist[$hist.Count - 1]
    $p = $last.Patch
    $ans = & $script:AskYesNo ('Reverse this patch (applied ' + $last.AppliedAt + ')?' + "`r`n`r`n" + $p + "`r`n`r`nYou will need to build again afterwards.") 'Undo last patch?'
    if ($ans -ne 'Yes') { Write-Log 'Undo cancelled.'; return $false }
    if (-not (Test-Path -LiteralPath $p)) { Write-Log ('ERROR: patch file missing: ' + $p); return $false }
    Write-Log ('Checking reverse of ' + (Split-Path -Leaf $p))
    if ((Invoke-GitApply $git $script:S.ForkRoot $p -Reverse -Check) -ne 0) {
        Write-Log 'ERROR: the patch cannot be reversed cleanly (the files changed since). Nothing was changed.'
        return $false
    }
    if ((Invoke-GitApply $git $script:S.ForkRoot $p -Reverse) -ne 0) { Write-Log 'ERROR: reverse apply failed.'; return $false }
    if ($hist.Count -gt 1) { $script:S.History = @($hist[0..($hist.Count - 2)]) } else { $script:S.History = @() }
    Save-Settings
    Write-Log ('Reversed ' + (Split-Path -Leaf $p) + '. Press "Build only" to rebuild without it.')
    return $true
}

# Test hook: set COAPATCHER_NOGUI=1 and dot-source this file to use the functions without a window.
$script:S = Load-Settings
if ($env:COAPATCHER_NOGUI -eq '1') { return }

# ===========================================================================
# GUI
# ===========================================================================
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$script:Pump = { [System.Windows.Forms.Application]::DoEvents() }
$script:AskYesNo = {
    param($text, $title)
    [System.Windows.Forms.MessageBox]::Show($text, $title, [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Question).ToString()
}

$form = New-Object System.Windows.Forms.Form
$form.Text = 'CoA Patcher - mod-coa-playerbots'
$form.Size = New-Object System.Drawing.Size(980, 860)
$form.MinimumSize = New-Object System.Drawing.Size(900, 700)
$form.StartPosition = 'CenterScreen'
$font = New-Object System.Drawing.Font('Segoe UI', 9)
$form.Font = $font

# --- settings area -----------------------------------------------------------
$grp = New-Object System.Windows.Forms.GroupBox
$grp.Text = 'Settings (saved to CoAPatcher.settings.json next to this script)'
$grp.Location = New-Object System.Drawing.Point(10, 8)
$grp.Size = New-Object System.Drawing.Size(945, 250)
$grp.Anchor = 'Top,Left,Right'
$form.Controls.Add($grp)

$script:Fields = @{}
function Add-PathRow([string]$key, [string]$label, [int]$y, [bool]$browse) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $label; $l.Location = New-Object System.Drawing.Point(10, ($y + 3)); $l.Size = New-Object System.Drawing.Size(120, 20)
    $grp.Controls.Add($l)
    $t = New-Object System.Windows.Forms.TextBox
    $t.Location = New-Object System.Drawing.Point(130, $y); $t.Size = New-Object System.Drawing.Size(720, 22)
    $t.Anchor = 'Top,Left,Right'
    $t.Text = [string]$script:S[$key]
    $grp.Controls.Add($t)
    $script:Fields[$key] = $t
    if ($browse) {
        $b = New-Object System.Windows.Forms.Button
        $b.Text = '...'; $b.Location = New-Object System.Drawing.Point(858, ($y - 1)); $b.Size = New-Object System.Drawing.Size(75, 24)
        $b.Anchor = 'Top,Right'
        $b.Tag = $t
        $b.Add_Click({
            $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
            if ($this.Tag.Text -and (Test-Path -LiteralPath $this.Tag.Text)) { $dlg.SelectedPath = $this.Tag.Text }
            if ($dlg.ShowDialog() -eq 'OK') { $this.Tag.Text = $dlg.SelectedPath }
        })
        $grp.Controls.Add($b)
    }
}
Add-PathRow 'ForkRoot'   'Fork root'      22  $true
Add-PathRow 'BuildDir'   'Build dir'      50  $true
Add-PathRow 'RuntimeDir' 'Runtime folder' 78  $true
Add-PathRow 'MySqlBin'   'MySQL bin'      106 $true
Add-PathRow 'BackupDir'  'Backup dir'     134 $true
Add-PathRow 'PatchDir'   'Patch dir'      162 $true

function Add-SmallBox([string]$key, [string]$label, [int]$x, [int]$y, [int]$w, [bool]$pwd) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $label; $l.Location = New-Object System.Drawing.Point($x, ($y + 3)); $l.AutoSize = $true
    $grp.Controls.Add($l)
    $t = New-Object System.Windows.Forms.TextBox
    $t.Location = New-Object System.Drawing.Point(($x + $l.PreferredWidth + 4), $y); $t.Size = New-Object System.Drawing.Size($w, 22)
    $t.Text = [string]$script:S[$key]
    if ($pwd) { $t.UseSystemPasswordChar = $true }
    $grp.Controls.Add($t)
    $script:Fields[$key] = $t
}
Add-SmallBox 'BuildConfig'   'Config'    10  190 110 $false
Add-SmallBox 'BuildTarget'   'Target'    180 190 110 $false
Add-SmallBox 'MySqlUser'     'MySQL user' 350 190 80 $false
Add-SmallBox 'MySqlPassword' 'Password'  510 190 80 $true

$script:Checks = @{}
function Add-DbRow([string]$chkKey, [string]$dbKey, [int]$x) {
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = 'Back up'; $c.Location = New-Object System.Drawing.Point($x, 218); $c.AutoSize = $true
    $c.Checked = [bool]$script:S[$chkKey]
    $grp.Controls.Add($c)
    $script:Checks[$chkKey] = $c
    $t = New-Object System.Windows.Forms.TextBox
    $t.Location = New-Object System.Drawing.Point(($x + 68), 216); $t.Size = New-Object System.Drawing.Size(200, 22)
    $t.Text = [string]$script:S[$dbKey]
    $grp.Controls.Add($t)
    $script:Fields[$dbKey] = $t
}
Add-DbRow 'BackupChars' 'DbCharacters' 10
Add-DbRow 'BackupWorld' 'DbWorld'      300
Add-DbRow 'BackupAuth'  'DbAuth'       590

# --- patch list ----------------------------------------------------------------
$lblDrop = New-Object System.Windows.Forms.Label
$lblDrop.Text = 'Patches - drag .patch files here (applied top to bottom):'
$lblDrop.Location = New-Object System.Drawing.Point(10, 266); $lblDrop.AutoSize = $true
$form.Controls.Add($lblDrop)

$list = New-Object System.Windows.Forms.ListBox
$list.Location = New-Object System.Drawing.Point(10, 286)
$list.Size = New-Object System.Drawing.Size(820, 130)
$list.Anchor = 'Top,Left,Right'
$list.AllowDrop = $true
$list.HorizontalScrollbar = $true
$form.Controls.Add($list)
foreach ($p in $script:S.Patches) { [void]$list.Items.Add($p) }

function Sync-Patches { $script:S.Patches = @($list.Items | ForEach-Object { [string]$_ }) }

function Add-Patches([string[]]$files) {
    foreach ($f in $files) {
        try {
            $t = Import-PatchFile $f
            if ($t -and -not $list.Items.Contains($t)) { [void]$list.Items.Add($t) }
        } catch { Write-Log ('Could not add ' + $f + ': ' + $_.Exception.Message) }
    }
    Sync-Patches
}

$list.Add_DragEnter({
    if ($_.Data.GetDataPresent([System.Windows.Forms.DataFormats]::FileDrop)) { $_.Effect = 'Copy' } else { $_.Effect = 'None' }
})
$list.Add_DragDrop({
    $files = $_.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop)
    Read-Fields
    Add-Patches $files
})

function New-SideButton([string]$text, [int]$y) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point(840, $y); $b.Size = New-Object System.Drawing.Size(115, 26)
    $b.Anchor = 'Top,Right'
    $form.Controls.Add($b)
    return $b
}
$btnBrowse = New-SideButton 'Browse...' 286
$btnUp     = New-SideButton 'Move up'   314
$btnDown   = New-SideButton 'Move down' 342
$btnRemove = New-SideButton 'Remove'    370

$btnBrowse.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Patch files (*.patch)|*.patch'
    $dlg.Multiselect = $true
    if ($dlg.ShowDialog() -eq 'OK') { Read-Fields; Add-Patches $dlg.FileNames }
})
$btnUp.Add_Click({
    $i = $list.SelectedIndex
    if ($i -gt 0) { $x = $list.Items[$i]; $list.Items.RemoveAt($i); $list.Items.Insert($i - 1, $x); $list.SelectedIndex = $i - 1; Sync-Patches }
})
$btnDown.Add_Click({
    $i = $list.SelectedIndex
    if ($i -ge 0 -and $i -lt $list.Items.Count - 1) { $x = $list.Items[$i]; $list.Items.RemoveAt($i); $list.Items.Insert($i + 1, $x); $list.SelectedIndex = $i + 1; Sync-Patches }
})
$btnRemove.Add_Click({
    $i = $list.SelectedIndex
    if ($i -ge 0) { $list.Items.RemoveAt($i); Sync-Patches }   # removes from the list only; the file stays in the patch dir
})

# --- actions -------------------------------------------------------------------
$chkStart = New-Object System.Windows.Forms.CheckBox
$chkStart.Text = 'Start worldserver after build'
$chkStart.Location = New-Object System.Drawing.Point(10, 424); $chkStart.AutoSize = $true
$chkStart.Checked = [bool]$script:S.StartAfter
$form.Controls.Add($chkStart)

$btnGo = New-Object System.Windows.Forms.Button
$btnGo.Text = 'Apply && Build'
$btnGo.Font = New-Object System.Drawing.Font('Segoe UI', 13, [System.Drawing.FontStyle]::Bold)
$btnGo.Location = New-Object System.Drawing.Point(10, 450); $btnGo.Size = New-Object System.Drawing.Size(260, 48)
$btnGo.BackColor = [System.Drawing.Color]::FromArgb(200, 235, 200)
$form.Controls.Add($btnGo)

function New-ActionButton([string]$text, [int]$x) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, 458); $b.Size = New-Object System.Drawing.Size(130, 34)
    $form.Controls.Add($b)
    return $b
}
$btnCheck  = New-ActionButton 'Check only'       285
$btnBuild  = New-ActionButton 'Build only'       425
$btnBackup = New-ActionButton 'Backup only'      565
$btnUndo   = New-ActionButton 'Undo last patch'  705

# --- log -----------------------------------------------------------------------
$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ScrollBars = 'Both'
$logBox.WordWrap = $false
$logBox.ReadOnly = $true
$logBox.Font = New-Object System.Drawing.Font('Consolas', 9)
$logBox.Location = New-Object System.Drawing.Point(10, 508)
$logBox.Size = New-Object System.Drawing.Size(945, 300)
$logBox.Anchor = 'Top,Bottom,Left,Right'
$form.Controls.Add($logBox)
$script:LogBox = $logBox

function Read-Fields {
    foreach ($k in $script:Fields.Keys) { $script:S[$k] = $script:Fields[$k].Text.Trim() }
    foreach ($k in $script:Checks.Keys) { $script:S[$k] = $script:Checks[$k].Checked }
    $script:S.StartAfter = $chkStart.Checked
    Sync-Patches
    try { Save-Settings } catch { Write-Log ('Could not save settings: ' + $_.Exception.Message) }
}

$script:Busy = $false
$script:ActionButtons = @($btnGo, $btnCheck, $btnBuild, $btnBackup, $btnUndo, $btnBrowse, $btnUp, $btnDown, $btnRemove)

function Run-Action([string]$name, [scriptblock]$action) {
    if ($script:Busy) { return }
    $script:Busy = $true
    foreach ($b in $script:ActionButtons) { $b.Enabled = $false }
    $list.AllowDrop = $false
    $form.Cursor = [System.Windows.Forms.Cursors]::AppStarting
    try {
        Read-Fields
        $logBox.Clear()
        Start-RunLog $name
        Write-Log ('--- ' + $name + ' ---')
        $ok = & $action
        if ($ok) { Write-Log ('--- ' + $name + ': finished OK ---') }
        else {
            Write-Log ('--- ' + $name + ': FAILED / STOPPED (details above) ---')
            [void][System.Windows.Forms.MessageBox]::Show(($name + ' stopped. Read the log for the reason.'), 'CoA Patcher', [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Warning)
        }
    } catch {
        Write-Log ('UNEXPECTED ERROR: ' + $_.Exception.Message)
        [void][System.Windows.Forms.MessageBox]::Show(('Unexpected error: ' + $_.Exception.Message), 'CoA Patcher', [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Error)
    } finally {
        $script:RunLog = $null
        foreach ($b in $script:ActionButtons) { $b.Enabled = $true }
        $list.AllowDrop = $true
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
        $script:Busy = $false
    }
}

$btnGo.Add_Click({     Run-Action 'ApplyAndBuild' { Do-ApplyAndBuild } })
$btnCheck.Add_Click({  Run-Action 'CheckOnly'     { Do-CheckOnly } })
$btnBuild.Add_Click({  Run-Action 'BuildOnly'     { Do-BuildOnly } })
$btnBackup.Add_Click({ Run-Action 'BackupOnly'    { Do-BackupOnly } })
$btnUndo.Add_Click({   Run-Action 'UndoLast'      { Do-UndoLast } })

$form.Add_FormClosing({
    if ($script:Busy) {
        [void][System.Windows.Forms.MessageBox]::Show('A step is still running. Wait for it to finish before closing.', 'CoA Patcher')
        $_.Cancel = $true
        return
    }
    Read-Fields
})

Write-Log 'Ready. Drag .patch files into the list, then press Apply & Build (or Check only first).'
$h = @($script:S.History)
if ($h.Count -gt 0) { Write-Log ('Last applied patch: ' + $h[$h.Count - 1].Patch + ' (' + $h[$h.Count - 1].AppliedAt + ')') }

[void]$form.ShowDialog()
