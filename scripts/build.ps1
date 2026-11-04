param(
    [switch]$InterpreterOnly,
    [string]$LLVMDir,
    [ValidateRange(1, 64)][int]$Jobs = 2
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

if ($env:OS -eq 'Windows_NT') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Install Visual Studio C++ build tools and the Windows SDK first.'
    }
    $vsPath = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'No Visual Studio x64 C++ toolchain found.' }
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    $cmakeBin = Join-Path $env:ProgramFiles 'CMake\bin'
    if (Test-Path -LiteralPath $cmakeBin) { $env:Path = "$cmakeBin;$env:Path" }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($userPath) { $env:Path += ";$userPath" }
}

$buildName = if ($InterpreterOnly) { 'interpreter' } else { 'native' }
$buildDir = Join-Path $projectRoot "build\$buildName"
$options = @('-S', $projectRoot, '-B', $buildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
if ($InterpreterOnly) {
    $options += '-DPIKAFLOW_ENABLE_LLVM=OFF'
} else {
    if (-not $LLVMDir) {
        $LLVMDir = Join-Path (Split-Path -Parent $projectRoot) 'llvm-work\llvm-project\build\lib\cmake\llvm'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $LLVMDir 'LLVMConfig.cmake'))) {
        throw 'LLVMConfig.cmake not found. Supply -LLVMDir pointing to a built LLVM 24 lib/cmake/llvm directory.'
    }
    $options += @('-DPIKAFLOW_ENABLE_LLVM=ON', "-DLLVM_DIR=$LLVMDir")
}
& cmake @options
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
& cmake --build $buildDir --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
& ctest --test-dir $buildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)." }
