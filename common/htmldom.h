#pragma once
// htmldom.h - a small, forgiving HTML parser with CSS-selector queries.
//
// Replaces the JavaScript that scraper.py ran inside Chrome: pages are fetched
// over plain HTTP, parsed into this tree, and queried with the same selectors.
//
// Supported selectors: tag, *, #id, .class, [attr], [attr=v], [attr^=v],
// [attr$=v], [attr*=v], [attr~=v], the descendant (' ') and child ('>')
// combinators, and ',' groups.

#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

namespace html {

struct Node {
    enum Type { Document, Element, Text, Comment };

    Type type = Element;
    QString name;                           // lower-case tag name (elements)
    QList<QPair<QString, QString>> attrs;   // lower-case names, decoded values
    QString text;                           // Text / Comment content (decoded)
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;

    bool isElement() const { return type == Element; }
    QString attr(const QString &name, const QString &fallback = QString()) const;
    bool hasAttr(const QString &name) const;
    bool hasClass(const QString &cls) const;
    QList<Node *> childElements() const;

    // All text below this node, like DOM textContent.
    QString textContent() const;
    // Readable text: skips script/style, breaks at blocks, collapses spaces.
    QString innerText() const;

    // Matching descendants in document order.
    QList<Node *> select(const QString &selector) const;
    Node *selectOne(const QString &selector) const;
    // Like select(), but never looks inside an element matching `stop`
    // (e.g. a comment's own author, not one from its nested replies).
    QList<Node *> selectOwn(const QString &selector, const QString &stop) const;
    Node *selectOwnOne(const QString &selector, const QString &stop) const;
    bool matches(const QString &selector) const;
};

std::unique_ptr<Node> parse(const QString &source);

QString decodeEntities(const QString &s);
QString escape(const QString &s);   // safe for text and quoted attributes

// Re-serialise a subtree as simple, safe HTML for QTextDocument: keeps text
// formatting, lists, quotes, code and tables; drops scripts, forms, styles and
// classes; makes links absolute against `base`. Each <img> is replaced by
// whatever `image(absoluteSrc, alt)` returns.
using ImageFn = std::function<QString(const QString &src, const QString &alt)>;
QString toSafeHtml(const Node *root, const QUrl &base, const ImageFn &image);

} // namespace html
