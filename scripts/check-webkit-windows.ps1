param(
    [string]$WebKitRoot = (Join-Path $PSScriptRoot '..\third_party\WebKit'),
    [string]$ClangCl = ''
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WebKitToolchain.psm1') -Force

$resolvedRoot = [System.IO.Path]::GetFullPath($WebKitRoot)
$requiredTools = @('git', 'cmake', 'ninja', 'perl', 'python', 'ruby')
$missingTools = @(
    $requiredTools | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) }
)

$clangError = ''
try {
    $clang = Resolve-WebKitClangCl -ClangCl $ClangCl
} catch {
    $clang = $null
    $clangError = $_.Exception.Message
}

Write-Output "WebKit source root: $resolvedRoot"
if (Test-Path (Join-Path $resolvedRoot '.git')) {
    Write-Output 'Source checkout: present'
} else {
    Write-Output 'Source checkout: missing'
}

if ($missingTools.Count -eq 0) {
    Write-Output 'Build tools: present'
} else {
    Write-Output ("Build tools missing: " + ($missingTools -join ', '))
}

if ($clang) {
    Write-Output "clang-cl: $($clang.Path) (clang $($clang.Major))"
} else {
    Write-Output "clang-cl: $clangError"
}

$runtime = Join-Path $resolvedRoot 'WebKitBuild\Release\bin\WebKit2.dll'
if (Test-Path $runtime) {
    Write-Output "Built runtime: present ($runtime)"
} else {
    Write-Output 'Built runtime: missing'
}

if ($missingTools.Count -gt 0 -or -not $clang -or -not (Test-Path (Join-Path $resolvedRoot '.git')) -or -not (Test-Path $runtime)) {
    exit 1
}

exit 0
