param(
    [string]$WebKitRoot = (Join-Path $PSScriptRoot '..\third_party\WebKit'),
    [string]$VcpkgRoot = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$Jobs = [Math]::Max(2, [int]$env:NUMBER_OF_PROCESSORS / 2),
    [ValidateSet('Idle', 'BelowNormal', 'Normal')]
    [string]$Priority = 'BelowNormal',
    [switch]$UnifiedBuilds,
    [switch]$KeepGoing,
    [string]$ClangCl = ''
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WebKitToolchain.psm1') -Force
$resolvedRoot = [System.IO.Path]::GetFullPath($WebKitRoot)
if (-not (Test-Path (Join-Path $resolvedRoot '.git'))) {
    throw "WebKit checkout not found at $resolvedRoot"
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    if (Test-Path (Join-Path $env:USERPROFILE 'vcpkg\vcpkg.exe')) {
        $VcpkgRoot = Join-Path $env:USERPROFILE 'vcpkg'
    } elseif (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $VcpkgRoot = $env:VCPKG_ROOT
    } else {
        $VcpkgRoot = Join-Path $resolvedRoot 'WebKitLibraries\windows\vcpkg'
    }
}

$resolvedVcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot)
$vcpkgExecutable = Join-Path $resolvedVcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $vcpkgExecutable)) {
    throw "vcpkg.exe not found at $vcpkgExecutable. Install or bootstrap vcpkg, then pass -VcpkgRoot."
}

foreach ($tool in @('perl', 'python', 'cmake', 'ninja')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required by the upstream WebKit Windows build"
    }
}

$env:VCPKG_ROOT = $resolvedVcpkgRoot
# WebKit's Windows port uses clang-cl with the MSVC SDK/linker environment.
$clang = Resolve-WebKitClangCl -ClangCl $ClangCl
Write-Output "clang-cl: $($clang.Path) (clang $($clang.Major))"
$env:CC = $clang.Path
$env:CXX = $clang.Path
# Keep the rest of LLVM's tools in step with the compiler CMake was given.
$env:PATH = (Split-Path -Parent $clang.Path) + ';' + $env:PATH
$env:WK_USE_CCACHE = 'NO'

# Windows child processes inherit the parent's priority class, so demoting this
# process keeps perl, ninja, and every clang-cl behind interactive work.
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = $Priority

Push-Location $resolvedRoot
try {
    # Current WebKit uses vcpkg manifest mode; the old
    # update-webkit-win-libs.py helper is no longer part of the checkout.
    $configureArgs = @(
        'Tools/Scripts/build-webkit',
        "--$($Configuration.ToLowerInvariant())",
        '--no-use-ccache',
        '--generate-project-only',
        '--cmakeargs=-DCMAKE_C_FLAGS=-Wno-cast-function-type-mismatch',
        '--cmakeargs=-DCMAKE_CXX_FLAGS=-Wno-cast-function-type-mismatch',
        # WebCore's include list pushes compile commands past the 32767-character
        # CreateProcess limit. Response files keep the invocation independent of
        # how deep the checkout sits.
        '--cmakeargs=-DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON'
    )
    # Unified sources bundle dozens of translation units together, so one edited
    # file rebuilds the whole bundle. Harmony trades cold-build throughput for
    # an incremental loop that recompiles only what changed.
    if (-not $UnifiedBuilds) {
        $configureArgs += '--no-unified-builds'
    }
    & perl @configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "WebKit project generation failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

# Drive Ninja directly. build-webkit forwards --makeargs through Perl, CMake and
# a shell, and -k does not survive the trip; job count and keep-going matter too
# much on a memory-bound machine to leave to that path.
$buildPath = Join-Path $resolvedRoot "WebKitBuild\$Configuration"
$ninjaArgs = @('-C', $buildPath, "-j$Jobs")
if ($KeepGoing) {
    # Collect every independent error in one pass instead of one rebuild each.
    $ninjaArgs += @('-k', '0')
}

Write-Output "ninja $ninjaArgs"
& ninja @ninjaArgs
if ($LASTEXITCODE -ne 0) {
    throw "WebKit build failed with exit code $LASTEXITCODE"
}
