# ======================================================================
# deploy.ps1 - encodermain + encalertd one-shot cross-compile + multi-board deploy
# Strict contract: Makefile.local ONLY for cross-build, NEVER trigger build.sh loop
#
# Usage (run from deploy/ dir or anywhere):
#   1) Build + Deploy ALL 3 boards (00/01/02)
#      powershell -ExecutionPolicy Bypass -File .\deploy.ps1
#
#   2) Deploy only (use existing binaries)
#      powershell -ExecutionPolicy Bypass -File .\deploy.ps1 -SkipBuild
#
#   3) Build only (don't deploy)
#      powershell -ExecutionPolicy Bypass -File .\deploy.ps1 -BuildOnly
#
#   4) Specific IPs (hostkey=auto uses registry cache)
#      powershell -ExecutionPolicy Bypass -File .\deploy.ps1 `
#          -IPs 192.168.250.116,192.168.250.126
#
# Outputs (under -BinsDir, default <project>\tmp_deploy\artifacts_encdual\):
#   encodermain / encalertd / S96encodermain / S43encalertd / encalertd.conf (opt)
# ======================================================================
param(
    [switch]$SkipBuild,
    [switch]$BuildOnly,
    [switch]$DeployOnly,
    [string[]]$IPs = @(),
    [string]$BinsDir = "",

    # ==== VM cross-compile host (default VM104) ====
    [string]$BuildVm      = "192.168.0.104",
    [string]$BuildUser    = "hzhou",
    [string]$BuildPass    = "123456",
    [string]$BuildHostkey = "SHA256:po2lAWqJGASgx9UqZlV6b/G2T+EbC2f9yzhpSeto66Y",

    # ==== Device default credentials ====
    [string]$DevUser = "root",
    [string]$DevPass = "12345",

    # ==== PuTTY paths (default %ProgramFiles%\PuTTY) ====
    [string]$PlinkBin = "",
    [string]$PscpBin  = ""
)
$ErrorActionPreference = "Continue"
$ProgressPreference    = "SilentlyContinue"

# -------- Resolve PuTTY binaries --------
if ([string]::IsNullOrEmpty($PlinkBin)) {
    $PlinkBin = Join-Path $env:ProgramFiles "PuTTY\plink.exe"
}
if ([string]::IsNullOrEmpty($PscpBin)) {
    $PscpBin  = Join-Path $env:ProgramFiles "PuTTY\pscp.exe"
}
if (-not (Test-Path $PlinkBin) -or -not (Test-Path $PscpBin)) {
    Write-Error ("PuTTY (plink/pscp) not found under " + $PlinkBin +
        "; install PuTTY or pass -PlinkBin / -PscpBin explicitly")
    exit 2
}

# -------- Locate project root --------
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
# Layout: <project>/firmware/general/package/encodermain/deploy => up 4
$PkgEncMain  = Split-Path -Parent $ScriptDir
$PkgEncPkg   = Split-Path -Parent $PkgEncMain
$PkgGeneral  = Split-Path -Parent $PkgEncPkg
$FirmwareDir = Split-Path -Parent $PkgGeneral
$ProjectRoot = Split-Path -Parent $FirmwareDir
if ([string]::IsNullOrEmpty($BinsDir)) {
    $BinsDir = Join-Path $ProjectRoot ("tmp_deploy" + [IO.Path]::DirectorySeparatorChar + "artifacts_encdual")
}
New-Item -ItemType Directory -Force -Path $BinsDir | Out-Null
$BinsDir = (Resolve-Path $BinsDir).Path

Write-Host "=== encodermain + encalertd deploy ==="
Write-Host ("  Project    : " + $ProjectRoot)
Write-Host ("  Firmware   : " + $FirmwareDir)
Write-Host ("  BinsDir    : " + $BinsDir)
Write-Host ("  Build VM   : " + $BuildUser + "@" + $BuildVm)

# -------- Default device table (00/01/02).  Explicit -IPs overrides.
# hostkey modes:
#   "accept"   -> auto accept on first contact (registry cache is used later)
#   ""         -> use PuTTY registry only (will hang if hostkey unknown)
#   "SHA256:x" -> explicit pinned hostkey; 125 board REQUIRED because it
#                 rejects auto-accept when reflashed periodically
$DefaultDevs = @(
    @{ ip="192.168.250.116"; name="ENC00-116"; hk="accept" },
    @{ ip="192.168.250.126"; name="ENC01-126"; hk="accept" },
    @{ ip="192.168.250.125"; name="ENC02-125"; hk="SHA256:w7vfv5kGsXJSbGaK+gutB+mVzCUIar+XFMjTzqONzKQ" }
)
if ($IPs.Count -gt 0) {
    $DevList = @()
    foreach ($ip in $IPs) {
        $DevList += @{ ip=$ip; name=("ENC-"+$ip); hk="" }
    }
} else {
    $DevList = $DefaultDevs
}
$_t = ($DevList | ForEach-Object { $_.name + "=" + $_.ip }) -join ", "
Write-Host ("  Targets    : " + $_t)

# -------- helpers --------
function Run-Vm  { param($c)
    # Write a standalone sh -> upload VM -> run -> rm.
    # This avoids "$()" / "${var}" / backticks being mangled by either the
    # local PS parser or the remote shell via plink's argv quoting.
    $stamp = [guid]::NewGuid().ToString("N").Substring(0,8)
    $tmp  = Join-Path $env:TEMP ("vm_" + $stamp + ".sh")
    [System.IO.File]::WriteAllText($tmp, ($c + "`n"))
    $rmt  = "/tmp/vm_" + $stamp + ".sh"
    $_lines = ($c -split [Environment]::NewLine).Count
    Write-Host ("[VM] exec script (lines=" + $_lines + ")")
    try {
        Copy-VmUp $tmp $rmt
    } catch {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue | Out-Null
        throw ("VM script upload failed: " + $c)
    }
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue | Out-Null
    $_cmd = "bash " + $rmt + "; rc=$?; rm -f " + $rmt + "; exit $rc"
    $out = & $PlinkBin -batch -ssh ($BuildUser + "@" + $BuildVm) -hostkey $BuildHostkey -pw $BuildPass $_cmd 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error ("VM script FAIL exit=" + $LASTEXITCODE + "; script=<<EOF`n" + $c + "`nEOF")
        throw "VM_FAIL"
    }
    return $out
}
function Copy-VmUp  { param($src,$dst)
    & $PscpBin -batch -hostkey $BuildHostkey -pw $BuildPass $src ($BuildUser + "@" + $BuildVm + ":" + $dst)
    if ($LASTEXITCODE -ne 0) { throw ("pscp UP failed " + $src + " -> " + $dst) }
}
function Copy-VmDown { param($src,$dst)
    & $PscpBin -batch -hostkey $BuildHostkey -pw $BuildPass ($BuildUser + "@" + $BuildVm + ":" + $src) $dst
    if ($LASTEXITCODE -ne 0) { throw ("pscp DOWN failed " + $src + " -> " + $dst) }
}

function Run-Dev { param($ip,$hk,$c)
    # Same pattern: write tmp sh -> pscp upload -> plink bash exec -> rm.
    # Escaping plink argv through PS's splat of the $()-result is painful,
    # so we use a local temp file and never pass "$?" through a double-quoted
    # PS expandable string directly.
    $stamp = [guid]::NewGuid().ToString("N").Substring(0,8)
    $tmp  = Join-Path $env:TEMP ("rd_" + $stamp + ".sh")
    [System.IO.File]::WriteAllText($tmp, ($c + "`n"))
    $rmt  = "/tmp/rd_" + $stamp + ".sh"
    # hostkey three-state:
    #   "" / "accept" -> rely on PuTTY registry cache; first connection is
    #       interactive so caller MUST pass "accept" in that case;
    #   "SHA256:xxx"  -> explicit pin.
    $extraScp = @()
    $extraSsh = @()
    if (-not [string]::IsNullOrEmpty($hk) -and $hk -ne "accept") {
        $extraScp = @('-hostkey', $hk)
        $extraSsh = @('-hostkey', $hk)
    }
    & $PscpBin -batch -scp @extraScp -pw $DevPass $tmp ($DevUser + "@" + $ip + ":" + $rmt) | Out-Null
    $_cmd = "/bin/sh " + $rmt + "; rc=$?; rm -f " + $rmt + "; exit $rc"
    $out = & $PlinkBin -batch -ssh @extraSsh ($DevUser + "@" + $ip) -pw $DevPass $_cmd 2>&1
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning ("DEV " + $ip + " nonzero exit=" + $LASTEXITCODE + " cmd=" + $c)
    }
    return $out
}
function Copy-Dev { param($ip,$hk,$src,$dst)
    $extra = @()
    if (-not [string]::IsNullOrEmpty($hk) -and $hk -ne "accept") {
        $extra = @('-hostkey', $hk)
    }
    & $PscpBin -batch -scp @extra -pw $DevPass $src ($DevUser + "@" + $ip + ":" + $dst)
    if ($LASTEXITCODE -ne 0) { throw ("COPY-DEV " + $ip + " FAIL " + $src + " -> " + $dst) }
}

# ============================================================
# PHASE 1 - Source sync + VM isolated cross-build (Makefile.local ONLY)
# ============================================================
if ($DeployOnly) { $SkipBuild = $true }
if (-not $SkipBuild) {
    Write-Host ""
    Write-Host "=== PHASE 1: cross-compile (Makefile.local only) ==="

    # 1a. Pack source tree to plain tar (openipc busybox tar does not do gz,
    #     and rejects -C placed after tar filename - keep layout relative).
    $srcTar = Join-Path $BinsDir "encdual_src.tar"
    Push-Location $ProjectRoot
    $files = @(
        ("firmware" + [IO.Path]::DirectorySeparatorChar + "general" + [IO.Path]::DirectorySeparatorChar + "package" + [IO.Path]::DirectorySeparatorChar + "encodermain" + [IO.Path]::DirectorySeparatorChar + "src"),
        ("firmware" + [IO.Path]::DirectorySeparatorChar + "general" + [IO.Path]::DirectorySeparatorChar + "package" + [IO.Path]::DirectorySeparatorChar + "encalertd" + [IO.Path]::DirectorySeparatorChar + "src"),
        ("firmware" + [IO.Path]::DirectorySeparatorChar + "general" + [IO.Path]::DirectorySeparatorChar + "package" + [IO.Path]::DirectorySeparatorChar + "encodermain" + [IO.Path]::DirectorySeparatorChar + "S96encodermain"),
        ("firmware" + [IO.Path]::DirectorySeparatorChar + "general" + [IO.Path]::DirectorySeparatorChar + "package" + [IO.Path]::DirectorySeparatorChar + "encalertd" + [IO.Path]::DirectorySeparatorChar + "S43encalertd"),
        ("firmware" + [IO.Path]::DirectorySeparatorChar + "general" + [IO.Path]::DirectorySeparatorChar + "package" + [IO.Path]::DirectorySeparatorChar + "encalertd" + [IO.Path]::DirectorySeparatorChar + "encalertd.conf")
    )
    if (Test-Path $srcTar) { Remove-Item $srcTar -Force }
    tar cf $srcTar @files
    Pop-Location
    if (-not (Test-Path $srcTar) -or (Get-Item $srcTar).Length -lt 1024) {
        Write-Error ("PHASE1 FAIL: source tarball empty - " + $srcTar)
        exit 3
    }
    Write-Host ("  src tar: " + (Get-Item $srcTar).Length + " bytes")

    # 1b. Upload tar + vm_build.sh, build
    # NOTE: pscp -scp does NOT expand tilde (~) on remote paths - use absolute paths.
    $vmBuildRoot = "/home/" + $BuildUser + "/encdual_build"
    $FIRM_VM     = "/home/" + $BuildUser + "/openipc/firmware"
    Run-Vm ("rm -rf " + $vmBuildRoot + " && mkdir -p " + $vmBuildRoot) | Out-Null
    Copy-VmUp $srcTar  ($vmBuildRoot + "/encdual_src.tar")
    $vmBuildSh = Join-Path $ScriptDir "vm_build.sh"
    if (-not (Test-Path $vmBuildSh)) { Write-Error ("missing " + $vmBuildSh); exit 3 }
    Copy-VmUp $vmBuildSh ($vmBuildRoot + "/vm_build.sh")
    Run-Vm ('chmod +x ' + $vmBuildRoot + '/vm_build.sh') | Out-Null
    $out = Run-Vm ('bash ' + $vmBuildRoot + '/vm_build.sh')
    $out | Select-Object -Last 30 | ForEach-Object { Write-Host ("  | " + $_) }

    # 1c. Pull artifacts (pscp down also needs absolute remote path)
    Copy-VmDown ($FIRM_VM     + "/general/package/encodermain/src/encodermain") (Join-Path $BinsDir "encodermain")
    Copy-VmDown ($FIRM_VM     + "/general/package/encalertd/src/encalertd")     (Join-Path $BinsDir "encalertd")
    Copy-VmDown ($vmBuildRoot + "/S96encodermain") (Join-Path $BinsDir "S96encodermain")
    Copy-VmDown ($vmBuildRoot + "/S43encalertd")   (Join-Path $BinsDir "S43encalertd")
    try { Copy-VmDown ($vmBuildRoot + "/encalertd.conf") (Join-Path $BinsDir "encalertd.conf") } catch { }

    foreach ($b in @("encodermain","encalertd","S96encodermain","S43encalertd")) {
        $p = Join-Path $BinsDir $b
        if (-not (Test-Path $p)) { Write-Error ("PHASE1 FAIL: missing artifact " + $b); exit 4 }
        $len = (Get-Item $p).Length
        Write-Host ("  {0,-16} = {1,8} bytes" -f $b, $len)
    }
    Write-Host "=== PHASE 1 OK ==="
} else {
    Write-Host ""
    Write-Host "=== PHASE 1 SKIPPED (-SkipBuild / -DeployOnly) ==="
    foreach ($b in @("encodermain","encalertd","S96encodermain","S43encalertd")) {
        $p = Join-Path $BinsDir $b
        if (-not (Test-Path $p)) { Write-Error ("PHASE1 (Skip): missing local " + $p); exit 5 }
    }
}

if ($BuildOnly) {
    Write-Host ""
    Write-Host "=== -BuildOnly specified; skipping deploy ==="
    exit 0
}

# ============================================================
# PHASE 2 - Per-board deploy (kill -> copy -> chmod -> start -> verify)
# ============================================================
Write-Host ""
Write-Host "=== PHASE 2: multi-board deploy (encodermain + encalertd dual) ==="
$binEm   = Join-Path $BinsDir "encodermain"
$binAl   = Join-Path $BinsDir "encalertd"
$binS96  = Join-Path $BinsDir "S96encodermain"
$binS43  = Join-Path $BinsDir "S43encalertd"
$binConf = Join-Path $BinsDir "encalertd.conf"

$DevInstallSh = Join-Path $ScriptDir "dev_install.sh"
if (-not (Test-Path $DevInstallSh)) { Write-Error ("missing " + $DevInstallSh); exit 6 }

$OK = 0
$FAIL = 0
foreach ($d in $DevList) {
    $ip  = $d.ip
    $nm  = $d.name
    $hk  = $d.hk
    Write-Host ""
    Write-Host ("--- DEPLOY " + $nm + " (" + $ip + ") ---")
    try {
        # 0. Kill both services first (avoid "Text file busy" on overwrite)
        Run-Dev $ip $hk 'killall -9 encodermain encalertd 2>/dev/null; sleep 1; echo KILLED' | Out-Null
        # 1. Upload binaries + init.d scripts + optional conf + installer
        Copy-Dev $ip $hk $binEm        "/tmp/encodermain.new"
        Copy-Dev $ip $hk $binAl        "/tmp/encalertd.new"
        Copy-Dev $ip $hk $binS96       "/tmp/S96encodermain.new"
        Copy-Dev $ip $hk $binS43       "/tmp/S43encalertd.new"
        if (Test-Path $binConf) { Copy-Dev $ip $hk $binConf "/tmp/encalertd.conf.new" }
        Copy-Dev $ip $hk $DevInstallSh "/tmp/dev_install.sh"
        Run-Dev $ip $hk 'chmod +x /tmp/dev_install.sh' | Out-Null
        # 2. Run installer (handles copy, SD-dual, start, verify, DEPLOY_DONE marker)
        $out = Run-Dev $ip $hk '/bin/sh /tmp/dev_install.sh; rc=$?; rm -f /tmp/dev_install.sh; exit $rc'
        $out | ForEach-Object { Write-Host ("  | " + $_) }
        if ($out -match 'DEPLOY_DONE') {
            Write-Host ("  -> " + $nm + " OK")
            $OK++
        } else {
            Write-Warning ("  -> " + $nm + " incomplete (missing DEPLOY_DONE marker)")
            $FAIL++
        }
    } catch {
        Write-Error ("  -> " + $nm + " EXCEPTION: " + $_.ToString())
        $FAIL++
    }
}

Write-Host ""
Write-Host ("=== PHASE 2 SUMMARY: OK=" + $OK + " FAIL=" + $FAIL + " ===")
if ($FAIL -gt 0) { exit 10 }
exit 0
