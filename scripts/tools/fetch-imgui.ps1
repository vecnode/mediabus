# fetch-imgui.ps1 - vendor Dear ImGui at a pinned release.
#
# ImGui is tracked in the tree (like httplib.h and json.hpp) so a clone builds
# with no network access. This script is how that copy is produced or refreshed;
# it is not part of the normal build.
#
# Only the files the build compiles are taken: the five core sources and the two
# backends. Everything else in the upstream repository - the examples, the
# documentation, the other backends, misc/cpp - is deliberately not vendored.
#
#     pwsh -File scripts/tools/fetch-imgui.ps1                 # the pinned version
#     pwsh -File scripts/tools/fetch-imgui.ps1 -Version v1.92.8
#     pwsh -File scripts/tools/fetch-imgui.ps1 -Force           # replace an existing copy

param(
    [string]$Version = 'v1.92.9b',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# This script lives in scripts/tools/, so the repository root is two levels up.
$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Dest = Join-Path $Repo 'vendor\imgui'

# The exact upstream files the build needs. Keep this list in step with the
# imgui target in CMakeLists.txt.
$CoreFiles = @(
    'imgui.cpp'
    'imgui_draw.cpp'
    'imgui_tables.cpp'
    'imgui_widgets.cpp'
    'imgui.h'
    'imgui_internal.h'
    'imconfig.h'
    'imgui.cpp'
    'imstb_rectpack.h'
    'imstb_textedit.h'
    'imstb_truetype.h'
)
$BackendFiles = @(
    'backends/imgui_impl_glfw.cpp'
    'backends/imgui_impl_glfw.h'
    'backends/imgui_impl_opengl3.cpp'
    'backends/imgui_impl_opengl3.h'
    # Dear ImGui 1.92's OpenGL3 backend always uses its own bundled gl3w-derived
    # loader: the IMGUI_IMPL_OPENGL_LOADER_CUSTOM hook older releases honoured is
    # gone (the macro is still tested but nothing includes a custom header any
    # more). Pointing it at GLEW therefore SILENTLY skips the include entirely
    # and every GL type is undefined. This file must be vendored with the rest.
    'backends/imgui_impl_opengl3_loader.h'
)
# Deduplicate: imgui.cpp appears above because it is both a source and a file.
$AllFiles = ($CoreFiles + $BackendFiles) | Sort-Object -Unique

if ((Test-Path (Join-Path $Dest 'imgui.cpp')) -and -not $Force) {
    Write-Host "vendor/imgui already present. Use -Force to replace it."
    exit 0
}

if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }

$Tarball = Join-Path ([System.IO.Path]::GetTempPath()) "imgui-$Version.tar.gz"
$Url = "https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/$Version"
Write-Host ">>> downloading $Version"
Invoke-WebRequest -Uri $Url -OutFile $Tarball -UseBasicParsing

# MSYS2 GNU tar. The Windows bsdtar on PATH parses C:\... as a remote host and
# fails with "Cannot connect to C" - the same trap BUILDING.md records.
$Tar = 'C:\msys64\usr\bin\tar.exe'
if (-not (Test-Path $Tar)) {
    $found = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $found) { throw "tar not found (looked for MSYS2 GNU tar and PATH)" }
    $Tar = $found.Source
}

$Stage = Join-Path ([System.IO.Path]::GetTempPath()) "imgui-stage-$PID"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

Write-Host ">>> extracting"
$Prefix = "imgui-$($Version.TrimStart('v'))"
# GNU tar shells out to gzip, which lives in MSYS2's /usr/bin and is not
# necessarily on PATH (the build scripts deliberately replace PATH entirely).
$MsysUsrBin = 'C:\msys64\usr\bin'
$HadMsys = $env:PATH -like "*$MsysUsrBin*"
if (-not $HadMsys -and (Test-Path (Join-Path $MsysUsrBin 'gzip.exe'))) {
    $env:PATH = "$MsysUsrBin;$env:PATH"
}
try {
    # MSYS tar parses a Windows "C:\..." argument as a remote "host:path" and
    # fails with "Cannot connect to C" (BUILDING.md records the same trap). Copy
    # the archive into the staging directory and name it relatively, so no
    # argument handed to tar contains a colon at all.
    $TarballName = Split-Path -Leaf $Tarball
    Copy-Item $Tarball (Join-Path $Stage $TarballName) -Force
    Push-Location $Stage
    try {
        & $Tar -xzf $TarballName
        if ($LASTEXITCODE -ne 0) { throw "tar failed: $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
} finally {
    if (-not $HadMsys -and $env:PATH -like "*$MsysUsrBin*") {
        $env:PATH = ($env:PATH -replace [regex]::Escape("$MsysUsrBin;"), '')
    }
}

$Src = Join-Path $Stage $Prefix
if (-not (Test-Path $Src)) {
    # The tag and the archive prefix do not always agree; find whatever came out.
    $Src = (Get-ChildItem -Directory $Stage | Select-Object -First 1).FullName
}
if (-not $Src -or -not (Test-Path (Join-Path $Src 'imgui.cpp'))) {
    throw "extracted archive has no imgui.cpp at $Src"
}

Write-Host ">>> installing into vendor/imgui"
New-Item -ItemType Directory -Force -Path (Join-Path $Dest 'backends') | Out-Null
foreach ($f in $AllFiles) {
    $from = Join-Path $Src ($f -replace '/', '\')
    if (-not (Test-Path $from)) { throw "upstream is missing $f" }
    $to = Join-Path $Dest ($f -replace '/', '\')
    Copy-Item $from $to -Force
}

# Provenance, so the tracked copy says where it came from and why the version
# is pinned. The one behavioural patch we carry is documented here too.
$Commit = (& git ls-remote "https://github.com/ocornut/imgui.git" "refs/tags/$Version" 2>$null) -split '\s+' | Select-Object -First 1
@"
Dear ImGui $Version
Source: https://github.com/ocornut/imgui
Commit: $Commit
Fetched by: scripts/tools/fetch-imgui.ps1

Vendored deliberately, exactly like vendor/httplib.h and vendor/json.hpp, so a
clone builds with no network access. The version is pinned: ImGui's API does
change between releases and an unpinned copy would make the build unrepeatable.

Only the five core sources and the GLFW + OpenGL3 backends are taken, including
the backend's bundled gl3w-derived loader (imgui_impl_opengl3_loader.h), which
Dear ImGui 1.92 always uses.

On GL loaders: the backend's loader is a private, file-local set of function
pointers and defines, used only inside imgui_impl_opengl3.cpp - it does not
collide with GLEW, which is what every other GL consumer in this tree uses
(RenderDevice, MPVSurface). One loader is therefore still the rule for our own
code; the backend simply carries its own, as upstream intends.

Older ImGui releases honoured IMGUI_IMPL_OPENGL_LOADER_CUSTOM, which made it
tempting to compile the backend directly against GLEW. Dear ImGui 1.92 dropped
that hook: the macro is still tested but nothing includes a custom header any
more, so defining it makes every GL type undefined. Do not reintroduce it.

To refresh:   pwsh -File scripts/tools/fetch-imgui.ps1 -Version vX.Y.Z -Force
"@ | Set-Content (Join-Path $Dest 'IMGUI_VERSION.txt') -Encoding ascii

Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
Remove-Item -Force $Tarball -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "installed $($AllFiles.Count) file(s) into vendor/imgui:"
Get-ChildItem -Recurse -File $Dest | ForEach-Object {
    "  {0,8}  {1}" -f $_.Length, $_.FullName.Replace("$Repo\", '')
}
