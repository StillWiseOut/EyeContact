# EyeContact - установка плагина для Premiere Pro / After Effects.
# Запускается через Install.bat.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Pause-Exit([int]$code) {
    Write-Host ""
    Read-Host "Нажмите Enter, чтобы закрыть окно" | Out-Null
    exit $code
}

# --- права администратора (нужны для папки плагинов Adobe и установщика NVIDIA)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`""
    exit 0
}

Write-Host "=== EyeContact: установка ===" -ForegroundColor Cyan
Write-Host ""

# --- 1. видеокарта
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $smi) {
    Write-Host "Не найдена видеокарта NVIDIA (нет nvidia-smi). EyeContact работает только на NVIDIA RTX." -ForegroundColor Red
    Pause-Exit 1
}
$smiOut = @(& nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader)
$gpu = "$($smiOut[0])".Split(",") | ForEach-Object { $_.Trim() }
$gpuName = $gpu[0]; $cc = $gpu[1]
$arch = switch -Regex ($cc) {
    "^7\.5$"        { "turing" }
    "^8\.[67]$"     { "ampere" }
    "^8\.9$"        { "ada" }
    "^1[02]\.\d$"   { "blackwell" }
    default         { $null }
}
Write-Host "Видеокарта: $gpuName (compute $cc)"
if (-not $arch) {
    Write-Host "Эта видеокарта не поддерживается NVIDIA Maxine (нужна RTX 20/30/40/50)." -ForegroundColor Red
    Pause-Exit 1
}

# --- 2. runtime NVIDIA Maxine AR
$runtime = Join-Path $env:ProgramFiles "NVIDIA Corporation\NVIDIA AR SDK"
$hasRuntime = (Test-Path "$runtime\nvARPose.dll") -and (Get-ChildItem "$runtime\models" -Filter "gazeredir_*" -ErrorAction SilentlyContinue)
if ($hasRuntime) {
    Write-Host "NVIDIA AR SDK runtime: установлен" -ForegroundColor Green
} else {
    $url = "https://international.download.nvidia.com/Windows/broadcast/sdk/AR/nvidia_ar_sdk_installer_v0.8.7_$arch.exe"
    Write-Host ""
    Write-Host "Нужен NVIDIA AR SDK runtime v0.8.7 для вашей видеокарты ($arch), около 1,3 ГБ:" -ForegroundColor Yellow
    Write-Host "  $url"
    Write-Host "Это бесплатный установщик NVIDIA для конечных пользователей (страница www.nvidia.com/broadcast-sdk-resources)."
    Write-Host "Windows может предупредить о неизвестном издателе: NVIDIA подписала этот пакет внутренним сертификатом."
    $ans = Read-Host "Скачать и запустить установщик NVIDIA сейчас? (y/n)"
    if ($ans -notmatch "^[yYдД]") {
        Write-Host "Установите runtime по ссылке выше и запустите Install.bat снова." -ForegroundColor Yellow
        Pause-Exit 1
    }
    $exe = Join-Path $env:TEMP "nvidia_ar_sdk_installer_v0.8.7_$arch.exe"
    Write-Host "Скачиваю... (это займёт несколько минут)"
    $ProgressPreference = "SilentlyContinue"
    Invoke-WebRequest $url -OutFile $exe
    Write-Host "Запускаю установщик NVIDIA. Пройдите шаги мастера, путь по умолчанию не меняйте."
    Start-Process $exe -Wait
    Remove-Item $exe -ErrorAction SilentlyContinue
    if (-not (Test-Path "$runtime\nvARPose.dll")) {
        Write-Host "Runtime так и не появился в $runtime. Установка прервана." -ForegroundColor Red
        Pause-Exit 1
    }
    Write-Host "NVIDIA AR SDK runtime: установлен" -ForegroundColor Green
}

# --- 3. плагин
while (Get-Process "Adobe Premiere Pro", "AfterFX" -ErrorAction SilentlyContinue) {
    Read-Host "Закройте Premiere Pro и After Effects, затем нажмите Enter" | Out-Null
}
$dest = Join-Path $env:ProgramFiles "Adobe\Common\Plug-ins\7.0\MediaCore"
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $here "EyeContact.aex") $dest -Force
Write-Host "Плагин скопирован в $dest" -ForegroundColor Green

Write-Host ""
Write-Host "Готово. В Premiere: Effects -> Video Effects -> Maxine -> EyeContact" -ForegroundColor Cyan
Pause-Exit 0
