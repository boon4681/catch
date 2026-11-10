# Run from any directory. Builds native code and the upload regression harness.
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "build.ps1")
$out = Join-Path $PSScriptRoot "build/upload_test.exe"
& clang -O2 -std=c11 --target=x86_64-pc-windows-msvc -fms-runtime-lib=static `
    -DMG_ENABLE_LOG=1 -DMG_IO_SIZE=65536 '-DMG_MAX_RECV_SIZE=(16UL*1024UL*1024UL)' `
    -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -Wno-deprecated-declarations `
    (Join-Path $PSScriptRoot "tests/upload_test.c") `
    (Join-Path $PSScriptRoot "build/mongoose.obj") -o $out -lws2_32 -liphlpapi
if ($LASTEXITCODE -ne 0) { throw "clang failed on upload_test.c" }
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & $out
    if ($LASTEXITCODE -ne 0) { throw "Upload regression tests failed" }
    & odin test src/backend -out:native/build/backend_test.exe -define:ODIN_TEST_THREADS=1
    if ($LASTEXITCODE -ne 0) { throw "Backend regression tests failed" }
} finally {
    Pop-Location
}
