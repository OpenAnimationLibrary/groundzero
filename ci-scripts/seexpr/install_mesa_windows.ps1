# CI-only desktop OpenGL for the actual application fixture. The runtime is
# verified before compilation and never becomes an installed app dependency.
$ErrorActionPreference = "Stop"
$mesaRoot = Join-Path $env:RUNNER_TEMP "seexpr-native-mesa-26.2.4"
New-Item -ItemType Directory -Path $mesaRoot -Force | Out-Null
$archive = Join-Path $mesaRoot "mesa3d-26.2.4-release-msvc.7z"
curl.exe -fSL --retry 2 --retry-delay 2 --max-time 90 -o $archive https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z
if ($LASTEXITCODE -ne 0) { throw "The native fixture renderer could not be downloaded" }
$expected = "351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695"
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    throw "The native fixture renderer checksum does not match"
}
7z x $archive "x64/opengl32.dll" "x64/libgallium_wgl.dll" "-o$mesaRoot" -y
if ($LASTEXITCODE -ne 0) { throw "The native fixture renderer could not be extracted" }
$runtime = Join-Path $mesaRoot "x64"
foreach ($name in @("opengl32.dll", "libgallium_wgl.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf)) {
        throw "The verified native fixture renderer is incomplete"
    }
}
Add-Content -LiteralPath $env:GITHUB_ENV -Value "SEEXPR_TEST_MESA=$runtime" -Encoding utf8
