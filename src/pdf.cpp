#include "openbooks/pdf.hpp"

#include <cmath>
#include <cstdint>

namespace ob::pdf {
namespace {

// Advance widths (1/1000 em) for WinAnsi 32..126, from Adobe's core-14 font metrics.
constexpr short kHelvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,  // space .. /
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                                // 0 .. 9
    278, 278, 584, 584, 584, 556, 1015,                                              // : .. @
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833,                 // A .. M
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,                 // N .. Z
    278, 278, 278, 469, 556, 333,                                                    // [ .. `
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833,                 // a .. m
    556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500,                 // n .. z
    334, 260, 334, 584,                                                              // { .. ~
};
constexpr short kHelveticaBold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,  // space .. /
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                                // 0 .. 9
    333, 333, 584, 584, 584, 611, 975,                                               // : .. @
    722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833,                 // A .. M
    722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,                 // N .. Z
    333, 278, 333, 584, 556, 333,                                                    // [ .. `
    556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889,                 // a .. m
    611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500,                 // n .. z
    389, 280, 389, 584,                                                              // { .. ~
};

double charWidth(unsigned char c, Font font) {
    const short* table = font == Font::Bold ? kHelveticaBold : kHelvetica;
    if (c >= 32 && c <= 126) return table[c - 32];
    if (c >= 0xC0) return c < 0xE0 ? (font == Font::Bold ? 722 : 667) : 556;  // accented letters
    return 556;
}

// Code points 0x80..0x9F of Windows-1252 map to these Unicode characters.
constexpr std::uint16_t kCp1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

char toCp1252(std::uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F) return ' ';
    if (cp < 0x80) return static_cast<char>(cp);
    if (cp >= 0xA0 && cp <= 0xFF) return static_cast<char>(cp);
    for (int i = 0; i < 32; ++i) {
        if (kCp1252High[i] != 0 && kCp1252High[i] == cp) return static_cast<char>(0x80 + i);
    }
    return '?';
}

// Locale-independent number formatting with at most 2 decimals.
std::string num(double v) {
    const long long hundredths = std::llround(v * 100.0);
    const bool negative = hundredths < 0;
    const unsigned long long u = negative ? 0ULL - static_cast<unsigned long long>(hundredths)
                                          : static_cast<unsigned long long>(hundredths);
    std::string out = negative ? "-" : "";
    out += std::to_string(u / 100);
    const unsigned long long frac = u % 100;
    if (frac) {
        out += '.';
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10) out += static_cast<char>('0' + frac % 10);
    }
    return out;
}

std::string colorOp(Color c, const char* op) { return num(c.r) + " " + num(c.g) + " " + num(c.b) + " " + op + "\n"; }

const char* fontName(Font f) { return f == Font::Bold ? "/F2" : "/F1"; }

}  // namespace

std::string toWinAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out += '?';
            ++i;
            continue;
        }
        bool valid = i + static_cast<std::size_t>(extra) < s.size();
        for (int k = 1; valid && k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid) {
            out += '?';
            ++i;
            continue;
        }
        out += toCp1252(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

double textWidth(std::string_view utf8, Font font, double size) {
    double units = 0;
    for (char c : toWinAnsi(utf8)) units += charWidth(static_cast<unsigned char>(c), font);
    return units * size / 1000.0;
}

std::vector<std::string> wrapText(std::string_view utf8, Font font, double size, double maxWidth) {
    std::vector<std::string> lines;
    auto flushParagraph = [&](std::string_view para) {
        std::string line;
        std::size_t pos = 0;
        bool any = false;
        while (pos < para.size()) {
            while (pos < para.size() && para[pos] == ' ') ++pos;
            if (pos >= para.size()) break;
            std::size_t end = para.find(' ', pos);
            if (end == std::string_view::npos) end = para.size();
            std::string word(para.substr(pos, end - pos));
            pos = end;
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (textWidth(candidate, font, size) <= maxWidth) {
                line = candidate;
                continue;
            }
            if (!line.empty()) {
                lines.push_back(line);
                any = true;
                line.clear();
            }
            // Break words that are wider than a whole line (by UTF-8 code point).
            while (textWidth(word, font, size) > maxWidth && word.size() > 1) {
                std::size_t cut = 0;
                std::size_t next = 0;
                while (next < word.size()) {
                    std::size_t step = 1;
                    while (next + step < word.size() && (static_cast<unsigned char>(word[next + step]) & 0xC0) == 0x80) ++step;
                    if (textWidth(word.substr(0, next + step), font, size) > maxWidth) break;
                    next += step;
                    cut = next;
                }
                if (cut == 0) cut = 1;
                lines.push_back(word.substr(0, cut));
                any = true;
                word = word.substr(cut);
            }
            line = word;
        }
        if (!line.empty() || !any) lines.push_back(line);
    };
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = utf8.find('\n', start);
        std::string_view para = utf8.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (!para.empty() && para.back() == '\r') para.remove_suffix(1);
        flushParagraph(para);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return lines;
}

std::string escapeString(std::string_view winAnsi) {
    std::string out = "(";
    for (char ch : winAnsi) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '(' || c == ')' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 32 || c > 126) {
            const char oct[] = {'\\', static_cast<char>('0' + ((c >> 6) & 7)), static_cast<char>('0' + ((c >> 3) & 7)),
                                static_cast<char>('0' + (c & 7)), '\0'};
            out += oct;
        } else {
            out += ch;
        }
    }
    out += ')';
    return out;
}

// ------------------------------------------------------------------- page

void Page::text(double x, double y, std::string_view utf8, Font font, double size, Color color, Align align) {
    if (align != Align::Left) {
        const double w = textWidth(utf8, font, size);
        x -= align == Align::Right ? w : w / 2.0;
    }
    ops_ += "BT\n" + colorOp(color, "rg") + fontName(font) + " " + num(size) + " Tf\n" + num(x) + " " + num(y) +
            " Td\n" + escapeString(toWinAnsi(utf8)) + " Tj\nET\n";
}

void Page::rotatedText(double cx, double cy, double degrees, std::string_view utf8, Font font, double size, Color color) {
    const double rad = degrees * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double half = textWidth(utf8, font, size) / 2.0;
    const double drop = size * 0.35;  // roughly center the cap height on the baseline
    const double tx = cx - half * c + drop * s;
    const double ty = cy - half * s - drop * c;
    ops_ += "BT\n" + colorOp(color, "rg") + fontName(font) + " " + num(size) + " Tf\n" + num(c) + " " + num(s) + " " +
            num(-s) + " " + num(c) + " " + num(tx) + " " + num(ty) + " Tm\n" + escapeString(toWinAnsi(utf8)) +
            " Tj\nET\n";
}

void Page::line(double x1, double y1, double x2, double y2, double width, Color color) {
    ops_ += "q\n" + colorOp(color, "RG") + num(width) + " w\n" + num(x1) + " " + num(y1) + " m " + num(x2) + " " +
            num(y2) + " l S\nQ\n";
}

void Page::fillRect(double x, double y, double w, double h, Color color) {
    ops_ += "q\n" + colorOp(color, "rg") + num(x) + " " + num(y) + " " + num(w) + " " + num(h) + " re f\nQ\n";
}

// --------------------------------------------------------------- document

Page& Document::addPage() {
    pages_.emplace_back();
    return pages_.back();
}

std::string Document::build() const {
    // Object numbers: 1 catalog, 2 page tree, 3-4 fonts, 5 info, then (page, contents) pairs.
    const std::size_t pageCount = pages_.empty() ? 1 : pages_.size();
    const std::size_t objectCount = 5 + pageCount * 2;
    std::vector<std::string> objects(objectCount + 1);

    std::string kids;
    for (std::size_t i = 0; i < pageCount; ++i) kids += std::to_string(6 + i * 2) + " 0 R ";
    objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    objects[2] = "<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(pageCount) + " >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    objects[4] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>";
    std::string info = "<< /Producer (OpenBooks)";
    if (!title_.empty()) info += " /Title " + escapeString(toWinAnsi(title_));
    if (!author_.empty()) info += " /Author " + escapeString(toWinAnsi(author_));
    objects[5] = info + " >>";

    static const std::string kEmpty;
    for (std::size_t i = 0; i < pageCount; ++i) {
        const std::size_t pageObj = 6 + i * 2;
        const std::string& content = pages_.empty() ? kEmpty : pages_[i].content();
        objects[pageObj] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(size_.width) + " " + num(size_.height) +
                           "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
                           std::to_string(pageObj + 1) + " 0 R >>";
        objects[pageObj + 1] =
            "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream";
    }

    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets(objectCount + 1, 0);
    for (std::size_t n = 1; n <= objectCount; ++n) {
        offsets[n] = out.size();
        out += std::to_string(n) + " 0 obj\n" + objects[n] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objectCount + 1) + "\n0000000000 65535 f \n";
    for (std::size_t n = 1; n <= objectCount; ++n) {
        std::string offset = std::to_string(offsets[n]);
        out += std::string(10 - offset.size(), '0') + offset + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + std::to_string(objectCount + 1) + " /Root 1 0 R /Info 5 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

}  // namespace ob::pdf
