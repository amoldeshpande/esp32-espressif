param(
    [switch]$Clang,
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (!$OutputRoot) { $OutputRoot = Join-Path $project 'build/windows' }
$OutputRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
if ($project -match '\s' -or $OutputRoot -match '\s') {
    throw 'ESP-IDF needs repository and output paths without spaces.'
}

foreach ($program in @('git.exe', 'curl.exe', 'tar.exe')) {
    if (!(Get-Command $program -ErrorAction SilentlyContinue)) {
        throw "$program is required. Install Git for Windows if git.exe is missing."
    }
}

$toolchain = Join-Path $OutputRoot 'toolchain'
$env:IDF_PATH = Join-Path $toolchain 'idf'
$env:IDF_TOOLS_PATH = Join-Path $toolchain 'esp-tools'
$env:TEMP = Join-Path $toolchain 'tmp'
$env:TMP = $env:TEMP
$env:PIP_CACHE_DIR = Join-Path $toolchain 'pip-cache'
$env:PYTHONNOUSERSITE = '1'
$env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
Remove-Item Env:IDF_PYTHON_ENV_PATH -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

$python = Join-Path $toolchain 'python/python.exe'
if (!(Test-Path -LiteralPath $python -PathType Leaf)) {
    $archive = Join-Path $toolchain 'python.tar.gz'
    $pythonUrl = 'https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.12.16%2B20261026-x86_64-pc-windows-msvc-install_only.tar.gz'
    $sha256 = '590DFCBE3A34C07301E6332E13E2188B98655CD072B08AFA45A7998DC4C22BC5'
    if (!(Test-Path -LiteralPath $archive -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ine $sha256) {
        # Retry transient download failures twice.
        & curl.exe -fsSL --retry 2 $pythonUrl -o $archive
        if ($LASTEXITCODE -ne 0) { throw "Python download failed (exit $LASTEXITCODE)." }
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ine $sha256) {
        throw 'Portable Python archive failed SHA-256 verification.'
    }
    & tar.exe -xf $archive -C $toolchain
    if ($LASTEXITCODE -ne 0) { throw "Python extraction failed (exit $LASTEXITCODE)." }
}

$idfVersion = 'v5.5.2'
$idfCommit = '30aaf64524299d3bde422ca9a2848090d1bc5d0f'
if (!(Test-Path -LiteralPath (Join-Path $env:IDF_PATH '.git'))) {
    $clone = @('clone', '-c', 'core.longpaths=true', '--depth', '1', '--branch', $idfVersion, '--single-branch',
               'https://github.com/espressif/esp-idf.git', $env:IDF_PATH)
    & git.exe @clone
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF clone failed (exit $LASTEXITCODE)." }
}
$head = & git.exe -C $env:IDF_PATH rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $idfCommit) {
    throw "Expected ESP-IDF $idfVersion in '$env:IDF_PATH'. Choose a new output root for a fresh setup."
}
# Shallow checkouts keep setup small. Limit parallel submodule fetches to eight.
& git.exe -c core.longpaths=true -C $env:IDF_PATH submodule update --init --recursive --depth 1 --jobs 8
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF submodule setup failed (exit $LASTEXITCODE)." }

$idfTools = Join-Path $env:IDF_PATH 'tools/idf_tools.py'
$tools = @('cmake', 'ninja', 'esp-rom-elfs')
if ($Clang) {
    $tools += 'esp-clang'
} else {
    $tools += @('xtensa-esp-elf', 'esp32ulp-elf')
}

& $python $idfTools --non-interactive install @tools
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF tools installation failed (exit $LASTEXITCODE)." }
& $python $idfTools --non-interactive install-python-env --features=core
if ($LASTEXITCODE -ne 0) { throw "ESP-IDF Python setup failed (exit $LASTEXITCODE)." }

Write-Host "Toolchain ready under $toolchain"
