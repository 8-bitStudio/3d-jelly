param(
    [string]$Makerom = "",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Output) {
    $Output = Join-Path $root "dist\3dJelly.cia"
}

$elf = Join-Path $root "3dJelly.elf"
$smdh = Join-Path $root "3dJelly.smdh"
if (-not (Test-Path $elf)) {
    throw "Missing $elf. Run make first."
}
if (-not (Test-Path $smdh)) {
    throw "Missing $smdh. Run make first."
}

if (-not $Makerom) {
    $cmd = Get-Command makerom -ErrorAction SilentlyContinue
    if ($cmd) {
        $Makerom = $cmd.Source
    } else {
        $local = Join-Path $root "tools\makerom.exe"
        if (Test-Path $local) {
            $Makerom = $local
        }
    }
}

if (-not $Makerom -or -not (Test-Path $Makerom)) {
    throw "makerom.exe was not found. Place makerom.exe in 3dJelly\tools or pass -Makerom."
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null

$titleId = "0x0004000003D7E11A"
$args = @(
    "-f", "cia",
    "-o", $Output,
    "-elf", $elf,
    "-icon", $smdh,
    "-rsf", (Join-Path $root "tools\cia.rsf"),
    "-target", "t",
    "-exefslogo",
    "-DAPP_TITLE=3dJelly",
    "-DAPP_PRODUCT_CODE=CTR-P-DJLY",
    "-DAPP_UNIQUE_ID=0xD7E11",
    "-DAPP_TITLE_ID=$titleId"
)

& $Makerom @args
if ($LASTEXITCODE -ne 0) {
    throw "makerom failed with exit code $LASTEXITCODE"
}

Write-Host "CIA written to $Output"
