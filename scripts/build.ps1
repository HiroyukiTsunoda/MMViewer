param([switch]$Test)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$taskVs = & $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual Studio C++ Build Tools が必要です。' }
$taskCmake = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$taskNinja = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$taskVcvars = Join-Path $taskVs 'VC\Auxiliary\Build\vcvars64.bat'
$taskBuild = Join-Path $taskRoot 'build-native'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskCommand = '"{0}" >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="{4}" && "{1}" --build "{3}" --parallel 4' -f $taskVcvars,$taskCmake,$taskRoot,$taskBuild,$taskNinja.Replace('\','/')
& $env:ComSpec /d /s /c $taskCommand *> "$taskBuild\build.log"
if ($LASTEXITCODE -ne 0) { Get-Content "$taskBuild\build.log" -Tail 60; throw 'Build failed (build-native\build.log)' }
if ($Test) {
    & (Join-Path (Split-Path $taskCmake) 'ctest.exe') --test-dir $taskBuild --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
New-Item -ItemType Directory -Force "$taskRoot\dist" | Out-Null
Copy-Item -LiteralPath "$taskBuild\MMViewer.exe" -Destination "$taskRoot\dist\MMViewer.exe"
Get-Item "$taskRoot\dist\MMViewer.exe" | Select-Object FullName,Length
