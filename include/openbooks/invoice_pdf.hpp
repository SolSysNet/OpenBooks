#pragma once

#include "openbooks/book.hpp"

#include <string>

namespace ob {

// Renders an invoice or bill as a complete PDF file (bytes). Multi-page when needed;
// voided documents carry a VOID stamp and fully paid ones a PAID stamp.
std::string documentPdf(const Book& book, int documentId);

// A safe default file name such as "Invoice 1001 - Acme Corp.pdf". Characters that are not
// allowed in file names (and path separators) are removed, so the result can never point
// outside the folder it is saved in.
std::string documentPdfFileName(const Book& book, int documentId);

}  // namespace ob
