# Post-link verification for Freikino.exe.
#
# Invoked by CMake as a POST_BUILD custom command for the `freikino` target.
# Fails the build if any required exploit mitigation is missing from the PE,
# or if a forbidden UI-framework DLL appears in the import table.

param(
    [Parameter(Mandatory = $true)][string] $ExePath,
    [Parameter(Mandatory = $true)][string] $DumpBin
)

$ErrorActionPreference = "Stop"
$fail = 0

function Fail([string]$msg) {
    Write-Host "verify: FAIL  $msg"
    $script:fail = 1
}
function Pass([string]$msg) {
    Write-Host "verify: ok    $msg"
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    Write-Host "verify: exe not found: $ExePath"
    exit 2
}
if (-not (Test-Path -LiteralPath $DumpBin)) {
    Write-Host "verify: dumpbin not found: $DumpBin"
    exit 2
}

$name = Split-Path -Leaf $ExePath
Write-Host "verify: $name"

# -- Exploit mitigations (PE headers) -----------------------------------------
$headers = & $DumpBin /headers $ExePath
if ($LASTEXITCODE -ne 0) {
    Write-Host "verify: dumpbin /headers failed (exit $LASTEXITCODE)"
    exit 2
}
$headersText = [string]::Join("`n", $headers)

$required = @(
    "Dynamic base",
    "High Entropy Virtual Addresses",
    "NX compatible",
    "Control Flow Guard",
    "CET compatible"
)
foreach ($r in $required) {
    if ($headersText -match [regex]::Escape($r)) { Pass $r }
    else                                         { Fail "missing mitigation: $r" }
}

# -- Dependency blacklist (UI frameworks we refuse to ship) --------------------
$deps = & $DumpBin /dependents $ExePath
if ($LASTEXITCODE -ne 0) {
    Write-Host "verify: dumpbin /dependents failed (exit $LASTEXITCODE)"
    exit 2
}
$depLines = $deps | Where-Object { $_ -match "\.dll" } | ForEach-Object { $_.Trim() }

$forbidden = @(
    @{ Pattern = '(?i)^mfc\d+.*\.dll$';         Tag = "MFC" },
    @{ Pattern = '(?i)^atl\d*\.dll$';           Tag = "ATL" },
    @{ Pattern = '(?i)^atls\.dll$';             Tag = "ATL" },
    @{ Pattern = '(?i)^qt\d+.*\.dll$';          Tag = "Qt" },
    @{ Pattern = '(?i)^libqt.*\.dll$';          Tag = "Qt" },
    @{ Pattern = '(?i)^wx(base|msw).*\.dll$';   Tag = "wxWidgets" },
    @{ Pattern = '(?i)^gtk-?\d*.*\.dll$';       Tag = "GTK" }
)

$forbiddenFound = $false
foreach ($line in $depLines) {
    foreach ($rule in $forbidden) {
        if ($line -match $rule.Pattern) {
            Fail ("forbidden {0} dependency: {1}" -f $rule.Tag, $line)
            $forbiddenFound = $true
        }
    }
}
if (-not $forbiddenFound) { Pass "no forbidden UI-framework deps" }

if ($fail -eq 0) {
    Write-Host "verify: PASS"
    exit 0
}
Write-Host "verify: FAILED"
exit 1
