# Install only Microsoft's debugger component when the hosted image lacks CDB.
# The SDK/compiler selected for the application build remains unchanged.
$ErrorActionPreference = 'Stop'
$debugger = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'
if (Test-Path -LiteralPath $debugger -PathType Leaf) {
    Write-Host 'Windows SDK x64 CDB is available.'
    exit 0
}

# Windows SDK 10.0.26100.9457, from Microsoft's Windows SDK downloads page.
$installerUrl = 'https://download.microsoft.com/download/46742ab5-6592-4968-a793-129e7f3bc55a/KIT_BUNDLE_WINDOWSSDK_MEDIACREATION/winsdksetup.exe'
$installerPath = Join-Path $env:RUNNER_TEMP ('groundzero-debugger-' + [guid]::NewGuid().ToString('N') + '.exe')
try {
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Invoke-WebRequest -Uri $installerUrl -OutFile $installerPath -TimeoutSec 60
            break
        } catch {
            if ($attempt -eq 3) { throw }
            Write-Host "Debugger installer download failed; retrying delivery ($attempt/3)."
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $installerPath
    if ($signature.Status -ne 'Valid' -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'The debugger installer does not have a valid Microsoft signature.'
    }
    $installer = Start-Process -FilePath $installerPath -ArgumentList @(
        '/features', 'OptionId.WindowsDesktopDebuggers', '/quiet', '/norestart'
    ) -PassThru
    if (-not $installer.WaitForExit(480000)) {
        # Stop installation before failing this ephemeral CI job. Do not start
        # application tests while the installation outcome is uncertain.
        & taskkill.exe /PID $installer.Id /T /F 2>&1 | Write-Host
        throw 'The debugger-only SDK installation exceeded eight minutes.'
    }
    $installer.Refresh()
    if ($installer.ExitCode -notin @(0, 3010)) {
        throw "The debugger-only SDK installer failed with status $($installer.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $debugger -PathType Leaf)) {
        throw 'The debugger-only installation did not provide Windows SDK x64 CDB.'
    }
    Write-Host 'Windows SDK x64 CDB installed; the original-crash probe will qualify decoding.'
} finally {
    if (Test-Path -LiteralPath $installerPath) {
        Remove-Item -LiteralPath $installerPath -Force -ErrorAction Continue
    }
}
