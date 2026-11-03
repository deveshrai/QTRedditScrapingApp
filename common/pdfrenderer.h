#pragma once
// pdfrenderer.h - lays out a PostPage with QTextDocument and prints it to PDF
// with QPdfWriter (replaces Chrome's page.pdf()).

#include <QString>

struct PostPage;

// The full HTML handed to QTextDocument (useful for debugging the layout).
QString postPageHtml(const PostPage &page, double contentWidthPx);

// paper: "Letter" or "A4". headerFooter: date + title on top, URL + page
// numbers at the bottom, like Chrome's print header/footer. Throws ScrapeError.
void renderPostPdf(const PostPage &page, const QString &path, const QString &paper, bool headerFooter);
