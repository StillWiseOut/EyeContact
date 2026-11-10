# EyeContact - удаление плагина. Запускается через Uninstall.bat.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`""
    exit 0
}

while (Get-Process "Adobe Premiere Pro", "AfterFX" -ErrorAction SilentlyContinue) {
    Read-Host "Закройте Premiere Pro и After Effects, затем нажмите Enter" | Out-Null
}
$aex = Join-Path $env:ProgramFiles "Adobe\Common\Plug-ins\7.0\MediaCore\EyeContact.aex"
if (Test-Path $aex) {
    Remove-Item $aex -Force
    Write-Host "Плагин EyeContact удалён." -ForegroundColor Green
} else {
    Write-Host "Плагин EyeContact не найден - удалять нечего."
}
Write-Host "NVIDIA AR SDK runtime не тронут: его можно удалить через «Установленные приложения» Windows."
Write-Host ""
Read-Host "Нажмите Enter, чтобы закрыть окно" | Out-Null
