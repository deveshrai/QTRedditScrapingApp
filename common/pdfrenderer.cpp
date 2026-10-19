#include "pdfrenderer.h"

#include "errors.h"
#include "htmldom.h"
#include "scraper.h"

#include <QDateTime>
#include <QFontMetricsF>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextDocument>

namespace {

// Layout happens in CSS pixels (96 per inch), like a browser.
constexpr double kDpi = 96.0;
constexpr double kMargin = 0.4 * kDpi;   // 0.4in on every side, as before
constexpr int kIndent = 16;              // per comment level

const char *kStyleSheet = R"(
body { font-family: Helvetica, Arial, sans-serif; font-size: 13px; color: #1a1a1b; }
p { margin-top: 0px; margin-bottom: 6px; }
h1 { font-size: 20px; margin-top: 0px; margin-bottom: 4px; }
h2 { font-size: 17px; } h3 { font-size: 15px; } h4, h5, h6 { font-size: 13px; }
a { color: #0b57d0; text-decoration: none; }
pre { font-family: "Courier New", Courier, monospace; font-size: 11px; background-color: #f3f3f3; }
code { font-family: "Courier New", Courier, monospace; background-color: #f3f3f3; }
blockquote { color: #4a4a4a; margin-left: 14px; }
.meta { color: #666666; font-size: 11px; margin-bottom: 8px; }
.cmeta { color: #666666; font-size: 11px; margin-bottom: 2px; }
.author { color: #1a1a1b; font-weight: bold; }
.op { color: #0b57d0; font-weight: bold; }
.note { color: #888888; font-style: italic; font-size: 11px; }
.link { font-size: 12px; }
)";

const QStringList kThreadColors = {"#cc3300", "#0079d3", "#46a508", "#ff8b60", "#7193ff", "#cc8b00", "#9a5ac9"};

// Give every <img src="img:N"> a size that fits `maxWidth`, or turn it into a
// link when the picture couldn't be downloaded.
QString placeImages(const QString &htmlIn, const PostPage &page, double maxWidth, double maxHeight)
{
    static const QRegularExpression re(QStringLiteral("<img src=\"img:(\\d+)\">"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(htmlIn);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QStringView(htmlIn).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const int idx = m.captured(1).toInt();
        const QString url = page.imageUrls.value(idx);
        const QImage img = page.images.value(idx);
        if (img.isNull()) {
            out += QStringLiteral("<a href=\"%1\">[image: %2]</a>").arg(html::escape(url), html::escape(url.left(80)));
            continue;
        }
        const double scale = qMin(1.0, qMin(maxWidth / img.width(), maxHeight / img.height()));
        out += QStringLiteral("<a href=\"%1\"><img src=\"img:%2\" width=\"%3\" height=\"%4\"></a>")
                   .arg(html::escape(url), QString::number(idx), QString::number(int(img.width() * scale)),
                        QString::number(int(img.height() * scale)));
    }
    out += QStringView(htmlIn).mid(last);
    return out;
}

QString points(const QString &score)
{
    bool numeric = false;
    score.toLongLong(&numeric);
    if (!numeric)
        return score;
    return score + (score == QLatin1String("1") ? QStringLiteral(" point") : QStringLiteral(" points"));
}

QString pageTitle(const PostPage &page)
{
    return page.subreddit.isEmpty() ? page.title : QStringLiteral("%1 : r/%2").arg(page.title, page.subreddit);
}

} // namespace

QString postPageHtml(const PostPage &page, double contentWidth)
{
    const double maxImageHeight = 9.0 * kDpi;   // about one page
    QString h = QStringLiteral("<html><body>");
    h += QStringLiteral("<h1>%1</h1>").arg(html::escape(page.title));

    QStringList meta;
    if (!page.subreddit.isEmpty())
        meta << QStringLiteral("r/") + page.subreddit;
    if (!page.author.isEmpty())
        meta << QStringLiteral("u/") + page.author;
    if (!page.score.isEmpty() && page.score != QLatin1String("?"))
        meta << points(page.score);
    meta << QStringLiteral("%1 comment%2").arg(page.numComments).arg(page.numComments == 1 ? "" : "s");
    if (!page.created.isEmpty())
        meta << page.created;
    h += QStringLiteral("<p class=\"meta\">%1</p>").arg(html::escape(meta.join(QStringLiteral("  ·  "))));

    if (!page.linkUrl.isEmpty())
        h += QStringLiteral("<p class=\"link\"><a href=\"%1\">%2</a></p>").arg(html::escape(page.linkUrl), html::escape(page.linkUrl));
    if (!page.mediaHtml.isEmpty())
        h += QStringLiteral("<p>%1</p>").arg(placeImages(page.mediaHtml, page, contentWidth, maxImageHeight));
    if (!page.bodyHtml.isEmpty())
        h += QStringLiteral("<div>%1</div>").arg(placeImages(page.bodyHtml, page, contentWidth, maxImageHeight));

    h += QStringLiteral("<hr>");
    int real = 0;
    for (const Comment &c : page.comments)
        real += c.note ? 0 : 1;
    h += real ? QStringLiteral("<h3>Comments (%1 shown)</h3>").arg(real) : QStringLiteral("<p class=\"note\">No comments.</p>");

    for (const Comment &c : page.comments) {
        const int indent = c.depth * kIndent;
        const double room = contentWidth - indent - 16;
        h += QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top: 6px;\"><tr>");
        if (indent)
            h += QStringLiteral("<td width=\"%1\"></td>").arg(indent);
        h += QStringLiteral("<td width=\"2\" bgcolor=\"%1\"></td><td width=\"8\"></td><td>")
                 .arg(kThreadColors[c.depth % kThreadColors.size()]);
        if (c.note) {
            h += QStringLiteral("<p class=\"note\">%1</p>").arg(c.bodyHtml);
        } else {
            QString line = QStringLiteral("<span class=\"author\">u/%1</span>").arg(html::escape(c.author));
            if (!page.author.isEmpty() && c.author == page.author && c.author != QLatin1String("[deleted]"))
                line += QStringLiteral(" <span class=\"op\">OP</span>");
            if (!c.score.isEmpty())
                line += QStringLiteral("  ·  ") + html::escape(points(c.score));
            if (!c.created.isEmpty())
                line += QStringLiteral("  ·  ") + html::escape(c.created);
            h += QStringLiteral("<p class=\"cmeta\">%1</p>").arg(line);
            h += c.bodyHtml.isEmpty() ? QStringLiteral("<p class=\"note\">[no text]</p>")
                                      : placeImages(c.bodyHtml, page, room, maxImageHeight);
        }
        h += QStringLiteral("</td></tr></table>");
    }
    h += QStringLiteral("</body></html>");
    return h;
}

void renderPostPdf(const PostPage &page, const QString &path, const QString &paper, bool headerFooter)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw ScrapeError(QStringLiteral("Can't write %1 (%2)").arg(path, file.errorString()));

    QPdfWriter writer(&file);
    writer.setResolution(300);
    writer.setPageSize(QPageSize(paper.compare(QLatin1String("A4"), Qt::CaseInsensitive) == 0 ? QPageSize::A4
                                                                                              : QPageSize::Letter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));   // margins are drawn by hand below
    writer.setTitle(pageTitle(page));
    writer.setCreator(QStringLiteral("RedditPDF (Qt) %1").arg(APP_VERSION));

    const QRectF full = writer.pageLayout().fullRect(QPageLayout::Inch);
    const QSizeF pageSize(full.width() * kDpi, full.height() * kDpi);
    const QSizeF content(pageSize.width() - 2 * kMargin, pageSize.height() - 2 * kMargin);

    QTextDocument doc;
    doc.setDocumentMargin(0);
    QFont base(QStringLiteral("Helvetica"));
    base.setStyleHint(QFont::SansSerif);
    base.setPixelSize(13);
    doc.setDefaultFont(base);
    doc.setDefaultStyleSheet(QString::fromLatin1(kStyleSheet));
    for (int i = 0; i < page.images.size(); ++i)
        if (!page.images[i].isNull())
            doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("img:%1").arg(i)), page.images[i]);
    doc.setHtml(postPageHtml(page, content.width()));
    doc.setPageSize(content);

    QPainter p;
    if (!p.begin(&writer))
        throw ScrapeError(QStringLiteral("Can't start the PDF writer for %1").arg(path));
    const double scale = writer.resolution() / kDpi;
    const int pages = doc.pageCount();
    const QString date = QDateTime::currentDateTime().toString(QStringLiteral("M/d/yy, h:mm AP"));
    QFont small(QStringLiteral("Arial"));
    small.setStyleHint(QFont::SansSerif);
    small.setPixelSize(8);
    const QFontMetricsF fm(small);

    for (int i = 0; i < pages; ++i) {
        if (i)
            writer.newPage();
        p.save();
        p.scale(scale, scale);

        p.save();
        p.translate(kMargin, kMargin - i * content.height());
        doc.drawContents(&p, QRectF(0, i * content.height(), content.width(), content.height()));
        p.restore();

        if (headerFooter) {
            p.setFont(small);
            p.setPen(Qt::black);
            const QRectF top(kMargin, 0, content.width(), kMargin);
            const QRectF bottom(kMargin, pageSize.height() - kMargin, content.width(), kMargin);
            const double dateW = fm.horizontalAdvance(date) + 24;
            p.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, date);
            p.drawText(top, Qt::AlignRight | Qt::AlignVCenter,
                       fm.elidedText(pageTitle(page), Qt::ElideRight, content.width() - dateW));
            const QString num = QStringLiteral("%1/%2").arg(i + 1).arg(pages);
            const double numW = fm.horizontalAdvance(num) + 24;
            p.drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(page.pageUrl, Qt::ElideMiddle, content.width() - numW));
            p.drawText(bottom, Qt::AlignRight | Qt::AlignVCenter, num);
        }
        p.restore();
    }
    if (!p.end() || !file.commit())
        throw ScrapeError(QStringLiteral("Couldn't finish writing %1").arg(path));
}
