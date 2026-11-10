param(
    [ValidateSet('Build', 'Probe', 'Backup', 'Flash', 'Monitor')]
    [string]$Action = 'Build',
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port = 'COM4',
    [switch]$LegacyRf,
    [switch]$Clang,
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (!$OutputRoot) { $OutputRoot = Join-Path $project 'build/windows' }
$OutputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
$toolchain = Join-Path $OutputRoot 'toolchain'
$env:IDF_PATH = Join-Path $toolchain 'idf'
$env:IDF_TOOLS_PATH = Join-Path $toolchain 'esp-tools'
$env:IDF_PYTHON_ENV_PATH = Join-Path $env:IDF_TOOLS_PATH 'python_env/idf5.5_py3.12_env'
$env:TEMP = Join-Path $toolchain 'tmp'
$env:TMP = $env:TEMP
$env:PIP_CACHE_DIR = Join-Path $toolchain 'pip-cache'
$env:PYTHONNOUSERSITE = '1'
$env:IDF_COMPONENT_MANAGER = '0'
$env:IDF_TARGET = 'esp32'

$tools = Join-Path $env:IDF_TOOLS_PATH 'tools'
$pythonScripts = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts'
$python = Join-Path $pythonScripts 'python.exe'
$idf = Join-Path $env:IDF_PATH 'tools/idf.py'
if (!(Test-Path -LiteralPath $python -PathType Leaf) -or !(Test-Path -LiteralPath $idf -PathType Leaf)) {
    throw "ESP-IDF tools not found under '$toolchain'. Run scripts/Install-SomfyTools.ps1 with the same output root first."
}
$gcc = Join-Path $tools 'xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin'
$cmake = Join-Path $tools 'cmake/3.30.2/bin'
$ninja = Join-Path $tools 'ninja/1.12.1'
$ulp = Join-Path $tools 'esp32ulp-elf/2.38_20240113/esp32ulp-elf/bin'
$env:ESP_ROM_ELF_DIR = (Join-Path $tools 'esp-rom-elfs/20241011') + '/'
$env:PATH = (@($pythonScripts, $gcc, $cmake, $ninja, $ulp) -join ';') + ';' + $env:PATH
if ($Clang) {
    $env:IDF_TOOLCHAIN = 'clang'
    $clangBin = Join-Path $tools 'esp-clang/esp-19.1.2_20250312/esp-clang/bin'
    if (!(Test-Path -LiteralPath (Join-Path $clangBin 'clang.exe') -PathType Leaf)) {
        $clangBin = Join-Path $toolchain 'esp-clang/esp-clang/bin'
    }
    if (!(Test-Path -LiteralPath (Join-Path $clangBin 'clang.exe') -PathType Leaf)) {
        throw "ESP Clang not found under '$toolchain'. Run Install-SomfyTools.ps1 -Clang first."
    }
    $env:PATH = $clangBin + ';' + $env:PATH
} else {
    $env:IDF_TOOLCHAIN = 'gcc'
}

if ($Action -eq 'Probe' -or $Action -eq 'Backup') {
    $arguments = @('-m', 'esptool', '--chip', 'esp32', '--port', $Port)
    if ($Action -eq 'Probe') {
        & $python @arguments flash_id
    } else {
        $backup = Join-Path $OutputRoot "esp32-flash-$Port-$(Get-Date -Format yyyyMMdd-HHmmss).bin"
        # Read from byte zero at the ESP-IDF flashing rate.
        & $python @arguments --baud 460800 read_flash 0 ALL $backup
        if ($LASTEXITCODE -eq 0) {
            Get-FileHash -Algorithm SHA256 -LiteralPath $backup
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "esptool failed on $Port (exit $LASTEXITCODE)." }
    return
}

$profile = if ($Clang) {
    if ($LegacyRf) { 'clang-legacy' } else { 'clang' }
} elseif ($LegacyRf) {
    'legacy'
} else {
    'tuned'
}
$legacy = if ($LegacyRf) { 'ON' } else { 'OFF' }
$buildDir = Join-Path $OutputRoot "build-$profile"
$sdkConfig = (Join-Path $buildDir 'sdkconfig').Replace('\', '/')

if ($Action -eq 'Build') {
    $cmakeExe = Join-Path $cmake 'cmake.exe'
    $configure = @(
        '-G', 'Ninja', '-S', $project, '-B', $buildDir, '-DESP_PLATFORM=1',
        "-DPYTHON=$python", '-DPYTHON_DEPS_CHECKED=1', '-DCCACHE_ENABLE=0',
        "-DSDKCONFIG=$sdkConfig", "-DSOMFY_LEGACY_RF=$legacy"
    )
    & $cmakeExe @configure
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF configure failed (exit $LASTEXITCODE)." }
    & $cmakeExe --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build failed (exit $LASTEXITCODE)." }
    Write-Host "Firmware built under $buildDir"
    return
}

$arguments = @(
    $idf, '-C', $project, '-B', $buildDir,
    '-D', "SDKCONFIG=$sdkConfig", '-D', "SOMFY_LEGACY_RF=$legacy"
)

switch ($Action) {
    'Flash'   { & $python @arguments -p $Port flash }
    'Monitor' { & $python @arguments -p $Port monitor }
}
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF $Action failed (exit $LASTEXITCODE)." }
