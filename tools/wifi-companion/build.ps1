param(
    [string]$Port = 'COM9',
    [int]$Baud = 115200,
    [switch]$Flash
)
$ErrorActionPreference = 'Stop'
if (-not $env:IDF_PATH) { throw 'Run from an ESP-IDF 5.5.5 PowerShell environment.' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$sourceDir = Join-Path $repoRoot '.cache/esp-hosted-mcu'
$commit = '0e8d0e3f0c2723deaf15441200cb4d8c53fea230'
if (-not (Test-Path $sourceDir)) {
    git clone --depth 1 --branch v2.13.1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-hosted-mcu.git $sourceDir
    if ($LASTEXITCODE) { throw 'ESP-Hosted clone failed.' }
}
$actualCommit = git -C $sourceDir rev-parse HEAD
if ($actualCommit -ne $commit) { throw "Expected ESP-Hosted v2.13.1 ($commit), found $actualCommit" }
$patchFile = Join-Path $PSScriptRoot 'esp-hosted-1bit.patch'
if (-not (Select-String -LiteralPath (Join-Path $sourceDir 'slave/main/sdio_slave_api.c') -Pattern 'Retro-Go uses 1-bit SDIO' -Quiet)) {
    git -C $sourceDir apply --check $patchFile
    if ($LASTEXITCODE) { throw 'Companion source differs from the expected upstream version.' }
    git -C $sourceDir apply $patchFile
    if ($LASTEXITCODE) { throw 'Could not apply 1-bit SDIO companion patch.' }
}
else {
    git -C $sourceDir apply --reverse --check $patchFile
    if ($LASTEXITCODE) { throw 'Companion source differs from the expected 1-bit SDIO patch.' }
}
$slaveDir = Join-Path $sourceDir 'slave'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sdkconfig.defaults.esp32') -Destination (Join-Path $slaveDir 'sdkconfig.retro-go') -Force
Push-Location $slaveDir
try {
    # The last defaults file overrides upstream Bluetooth/transport defaults.
    & python "$env:IDF_PATH/tools/idf.py" -DIDF_TARGET=esp32 -DSDKCONFIG=sdkconfig.retro-go.generated '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32;sdkconfig.retro-go' build
    if ($LASTEXITCODE) { throw 'Wi-Fi companion build failed.' }
    if ($Flash) {
        & python "$env:IDF_PATH/tools/idf.py" -p $Port -b $Baud flash
        if ($LASTEXITCODE) { throw 'Wi-Fi companion flash failed.' }
    }
}
finally { Pop-Location }
