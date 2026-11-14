param([string]$Version = "0.4.1")
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)
python -m PyInstaller --noconfirm --clean --onedir --windowed --name MoYi --specpath build --workpath build/pyinstaller --add-data "$PWD/static:static" --add-data "$PWD/official:official" --add-data "$PWD/sandbox:sandbox" --add-data "$PWD/bot.py:." --add-data "$PWD/.env.example:." desktop.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller 打包失败" }
$check = Start-Process -FilePath "$PWD/dist/MoYi/MoYi.exe" -ArgumentList @("--self-test", ('"{0}"' -f "$PWD/dist/self-test.json")) -WindowStyle Hidden -Wait -PassThru
if ($check.ExitCode -ne 0) {
    Get-Content -LiteralPath dist/self-test.json
    throw "安装版自检失败"
}
Get-Content -LiteralPath dist/self-test.json
$compiler = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6/ISCC.exe"
if (!(Test-Path -LiteralPath $compiler)) { $compiler = (Get-Command ISCC.exe -ErrorAction Stop).Source }
& $compiler "/DAppVersion=$Version" packaging/MoYi.iss
if ($LASTEXITCODE -ne 0) { throw "安装程序构建失败" }
Compress-Archive -Path dist/MoYi -DestinationPath dist/installer/MoYi-Portable-Windows-x64.zip -Force
Get-ChildItem -LiteralPath dist/installer -File | Where-Object { $_.Extension -in ".exe", ".zip" } | ForEach-Object {
    $digest = Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
    "$($digest.Hash.ToLower())  $($_.Name)"
} | Set-Content -LiteralPath dist/installer/SHA256SUMS.txt -Encoding ascii
