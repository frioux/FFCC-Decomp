param(
    [string]$DolphinRoot = "C:\Projects\dolphin",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -property installationPath
if (-not $vsRoot) {
    throw "Visual Studio installation not found"
}

$devCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
$source = Join-Path $PSScriptRoot "gba_joybus_harness.cpp"
$outputDir = Join-Path (Split-Path $PSScriptRoot) "build\tools"
$mgbaInclude = Join-Path $DolphinRoot "Externals\mGBA\mgba\include"
$mgbaLib = Join-Path $DolphinRoot "Build\x64\$Configuration\mgba\bin\mgba.lib"

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
if (-not (Test-Path -LiteralPath $mgbaLib)) {
    throw "Bundled mGBA library not found: $mgbaLib"
}

$zlib = Join-Path $DolphinRoot "Build\x64\$Configuration\zlib-ng\bin\zlib-ng.lib"

$compiler = 'cl /nologo /std:c++17 /EHsc /O2 /MD /DNDEBUG ' +
    '"' + $source + '" /I"' + $mgbaInclude + '" ' +
    '/Fe:"' + (Join-Path $outputDir "gba_joybus_harness.exe") + '" ' +
    '/link "' + $mgbaLib + '" "' + $zlib + '" ws2_32.lib shlwapi.lib shell32.lib ole32.lib'

$command = 'call "' + $devCmd + '" -arch=x64 >nul && ' + $compiler
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Harness build failed with exit code $LASTEXITCODE"
}
