<#
Build third-party dependencies (independent of the project build).

Layout inside the project:
  third_party\       third-party installs (headers + static libs + CMake configs)
  third_party_src\   third-party sources (downloads / src / build)

Copying the whole project folder to another machine keeps everything needed
to compile; the project builds against third_party\ installs and does NOT
rebuild them (use -Force to rebuild from third_party_src\ on the new machine).

Usage:
  powershell -ExecutionPolicy Bypass -File .\tools\build_third_party.ps1
  powershell -ExecutionPolicy Bypass -File .\tools\build_third_party.ps1 -ProjectRoot D:\path\TestTracking

Parameters:
  -ProjectRoot  Project root (default: parent of this script's directory)
  -Mirror       Optional URL prefix for downloads, for networks with flaky
                GitHub access (e.g. "-Mirror https://ghproxy.net/")
  -Force        Re-download, clean build dirs and rebuild

Outputs:
  <ProjectRoot>\third_party\libzmq-4.3.5\           libzmq headers + static libs (Release + Debug) + CMake config
  <ProjectRoot>\third_party\abseil-cpp-20230802.3\  abseil headers + static libs (Release + Debug) + CMake config
  <ProjectRoot>\third_party\protobuf-25.3\          protobuf headers + static libs (Release + Debug) + protoc + CMake config
  <ProjectRoot>\third_party\concurrentqueue\       concurrentqueue.h (single header)
  <ProjectRoot>\third_party\nlohmann\              json.hpp (single header)
#>

param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Mirror = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$VersionZmq      = "4.3.5"
$VersionAbseil   = "20230802.3"
$VersionProtobuf = "25.3"
$VersionJson     = "3.11.3"

$installRoot  = Join-Path $ProjectRoot "third_party"
$srcRoot      = Join-Path $ProjectRoot "third_party_src"
$downloadsDir = Join-Path $srcRoot "downloads"
$extractDir   = Join-Path $srcRoot "src"
$buildDir     = Join-Path $srcRoot "build"

function Invoke-Checked([string]$Title, [scriptblock]$Action) {
    Write-Host ""
    Write-Host "==> $Title"
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Title failed (exit code $LASTEXITCODE)"
    }
}

function Download-File([string]$Url, [string]$OutFile) {
    $fetchUrl = if ($Mirror -ne "") { "$Mirror$Url" } else { $Url }
    if (Test-Path $OutFile) {
        Write-Host "Use local file: $OutFile"
        return
    }
    Write-Host "Downloading: $fetchUrl"
    & curl.exe -L --fail -sS --connect-timeout 30 --retry 3 --retry-delay 5 --retry-all-errors -o $OutFile $fetchUrl
    if ($LASTEXITCODE -ne 0) {
        throw "Download failed: $Url"
    }
}

function Expand-Zip([string]$Zip, [string]$Dest) {
    & tar.exe -xf $Zip -C $Dest
    if ($LASTEXITCODE -ne 0) {
        Write-Host "tar extraction failed, falling back to Expand-Archive"
        Expand-Archive -Path $Zip -DestinationPath $Dest -Force
    }
}

function Prepare-BuildDir([string]$Dir) {
    if ($Force -and (Test-Path $Dir)) {
        Remove-Item -LiteralPath $Dir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
}

function Install-SingleFile([string]$Url, [string]$SrcFile, [string]$DestFile) {
    if ((Test-Path $SrcFile) -and -not $Force) {
        Write-Host "Use local source: $SrcFile"
    } else {
        Download-File $Url $SrcFile
    }
    Copy-Item -LiteralPath $SrcFile -Destination $DestFile -Force
}

New-Item -ItemType Directory -Force -Path $installRoot, $srcRoot, $downloadsDir, $extractDir, $buildDir | Out-Null

# ---------- libzmq ----------
$zmqZip    = Join-Path $downloadsDir "libzmq-$VersionZmq.zip"
$zmqSrc    = Join-Path $extractDir "libzmq-$VersionZmq"
$zmqBuild  = Join-Path $buildDir "libzmq-$VersionZmq"
$zmqPrefix = Join-Path $installRoot "libzmq-$VersionZmq"

Download-File "https://github.com/zeromq/libzmq/archive/refs/tags/v$VersionZmq.zip" $zmqZip
if ($Force -or -not (Test-Path $zmqSrc)) {
    if ($Force -and (Test-Path $zmqSrc)) {
        Remove-Item -LiteralPath $zmqSrc -Recurse -Force
    }
    Expand-Zip $zmqZip $extractDir
}

$zmqConfigFile = Join-Path $zmqPrefix "CMake\ZeroMQConfig.cmake"
$zmqInstalledRelease = Test-Path $zmqConfigFile
$zmqInstalledDebug   = $zmqInstalledRelease -and (Test-Path (Join-Path $zmqPrefix "CMake\ZeroMQTargets-debug.cmake"))
$zmqCacheFile = Join-Path $zmqBuild "CMakeCache.txt"
if ($Force -or -not ($zmqInstalledRelease -and $zmqInstalledDebug)) {
    if ($Force -or -not (Test-Path $zmqCacheFile)) {
        Prepare-BuildDir $zmqBuild
        Invoke-Checked "libzmq: CMake configure" {
            cmake -S $zmqSrc -B $zmqBuild -A x64 `
                "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
                "-DCMAKE_INSTALL_PREFIX=$zmqPrefix" `
                "-DBUILD_SHARED=OFF" `
                "-DBUILD_STATIC=ON" `
                "-DBUILD_TESTS=OFF" `
                "-DBUILD_BENCHMARKS=OFF" `
                "-DENABLE_DRAFTS=OFF"
        }
    }
    if ($Force -or -not $zmqInstalledRelease) {
        Invoke-Checked "libzmq: build and install (Release)" {
            cmake --build $zmqBuild --config Release --target install
        }
    }
    if ($Force -or -not $zmqInstalledDebug) {
        Invoke-Checked "libzmq: build and install (Debug)" {
            cmake --build $zmqBuild --config Debug --target install
        }
    }
} else {
    Write-Host "libzmq already installed (Release + Debug), skip: $zmqPrefix"
}

# ---------- abseil-cpp (protobuf dependency) ----------
$abslZip    = Join-Path $downloadsDir "abseil-cpp-$VersionAbseil.zip"
$abslSrc    = Join-Path $extractDir "abseil-cpp-$VersionAbseil"
$abslBuild  = Join-Path $buildDir "abseil-cpp-$VersionAbseil"
$abslPrefix = Join-Path $installRoot "abseil-cpp-$VersionAbseil"

Download-File "https://github.com/abseil/abseil-cpp/archive/refs/tags/$VersionAbseil.zip" $abslZip
if ($Force -or -not (Test-Path $abslSrc)) {
    if ($Force -and (Test-Path $abslSrc)) {
        Remove-Item -LiteralPath $abslSrc -Recurse -Force
    }
    Expand-Zip $abslZip $extractDir
}

$abslConfigFile = Join-Path $abslPrefix "lib\cmake\absl\abslConfig.cmake"
$abslInstalledRelease = Test-Path $abslConfigFile
$abslInstalledDebug   = $abslInstalledRelease -and (Test-Path (Join-Path $abslPrefix "lib\cmake\absl\abslTargets-debug.cmake"))
$abslCacheFile = Join-Path $abslBuild "CMakeCache.txt"
if ($Force -or -not ($abslInstalledRelease -and $abslInstalledDebug)) {
    if ($Force -or -not (Test-Path $abslCacheFile)) {
        Prepare-BuildDir $abslBuild
        Invoke-Checked "abseil-cpp: CMake configure" {
            cmake -S $abslSrc -B $abslBuild -A x64 `
                "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
                "-DCMAKE_INSTALL_PREFIX=$abslPrefix" `
                "-DABSL_BUILD_TESTING=OFF" `
                "-DABSL_ENABLE_INSTALL=ON" `
                "-DBUILD_SHARED_LIBS=ON" `
                "-DCMAKE_DEBUG_POSTFIX=d"
        }
    }
    if ($Force -or -not $abslInstalledRelease) {
        Invoke-Checked "abseil-cpp: build and install (Release)" {
            cmake --build $abslBuild --config Release --target install
        }
    }
    if ($Force -or -not $abslInstalledDebug) {
        Invoke-Checked "abseil-cpp: build and install (Debug)" {
            cmake --build $abslBuild --config Debug --target install
        }
    }
} else {
    Write-Host "abseil-cpp already installed (Release + Debug), skip: $abslPrefix"
}

# ---------- protobuf ----------
$pbZip    = Join-Path $downloadsDir "protobuf-v$VersionProtobuf.zip"
$pbSrc    = Join-Path $extractDir "protobuf-$VersionProtobuf"
$pbBuild  = Join-Path $buildDir "protobuf-$VersionProtobuf"
$pbPrefix = Join-Path $installRoot "protobuf-$VersionProtobuf"

Download-File "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v$VersionProtobuf.zip" $pbZip
if ($Force -or -not (Test-Path $pbSrc)) {
    if ($Force -and (Test-Path $pbSrc)) {
        Remove-Item -LiteralPath $pbSrc -Recurse -Force
    }
    Expand-Zip $pbZip $extractDir
}

$pbConfigFile = Join-Path $pbPrefix "cmake\protobuf-config.cmake"
$pbInstalledRelease = Test-Path $pbConfigFile
$pbInstalledDebug   = $pbInstalledRelease -and (Test-Path (Join-Path $pbPrefix "cmake\protobuf-targets-debug.cmake"))
$pbCacheFile = Join-Path $pbBuild "CMakeCache.txt"
if ($Force -or -not ($pbInstalledRelease -and $pbInstalledDebug)) {
    if ($Force -or -not (Test-Path $pbCacheFile)) {
        Prepare-BuildDir $pbBuild
        Invoke-Checked "protobuf: CMake configure" {
            cmake -S $pbSrc -B $pbBuild -A x64 `
                "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
                "-DCMAKE_INSTALL_PREFIX=$pbPrefix" `
                "-Dprotobuf_ABSL_PROVIDER=package" `
                "-DCMAKE_PREFIX_PATH=$abslPrefix" `
                "-Dprotobuf_BUILD_SHARED_LIBS=ON" `
                "-Dprotobuf_BUILD_TESTS=OFF" `
                "-Dprotobuf_MSVC_STATIC_RUNTIME=OFF" `
                "-DCMAKE_DEBUG_POSTFIX=d"
        }
    }
    if ($Force -or -not $pbInstalledRelease) {
        Invoke-Checked "protobuf: build and install (Release)" {
            cmake --build $pbBuild --config Release --target install
        }
    }
    if ($Force -or -not $pbInstalledDebug) {
        Invoke-Checked "protobuf: build and install (Debug)" {
            cmake --build $pbBuild --config Debug --target install
        }
    }
    # protoc.exe needs protobuf.dll and abseil_dll.dll at runtime; keep all
    # three in the same bin directory.
    if (Test-Path (Join-Path $abslPrefix "bin")) {
        Copy-Item (Join-Path $abslPrefix "bin\abseil_dll*.dll") (Join-Path $pbPrefix "bin\") -Force
    }
} else {
    Write-Host "protobuf already installed (Release + Debug), skip: $pbPrefix"
}

# ---------- concurrentqueue (single header) ----------
$cqDir = Join-Path $installRoot "concurrentqueue"
New-Item -ItemType Directory -Force -Path $cqDir | Out-Null
Install-SingleFile "https://raw.githubusercontent.com/cameron314/concurrentqueue/master/concurrentqueue.h" (Join-Path $downloadsDir "concurrentqueue.h") (Join-Path $cqDir "concurrentqueue.h")
Install-SingleFile "https://raw.githubusercontent.com/cameron314/concurrentqueue/master/LICENSE.md" (Join-Path $downloadsDir "concurrentqueue-LICENSE.md") (Join-Path $cqDir "LICENSE.md")

# ---------- nlohmann/json (single header) ----------
$jsonDir = Join-Path $installRoot "nlohmann"
New-Item -ItemType Directory -Force -Path $jsonDir | Out-Null
Install-SingleFile "https://raw.githubusercontent.com/nlohmann/json/v$VersionJson/single_include/nlohmann/json.hpp" (Join-Path $downloadsDir "json.hpp") (Join-Path $jsonDir "json.hpp")
Install-SingleFile "https://raw.githubusercontent.com/nlohmann/json/v$VersionJson/LICENSE.MIT" (Join-Path $downloadsDir "json-LICENSE.MIT") (Join-Path $jsonDir "LICENSE.MIT")

Write-Host ""
Write-Host "==== Build complete ===="
Write-Host "Install root: $installRoot"
Write-Host "Source root : $srcRoot"
Write-Host ""
Write-Host "The project root CMakeLists.txt reads THIRD_PARTY_ROOT automatically."
