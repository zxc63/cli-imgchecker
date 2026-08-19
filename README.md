# imgcheck
### Сделано чисто под Telegraph, метаданные изображение.

Разбор метаданных изображения + валидация. C++17, ноль зависимостей, один файл.

## Сборка

```bash
g++ -std=c++17 -O2 -Wall -Wextra -o imgcheck imgcheck.cpp
```


## Что умеет

| Формат | Размеры | EXIF | Анимация |
|--------|---------|------|----------|
| JPEG   | ✅ (SOFn) | ✅ APP1 | — |
| PNG    | ✅ (IHDR) | ✅ чанк `eXIf` | ✅ APNG (`acTL`) |
| WebP   | ✅ (VP8/VP8L/VP8X) | ✅ чанк `EXIF` | ✅ (`ANIM`) |
| GIF    | ✅ | — | ✅ (NETSCAPE2.0) |
| BMP    | ✅ | — | — |
| HEIC/AVIF | только детект формата | — | — |

EXIF-парсер полноценный: TIFF-заголовок в обоих порядках байтов (`II`/`MM`),
IFD0 + ExifIFD + GPS IFD, типы BYTE/ASCII/SHORT/LONG/RATIONAL, inline-значения
и значения по смещению.

Проверки:

- размер файла, минимальные/максимальные размеры, лимит по пикселям (decompression bomb)
- соотношение сторон
- белый список форматов **по сигнатуре**, а не по расширению или `Content-Type`
- анимация
- GPS-координаты в EXIF (по умолчанию — ошибка)
- обрезанные файлы (нет `FFD9` у JPEG / `IEND` у PNG)
- `Orientation` — предупреждение, что при ресайзе картинку надо повернуть

## Использование

```bash
./imgcheck avatar.jpg
./imgcheck banner.png --max-bytes 2097152 --max-size 2000x2000 --formats jpeg,webp
./imgcheck sticker.gif --allow-animated --formats gif,webp
```

Флаги:

```
--max-bytes N     максимальный размер файла (по умолчанию 8388608)
--min-size WxH    минимальные размеры (64x64)
--max-size WxH    максимальные размеры (8000x8000)
--max-pixels N    лимит на W*H (40000000)
--max-aspect F    максимальное соотношение сторон (4.0)
--formats a,b,c   разрешённые форматы (jpeg,png,webp)
--allow-animated  разрешить GIF/APNG/анимированный WebP
--allow-gps       не считать GPS в EXIF ошибкой
```

Коды выхода: `0` — прошёл, `1` — не прошёл валидацию, `2` — ошибка чтения/аргументов.

## Пример вывода

```json
{
  "ok": false,
  "file": "photo.jpg",
  "format": "jpeg",
  "bytes": 8627,
  "width": 800,
  "height": 600,
  "animated": false,
  "orientation": 6,
  "exifPresent": true,
  "gpsPresent": true,
  "exif": {
    "Make": "Canon",
    "Model": "EOS 5D Mark IV",
    "DateTimeOriginal": "2026:08:19 10:11:12",
    "GPSLatitude": "49, 36, 0"
  },
  "errors": ["в EXIF есть GPS-координаты — файл нужно очистить"],
  "warnings": ["orientation=6 — при ресайзе картинку нужно повернуть"]
}
```

## Интеграция в Node/Express

```js
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const run = promisify(execFile);

export async function checkImage(path, opts = []) {
  try {
    const { stdout } = await run("./bin/imgcheck", [path, ...opts], {
      timeout: 5000,
      maxBuffer: 1 << 20,
    });
    return JSON.parse(stdout);           // exit 0 → всё ок
  } catch (err) {
    if (err.code === 1 && err.stdout) {  // не прошёл валидацию
      return JSON.parse(err.stdout);
    }
    throw new Error(`imgcheck упал: ${err.stderr || err.message}`);
  }
}
```

Дальше в роуте аплоада: если `!result.ok` — отдаёте `400` с `result.errors`,
если `result.exifPresent` — прогоняете файл через ресайзер, который метаданные
не переносит (sharp с `.rotate()` вырежет EXIF и заодно применит `Orientation`).

## Тесты

```bash
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -o imgcheck_asan imgcheck.cpp
```

Прогнан фаззинг на 600 мутированных/обрезанных файлах под ASan+UBSan — падений
и UB нет. Все чтения идут через bounds-checked `ByteView`, рекурсия по IFD
ограничена глубиной 3, счётчики записей и элементов — потолками.
