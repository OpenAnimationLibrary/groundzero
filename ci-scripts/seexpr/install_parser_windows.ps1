$ErrorActionPreference = "Stop"
if (-not $env:GITHUB_PATH) { throw "Parser setup requires the CI path output file" }
$toolRoot = Join-Path $env:RUNNER_TEMP "groundzero-seexpr-parser-2.5.25"
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
$archive = Join-Path $toolRoot "win_flex_bison-2.5.25.zip"
curl.exe -fSL --retry 2 --retry-delay 2 --max-time 90 -o $archive https://github.com/lexxmark/winflexbison/releases/download/v2.5.25/win_flex_bison-2.5.25.zip
if ($LASTEXITCODE -ne 0) { throw "Could not download the pinned SeExpr parser tools" }
$expected = "8d324b62be33604b2c45ad1dd34ab93d722534448f55a16ca7292de32b6ac135"
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    throw "SeExpr parser tool archive checksum does not match"
}
Expand-Archive -LiteralPath $archive -DestinationPath $toolRoot -Force
foreach ($name in @("win_bison.exe", "win_flex.exe")) {
    if (-not (Test-Path -LiteralPath (Join-Path $toolRoot $name) -PathType Leaf)) {
        throw "The pinned SeExpr parser archive is missing a required tool"
    }
}
Add-Content -LiteralPath $env:GITHUB_PATH -Value $toolRoot -Encoding utf8
Write-Output "Pinned SeExpr parser tools are ready."
