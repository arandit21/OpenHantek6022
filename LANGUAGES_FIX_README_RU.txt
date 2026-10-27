OpenHantek Laboratory — multilingual source fix — 2026-10-27

ЭТО ПАТЧ ИСХОДНИКОВ, НЕ ГОТОВЫЙ EXE.
Проверенная база: OpenHantek6022-source.zip (с анализатором 2x2 из каталога Codex 2026-10-22).

Что исправлено:
- Выбранный язык сохраняется в QSettings/view/language, включая сброс старой конфигурации.
- Язык EN загружает собственный словарь, а не оставляет русские надписи.
- Мгновенная смена текста всех основных элементов MainWindow, MediaLibraryWidget, AnalysisWorkspace/Panel,
  графиков, стандартных док-панелей без уничтожения данных и пересоздания графиков.
- Окно не сбрасывает заголовок в MainWindow.
- Все 10 языков включены в CMake embedded Qt resources. .qm собираются автоматически lrelease при сборке.

Применение к РЕАЛЬНЫМ исходникам на Windows:
  $src = "C:\Users\Artyom\Documents\Codex\2026-10-22\files-pasted-by-the-user-openhantek\work\OpenHantek6022-clean"
  Expand-Archive -LiteralPath "$env:USERPROFILE\Downloads\OpenHantek_LiveLanguage_Overlay_2026-10-27.zip" -DestinationPath $src -Force

Перед копированием сделайте резервную копию каталога исходников. В обычную папку с OpenHantek-Lab.exe
этот пакет НЕ копировать: EXE от копирования исходников не изменится.

Для сборки требуется Qt 6 LinguistTools, Qt6 Widgets/OpenGL и совместимая среда CMake/компилятор.
Если каталог build-clean уже настроен для ЭТОГО компьютера, команда:
  cmake --build "$src\build-clean" --config Release --parallel 8
При несовпадении пути, toolchain или отсутствии Qt6 эта команда не сработает; необходима корректная локальная
Windows-конфигурация CMake. Нельзя заменять рабочую программу до реального запуска и проверки новой сборки.

Внутренние проверки: XML TS каталоги, наличие переводов 10 языков, точность %1/%2/%3,
отсутствие изменений в обработке, захвате, записи и драйверах.
Исполнение приложения на Windows и компиляция не проверены (в среде сборки отсутствует Qt6 SDK).
