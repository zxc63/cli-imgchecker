// imgcheck — разбор метаданных изображения + валидация. C++17, без зависимостей.
//
// Сборка:  g++ -std=c++17 -O2 -Wall -Wextra -o imgcheck imgcheck.cpp
// Запуск:  ./imgcheck photo.jpg [флаги]
//
// Вывод: JSON в stdout.
// Коды выхода: 0 — файл прошёл валидацию, 1 — не прошёл, 2 — ошибка чтения/парсинга.
//
// ВАЖНО: парсер работает с недоверенными данными (аватарки от юзеров),
// поэтому каждое чтение проходит через bounds-check. Никаких сырых указателей наружу.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Безопасное чтение байтов
// ---------------------------------------------------------------------------

struct ByteView {
    const uint8_t* p = nullptr;
    size_t n = 0;

    bool has(size_t off, size_t len) const {
        return off <= n && len <= n - off;
    }
    uint8_t u8(size_t off) const { return has(off, 1) ? p[off] : 0; }

    uint16_t u16(size_t off, bool le) const {
        if (!has(off, 2)) return 0;
        return le ? uint16_t(p[off] | (p[off + 1] << 8))
                  : uint16_t((p[off] << 8) | p[off + 1]);
    }
    uint32_t u32(size_t off, bool le) const {
        if (!has(off, 4)) return 0;
        if (le)
            return uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8) |
                   (uint32_t(p[off + 2]) << 16) | (uint32_t(p[off + 3]) << 24);
        return (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
               (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
    }
    ByteView slice(size_t off, size_t len) const {
        if (!has(off, len)) return ByteView{nullptr, 0};
        return ByteView{p + off, len};
    }
    bool match(size_t off, const char* magic, size_t len) const {
        return has(off, len) && std::memcmp(p + off, magic, len) == 0;
    }
};

// ---------------------------------------------------------------------------
// Результат разбора
// ---------------------------------------------------------------------------

struct ImageInfo {
    std::string format = "unknown";
    uint64_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool animated = false;              // GIF/WebP с анимацией
    bool exif_present = false;
    bool gps_present = false;
    int orientation = 0;                // 1..8, 0 — не указано
    std::vector<std::pair<std::string, std::string>> exif;  // порядок сохраняем
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    void put(const std::string& k, const std::string& v) {
        if (v.empty()) return;
        for (auto& kv : exif)
            if (kv.first == k) return;  // первый выигрывает
        exif.emplace_back(k, v);
    }
};

// ---------------------------------------------------------------------------
// EXIF / TIFF
// ---------------------------------------------------------------------------

namespace exifns {

enum IfdKind { IFD0, IFD_EXIF, IFD_GPS };

const char* tagName(IfdKind kind, uint16_t tag) {
    if (kind == IFD_GPS) {
        switch (tag) {
            case 0x0001: return "GPSLatitudeRef";
            case 0x0002: return "GPSLatitude";
            case 0x0003: return "GPSLongitudeRef";
            case 0x0004: return "GPSLongitude";
            case 0x0006: return "GPSAltitude";
            case 0x001D: return "GPSDateStamp";
            default: return nullptr;
        }
    }
    if (kind == IFD_EXIF) {
        switch (tag) {
            case 0x829A: return "ExposureTime";
            case 0x829D: return "FNumber";
            case 0x8827: return "ISO";
            case 0x9003: return "DateTimeOriginal";
            case 0x9004: return "DateTimeDigitized";
            case 0x920A: return "FocalLength";
            case 0x9209: return "Flash";
            case 0xA002: return "PixelXDimension";
            case 0xA003: return "PixelYDimension";
            case 0xA430: return "OwnerName";
            case 0xA433: return "LensMake";
            case 0xA434: return "LensModel";
            default: return nullptr;
        }
    }
    switch (tag) {
        case 0x010E: return "ImageDescription";
        case 0x010F: return "Make";
        case 0x0110: return "Model";
        case 0x0112: return "Orientation";
        case 0x011A: return "XResolution";
        case 0x011B: return "YResolution";
        case 0x0131: return "Software";
        case 0x0132: return "DateTime";
        case 0x013B: return "Artist";
        case 0x8298: return "Copyright";
        default: return nullptr;
    }
}

size_t typeSize(uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;   // BYTE/ASCII/SBYTE/UNDEFINED
        case 3: case 8: return 2;                   // SHORT/SSHORT
        case 4: case 9: case 11: return 4;          // LONG/SLONG/FLOAT
        case 5: case 10: case 12: return 8;         // RATIONAL/SRATIONAL/DOUBLE
        default: return 0;
    }
}

std::string fmtRational(uint32_t num, uint32_t den) {
    if (den == 0) return "0";
    std::ostringstream os;
    if (num % den == 0) {
        os << (num / den);
    } else {
        os.precision(6);
        os << (double(num) / double(den));
    }
    return os.str();
}

// Читает значение тега в строку. tiff — вся TIFF-область начиная с "II"/"MM".
std::string readValue(const ByteView& tiff, size_t entryOff, bool le,
                      uint16_t type, uint32_t count) {
    const size_t tsz = typeSize(type);
    if (tsz == 0 || count == 0) return "";
    if (count > 4096) count = 4096;  // защита от мусорных счётчиков

    const size_t total = tsz * size_t(count);
    size_t dataOff = entryOff + 8;
    if (total > 4) {
        dataOff = tiff.u32(entryOff + 8, le);
        if (!tiff.has(dataOff, total)) return "";
    } else if (!tiff.has(dataOff, total)) {
        return "";
    }

    if (type == 2 || type == 7) {  // ASCII / UNDEFINED
        std::string s;
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t c = tiff.u8(dataOff + i);
            if (c == 0) break;
            s += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    std::ostringstream os;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) os << ", ";
        const size_t off = dataOff + i * tsz;
        switch (type) {
            case 1: case 6: os << int(tiff.u8(off)); break;
            case 3: case 8: os << tiff.u16(off, le); break;
            case 4: case 9: os << tiff.u32(off, le); break;
            case 5: case 10:
                os << fmtRational(tiff.u32(off, le), tiff.u32(off + 4, le));
                break;
            default: return "";
        }
        if (i >= 15) { os << ", ..."; break; }
    }
    return os.str();
}

void parseIfd(const ByteView& tiff, size_t ifdOff, bool le, IfdKind kind,
              ImageInfo& out, int depth) {
    if (depth > 3) return;
    if (!tiff.has(ifdOff, 2)) return;

    const uint16_t count = tiff.u16(ifdOff, le);
    if (count == 0 || count > 512) return;  // адекватный IFD столько не содержит

    for (uint16_t i = 0; i < count; ++i) {
        const size_t e = ifdOff + 2 + size_t(i) * 12;
        if (!tiff.has(e, 12)) return;

        const uint16_t tag = tiff.u16(e, le);
        const uint16_t type = tiff.u16(e + 2, le);
        const uint32_t cnt = tiff.u32(e + 4, le);

        // Указатели на вложенные IFD
        if (kind == IFD0 && tag == 0x8769) {
            parseIfd(tiff, tiff.u32(e + 8, le), le, IFD_EXIF, out, depth + 1);
            continue;
        }
        if (kind == IFD0 && tag == 0x8825) {
            out.gps_present = true;
            parseIfd(tiff, tiff.u32(e + 8, le), le, IFD_GPS, out, depth + 1);
            continue;
        }

        const char* name = tagName(kind, tag);
        if (!name) continue;

        const std::string val = readValue(tiff, e, le, type, cnt);
        if (val.empty()) continue;

        if (kind == IFD0 && tag == 0x0112) {
            out.orientation = std::atoi(val.c_str());
            if (out.orientation < 1 || out.orientation > 8) out.orientation = 0;
        }
        out.put(name, val);
    }
}

// Вход — блок, начинающийся с TIFF-заголовка ("II*\0" или "MM\0*").
void parseTiff(const ByteView& tiff, ImageInfo& out) {
    if (tiff.n < 8) return;
    bool le;
    if (tiff.match(0, "II", 2)) le = true;
    else if (tiff.match(0, "MM", 2)) le = false;
    else return;

    if (tiff.u16(2, le) != 42) return;
    const uint32_t ifd0 = tiff.u32(4, le);
    if (ifd0 < 8 || !tiff.has(ifd0, 2)) return;

    out.exif_present = true;
    parseIfd(tiff, ifd0, le, IFD0, out, 0);
}

}  // namespace exifns

// ---------------------------------------------------------------------------
// Форматы
// ---------------------------------------------------------------------------

void parseJpeg(const ByteView& b, ImageInfo& out) {
    out.format = "jpeg";
    size_t pos = 2;
    while (b.has(pos, 4)) {
        if (b.u8(pos) != 0xFF) {  // рассинхрон — пробуем найти следующий маркер
            size_t scan = pos;
            while (b.has(scan, 1) && b.u8(scan) != 0xFF) ++scan;
            if (!b.has(scan, 2)) break;
            pos = scan;
            continue;
        }
        size_t mp = pos + 1;
        while (b.has(mp, 1) && b.u8(mp) == 0xFF) ++mp;  // filler-байты
        if (!b.has(mp, 1)) break;

        const uint8_t marker = b.u8(mp);
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos = mp + 1;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) break;  // конец / начало скана

        if (!b.has(mp + 1, 2)) break;
        const uint16_t seglen = b.u16(mp + 1, false);
        if (seglen < 2) break;
        const size_t dataOff = mp + 3;
        const size_t dataLen = size_t(seglen) - 2;
        if (!b.has(dataOff, dataLen)) break;

        // SOFn (кроме DHT/JPG/DAC) — размеры кадра
        const bool isSof = (marker >= 0xC0 && marker <= 0xCF) &&
                           marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isSof && dataLen >= 5) {
            out.height = b.u16(dataOff + 1, false);
            out.width = b.u16(dataOff + 3, false);
            if (marker == 0xC2) out.put("Progressive", "yes");
        } else if (marker == 0xE1 && dataLen > 6 &&
                   b.match(dataOff, "Exif\0\0", 6)) {
            exifns::parseTiff(b.slice(dataOff + 6, dataLen - 6), out);
        } else if (marker == 0xEE) {
            out.put("AdobeMarker", "yes");
        }

        pos = mp + 1 + seglen;
    }
    if (out.width == 0 || out.height == 0)
        out.errors.push_back("не удалось определить размеры JPEG (битый файл?)");

    // EOI (FFD9) должен быть в хвосте. Небольшой мусор после него допускаем.
    bool eoi = false;
    for (size_t k = b.n >= 64 ? b.n - 64 : 0; k + 2 <= b.n; ++k) {
        if (b.u8(k) == 0xFF && b.u8(k + 1) == 0xD9) { eoi = true; break; }
    }
    if (!eoi) out.errors.push_back("файл обрезан: нет маркера конца JPEG (EOI)");
}

void parsePng(const ByteView& b, ImageInfo& out) {
    out.format = "png";
    size_t pos = 8;
    while (b.has(pos, 8)) {
        const uint32_t len = b.u32(pos, false);
        if (len > (1u << 31)) break;
        const size_t dataOff = pos + 8;
        if (!b.has(dataOff, len)) break;

        if (b.match(pos + 4, "IHDR", 4) && len >= 13) {
            out.width = b.u32(dataOff, false);
            out.height = b.u32(dataOff + 4, false);
            const uint8_t depth = b.u8(dataOff + 8);
            const uint8_t color = b.u8(dataOff + 9);
            out.put("BitDepth", std::to_string(int(depth)));
            out.put("ColorType", std::to_string(int(color)));
            if (b.u8(dataOff + 12) == 1) out.put("Interlaced", "yes");
        } else if (b.match(pos + 4, "eXIf", 4)) {
            exifns::parseTiff(b.slice(dataOff, len), out);
        } else if (b.match(pos + 4, "acTL", 4)) {
            out.animated = true;  // APNG
        } else if (b.match(pos + 4, "IDAT", 4) || b.match(pos + 4, "IEND", 4)) {
            break;  // дальше метаданных обычно нет
        }
        pos = dataOff + len + 4;
    }
    if (out.width == 0 || out.height == 0)
        out.errors.push_back("не удалось прочитать IHDR");

    bool iend = false;
    for (size_t k = b.n >= 64 ? b.n - 64 : 0; k + 4 <= b.n; ++k) {
        if (b.match(k, "IEND", 4)) { iend = true; break; }
    }
    if (!iend) out.errors.push_back("файл обрезан: нет чанка IEND");
}

void parseGif(const ByteView& b, ImageInfo& out) {
    out.format = "gif";
    out.width = b.u16(6, true);
    out.height = b.u16(8, true);
    // Наличие NETSCAPE2.0 → зацикленная анимация; грубо, но для валидации хватает
    for (size_t i = 13; i + 11 < b.n && i < 4096; ++i) {
        if (b.match(i, "NETSCAPE2.0", 11)) { out.animated = true; break; }
    }
}

void parseBmp(const ByteView& b, ImageInfo& out) {
    out.format = "bmp";
    out.width = b.u32(18, true);
    const int32_t h = int32_t(b.u32(22, true));
    out.height = uint32_t(h < 0 ? -h : h);
}

void parseWebp(const ByteView& b, ImageInfo& out) {
    out.format = "webp";
    size_t pos = 12;
    while (b.has(pos, 8)) {
        const uint32_t len = b.u32(pos + 4, true);
        const size_t dataOff = pos + 8;
        if (!b.has(dataOff, len)) break;

        if (b.match(pos, "VP8X", 4) && len >= 10) {
            out.animated = (b.u8(dataOff) & 0x02) != 0;
            out.width = 1 + (uint32_t(b.u8(dataOff + 4)) |
                             (uint32_t(b.u8(dataOff + 5)) << 8) |
                             (uint32_t(b.u8(dataOff + 6)) << 16));
            out.height = 1 + (uint32_t(b.u8(dataOff + 7)) |
                              (uint32_t(b.u8(dataOff + 8)) << 8) |
                              (uint32_t(b.u8(dataOff + 9)) << 16));
        } else if (b.match(pos, "VP8 ", 4) && len >= 10 && out.width == 0) {
            if (b.u8(dataOff + 3) == 0x9D && b.u8(dataOff + 4) == 0x01 &&
                b.u8(dataOff + 5) == 0x2A) {
                out.width = b.u16(dataOff + 6, true) & 0x3FFF;
                out.height = b.u16(dataOff + 8, true) & 0x3FFF;
            }
        } else if (b.match(pos, "VP8L", 4) && len >= 5 && out.width == 0) {
            if (b.u8(dataOff) == 0x2F) {
                const uint32_t bits = b.u32(dataOff + 1, true);
                out.width = (bits & 0x3FFF) + 1;
                out.height = ((bits >> 14) & 0x3FFF) + 1;
            }
        } else if (b.match(pos, "EXIF", 4)) {
            exifns::parseTiff(b.slice(dataOff, len), out);
        } else if (b.match(pos, "ANIM", 4)) {
            out.animated = true;
        }
        pos = dataOff + len + (len & 1);  // чанки выравнены по 2 байта
    }
}

bool detectAndParse(const ByteView& b, ImageInfo& out) {
    if (b.match(0, "\xFF\xD8\xFF", 3)) { parseJpeg(b, out); return true; }
    if (b.match(0, "\x89PNG\r\n\x1A\n", 8)) { parsePng(b, out); return true; }
    if (b.match(0, "GIF87a", 6) || b.match(0, "GIF89a", 6)) { parseGif(b, out); return true; }
    if (b.match(0, "RIFF", 4) && b.match(8, "WEBP", 4)) { parseWebp(b, out); return true; }
    if (b.match(0, "BM", 2) && b.n >= 26) { parseBmp(b, out); return true; }
    if (b.match(4, "ftyp", 4)) {  // HEIC/AVIF — определяем, но не парсим
        out.format = b.match(8, "avif", 4) ? "avif" : "heic";
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Валидация
// ---------------------------------------------------------------------------

struct Limits {
    uint64_t maxBytes = 8ull * 1024 * 1024;
    uint32_t minW = 64, minH = 64;
    uint32_t maxW = 8000, maxH = 8000;
    uint64_t maxPixels = 40ull * 1000 * 1000;  // защита от decompression bomb
    double maxAspect = 4.0;
    bool allowAnimated = false;
    bool rejectGps = true;
    std::set<std::string> formats = {"jpeg", "png", "webp"};
};

void validate(const ImageInfo& in, const Limits& L, ImageInfo& out) {
    if (!L.formats.count(out.format))
        out.errors.push_back("формат '" + out.format + "' не разрешён");

    if (out.bytes > L.maxBytes)
        out.errors.push_back("файл больше лимита: " + std::to_string(out.bytes) +
                             " > " + std::to_string(L.maxBytes) + " байт");

    if (out.width == 0 || out.height == 0) {
        out.errors.push_back("не определены размеры изображения");
    } else {
        if (out.width < L.minW || out.height < L.minH)
            out.errors.push_back("слишком маленькое: " + std::to_string(out.width) +
                                 "x" + std::to_string(out.height));
        if (out.width > L.maxW || out.height > L.maxH)
            out.errors.push_back("слишком большое: " + std::to_string(out.width) +
                                 "x" + std::to_string(out.height));

        const uint64_t px = uint64_t(out.width) * out.height;
        if (px > L.maxPixels)
            out.errors.push_back("слишком много пикселей (" + std::to_string(px) +
                                 ") — возможна decompression bomb");

        const double a = double(std::max(out.width, out.height)) /
                         double(std::min(out.width, out.height));
        if (a > L.maxAspect) {
            std::ostringstream os;
            os.precision(2);
            os << std::fixed << "соотношение сторон " << a << " превышает " << L.maxAspect;
            out.errors.push_back(os.str());
        }
    }

    if (out.animated && !L.allowAnimated)
        out.errors.push_back("анимация не разрешена");

    if (out.gps_present) {
        if (L.rejectGps)
            out.errors.push_back("в EXIF есть GPS-координаты — файл нужно очистить");
        else
            out.warnings.push_back("в EXIF есть GPS-координаты");
    }

    if (out.orientation > 1)
        out.warnings.push_back("orientation=" + std::to_string(out.orientation) +
                               " — при ресайзе картинку нужно повернуть");

    if (out.exif_present && out.exif.size() > 0)
        out.warnings.push_back("EXIF присутствует — перед публикацией стоит вырезать");

    (void)in;
}

// ---------------------------------------------------------------------------
// JSON-вывод
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += char(c);
                }
        }
    }
    return r;
}

void printJson(const std::string& path, const ImageInfo& i, bool valid) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"ok\": " << (valid ? "true" : "false") << ",\n";
    o << "  \"file\": \"" << jsonEscape(path) << "\",\n";
    o << "  \"format\": \"" << jsonEscape(i.format) << "\",\n";
    o << "  \"bytes\": " << i.bytes << ",\n";
    o << "  \"width\": " << i.width << ",\n";
    o << "  \"height\": " << i.height << ",\n";
    o << "  \"animated\": " << (i.animated ? "true" : "false") << ",\n";
    o << "  \"orientation\": " << i.orientation << ",\n";
    o << "  \"exifPresent\": " << (i.exif_present ? "true" : "false") << ",\n";
    o << "  \"gpsPresent\": " << (i.gps_present ? "true" : "false") << ",\n";

    o << "  \"exif\": {";
    for (size_t k = 0; k < i.exif.size(); ++k) {
        o << (k ? ",\n    " : "\n    ");
        o << "\"" << jsonEscape(i.exif[k].first) << "\": \""
          << jsonEscape(i.exif[k].second) << "\"";
    }
    o << (i.exif.empty() ? "}" : "\n  }") << ",\n";

    auto arr = [&](const char* name, const std::vector<std::string>& v, bool last) {
        o << "  \"" << name << "\": [";
        for (size_t k = 0; k < v.size(); ++k) {
            o << (k ? ",\n    " : "\n    ") << "\"" << jsonEscape(v[k]) << "\"";
        }
        o << (v.empty() ? "]" : "\n  ]") << (last ? "\n" : ",\n");
    };
    arr("errors", i.errors, false);
    arr("warnings", i.warnings, true);
    o << "}\n";
    std::cout << o.str();
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

void usage() {
    std::cerr <<
        "imgcheck — разбор метаданных изображения и валидация\n\n"
        "Использование: imgcheck <файл> [флаги]\n\n"
        "  --max-bytes N     максимальный размер файла (по умолчанию 8388608)\n"
        "  --min-size WxH    минимальные размеры (64x64)\n"
        "  --max-size WxH    максимальные размеры (8000x8000)\n"
        "  --max-pixels N    лимит на W*H (40000000)\n"
        "  --max-aspect F    максимальное соотношение сторон (4.0)\n"
        "  --formats a,b,c   разрешённые форматы (jpeg,png,webp)\n"
        "  --allow-animated  разрешить GIF/APNG/анимированный WebP\n"
        "  --allow-gps       не считать GPS в EXIF ошибкой\n\n"
        "Коды выхода: 0 — ок, 1 — не прошёл валидацию, 2 — ошибка.\n";
}

bool parseSize(const std::string& s, uint32_t& w, uint32_t& h) {
    const size_t x = s.find_first_of("xX");
    if (x == std::string::npos) return false;
    w = uint32_t(std::strtoul(s.substr(0, x).c_str(), nullptr, 10));
    h = uint32_t(std::strtoul(s.substr(x + 1).c_str(), nullptr, 10));
    return w > 0 && h > 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }

    std::string path;
    Limits L;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "нет значения для " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--max-bytes") L.maxBytes = std::strtoull(next("--max-bytes").c_str(), nullptr, 10);
        else if (a == "--max-pixels") L.maxPixels = std::strtoull(next("--max-pixels").c_str(), nullptr, 10);
        else if (a == "--max-aspect") L.maxAspect = std::strtod(next("--max-aspect").c_str(), nullptr);
        else if (a == "--min-size") { if (!parseSize(next("--min-size"), L.minW, L.minH)) { usage(); return 2; } }
        else if (a == "--max-size") { if (!parseSize(next("--max-size"), L.maxW, L.maxH)) { usage(); return 2; } }
        else if (a == "--allow-animated") L.allowAnimated = true;
        else if (a == "--allow-gps") L.rejectGps = false;
        else if (a == "--formats") {
            L.formats.clear();
            std::stringstream ss(next("--formats"));
            std::string item;
            while (std::getline(ss, item, ',')) if (!item.empty()) L.formats.insert(item);
        }
        else if (!a.empty() && a[0] == '-') { std::cerr << "неизвестный флаг: " << a << "\n"; return 2; }
        else if (path.empty()) path = a;
        else { std::cerr << "лишний аргумент: " << a << "\n"; return 2; }
    }

    if (path.empty()) { usage(); return 2; }

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "не могу открыть файл: " << path << "\n"; return 2; }

    // Читаем целиком: лимит + запас, чтобы поймать «файл больше лимита»
    const size_t readCap = size_t(L.maxBytes) + 1024;
    std::vector<uint8_t> buf;
    buf.reserve(std::min<size_t>(readCap, 1 << 20));
    {
        char chunk[65536];
        while (f.read(chunk, sizeof chunk) || f.gcount() > 0) {
            buf.insert(buf.end(), chunk, chunk + f.gcount());
            if (buf.size() > readCap) break;
        }
    }
    if (buf.size() < 12) { std::cerr << "файл слишком мал или пуст\n"; return 2; }

    ImageInfo info;
    info.bytes = buf.size();

    ByteView view{buf.data(), buf.size()};
    if (!detectAndParse(view, info)) {
        info.errors.push_back("не удалось распознать формат по сигнатуре");
    }

    validate(info, L, info);
    const bool valid = info.errors.empty();
    printJson(path, info, valid);
    return valid ? 0 : 1;
}
