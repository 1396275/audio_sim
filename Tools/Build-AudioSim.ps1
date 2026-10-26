param([switch]$ConfigureOnly, [switch]$Clean, [switch]$SkipDeploy)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$repo = $taskRoot
$build = Join-Path $repo 'build\windows-release'
$dependencyRoot = Join-Path $taskRoot 'dependencies'
$packageRoot = Join-Path $dependencyRoot 'nuget'
$sdkBase = Join-Path $packageRoot 'microsoft.windows.sdk.cpp.10.0.26100.7175'
$sdkX64 = Join-Path $packageRoot 'microsoft.windows.sdk.cpp.x64.10.0.26100.7175'
$webview = Join-Path $packageRoot 'microsoft.web.webview2.1.0.1901.177'
$msvc = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231'
$compilerBin = Join-Path $msvc 'bin\Hostx64\x64'
$cmake = 'D:\Software\msys64\ucrt64\bin\cmake.exe'
$ninja = 'D:\Software\msys64\ucrt64\bin\ninja.exe'

$sdkInclude = Join-Path $sdkBase 'c\Include'
$sdkVersion = (Get-ChildItem -LiteralPath $sdkInclude -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
if (-not $sdkVersion) { throw 'Windows SDK headers were not found. Run Get-AudioSimDependencies.ps1 first.' }
$includeRoot = Join-Path $sdkInclude $sdkVersion
$includePaths = @((Join-Path $msvc 'include'))
foreach ($part in @('ucrt', 'shared', 'um', 'winrt', 'cppwinrt')) {
    $candidate = Join-Path $includeRoot $part
    if (Test-Path -LiteralPath $candidate) { $includePaths += $candidate }
}
$libraryPaths = @((Join-Path $msvc 'lib\x64'))
foreach ($part in @('ucrt', 'um')) {
    $found = $false
    foreach ($sdkPackage in @($sdkX64, $sdkBase)) {
        $candidate = Join-Path $sdkPackage "c\$part\x64"
        if (Test-Path -LiteralPath $candidate) {
            $libraryPaths += $candidate
            $found = $true
            break
        }
    }
    if (-not $found) { throw "Windows SDK $part x64 libraries were not found." }
}
$resourceCompiler = Get-Item -LiteralPath (Join-Path $sdkBase "c\bin\$sdkVersion\x64\rc.exe")
$manifestTool = Get-Item -LiteralPath (Join-Path $sdkBase "c\bin\$sdkVersion\x64\mt.exe")
if (-not $resourceCompiler -or -not $manifestTool) { throw 'Windows SDK rc.exe or mt.exe was not found.' }

$env:PATH = "$compilerBin;$($resourceCompiler.DirectoryName);D:\Program Files\nodejs;$env:PATH"
$env:INCLUDE = $includePaths -join ';'
$env:LIB = $libraryPaths -join ';'
$env:VCToolsInstallDir = "$msvc\"
$env:WindowsSdkDir = "$(Join-Path $sdkBase 'c')\"
$env:WindowsSDKVersion = "$sdkVersion\"
$env:npm_config_cache = Join-Path $dependencyRoot 'npm-cache'
$env:VSLANG = '1033'
$env:CL = "$env:CL /utf-8".Trim()
$env:CMAKE_BUILD_PARALLEL_LEVEL = '2'
New-Item -ItemType Directory -Path $build -Force | Out-Null
# Probe the compiler's real UTF-8 /showIncludes output. VSLANG may fall back
# to the installed UI language, and stale CMake probes can contain mojibake.
$probeHeader = Join-Path $build 'AudioSim-IncludeProbe.h'
$probeSource = Join-Path $build 'AudioSim-IncludeProbe.cpp'
$probeStdout = Join-Path $build 'AudioSim-IncludeProbe.stdout.log'
$probeStderr = Join-Path $build 'AudioSim-IncludeProbe.stderr.log'
[IO.File]::WriteAllText($probeHeader, "#pragma once`n")
[IO.File]::WriteAllText($probeSource, "#include `"AudioSim-IncludeProbe.h`"`n")
$probe = Start-Process -FilePath (Join-Path $compilerBin 'cl.exe') `
    -ArgumentList @('/nologo', '/EP', '/showIncludes', "`"$probeSource`"") `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput $probeStdout -RedirectStandardError $probeStderr
# Start-Process -Wait also waits for descendant processes such as MSVC's
# long-lived vctip.exe helper. Only the compiler probe itself must finish.
$probe.WaitForExit()
$probeText = [IO.File]::ReadAllText($probeStdout, [Text.Encoding]::UTF8) + [IO.File]::ReadAllText($probeStderr, [Text.Encoding]::UTF8)
if ($probe.ExitCode -ne 0) { throw "MSVC include-prefix probe failed: $probeText" }
$probeLine = $probeText -split '\r?\n' | Where-Object { $_.Contains($probeHeader) } | Select-Object -First 1
if (-not $probeLine) { throw 'MSVC include-prefix probe returned no included header.' }
$includePrefix = $probeLine.Substring(0, $probeLine.IndexOf($probeHeader, [StringComparison]::OrdinalIgnoreCase)).TrimEnd()
Write-Host "MSVC header dependency prefix: $includePrefix"

$configureLog = Join-Path $build 'configure.log'
$buildLog = Join-Path $build 'build.log'
Write-Host "Configuring AudioSim (log: $configureLog)"

& $cmake -S $repo -B $build -G Ninja `
    "-DAUDIOSIM_MSVC_INCLUDE_PREFIX=$includePrefix" `
    '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_NINJA_CMCLDEPS_RC=OFF'  "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_C_COMPILER=$(Join-Path $compilerBin 'cl.exe')" `
    "-DCMAKE_CXX_COMPILER=$(Join-Path $compilerBin 'cl.exe')" `
    "-DCMAKE_RC_COMPILER=$($resourceCompiler.FullName.Replace('\', '/'))" `
    "-DCMAKE_MT=$($manifestTool.FullName.Replace('\', '/'))" `
    "-DFETCHCONTENT_SOURCE_DIR_JUCE=$(Join-Path $dependencyRoot 'JUCE-8.0.12')" `
    "-DFETCHCONTENT_SOURCE_DIR_MOSQUITTO=$(Join-Path $dependencyRoot 'mosquitto-2.0.20')" `
    "-DFETCHCONTENT_SOURCE_DIR_PTHREADS4W=$(Join-Path $dependencyRoot 'pthreads4w-cmake')" `
    "-DJUCE_WEBVIEW2_PACKAGE_LOCATION=$packageRoot" "-DWebView2_root_dir=$webview" `
    '-DPATCHY_FYI_BACKUP=OFF' '-DPATCHY_BUILD_TESTS=ON' *> $configureLog
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $configureLog | Select-String -Pattern 'error [A-Z]+[0-9]+|fatal error|CMake Error|FAILED:' -Context 1,2 | Select-Object -First 20
    Write-Host "Full configure log: $configureLog"
    exit $LASTEXITCODE
}
Write-Host 'Configure completed.'
if ($ConfigureOnly) { exit 0 }
if ($Clean) {
    Write-Host 'Cleaning generated build products for a full rebuild.'
    & $cmake --build $build --target clean
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Host "Building AudioSim (log: $buildLog)"
& $cmake --build $build --target Patchy_Standalone PatchyTests AudioRoutingTests PaxDllSmoke LevelPax AmpPax SpectrumyserPax StereoSplitterPax --parallel 2 *> $buildLog
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath $buildLog | Select-String -Pattern 'error [A-Z]+[0-9]+|fatal error' -Context 0,1 | Select-Object -First 20
    Write-Host "Full build log: $buildLog"
    exit $LASTEXITCODE
}
Write-Host 'Build completed. Running application and DLL tests.'
$testLog = Join-Path $build 'tests.log'
& (Join-Path (Split-Path -Parent $cmake) 'ctest.exe') --test-dir $build --output-on-failure *> $testLog
$testExit = $LASTEXITCODE
Get-Content -LiteralPath $testLog -Tail 25
if ($testExit -ne 0) { exit $testExit }
if ($SkipDeploy) {
    Write-Host 'Build and tests completed. Run files were not replaced (-SkipDeploy).'
    exit 0
}

$trialRoot = Join-Path $taskRoot 'Run'
$trialPax = Join-Path $trialRoot 'AudioSimPax'
$trialLicenses = Join-Path $trialRoot 'Licenses'
New-Item -ItemType Directory -Path $trialRoot, $trialPax, $trialLicenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Patchy_artefacts\Release\Standalone\AudioSim.exe') -Destination (Join-Path $trialRoot 'AudioSim.exe') -Force
foreach ($name in @('LevelPax', 'AmpPax', 'SpectrumyserPax', 'StereoSplitterPax')) {
    Copy-Item -LiteralPath (Join-Path $build "pax\$name.dll") -Destination $trialPax -Force
}
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $trialLicenses 'Patchy.txt') -Force
Copy-Item -LiteralPath (Join-Path $dependencyRoot 'JUCE-8.0.12\LICENSE.md') -Destination (Join-Path $trialLicenses 'JUCE.txt') -Force
Copy-Item -LiteralPath (Join-Path $repo 'Pax\LICENSE') -Destination (Join-Path $trialLicenses 'Pax.txt') -Force
Copy-Item -LiteralPath (Join-Path $dependencyRoot 'pthreads4w-cmake\LICENSE') -Destination (Join-Path $trialLicenses 'pthreads4w.txt') -Force
Write-Host "AudioSim ready: $(Join-Path $trialRoot 'AudioSim.exe')"
