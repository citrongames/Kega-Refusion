# Contra: Hard Corps Combat Lab

[English](#run-the-tool)

## Запуск

Загрузите оригинальный картридж **Contra: Hard Corps (USA, Korea)** и запустите `games/contra-hard-corps-lab.lua` через **Файл → Скрипты…**. Скрипт сверяет размер, серийный номер и сигнатуры инструкций ROM.

- **Игроки** показывает зоны получения урона. Серый прямоугольник означает, что игровой флаг защищает персонажа от попаданий. В инспекторе слота виден таймер неуязвимости.
- **Зоны боя** показывает прямоугольники, проверенные общей процедурой 68000 за отображаемый кадр. Красный обозначает атаку, жёлтый — цель для снарядов игроков, розовый — обе проверки. Синим выделены снаряды из текущего списка коллизий. **Твёрдые объекты** показывает зарегистрированные поверхности, а **Опорные точки** отмечает позиции активных объектов. У отдельных боссов могут быть собственные проверки. При достижении лимита вызовов эмулятора панель помечает выборку как неполную.
- Выберите группу и слот, чтобы посмотреть дробные координаты, собственные скорости до поправки на скроллинг и состояние объекта. Для проверенных типов доступны здоровье, пауза между попаданиями или остаток до порога смены фазы.

HUD показывает числовое HP активных объектов, включая контроллеры составных боссов вне видимого кадра. Общее здоровье нескольких частей выводится один раз. «Ост.» означает урон, оставшийся до перехода в следующую фазу; «удары» — отдельный счётчик срабатываний механизма. Одновременно показано до восьми строк: выбранный слот и объекты с наибольшим запасом HP; остальные доступны в инспекторе. График урона удалён.

В локальной игре нажмите **Запомнить точку**, чтобы сохранить текущий кадр в памяти, и **Повторить**, чтобы его восстановить. Режим записи и настройки эмуляции могут ограничивать временные сохранения или сбрасывать сохранённую точку. Закрытие панели освобождает точку тренировки. Выбранные слои оверлея продолжают отображаться до остановки скрипта.

## Run the tool

Load the original **Contra: Hard Corps (USA, Korea)** cartridge and run `games/contra-hard-corps-lab.lua` through **File → Scripts…**. The script checks the cartridge size, product code and instruction signatures.

- **Players** draws player hurtboxes. Gray means the game's collision flag currently protects that player. The slot inspector shows the invulnerability timer.
- **Combat zones** draws rectangles passed to the shared 68000 collision test during the displayed frame. Red is an attack, gold is a target checked against player shots, and pink means both. Blue shows the player shots in the active collision list. **Solid objects** draws registered surfaces, and **Object origins** marks live actor positions. Some bosses run additional collision checks. The panel marks partial samples when the emulator reaches its callback limit.
- Choose a pool and slot to see fractional coordinates, the object's own velocity before camera corrections, and its state. Verified objects can also show health, time between hits or the remaining damage before a phase transition.

The HUD lists numeric health, including multipart controllers outside the visible picture. Parts sharing one health field produce one row. “Left” means damage remaining before the next phase; “hits” identifies a separate mechanism-strike counter. Up to eight rows are shown: the selected slot followed by the largest remaining HP values; other slots remain available in the inspector. The damage graph has been removed.

In local play, select **Save practice point** to hold the current state in memory and **Retry** to restore it. Recording and emulation settings can restrict temporary saves or invalidate an existing point. Closing the panel releases the practice point. The selected overlay layers stay visible until the script stops.

[Health verification / Проверка HP](contra-hard-corps-health.md)
