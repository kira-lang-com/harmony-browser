# Toolchain resolution shared by the WebKit check and build scripts.

# The MSVC STL refuses older Clang with "error STL1000: Unexpected compiler
# version". Visual Studio 18's 14.51 toolset requires Clang 20 or newer.
$script:MinimumClangMajor = 20

function Get-ClangMajorVersion {
    param([Parameter(Mandatory)][string]$Executable)

    $banner = & $Executable --version 2>&1 | Select-Object -First 1
    if ($banner -match 'clang version (\d+)\.') {
        return [int]$Matches[1]
    }
    return 0
}

function Get-WebKitClangClCandidate {
    param([string]$ClangCl = '')

    if (-not [string]::IsNullOrWhiteSpace($ClangCl)) {
        return @([System.IO.Path]::GetFullPath($ClangCl))
    }

    $candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($env:LLVM_ROOT)) {
        $candidates.Add((Join-Path $env:LLVM_ROOT 'bin\clang-cl.exe'))
    }
    $candidates.Add('C:\Program Files\LLVM\bin\clang-cl.exe')
    foreach ($command in @(Get-Command clang-cl -All -ErrorAction SilentlyContinue)) {
        $candidates.Add($command.Source)
    }

    return @($candidates | Where-Object { Test-Path $_ } | Select-Object -Unique)
}

<#
.SYNOPSIS
Resolves a clang-cl new enough for the installed MSVC STL.

.DESCRIPTION
Selects by capability rather than PATH order, because unrelated toolchains that
ship their own clang-cl (the Swift toolchain, for one) otherwise shadow a full
LLVM installation and fail deep into the build.
#>
function Resolve-WebKitClangCl {
    param([string]$ClangCl = '')

    $rejected = [System.Collections.Generic.List[string]]::new()
    foreach ($candidate in Get-WebKitClangClCandidate -ClangCl $ClangCl) {
        $major = Get-ClangMajorVersion -Executable $candidate
        if ($major -ge $script:MinimumClangMajor) {
            return [pscustomobject]@{ Path = $candidate; Major = $major }
        }
        $rejected.Add("$candidate (clang $major)")
    }

    $detail = if ($rejected.Count -gt 0) {
        "Rejected: " + ($rejected -join '; ')
    } else {
        'No clang-cl.exe was found.'
    }
    throw "WebKit needs clang-cl $script:MinimumClangMajor or newer. $detail"
}

Export-ModuleMember -Function Resolve-WebKitClangCl
