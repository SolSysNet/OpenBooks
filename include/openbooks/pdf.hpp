#pragma once

// A deliberately small PDF 1.4 writer with no dependencies.
//
// Security properties (by construction, not configuration):
//   * Only the standard Helvetica fonts are referenced; nothing is embedded or fetched.
//   * No JavaScript, actions, links/URIs, forms, attachments or external references
//     are ever written. The writer has no API for them.
//   * All text is converted to WinAnsi and written as fully escaped string literals, so
//     user data (names, memos, ...) can never inject PDF operators.
//   * Output is deterministic: no timestamps or random ids.

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ob::pdf {

enum class Font { Regular, Bold };
enum class Align { Left, Center, Right };

struct Color {
    double r = 0;
    double g = 0;
    double b = 0;
};

// Page sizes in points (1/72 inch).
struct PageSize {
    double width;
    double height;
};
constexpr PageSize kLetter{612, 792};
constexpr PageSize kA4{595.28, 841.89};

// UTF-8 -> Windows-1252 (WinAnsiEncoding). Unsupported characters become '?';
// control characters become spaces.
std::string toWinAnsi(std::string_view utf8);

// Width in points of UTF-8 text in the given font and size.
double textWidth(std::string_view utf8, Font font, double size);

// Greedy word wrap to `maxWidth` points; long words are broken. Always returns at least one line.
std::vector<std::string> wrapText(std::string_view utf8, Font font, double size, double maxWidth);

// Escapes WinAnsi bytes for a PDF literal string, including the parentheses.
std::string escapeString(std::string_view winAnsi);

// Coordinates are in points with the origin at the bottom-left of the page.
class Page {
public:
    void text(double x, double y, std::string_view utf8, Font font, double size, Color color = {},
              Align align = Align::Left);
    // Text rotated by `degrees` counter-clockwise, centered on (cx, cy).
    void rotatedText(double cx, double cy, double degrees, std::string_view utf8, Font font, double size, Color color);
    void line(double x1, double y1, double x2, double y2, double width = 0.5, Color color = {});
    void fillRect(double x, double y, double w, double h, Color color);
    const std::string& content() const { return ops_; }

private:
    std::string ops_;
};

class Document {
public:
    explicit Document(PageSize size = kLetter) : size_(size) {}

    Page& addPage();
    Page& page(std::size_t index) { return pages_[index]; }
    std::size_t pageCount() const { return pages_.size(); }
    PageSize size() const { return size_; }

    void setTitle(std::string title) { title_ = std::move(title); }
    void setAuthor(std::string author) { author_ = std::move(author); }

    // Serializes the complete PDF file.
    std::string build() const;

private:
    PageSize size_;
    std::deque<Page> pages_;  // deque: references stay valid as pages are added
    std::string title_;
    std::string author_;
};

}  // namespace ob::pdf
