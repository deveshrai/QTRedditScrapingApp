#include "htmldom.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace html {

// --------------------------------------------------------------------------- //
// entities
// --------------------------------------------------------------------------- //

namespace {

const QHash<QString, char32_t> &namedEntities()
{
    static const QHash<QString, char32_t> map = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
        {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"bull", 0x2022}, {"middot", 0xB7},
        {"laquo", 0xAB}, {"raquo", 0xBB}, {"times", 0xD7}, {"divide", 0xF7},
        {"deg", 0xB0}, {"plusmn", 0xB1}, {"para", 0xB6}, {"sect", 0xA7},
        {"cent", 0xA2}, {"pound", 0xA3}, {"euro", 0x20AC}, {"yen", 0xA5},
        {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"iexcl", 0xA1},
        {"iquest", 0xBF}, {"shy", 0xAD}, {"zwj", 0x200D}, {"zwnj", 0x200C},
        {"thinsp", 0x2009}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"larr", 0x2190},
        {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"hearts", 0x2665},
        {"prime", 0x2032}, {"Prime", 0x2033}, {"micro", 0xB5}, {"sup2", 0xB2},
        {"sup3", 0xB3}, {"acute", 0xB4}, {"uml", 0xA8}, {"ordf", 0xAA},
        {"ordm", 0xBA}, {"not", 0xAC}, {"macr", 0xAF}, {"cedil", 0xB8},
    };
    return map;
}

void appendCodePoint(QString &out, char32_t cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    out += QString::fromUcs4(&cp, 1);
}

} // namespace

QString decodeEntities(const QString &s)
{
    if (!s.contains(QLatin1Char('&')))
        return s;
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = s[i];
        if (c != QLatin1Char('&')) {
            out += c;
            continue;
        }
        int j = i + 1;
        if (j < n && s[j] == QLatin1Char('#')) {
            ++j;
            bool hex = j < n && (s[j] == QLatin1Char('x') || s[j] == QLatin1Char('X'));
            if (hex)
                ++j;
            const int start = j;
            while (j < n && (hex ? isxdigit(s[j].unicode()) : s[j].isDigit()) && j - start < 8)
                ++j;
            if (j > start) {
                bool ok = false;
                const uint cp = s.mid(start, j - start).toUInt(&ok, hex ? 16 : 10);
                if (ok) {
                    appendCodePoint(out, cp);
                    i = (j < n && s[j] == QLatin1Char(';')) ? j : j - 1;
                    continue;
                }
            }
        } else {
            const int start = j;
            while (j < n && s[j].isLetterOrNumber() && j - start < 10)
                ++j;
            const QString name = s.mid(start, j - start);
            const auto it = namedEntities().constFind(name);
            const bool semi = j < n && s[j] == QLatin1Char(';');
            // Without ';' only the old core entities are recognised, as browsers do.
            static const QSet<QString> legacy = {"amp", "lt", "gt", "quot", "nbsp"};
            if (it != namedEntities().constEnd() && (semi || legacy.contains(name))) {
                appendCodePoint(out, it.value());
                i = semi ? j : j - 1;
                continue;
            }
        }
        out += c;
    }
    return out;
}

QString escape(const QString &s)
{
    QString out;
    out.reserve(s.size() + 8);
    for (const QChar c : s) {
        switch (c.unicode()) {
        case '&': out += QLatin1String("&amp;"); break;
        case '<': out += QLatin1String("&lt;"); break;
        case '>': out += QLatin1String("&gt;"); break;
        case '"': out += QLatin1String("&quot;"); break;
        default: out += c;
        }
    }
    return out;
}

// --------------------------------------------------------------------------- //
// parser
// --------------------------------------------------------------------------- //

namespace {

const QSet<QString> kVoid = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "keygen",
    "link", "meta", "param", "source", "track", "wbr",
};
// Contents are raw text up to the matching end tag.
const QSet<QString> kRawText = {
    "script", "style", "textarea", "title", "xmp", "iframe", "noembed", "noframes",
};
// Starting one of these closes an open <p>.
const QSet<QString> kClosesP = {
    "address", "article", "aside", "blockquote", "details", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3",
    "h4", "h5", "h6", "header", "hr", "main", "menu", "nav", "ol", "p", "pre",
    "section", "table", "ul",
};
const QSet<QString> kPScope = {
    "button", "td", "th", "li", "blockquote", "div", "section", "article",
    "body", "html", "details", "summary", "table", "dd", "dt", "figure",
};

class Parser {
public:
    explicit Parser(const QString &src) : s(src), n(src.size()) {}

    std::unique_ptr<Node> run()
    {
        auto doc = std::make_unique<Node>();
        doc->type = Node::Document;
        cur = doc.get();
        while (i < n) {
            if (s[i] == QLatin1Char('<') && i + 1 < n) {
                const QChar c = s[i + 1];
                if (s.mid(i, 4) == QLatin1String("<!--")) {
                    comment();
                    continue;
                }
                if (c == QLatin1Char('!') || c == QLatin1Char('?')) {
                    declaration();
                    continue;
                }
                if (c == QLatin1Char('/') && i + 2 < n && s[i + 2].isLetter()) {
                    endTag();
                    continue;
                }
                if (c.isLetter()) {
                    startTag();
                    continue;
                }
            }
            const int next = s.indexOf(QLatin1Char('<'), i + 1);
            const int end = next < 0 ? n : next;
            addText(decodeEntities(s.mid(i, end - i)));
            i = end;
        }
        return doc;
    }

private:
    const QString &s;
    int i = 0;
    const int n;
    Node *cur = nullptr;

    Node *append(std::unique_ptr<Node> node)
    {
        node->parent = cur;
        cur->children.push_back(std::move(node));
        return cur->children.back().get();
    }

    void addText(const QString &t)
    {
        if (t.isEmpty())
            return;
        if (!cur->children.empty() && cur->children.back()->type == Node::Text) {
            cur->children.back()->text += t;
            return;
        }
        auto node = std::make_unique<Node>();
        node->type = Node::Text;
        node->text = t;
        append(std::move(node));
    }

    void comment()
    {
        const int end = s.indexOf(QLatin1String("-->"), i + 4);
        auto node = std::make_unique<Node>();
        node->type = Node::Comment;
        node->text = s.mid(i + 4, (end < 0 ? n : end) - i - 4);
        append(std::move(node));
        i = end < 0 ? n : end + 3;
    }

    void declaration()   // <!DOCTYPE ...>, <![CDATA[...]]>, <?xml ...?>
    {
        if (s.mid(i, 9) == QLatin1String("<![CDATA[")) {
            const int end = s.indexOf(QLatin1String("]]>"), i + 9);
            addText(s.mid(i + 9, (end < 0 ? n : end) - i - 9));
            i = end < 0 ? n : end + 3;
            return;
        }
        const int end = s.indexOf(QLatin1Char('>'), i);
        i = end < 0 ? n : end + 1;
    }

    QString readName()
    {
        const int start = i;
        while (i < n && !s[i].isSpace() && s[i] != QLatin1Char('>') && s[i] != QLatin1Char('/'))
            ++i;
        return s.mid(start, i - start).toLower();
    }

    void skipSpace()
    {
        while (i < n && s[i].isSpace())
            ++i;
    }

    // Walk up from the current node looking for one of `names`, giving up at
    // any element in `stop`.
    Node *findOpen(const QSet<QString> &names, const QSet<QString> &stop) const
    {
        for (Node *x = cur; x && x->type == Node::Element; x = x->parent) {
            if (names.contains(x->name))
                return x;
            if (stop.contains(x->name))
                return nullptr;
        }
        return nullptr;
    }

    void closeImplied(const QString &name)
    {
        Node *open = nullptr;
        if (name == QLatin1String("li"))
            open = findOpen({"li"}, {"ul", "ol"});
        else if (name == QLatin1String("dt") || name == QLatin1String("dd"))
            open = findOpen({"dt", "dd"}, {"dl"});
        else if (name == QLatin1String("tr"))
            open = findOpen({"tr"}, {"table", "tbody", "thead", "tfoot"});
        else if (name == QLatin1String("td") || name == QLatin1String("th"))
            open = findOpen({"td", "th"}, {"tr", "table"});
        else if (name == QLatin1String("tbody") || name == QLatin1String("thead")
                 || name == QLatin1String("tfoot"))
            open = findOpen({"tbody", "thead", "tfoot"}, {"table"});
        else if (name == QLatin1String("option"))
            open = findOpen({"option"}, {"select", "datalist"});
        if (open)
            cur = open->parent;
        if (kClosesP.contains(name))
            if (Node *p = findOpen({"p"}, kPScope))
                cur = p->parent;
    }

    void startTag()
    {
        ++i;   // '<'
        auto el = std::make_unique<Node>();
        el->name = readName();
        bool selfClosing = false;
        while (i < n) {
            skipSpace();
            if (i >= n)
                break;
            if (s[i] == QLatin1Char('>')) {
                ++i;
                break;
            }
            if (s[i] == QLatin1Char('/')) {
                ++i;
                if (i < n && s[i] == QLatin1Char('>')) {
                    selfClosing = true;
                    ++i;
                    break;
                }
                continue;
            }
            const int start = i;
            while (i < n && !s[i].isSpace() && s[i] != QLatin1Char('=')
                   && s[i] != QLatin1Char('>') && !(s[i] == QLatin1Char('/') && i > start))
                ++i;
            const QString attrName = s.mid(start, i - start).toLower();
            QString value;
            skipSpace();
            if (i < n && s[i] == QLatin1Char('=')) {
                ++i;
                skipSpace();
                if (i < n && (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\''))) {
                    const QChar q = s[i++];
                    const int end = s.indexOf(q, i);
                    value = s.mid(i, (end < 0 ? n : end) - i);
                    i = end < 0 ? n : end + 1;
                } else {
                    const int vs = i;
                    while (i < n && !s[i].isSpace() && s[i] != QLatin1Char('>'))
                        ++i;
                    value = s.mid(vs, i - vs);
                }
            }
            if (!attrName.isEmpty())
                el->attrs.append({attrName, decodeEntities(value)});
        }

        closeImplied(el->name);
        const QString name = el->name;
        Node *node = append(std::move(el));

        if (kRawText.contains(name) && !selfClosing) {
            const QString endTag = QLatin1String("</") + name;
            int end = i;
            while ((end = s.indexOf(endTag, end, Qt::CaseInsensitive)) >= 0) {
                const int after = end + endTag.size();
                if (after >= n || s[after] == QLatin1Char('>') || s[after].isSpace()
                    || s[after] == QLatin1Char('/'))
                    break;
                end = after;
            }
            QString raw = s.mid(i, (end < 0 ? n : end) - i);
            if (name == QLatin1String("title") || name == QLatin1String("textarea"))
                raw = decodeEntities(raw);
            if (!raw.isEmpty()) {
                auto t = std::make_unique<Node>();
                t->type = Node::Text;
                t->text = raw;
                t->parent = node;
                node->children.push_back(std::move(t));
            }
            if (end < 0) {
                i = n;
            } else {
                const int gt = s.indexOf(QLatin1Char('>'), end);
                i = gt < 0 ? n : gt + 1;
            }
            return;
        }
        if (!selfClosing && !kVoid.contains(name))
            cur = node;
    }

    void endTag()
    {
        i += 2;   // '</'
        const QString name = readName();
        const int gt = s.indexOf(QLatin1Char('>'), i);
        i = gt < 0 ? n : gt + 1;
        if (name == QLatin1String("br")) {   // </br> acts like <br>
            auto br = std::make_unique<Node>();
            br->name = name;
            append(std::move(br));
            return;
        }
        for (Node *x = cur; x && x->type == Node::Element; x = x->parent) {
            if (x->name == name) {
                cur = x->parent;
                return;
            }
        }
        // No matching open element: ignore the stray end tag.
    }
};

} // namespace

std::unique_ptr<Node> parse(const QString &source)
{
    return Parser(source).run();
}

// --------------------------------------------------------------------------- //
// node helpers
// --------------------------------------------------------------------------- //

QString Node::attr(const QString &key, const QString &fallback) const
{
    for (const auto &a : attrs)
        if (a.first == key)
            return a.second;
    return fallback;
}

bool Node::hasAttr(const QString &key) const
{
    for (const auto &a : attrs)
        if (a.first == key)
            return true;
    return false;
}

bool Node::hasClass(const QString &cls) const
{
    const QString c = attr(QStringLiteral("class")).simplified();
    if (c.isEmpty())
        return false;
    for (const QStringView part : QStringView(c).split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (part == cls)
            return true;
    return false;
}

QList<Node *> Node::childElements() const
{
    QList<Node *> out;
    for (const auto &c : children)
        if (c->type == Element)
            out.append(c.get());
    return out;
}

QString Node::textContent() const
{
    if (type == Text)
        return text;
    QString out;
    for (const auto &c : children)
        if (c->type == Text || c->type == Element)
            out += c->textContent();
    return out;
}

namespace {

void collectText(const Node *n, QString &out)
{
    static const QSet<QString> skip = {"script", "style", "noscript", "template", "head"};
    static const QSet<QString> block = {
        "p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "header", "footer", "table", "ul", "ol", "pre",
        "blockquote", "main", "nav", "form", "details", "summary",
    };
    for (const auto &c : n->children) {
        if (c->type == Node::Text) {
            out += c->text;
        } else if (c->type == Node::Element && !skip.contains(c->name)) {
            const bool b = block.contains(c->name);
            if (b)
                out += QLatin1Char(' ');
            collectText(c.get(), out);
            if (b)
                out += QLatin1Char(' ');
        }
    }
}

} // namespace

QString Node::innerText() const
{
    QString out;
    collectText(this, out);
    return out.simplified();
}

// --------------------------------------------------------------------------- //
// selectors
// --------------------------------------------------------------------------- //

namespace {

struct AttrTest {
    QString name, op, value;
};
struct Compound {
    QString tag;
    QString id;
    QStringList classes;
    QList<AttrTest> attrs;
};
struct Complex {
    QList<Compound> parts;
    QList<QChar> combinators;   // combinators[k] joins parts[k] and parts[k + 1]
};
using SelectorList = QList<Complex>;

SelectorList parseSelector(const QString &s)
{
    SelectorList list;
    Complex cur;
    Compound comp;
    bool haveComp = false;
    QChar pending;
    int i = 0;
    const int n = s.size();

    auto flush = [&] {
        if (!haveComp)
            return;
        if (!cur.parts.isEmpty())
            cur.combinators.append(pending.isNull() ? QLatin1Char(' ') : pending);
        cur.parts.append(comp);
        comp = Compound();
        haveComp = false;
        pending = QChar();
    };
    auto ident = [&] {
        const int st = i;
        while (i < n && (s[i].isLetterOrNumber() || s[i] == QLatin1Char('-') || s[i] == QLatin1Char('_')))
            ++i;
        return s.mid(st, i - st);
    };

    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) {
            flush();
            ++i;
        } else if (c == QLatin1Char('>')) {
            flush();
            pending = c;
            ++i;
        } else if (c == QLatin1Char(',')) {
            flush();
            if (!cur.parts.isEmpty())
                list.append(cur);
            cur = Complex();
            pending = QChar();
            ++i;
        } else if (c == QLatin1Char('#')) {
            ++i;
            comp.id = ident();
            haveComp = true;
        } else if (c == QLatin1Char('.')) {
            ++i;
            comp.classes << ident();
            haveComp = true;
        } else if (c == QLatin1Char('*')) {
            ++i;
            haveComp = true;
        } else if (c == QLatin1Char('[')) {
            ++i;
            while (i < n && s[i].isSpace())
                ++i;
            AttrTest t;
            t.name = ident().toLower();
            while (i < n && s[i].isSpace())
                ++i;
            if (i + 1 < n && QStringLiteral("^$*~|").contains(s[i]) && s[i + 1] == QLatin1Char('=')) {
                t.op = s.mid(i, 2);
                i += 2;
            } else if (i < n && s[i] == QLatin1Char('=')) {
                t.op = QStringLiteral("=");
                ++i;
            }
            while (i < n && s[i].isSpace())
                ++i;
            if (!t.op.isEmpty()) {
                if (i < n && (s[i] == QLatin1Char('"') || s[i] == QLatin1Char('\''))) {
                    const QChar q = s[i++];
                    const int end = s.indexOf(q, i);
                    t.value = s.mid(i, (end < 0 ? n : end) - i);
                    i = end < 0 ? n : end + 1;
                } else {
                    const int st = i;
                    while (i < n && s[i] != QLatin1Char(']') && !s[i].isSpace())
                        ++i;
                    t.value = s.mid(st, i - st);
                }
            }
            const int close = s.indexOf(QLatin1Char(']'), i);
            i = close < 0 ? n : close + 1;
            comp.attrs.append(t);
            haveComp = true;
        } else if (c.isLetter()) {
            comp.tag = ident().toLower();
            haveComp = true;
        } else {
            ++i;   // unsupported syntax: ignore the character
        }
    }
    flush();
    if (!cur.parts.isEmpty())
        list.append(cur);
    return list;
}

const SelectorList &compiled(const QString &s)
{
    thread_local QHash<QString, SelectorList> cache;
    auto it = cache.find(s);
    if (it == cache.end())
        it = cache.insert(s, parseSelector(s));
    return it.value();
}

bool matchCompound(const Node *n, const Compound &c)
{
    if (n->type != Node::Element)
        return false;
    if (!c.tag.isEmpty() && n->name != c.tag)
        return false;
    if (!c.id.isEmpty() && n->attr(QStringLiteral("id")) != c.id)
        return false;
    for (const QString &cls : c.classes)
        if (!n->hasClass(cls))
            return false;
    for (const AttrTest &a : c.attrs) {
        if (!n->hasAttr(a.name))
            return false;
        const QString v = n->attr(a.name);
        if (a.op == QLatin1String("=") && v != a.value)
            return false;
        if (a.op == QLatin1String("^=") && (a.value.isEmpty() || !v.startsWith(a.value)))
            return false;
        if (a.op == QLatin1String("$=") && (a.value.isEmpty() || !v.endsWith(a.value)))
            return false;
        if (a.op == QLatin1String("*=") && (a.value.isEmpty() || !v.contains(a.value)))
            return false;
        if (a.op == QLatin1String("~=") && !v.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(a.value))
            return false;
        if (a.op == QLatin1String("|=") && v != a.value && !v.startsWith(a.value + QLatin1Char('-')))
            return false;
    }
    return true;
}

bool matchComplex(const Node *n, const Complex &c, int idx)
{
    if (!matchCompound(n, c.parts[idx]))
        return false;
    if (idx == 0)
        return true;
    if (c.combinators[idx - 1] == QLatin1Char('>')) {
        const Node *p = n->parent;
        return p && p->type == Node::Element && matchComplex(p, c, idx - 1);
    }
    for (const Node *p = n->parent; p && p->type == Node::Element; p = p->parent)
        if (matchComplex(p, c, idx - 1))
            return true;
    return false;
}

bool matchAny(const Node *n, const SelectorList &list)
{
    for (const Complex &c : list)
        if (matchComplex(n, c, c.parts.size() - 1))
            return true;
    return false;
}

// Returns true when `firstOnly` is set and a match was found.
bool collect(const Node *n, const SelectorList &sel, const SelectorList *stop,
             QList<Node *> &out, bool firstOnly)
{
    for (const auto &child : n->children) {
        if (child->type != Node::Element)
            continue;
        if (stop && matchAny(child.get(), *stop))
            continue;
        if (matchAny(child.get(), sel)) {
            out.append(child.get());
            if (firstOnly)
                return true;
        }
        if (collect(child.get(), sel, stop, out, firstOnly))
            return true;
    }
    return false;
}

} // namespace

QList<Node *> Node::select(const QString &selector) const
{
    QList<Node *> out;
    collect(this, compiled(selector), nullptr, out, false);
    return out;
}

Node *Node::selectOne(const QString &selector) const
{
    QList<Node *> out;
    collect(this, compiled(selector), nullptr, out, true);
    return out.isEmpty() ? nullptr : out.first();
}

QList<Node *> Node::selectOwn(const QString &selector, const QString &stop) const
{
    QList<Node *> out;
    collect(this, compiled(selector), &compiled(stop), out, false);
    return out;
}

Node *Node::selectOwnOne(const QString &selector, const QString &stop) const
{
    QList<Node *> out;
    collect(this, compiled(selector), &compiled(stop), out, true);
    return out.isEmpty() ? nullptr : out.first();
}

bool Node::matches(const QString &selector) const
{
    return matchAny(this, compiled(selector));
}

// --------------------------------------------------------------------------- //
// safe re-serialisation
// --------------------------------------------------------------------------- //

namespace {

const QSet<QString> kKeep = {
    "p", "br", "a", "b", "strong", "i", "em", "u", "s", "strike", "del", "ins",
    "code", "pre", "blockquote", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5",
    "h6", "table", "thead", "tbody", "tfoot", "tr", "th", "td", "hr", "sup",
    "sub", "small", "big", "tt", "kbd", "samp", "var", "q", "cite", "dl", "dt",
    "dd", "center",
};
// Shown as a plain <div>.
const QSet<QString> kAsDiv = {
    "div", "section", "article", "figure", "figcaption", "details", "summary",
    "header", "footer", "main", "aside", "address",
};
// Dropped along with everything inside.
const QSet<QString> kDrop = {
    "script", "style", "noscript", "template", "svg", "form", "button", "input",
    "select", "textarea", "iframe", "video", "audio", "object", "embed", "head",
    "nav", "canvas", "map", "link", "meta", "title", "picture", "source",
};

void serialize(const Node *n, const QUrl &base, const ImageFn &image, QString &out)
{
    for (const auto &c : n->children) {
        if (c->type == Node::Text) {
            out += escape(c->text);
            continue;
        }
        if (c->type != Node::Element || kDrop.contains(c->name))
            continue;
        const QString &tag = c->name;
        if (tag == QLatin1String("img")) {
            const QString src = c->attr(QStringLiteral("src"));
            if (!src.isEmpty() && image)
                out += image(base.resolved(QUrl(src)).toString(), c->attr(QStringLiteral("alt")));
            continue;
        }
        QString open;
        if (kKeep.contains(tag)) {
            open = tag;
            QString attrs;
            if (tag == QLatin1String("a")) {
                const QString href = c->attr(QStringLiteral("href")).trimmed();
                if (!href.isEmpty() && !href.startsWith(QLatin1String("javascript:"), Qt::CaseInsensitive))
                    attrs += QStringLiteral(" href=\"%1\"").arg(escape(base.resolved(QUrl(href)).toString()));
            } else if (tag == QLatin1String("td") || tag == QLatin1String("th")) {
                for (const char *a : {"colspan", "rowspan", "align"})
                    if (c->hasAttr(QLatin1String(a)))
                        attrs += QStringLiteral(" %1=\"%2\"").arg(QLatin1String(a), escape(c->attr(QLatin1String(a))));
            } else if (tag == QLatin1String("ol") && c->hasAttr(QStringLiteral("start"))) {
                attrs += QStringLiteral(" start=\"%1\"").arg(escape(c->attr(QStringLiteral("start"))));
            } else if (tag == QLatin1String("table")) {
                attrs += QStringLiteral(" border=\"1\" cellspacing=\"0\" cellpadding=\"4\"");
            }
            out += QLatin1Char('<') + open + attrs + QLatin1Char('>');
        } else if (kAsDiv.contains(tag)) {
            open = QStringLiteral("div");
            out += QLatin1String("<div>");
        }
        // Anything else (span, font, custom tags...) is unwrapped.
        if (tag != QLatin1String("br") && tag != QLatin1String("hr")) {
            serialize(c.get(), base, image, out);
            if (!open.isEmpty())
                out += QLatin1String("</") + open + QLatin1Char('>');
        }
    }
}

} // namespace

QString toSafeHtml(const Node *root, const QUrl &base, const ImageFn &image)
{
    QString out;
    if (root)
        serialize(root, base, image, out);
    return out.trimmed();
}

} // namespace html
