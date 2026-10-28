param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path $Source).Path
$destination = [System.IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$binaryDirectory = Join-Path $sourceRoot 'serving/sidecar/target/x86_64-pc-windows-msvc/release'
Copy-Item (Join-Path $binaryDirectory 'cadmium-sidecar.exe') $destination
foreach ($name in @('onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'DirectML.dll')) {
    Copy-Item (Join-Path $sourceRoot "serving/sidecar/vendor/$name") $destination
}
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item (Join-Path $sourceRoot 'LICENSE') (Join-Path $licenses 'Cadmium-LICENSE')
Copy-Item (Join-Path $sourceRoot 'NOTICE') (Join-Path $licenses 'Cadmium-NOTICE')
$roots = @('third_party', 'serving/sidecar/vendor-licenses')
foreach ($root in $roots) {
    $fullRoot = Join-Path $sourceRoot $root
    if (-not (Test-Path $fullRoot)) { throw "Missing dependency license directory: $root" }
    foreach ($file in Get-ChildItem $fullRoot -Recurse -File) {
        if ($file.Name -notmatch '^(LICENSE|NOTICE|COPYING|COPYRIGHT)' -and
            $root -ne 'serving/sidecar/vendor-licenses') { continue }
        $relative = $file.FullName.Substring($fullRoot.Length).TrimStart('\', '/')
        $relative = $root.Replace('/', '-') + '-' + $relative.Replace('\', '-').Replace('/', '-')
        Copy-Item $file.FullName (Join-Path $licenses $relative)
    }
}
$records = @()
foreach ($file in Get-ChildItem $destination -File) {
    $records += [ordered]@{
        file = $file.Name
        bytes = $file.Length
        sha256 = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [ordered]@{
    format = 1
    source = 'd817cae470a1499df4985be460e0fdc721233987'
    platform = 'windows-x64'
    files = $records
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $destination 'runtime-manifest.json')
