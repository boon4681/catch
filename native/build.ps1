# Builds native/build/catch_native.lib (Mongoose + LocalSend receiver + Win32 helpers).
# Needs clang and llvm-lib from LLVM on PATH.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$out = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

$defines = @(
    "-DMG_ENABLE_LOG=1",
    "-DMG_IO_SIZE=65536",
    "-DMG_MAX_RECV_SIZE=(16UL*1024UL*1024UL)",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-DUNICODE",
    "-D_UNICODE"
)
$sources = @("mongoose/mongoose.c", "localsend.c", "win_helpers.c")
$objects = @()
foreach ($src in $sources) {
    $obj = Join-Path $out (([IO.Path]::GetFileNameWithoutExtension($src)) + ".obj")
    & clang -c -O2 -std=c11 --target=x86_64-pc-windows-msvc -fms-runtime-lib=static @defines -Wno-deprecated-declarations (Join-Path $here $src) -o $obj
    if ($LASTEXITCODE -ne 0) { throw "clang failed on $src" }
    $objects += $obj
}
& llvm-lib "/OUT:$(Join-Path $out 'catch_native.lib')" @objects
if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed" }
Write-Host "Built $(Join-Path $out 'catch_native.lib')"
