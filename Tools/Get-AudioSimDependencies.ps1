$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$dependencyRoot = Join-Path $taskRoot 'dependencies'
$packageRoot = Join-Path $dependencyRoot 'nuget'
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

$packages = @(
    @{ Id = 'microsoft.windows.sdk.cpp'; Version = '10.0.26100.7175' },
    @{ Id = 'microsoft.windows.sdk.cpp.x64'; Version = '10.0.26100.7175' },
    @{ Id = 'microsoft.web.webview2'; Version = '1.0.1901.177' }
)
foreach ($package in $packages) {
    $name = "$($package.Id).$($package.Version)"
    $archivePath = Join-Path $packageRoot "$name.nupkg"
    $extractPath = Join-Path $packageRoot $name
    $marker = Join-Path $extractPath '.extracted'
    if (Test-Path -LiteralPath $marker) {
        Write-Host "Already available: $name"
        continue
    }
    $uri = "https://api.nuget.org/v3-flatcontainer/$($package.Id)/$($package.Version)/$name.nupkg"
    Write-Host "Downloading $name from NuGet"
    Invoke-WebRequest -Uri $uri -OutFile $archivePath -TimeoutSec 180
    Write-Host "Extracting $name"
    [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $extractPath, $true)
    Set-Content -LiteralPath $marker -Value $uri -Encoding utf8
}

$sources = @(
    @{ Name = 'JUCE-8.0.12'; Uri = 'https://codeload.github.com/juce-framework/JUCE/zip/refs/tags/8.0.12' },
    @{ Name = 'mosquitto-2.0.20'; Uri = 'https://codeload.github.com/eclipse-mosquitto/mosquitto/zip/refs/tags/v2.0.20' },
    @{ Name = 'pthreads4w-cmake'; Uri = 'https://codeload.github.com/jwinarske/pthreads4w/zip/refs/heads/cmake' }
)
foreach ($source in $sources) {
    $sourceRoot = Join-Path $dependencyRoot $source.Name
    if (Test-Path -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt')) {
        Write-Host "Already available: $($source.Name)"
        continue
    }
    $archivePath = Join-Path $dependencyRoot "$($source.Name).zip"
    Write-Host "Downloading source archive: $($source.Name)"
    Invoke-WebRequest -Uri $source.Uri -OutFile $archivePath -TimeoutSec 180
    [System.IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $dependencyRoot, $true)
}
