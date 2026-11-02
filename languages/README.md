# Interface translations

The language files are UTF-8 JSON. To add a translation:

1. Copy `en.json` to `<code>.json`, for example `fr.json`.
2. Change `code` to the filename's code and `name` to the name shown in the language menu.
3. Translate the values in `translations`. Keep the Russian keys exactly as they are.
4. Put the file in `data/languages` beside `data/settings.ini`, or in `languages` beside the executable. Restart Kega Refusion and select the language in Settings → Emulator settings → General.

Keep placeholders such as `%1` and `%2`, HTML tags, and `;;` separators in file filters. The `&` in menu labels marks the shortcut letter; move it to a suitable letter in your translation. Missing entries use the English text. If the selected language is unavailable, English is used. With **Use system language**, the application selects the system language when a matching file exists and otherwise uses English. Language changes take effect after a restart. The bundled English catalog is also embedded in the executable as a safety fallback.

## Русский

Для нового перевода скопируйте `en.json` в файл с кодом языка, например `de.json`. Измените поля `code` и `name`, переведите значения в `translations`, сохранив русские ключи без изменений. Файл можно положить в `data/languages` рядом с `data/settings.ini` либо в папку `languages` рядом с программой. После выбора языка в «Настройки → Настройки эмулятора → Общие» перезапустите программу. Сохраняйте `%1`, `%2`, HTML-теги, разделители `;;` и метку горячей клавиши `&`. Если строка отсутствует, используется английский перевод.
