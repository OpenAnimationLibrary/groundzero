param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$ThemeDirectory,
    [Parameter(Mandatory = $true)][string]$TestOutput,
    [Parameter(Mandatory = $true)][string]$QtDirectory
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $TestOutput)) { exit 0 }
$testReport = Get-Content -LiteralPath $TestOutput -Raw
if ($testReport -notmatch 'ui_themeapplication[^\r\n]*(Exception|SegFault)') {
    exit 0
}

$debugger = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Debuggers\x64\cdb.exe"
if (-not (Test-Path -LiteralPath $debugger)) {
    Write-Output "Windows SDK debugger is unavailable; use the flushed test checkpoints."
    exit 0
}

$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$themes = (Resolve-Path -LiteralPath $ThemeDirectory).Path
$executable = Join-Path $buildRoot "RelWithDebInfo\ui_themeapplication_test.exe"
$debugLog = Join-Path $buildRoot "ui-theme-debug-output.txt"
$env:PATH = "$QtDirectory\bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$QtDirectory\plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtDirectory\plugins\platforms"

# The original failing CTest remains the result. This bounded workflow step
# reruns only a crashed theme fixture to capture its exception stack.
$PSNativeCommandUseErrorActionPreference = $false
& $debugger -G -lines -c "g; .ecxr; kb 40; q" $executable $themes 2>&1 |
    Tee-Object -FilePath $debugLog
& python (Join-Path $PSScriptRoot "report_ui_tests.py") $debugLog "Theme crash stack"
exit 0
