# EyeContact — коррекция взгляда для Premiere Pro

Видеоэффект для Premiere Pro (и After Effects), который «переводит» взгляд человека в камеру
с помощью NVIDIA Maxine AR SDK (функция Gaze Redirection / Eye Contact).

## Установка

Нужны Windows 10/11, видеокарта NVIDIA RTX (20/30/40/50 серии) и Premiere Pro 2025/2026.

1. Откройте [последний релиз](https://github.com/StillWiseOut/EyeContact/releases/latest) и скачайте `EyeContact_v1.0.zip`.
2. Распакуйте архив в любую папку.
3. Закройте Premiere Pro.
4. Запустите `Install.bat` и разрешите права администратора.
5. Когда установщик предложит скачать NVIDIA AR SDK runtime, ответьте `y` и пройдите мастер установки NVIDIA с настройками по умолчанию.
6. Дождитесь сообщения «Готово» и закройте окно.
7. Откройте Premiere Pro: **Effects → Video Effects → Maxine → EyeContact** — перетащите эффект на клип.

Удаление — `Uninstall.bat` из того же архива.

## Сборка из исходников

Ниже — для разработчиков.

### Что нужно

| Что | Где взять |
|---|---|
| Видеокарта NVIDIA RTX + свежий драйвер | RTX 20/30/40/50 серии |
| Visual Studio 2022/2026 или Build Tools с «Desktop development with C++» | см. ниже |
| After Effects SDK (Windows) | developer.adobe.com/after-effects → Download the SDK (нужен Adobe ID). Подходит 26.5 и для Premiere 26.3 |
| Заголовки Maxine AR SDK (только для сборки) | репозиторий MAXINE-AR-SDK на GitHub (форк официального NVIDIA/MAXINE-AR-SDK, MIT): папки `nvar/include` и `nvar/src` |
| Runtime Maxine AR + модели (для работы) | www.nvidia.com/broadcast-sdk-resources → AR SDK v0.8.7, установщик под вашу архитектуру (RTX 30 = Ampere). Без входа в аккаунт |

Установка Build Tools одной командой:

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended --passive"
```

#### Про Maxine
- **Заголовки** — папка, где лежат `nvAR.h` и `nvARProxy.cpp` / `nvCVImageProxy.cpp`
  (`nvar/include` и `nvar/src`). Нужна только для сборки, в готовый `.aex` ничего из неё копировать не надо.
- **Runtime** — `nvARPose.dll`, `NVCVImage.dll` и модели. Плагин ищет их в
  `C:\Program Files\NVIDIA Corporation\NVIDIA AR SDK` (или в подпапке `bin`).
  Если runtime стоит в другом месте, задайте переменную окружения `MAXINE_AR_RUNTIME_DIR`.
  (`NV_AR_SDK_PATH` — это не путь: прокси NVIDIA понимает в ней только значение `USE_APP_PATH`.)
- Если модели лежат не в стандартном месте, задайте `MAXINE_EYECONTACT_MODEL_DIR`.

### Сборка

```bash
.\build.ps1 -AeSdk "D:\SDK\AfterEffectsSDK" -MaxineSdk "D:\SDK\MAXINE-AR-SDK" -BuildDir "C:\b\eyecontact"
```

`-BuildDir` нужен, если проект лежит по длинному пути: MSVC падает с ошибкой «Invalid argument»,
когда пути сборки длиннее 260 символов. Готовая сборка также лежит в `dist\EyeContact.aex`.

Установка (PowerShell от имени администратора) копирует `EyeContact.aex` в
`C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore`. Эту папку читают и Premiere, и After Effects
(какие папки сканирует ваш Premiere, видно в `%APPDATA%\Adobe\Premiere Pro\26.0\Plugin Loading.log`).

```bash
.\build.ps1 -AeSdk "D:\SDK\AfterEffectsSDK" -MaxineSdk "D:\SDK\MAXINE-AR-SDK" -Install
```

## Параметры эффекта

| Параметр | Что делает |
|---|---|
| Strength | Смешивание с оригиналом, 0–100 % |
| Temporal smoothing | Сглаживание между кадрами. Стабильнее на воспроизведении, но при прыжках по таймлайну первые кадры могут «догонять» |
| Eye size sensitivity | 2–5: размер области глаз, которую перерисовывает модель |
| Natural look-away | Случайно отводит взгляд, чтобы человек не «сверлил» камеру |
| Angle thresholds | В пределах «always redirect» взгляд всегда переводится в камеру, выше «give up above» эффект отключается. Плавный переход между ними |

Изменение любого параметра, кроме Strength, перезагружает модель (это занимает несколько секунд на первом кадре).

## Как это устроено

- `src/EyeContact.cpp` — эффект на AE/Premiere SDK: параметры, форматы пикселей
  (AE: 8/16 бит; Premiere: BGRA 8 бит и 32 бит float), смешивание результата.
  Плагин меняет только пиксели, которые изменила модель (область глаз), а всё остальное
  остаётся с исходной точностью (16 бит / float / HDR).
- `src/MaxineEngine.cpp` — обёртка над `NvAR_Feature_GazeRedirection`: загрузка, CUDA-поток,
  кэш до 3 экземпляров под разные разрешения (полное разрешение, 1/2, 1/4 в Premiere), мьютекс
  (Premiere рендерит кадры в нескольких потоках).
- `src/EyeContactPiPL.r` — ресурс-описание плагина. Сборка компилирует его через PiPLTool из AE SDK.

## Грабли, найденные при отладке в Premiere

1. **Папка моделей.** Установщик runtime прописывает системную переменную `NVAR_MODEL_DIR`; без неё
   `NvAR_Load` падает с -30 («problem deserializing the inference runtime engine»). Плагин теперь сам
   подставляет `<runtime>\models`.
2. **CUDA-контекст.** На потоках рендера Premiere бывает активен его собственный CUDA-контекст, и
   Maxine загружала модель в одном контексте, а запускала в другом (`NvAR_Run` = -1). Каждый вызов
   Maxine теперь идёт в primary context устройства 0 (`CudaContextGuard`, driver API из nvcuda.dll).
3. **Баг Temporal в AR SDK 0.8.7.** После кадра без лица (заглушка «Media pending», перебивка)
   все следующие кадры с лицом падают с -1. Лечится перезагрузкой фичи (~0,1 с) и повтором кадра.
4. **Цвет радужки.** На плоских/log-кадрах модель перекрашивает глаза в насыщенный синий —
   параметр *Keep original color* берёт из результата только яркость.

### Тестовая утилита

`EyeContactTest.exe in.bmp out.bmp [frames] [temporal]` прогоняет движок вне Premiere
(24-битный BMP, например `ffmpeg -i clip.mp4 -frames:v 1 -pix_fmt bgr24 in.bmp`).
Переменные для имитации Premiere: `EYECONTACT_TEST_PRELOAD` (DLL хоста), `EYECONTACT_TEST_FOREIGN_CTX=1`,
`EYECONTACT_TEST_THREADS=1`, `EYECONTACT_TEST_WARMUP=noface.bmp`, `EYECONTACT_TEST_ALTERNATE=noface.bmp`,
`EYECONTACT_TEST_KEEPCOLOR=1`. Отладочные дампы кадров из плагина — создать пустой `%TEMP%\EyeContact.debug`.

### Архив для монтажёров

Папка `package/` (Install.bat, Uninstall.bat, README_RU.txt) + `dist/EyeContact.aex` → `EyeContact_v1.0.zip`.

## Если не работает

1. Смотрите лог: `%TEMP%\EyeContact.log`. Там видно, нашёлся ли runtime, загрузилась ли модель
   и какие параметры SDK не принял.
2. При ошибке эффект не ломает рендер, а просто пропускает кадр без изменений. Сообщение об ошибке
   показывается один раз за сессию.
3. Если After Effects ругается на несовпадение флагов PiPL, сравните значения в
   `EyeContactPiPL.r` и в `GlobalSetup` (`EyeContact.cpp`).

## Известные ограничения

- Проверено: Premiere Pro 2026, RTX 3080, AR SDK runtime 0.8.7, 4K и 1/4 превью.
- Только Windows: Maxine не существует для macOS, а в Mac нет NVIDIA GPU.
- Maxine корректирует только одно лицо в кадре (по умолчанию).
- Несколько экземпляров эффекта на разных клипах одного разрешения делят одну модель, поэтому
  временное сглаживание между ними может смешиваться. Если будет мешать, отключите Temporal smoothing.
- Premiere содержит собственные CUDA/AI-библиотеки. Плагин заранее загружает DLL Maxine из папки
  runtime, чтобы избежать конфликтов, но проблема всё равно возможна. Ищите её в логе.
- Распространять плагин вместе с runtime Maxine можно только в рамках лицензии NVIDIA.

## Лицензия

Код EyeContact распространяется под лицензией [MIT](LICENSE).
NVIDIA Maxine AR SDK и Adobe After Effects SDK в репозиторий не входят и подчиняются собственным лицензиям.
