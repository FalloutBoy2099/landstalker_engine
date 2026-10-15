param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$src   = $PSScriptRoot
$build = Join-Path $src 'build'

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

# SDL submodule (HTTPS, car .gitmodules utilise SSH)
if (-not (Test-Path (Join-Path $src 'third_party\SDL\CMakeLists.txt'))) {
    git -C $src -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init third_party/SDL
    if ($LASTEXITCODE -ne 0) { throw 'Echec du telechargement de SDL' }
}

& $cmake -S $src -B $build
if ($LASTEXITCODE -ne 0) { throw 'Echec de la configuration CMake' }

& $cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw 'Echec de la compilation' }

# CMake only copies the assets when the executable is relinked: always sync them here.
robocopy (Join-Path $src 'assets') (Join-Path (Join-Path $build $Config) 'assets') /MIR /NFL /NDL /NJH /NJS /NP | Out-Null

Write-Host "OK -> $build\$Config\dds520.exe" -ForegroundColor Green
