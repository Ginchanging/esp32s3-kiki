param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('build', 'flash', 'setup', 'status', 'scan', 'retry', 'clear')]
    [string]$Action,

    [string]$Port,
    [switch]$Bootloader,
    [switch]$Recover
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$espToolRoot = Join-Path (Split-Path -Parent $PSScriptRoot) '.esp-tools'

if ($Action -eq 'build') {
    Push-Location -LiteralPath $PSScriptRoot
    try {
        $espIdf = Join-Path $espToolRoot 'idf/v6.1/esp-idf'
        $espExport = Join-Path $espIdf 'export.ps1'
        if (Test-Path -LiteralPath $espExport) {
            $env:IDF_TOOLS_PATH = Join-Path $espToolRoot 'runtime'
            . $espExport
        } elseif (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
            throw 'Activate ESP-IDF v6.1 first.'
        }
        $env:IDF_COMPONENT_MANAGER = '0'
        idf.py build
        exit $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

$espPython = Join-Path $espToolRoot 'runtime/python_env/idf6.1_py3.13_env/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $espPython)) {
    $espPython = (Get-Command python -CommandType Application | Select-Object -First 1).Source
}
$espCliArgs = @($Action)
if ($Port) {
    $espCliArgs += @('--port', $Port)
}
if ($Bootloader) {
    $espCliArgs += '--bootloader'
}
if ($Recover) {
    $espCliArgs += '--recover'
}
& $espPython -B (Join-Path $PSScriptRoot 'campus.py') @espCliArgs
exit $LASTEXITCODE
