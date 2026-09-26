# FarQoiViewer

[English version](README.md)

Плагин Far Manager 3.x для просмотра изображений `.qoi` в отдельном окне Windows.

Поток QOI декодируется напрямую. Кодеки PNG, WIC и GDI+ не используются.

## Возможности

* `F3` на файле `.qoi` открывает просмотрщик
* Остальные файлы обрабатывает Far Manager как обычно
* Поддержка RGB и RGBA QOI
* Сохранение пропорций
* Режимы «по размеру окна» и 1:1
* Масштаб и панорамирование
* RGBA на фоне шашечки
* Отдельное окно со своим циклом сообщений

## Управление

| Клавиша | Действие |
|---------|----------|
| `0` | Вписать в окно |
| `1` | 100% / 1:1 |
| `+` / `-` | Увеличить / уменьшить |
| Колёсико мыши | Масштаб |
| Стрелки | Сдвиг |
| `Home` | Центрировать |
| `Space` / ЛКМ | Переключение fit ↔ zoom |
| `Esc` | Закрыть |

Командная строка Far (префикс):

```
qoi C:\path\to\image.qoi
qoi "C:\path with spaces\image.qoi"
```

## Сборка

Плагин берёт `far/plugin.hpp` из исходников Far Manager (копия в репозитории не поставляется).

```
git clone --depth 1 https://github.com/FarGroup/FarManager.git
```

### MSVC

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DFAR_SOURCE_DIR=C:/src/FarManager

cmake --build build --config Release
```

DLL: `build/Release/FarQoiViewer.dll`

### MinGW (MSYS2 UCRT64)

```
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAR_SOURCE_DIR=/c/src/FarManager

cmake --build build -j
```

DLL: `build/FarQoiViewer.dll`

## Установка

Скопировать DLL в:

```
%FARHOME%\Plugins\FarQoiViewer\FarQoiViewer.dll
```

Перезапустить Far Manager.

## Замечания

* `ProcessConsoleInputW` перехватывает `F3` только для файлов `.qoi`
* Это не archive/virtual-panel плагин — только просмотр текущего файла
* Формат QOI: [phoboslab/qoi](https://github.com/phoboslab/qoi)
