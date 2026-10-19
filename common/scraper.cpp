// scraper.cpp - helpers, retries, the HTML sites (Redlib / old.reddit) and
// PDF saving. The Reddit API backend lives in redditapi.cpp.

#include "scraper.h"

#include "builtin_credentials.h"
#include "htmldom.h"
#include "http.h"
#include "pdfrenderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QThread>

#include <algorithm>

// --------------------------------------------------------------------------- //
// helpers
// --------------------------------------------------------------------------- //

QString Post::id() const
{
    const int i = fullname.indexOf(QLatin1Char('_'));
    return i < 0 ? fullname : fullname.mid(i + 1);
}

QString cleanSubreddit(const QString &input)
{
    QString name = input.trimmed();
    name.remove(QRegularExpression(QStringLiteral("^(https?://)?(old\\.|www\\.)?reddit\\.com/")));
    name.remove(QRegularExpression(QStringLiteral("^/?r/")));
    while (!name.isEmpty() && (name.front() == QLatin1Char('/') || name.front() == QLatin1Char(' ')))
        name.remove(0, 1);
    while (!name.isEmpty() && (name.back() == QLatin1Char('/') || name.back() == QLatin1Char(' ')))
        name.chop(1);
    name = name.section(QLatin1Char('/'), 0, 0);
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_+]{1,60}$"));
    if (!valid.match(name).hasMatch())
        throw ScrapeError(QStringLiteral("'%1' doesn't look like a valid subreddit name.").arg(name));
    return name;
}

QString safeFilename(const QString &input, int maxLen)
{
    QString text = input;
    text.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    auto stripEnds = [](QString &t) {
        while (!t.isEmpty() && (t.front() == QLatin1Char(' ') || t.front() == QLatin1Char('.')))
            t.remove(0, 1);
        while (!t.isEmpty() && (t.back() == QLatin1Char(' ') || t.back() == QLatin1Char('.')))
            t.chop(1);
    };
    stripEnds(text);
    if (text.size() > maxLen) {
        int cut = maxLen;
        if (text[cut - 1].isHighSurrogate())   // don't split an emoji in half
            --cut;
        text.truncate(cut);
        while (!text.isEmpty() && (text.back() == QLatin1Char(' ') || text.back() == QLatin1Char('.')))
            text.chop(1);
    }
    return text.isEmpty() ? QStringLiteral("untitled") : text;
}

QString siteMode(const QString &base)
{
    if (base.compare(API_SITE, Qt::CaseInsensitive) == 0 || base.compare(QLatin1String("api"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("api");
    return base.contains(QLatin1String("old.reddit.com")) ? QStringLiteral("old") : QStringLiteral("redlib");
}

QString normalizeSite(const QString &site)
{
    QString s = site.trimmed();
    if (s.isEmpty())
        return DEFAULT_BASE;
    if (siteMode(s) == QLatin1String("api"))
        return API_SITE;
    if (!s.startsWith(QLatin1String("http://")) && !s.startsWith(QLatin1String("https://")))
        s.prepend(QLatin1String("https://"));
    while (s.endsWith(QLatin1Char('/')))
        s.chop(1);
    return s;
}

QString encodeQuery(const QList<QPair<QString, QString>> &items)
{
    QStringList parts;
    for (const auto &kv : items) {
        QString v = QString::fromLatin1(QUrl::toPercentEncoding(kv.second));
        v.replace(QLatin1String("%20"), QLatin1String("+"));
        parts << QString::fromLatin1(QUrl::toPercentEncoding(kv.first)) + QLatin1Char('=') + v;
    }
    return parts.join(QLatin1Char('&'));
}

QString listingUrl(const QString &base, const QString &mode, const QString &sub,
                   const QString &sort, const QString &timeFilter, int limit,
                   const QString &after, const QString &query)
{
    const QString lim = QString::number(limit);
    if (!query.isEmpty()) {
        // Search inside the subreddit:  /r/<sub>/search?q=..&restrict_sr=on&sort=..&t=..
        QList<QPair<QString, QString>> q = {{"q", query}, {"restrict_sr", "on"}, {"sort", sort}};
        if (!timeFilter.isEmpty() && timeFilter != QLatin1String("all"))
            q.append(qMakePair(QStringLiteral("t"), timeFilter));
        if (mode == QLatin1String("old"))
            q.append(qMakePair(QStringLiteral("limit"), lim));
        if (!after.isEmpty())
            q.append(qMakePair(QStringLiteral("after"), after));
        return QStringLiteral("%1/r/%2/search?%3").arg(base, sub, encodeQuery(q));
    }
    if (mode == QLatin1String("old")) {
        QString url = QStringLiteral("%1/r/%2/%3/?limit=%4").arg(base, sub, sort, lim);
        if (TIMED_SORTS.contains(sort))
            url += QStringLiteral("&t=") + timeFilter;
        if (!after.isEmpty())
            url += QStringLiteral("&after=") + after;
        return url;
    }
    QStringList params;
    if (TIMED_SORTS.contains(sort))
        params << QStringLiteral("t=") + timeFilter;
    if (!after.isEmpty())
        params << QStringLiteral("after=") + after;
    return QStringLiteral("%1/r/%2/%3").arg(base, sub, sort)
           + (params.isEmpty() ? QString() : QLatin1Char('?') + params.join(QLatin1Char('&')));
}

QString runTag(const QString &sort, const QString &timeFilter, const QString &query)
{
    // Folder-name fragment describing what was fetched, e.g. 'top-month' or
    // 'search-visiteur_top-year'.
    if (!query.isEmpty()) {
        static const QRegularExpression nonWord(QStringLiteral("[^\\w]+"),
                                                QRegularExpression::UseUnicodePropertiesOption);
        QString slug = query.trimmed();
        slug.replace(nonWord, QStringLiteral("-"));
        while (slug.startsWith(QLatin1Char('-')))
            slug.remove(0, 1);
        while (slug.endsWith(QLatin1Char('-')))
            slug.chop(1);
        slug.truncate(40);
        if (slug.isEmpty())
            slug = QStringLiteral("query");
        QString tag = QStringLiteral("search-%1_%2").arg(slug, sort);
        if (!timeFilter.isEmpty() && timeFilter != QLatin1String("all"))
            tag += QLatin1Char('-') + timeFilter;
        return tag;
    }
    return TIMED_SORTS.contains(sort) ? QStringLiteral("%1-%2").arg(sort, timeFilter) : sort;
}

QString pdfFileName(const Post &post)
{
    return QStringLiteral("%1 - %2 [%3].pdf")
        .arg(QStringLiteral("%1").arg(post.rank, 3, 10, QLatin1Char('0')), safeFilename(post.title), post.id());
}

bool hasBuiltinCredentials()
{
    return !QStringLiteral(BUILTIN_REDDIT_CLIENT_ID).isEmpty();
}

QString imageRef(PostPage &page, const QString &url)
{
    int idx = page.imageUrls.indexOf(url);
    if (idx < 0) {
        idx = page.imageUrls.size();
        page.imageUrls.append(url);
    }
    return QStringLiteral("<img src=\"img:%1\">").arg(idx);
}

// --------------------------------------------------------------------------- //
// the scraper
// --------------------------------------------------------------------------- //

struct RedditScraper::Loaded {
    HttpResponse resp;
    QString html;
    std::unique_ptr<html::Node> doc;
};

RedditScraper::RedditScraper(const ScraperOptions &options, LogFn logger, const std::atomic_bool *cancelFlag)
    : opt(options), logFn(std::move(logger)), cancel(cancelFlag)
{
    opt.base = normalizeSite(opt.base);
    if (opt.clientId.trimmed().isEmpty()) {   // fall back to the built-in app
        opt.clientId = QStringLiteral(BUILTIN_REDDIT_CLIENT_ID);
        opt.clientSecret = QStringLiteral(BUILTIN_REDDIT_CLIENT_SECRET);
    }
    siteModeName = opt.mode.isEmpty() ? siteMode(opt.base) : opt.mode;
    if (opt.debugDir.isEmpty())
        opt.debugDir = QCoreApplication::applicationDirPath() + QStringLiteral("/debug");
    const bool api = siteModeName == QLatin1String("api");
    QString ua = opt.userAgent;
    if (ua.isEmpty())
        ua = api ? API_USER_AGENT : USER_AGENT;
    http = std::make_unique<SyncHttp>(ua, cancel);

    const QUrl baseUrl(opt.base);
    if (siteModeName == QLatin1String("old")) {
        // Skip the "are you over 18?" interstitial on NSFW subs.
        http->setCookie(QStringLiteral("over18"), QStringLiteral("1"), baseUrl);
    } else if (siteModeName == QLatin1String("redlib")) {
        // Redlib settings: light theme; allow NSFW subreddits.
        http->setCookie(QStringLiteral("theme"), QStringLiteral("light"), baseUrl);
        http->setCookie(QStringLiteral("show_nsfw"), QStringLiteral("on"), baseUrl);
    }
    if (!api && !opt.cookies.trimmed().isEmpty())
        http->addCookieHeader(opt.cookies, baseUrl);
}

RedditScraper::~RedditScraper() = default;

void RedditScraper::log(const QString &msg) const
{
    if (logFn)
        logFn(msg);
}

void RedditScraper::checkCancel() const
{
    if (cancel && *cancel)
        throw Cancelled();
}

void RedditScraper::sleep(double seconds)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < seconds * 1000) {
        checkCancel();
        QThread::msleep(std::clamp<qint64>(qint64(seconds * 1000) - t.elapsed(), 1, 100));
    }
    checkCancel();
}

// navigation with retry -------------------------------------------------------
HttpResponse RedditScraper::getWithRetry(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QString last;
    for (int attempt = 0; attempt < 4; ++attempt) {
        checkCancel();
        HttpResponse r = http->get(url, headers);
        if (r.status == 0) {   // network error / timeout
            last = r.error;
            sleep(2 + attempt * 2);
            continue;
        }
        if (r.status == 429) {
            const int wait = 10 * (attempt + 1);
            log(QStringLiteral("  Rate-limited (429). Waiting %1s...").arg(wait));
            sleep(wait);
            last = QStringLiteral("HTTP 429");
            continue;
        }
        if (r.status >= 500) {
            last = QStringLiteral("HTTP %1").arg(r.status);
            sleep(2 + attempt * 2);
            continue;
        }
        return r;
    }
    throw ScrapeError(QStringLiteral("Failed to load %1 (%2)").arg(url.toString(), last));
}

RedditScraper::Loaded RedditScraper::loadHtml(const QString &url, const QString &ready)
{
    // The browser version waited for `ready` to appear while JavaScript ran.
    // Without JavaScript the best we can do is re-fetch for a while.
    QElapsedTimer clock;
    clock.start();
    const qint64 limitMs = qint64(opt.readyTimeout * 1000);
    bool announced = false;
    for (;;) {
        Loaded page;
        page.resp = getWithRetry(QUrl(url), {});
        page.html = QString::fromUtf8(page.resp.body);
        page.doc = html::parse(page.html);
        checkBlocked(page);
        if (page.resp.status >= 400)
            throw ScrapeError(QStringLiteral("HTTP %1 for %2").arg(page.resp.status).arg(url));
        if (ready.isEmpty() || page.doc->selectOne(ready) || clock.elapsed() >= limitMs)
            return page;
        if (!announced) {
            log(QStringLiteral("  The page has no content yet; retrying for up to %1s...").arg(opt.readyTimeout));
            announced = true;
        }
        sleep(qMin(5.0, (limitMs - clock.elapsed()) / 1000.0));
    }
}

QString RedditScraper::saveDebug(const Loaded &page)
{
    QDir dir(opt.debugDir);
    if (!dir.mkpath(QStringLiteral(".")))
        return QString();
    QFile f(dir.filePath(QStringLiteral("last_listing.html")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(page.resp.body);
    return QStringLiteral("\nSaved page snapshot to: %1").arg(QDir::toNativeSeparators(dir.absolutePath()));
}

void RedditScraper::checkBlocked(const Loaded &page)
{
    const html::Node *t = page.doc->selectOne(QStringLiteral("title"));
    const QString title = t ? t->textContent().simplified() : QString();
    const QString host = page.resp.url.host();

    if (siteModeName == QLatin1String("old")
        && (page.resp.url.path().startsWith(QLatin1String("/login"))
            || title.contains(QLatin1String("Welcome to Reddit")))) {
        throw ScrapeError(QStringLiteral(
            "%1 sent the request to its login page (%2).\n"
            "Reddit now wants a logged-in session there. Copy the Cookie header from a logged-in "
            "browser into the Cookie field (CLI: --cookie), or use the Reddit API (site \"%3\").%4")
            .arg(host, page.resp.url.toString(), API_SITE, saveDebug(page)));
    }

    static const QRegularExpression wallTitle(
        QStringLiteral("verifying your browser|not a bot|just a moment|attention required|"
                       "ddos-guard|oh noes|please wait for verification|checking your browser"),
        QRegularExpression::CaseInsensitiveOption);
    const bool wall = page.html.contains(QLatin1String("anubis_challenge"))
                      || page.html.contains(QLatin1String("/.within.website/"))
                      || page.html.contains(QLatin1String("challenge-platform"))
                      || wallTitle.match(title).hasMatch();
    if (wall) {
        throw ScrapeError(QStringLiteral(
            "%1 answered with a JavaScript bot check (\"%2\", HTTP %3).\n"
            "This Qt version has no browser engine, so it can't pass that check.\n"
            "Use the Reddit API (site \"%4\"), a self-hosted Redlib, or a mirror without a bot check.%5")
            .arg(host, title, QString::number(page.resp.status), API_SITE, saveDebug(page)));
    }
}

QString RedditScraper::diagnose(const Loaded &page, const QString &sub, const QString &sort, const QString &query)
{
    // Explain an empty listing and save the page for inspection.
    const html::Node *t = page.doc->selectOne(QStringLiteral("title"));
    const QString title = t ? t->textContent().simplified() : QStringLiteral("?");
    const html::Node *body = page.doc->selectOne(QStringLiteral("body"));
    const QString text = (body ? body->innerText() : page.doc->innerText()).left(300);
    const QString saved = saveDebug(page);
    log(QStringLiteral("  Page URL:   ") + page.resp.url.toString());
    log(QStringLiteral("  Page title: ") + title);
    log(QStringLiteral("  Page text:  ") + text);
    const QString what = query.isEmpty()
                             ? QStringLiteral("r/%1 (%2)").arg(sub, sort)
                             : QStringLiteral("search \"%1\" in r/%2 (%3)").arg(query, sub, sort);
    return QStringLiteral("No posts found for %1.\nThe site returned: \"%2\" at %3\n%4%5")
        .arg(what, title, page.resp.url.toString(), text.left(200), saved);
}

// listing ---------------------------------------------------------------------
namespace {

struct RawPost {
    QString fullname, title, author, score, permalink;
    int comments = 0;
    bool stickied = false;
};

int firstNumber(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("[\\d,]+"));
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured().remove(QLatin1Char(',')).toInt() : 0;
}

QString commentsId(const QString &href)
{
    static const QRegularExpression re(QStringLiteral("/comments/([a-z0-9]+)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(href);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Same job as _PARSE_JS in scraper.py: the posts on a listing + the next-page link.
QList<RawPost> parseListing(const html::Node &doc, const QString &mode, const QUrl &pageUrl, QString *next)
{
    QList<RawPost> out;
    next->clear();
    if (mode == QLatin1String("old")) {
        for (const html::Node *el : doc.select(QStringLiteral("#siteTable div.thing[data-fullname^=\"t3_\"]"))) {
            if (el->hasClass(QStringLiteral("promoted")) || el->attr(QStringLiteral("data-promoted")) == QLatin1String("true"))
                continue;
            const html::Node *titleA = el->selectOne(QStringLiteral("a.title"));
            if (!titleA)
                continue;
            RawPost p;
            p.fullname = el->attr(QStringLiteral("data-fullname"));
            p.title = titleA->textContent().trimmed();
            p.author = el->attr(QStringLiteral("data-author"), QStringLiteral("[deleted]"));
            const html::Node *sc = el->selectOne(QStringLiteral(".score.unvoted, .score.likes, .score.dislikes"));
            p.score = sc ? sc->textContent() : QString();
            if (p.score.isEmpty())
                p.score = el->attr(QStringLiteral("data-score"));
            p.comments = el->attr(QStringLiteral("data-comments-count"), QStringLiteral("0")).toInt();
            p.permalink = el->attr(QStringLiteral("data-permalink"));
            p.stickied = el->hasClass(QStringLiteral("stickied"));
            out.append(p);
        }
        // search results use a different markup than normal listings
        for (const html::Node *el : doc.select(QStringLiteral("#siteTable div.search-result-link"))) {
            const html::Node *a = el->selectOne(QStringLiteral("a.search-title"));
            if (!a)
                continue;
            const QString href = a->attr(QStringLiteral("href"));
            const QString id = commentsId(href);
            if (id.isEmpty())
                continue;
            const html::Node *sc = el->selectOne(QStringLiteral(".search-score"));
            const html::Node *cm = el->selectOne(QStringLiteral(".search-comments"));
            const html::Node *au = el->selectOne(QStringLiteral(".search-author .author, .search-author a"));
            RawPost p;
            p.fullname = QStringLiteral("t3_") + id;
            p.title = a->textContent().trimmed();
            p.author = au ? au->textContent().trimmed() : QStringLiteral("[deleted]");
            p.score = sc ? sc->textContent().trimmed() : QString();
            p.comments = firstNumber(cm ? cm->textContent() : QStringLiteral("0"));
            p.permalink = pageUrl.resolved(QUrl(href)).path();
            out.append(p);
        }
        if (const html::Node *n = doc.selectOne(QStringLiteral("span.next-button a")))
            *next = pageUrl.resolved(QUrl(n->attr(QStringLiteral("href")))).toString();
    } else {
        // Redlib-style frontends
        for (const html::Node *el : doc.select(QStringLiteral("div.post"))) {
            if (el->hasClass(QStringLiteral("highlighted")))
                continue;
            const html::Node *cm = el->selectOne(QStringLiteral("a.post_comments"));
            // The first link in the title can be the flair, so prefer the one
            // that points at the comments page.
            const html::Node *ta = nullptr;
            for (const html::Node *a : el->select(QStringLiteral(".post_title a, h2 a"))) {
                if (!ta)
                    ta = a;
                if (!commentsId(a->attr(QStringLiteral("href"))).isEmpty()) {
                    ta = a;
                    break;
                }
            }
            QString perma;
            for (const html::Node *a : {cm, ta}) {
                if (a && !commentsId(a->attr(QStringLiteral("href"))).isEmpty()) {
                    perma = a->attr(QStringLiteral("href"));
                    break;
                }
            }
            if (perma.isEmpty())
                continue;
            const html::Node *sc = el->selectOne(QStringLiteral(".post_score"));
            const html::Node *au = el->selectOne(QStringLiteral(".post_author"));
            const QString cmText = cm ? cm->attr(QStringLiteral("title"), cm->textContent()) : QStringLiteral("0");
            RawPost p;
            p.fullname = QStringLiteral("t3_") + commentsId(perma);
            p.title = ta ? ta->textContent().trimmed() : QString();
            p.author = au ? au->textContent().trimmed().remove(QRegularExpression(QStringLiteral("^u/")))
                          : QStringLiteral("[deleted]");
            p.score = sc ? sc->attr(QStringLiteral("title"), sc->textContent()).trimmed() : QString();
            p.comments = firstNumber(cmText);
            p.permalink = perma;
            p.stickied = el->selectOne(QStringLiteral(".stickied")) || el->hasClass(QStringLiteral("stickied"));
            out.append(p);
        }
        static const QRegularExpression nextRe(QStringLiteral("next"), QRegularExpression::CaseInsensitiveOption);
        for (const html::Node *a : doc.select(QStringLiteral("a[href*=\"after=\"]"))) {
            if (nextRe.match(a->textContent()).hasMatch()) {
                *next = pageUrl.resolved(QUrl(a->attr(QStringLiteral("href")))).toString();
                break;
            }
        }
    }
    return out;
}

} // namespace

QList<Post> RedditScraper::fetchPosts(const QString &subIn, const QString &sort, const QString &timeFilter,
                                      int count, const ProgressFn &progress, const QString &query)
{
    const QString sub = cleanSubreddit(subIn);
    QList<Post> posts = siteModeName == QLatin1String("api")
                            ? fetchPostsApi(sub, sort, timeFilter, count, progress, query)
                            : fetchPostsHtml(sub, sort, timeFilter, count, progress, query);
    if (posts.isEmpty())
        throw ScrapeError(QStringLiteral("No posts found for r/%1 (%2).").arg(sub, sort));
    return posts;
}

QList<Post> RedditScraper::fetchPostsHtml(const QString &sub, const QString &sort, const QString &tf,
                                          int count, const ProgressFn &progress, const QString &query)
{
    QList<Post> posts;
    QSet<QString> seen;
    QString url = listingUrl(opt.base, siteModeName, sub, sort, tf, qMin(100, qMax(count, 25)), QString(), query);
    const QString ready = siteModeName == QLatin1String("old") ? QStringLiteral("#siteTable")
                                                                : QStringLiteral("div.post, #error, #posts");
    while (!url.isEmpty() && posts.size() < count) {
        log(QStringLiteral("Loading listing: ") + url);
        const Loaded page = loadHtml(url, ready);
        QString next;
        const QList<RawPost> found = parseListing(*page.doc, siteModeName, page.resp.url, &next);
        if (found.isEmpty() && posts.isEmpty())
            throw ScrapeError(diagnose(page, sub, sort, query));
        if (found.isEmpty())
            break;
        for (const RawPost &p : found) {
            if (seen.contains(p.fullname) || p.permalink.isEmpty())
                continue;
            seen.insert(p.fullname);
            Post post;
            post.rank = posts.size() + 1;
            post.fullname = p.fullname;
            post.title = p.title;
            post.author = p.author;
            post.score = p.score.trimmed().isEmpty() ? QStringLiteral("?") : p.score.trimmed();
            post.comments = p.comments;
            post.url = p.permalink.startsWith(QLatin1String("http")) ? p.permalink : opt.base + p.permalink;
            post.stickied = p.stickied;
            posts.append(post);
            if (posts.size() >= count)
                break;
        }
        if (progress)
            progress(posts.size(), count);
        url = next;
        if (!url.isEmpty() && posts.size() < count)
            sleep(opt.delay);
    }
    return posts;
}

// one post page (HTML sites) -------------------------------------------------
namespace {

QString titleOrText(const html::Node *n)
{
    return n ? n->attr(QStringLiteral("title"), n->textContent()).simplified() : QString();
}

Comment noteComment(int depth, const QString &text)
{
    Comment c;
    c.depth = depth;
    c.note = true;
    c.bodyHtml = html::escape(text);
    return c;
}

void addRedlibComment(const html::Node *c, int depth, PostPage &pg, const QUrl &base, const html::ImageFn &img)
{
    const QString stop = QStringLiteral("blockquote.replies");
    Comment cm;
    cm.depth = depth;
    if (const html::Node *a = c->selectOwnOne(QStringLiteral("a.comment_author, .comment_author"), stop))
        cm.author = a->textContent().trimmed().remove(QRegularExpression(QStringLiteral("^u/")));
    if (cm.author.isEmpty())
        cm.author = QStringLiteral("[deleted]");
    cm.score = titleOrText(c->selectOwnOne(QStringLiteral(".comment_score"), stop));
    cm.created = titleOrText(c->selectOwnOne(QStringLiteral(".created"), stop));
    cm.bodyHtml = html::toSafeHtml(c->selectOwnOne(QStringLiteral(".comment_body"), stop), base, img);
    pg.comments.append(cm);

    // Its own replies block is the first one in document order.
    if (const html::Node *replies = c->selectOne(stop)) {
        for (const html::Node *child : replies->childElements()) {
            if (child->hasClass(QStringLiteral("comment")))
                addRedlibComment(child, depth + 1, pg, base, img);
            else if (child->matches(QStringLiteral("a.deeper_replies")) || child->selectOne(QStringLiteral("a.deeper_replies")))
                pg.comments.append(noteComment(depth + 1, QStringLiteral("More replies on the site")));
        }
    }
}

bool parseRedlibPost(const html::Node &doc, PostPage &pg, const QUrl &base)
{
    const html::Node *p = doc.selectOne(QStringLiteral("div.post.highlighted"));
    if (!p)
        p = doc.selectOne(QStringLiteral("div.post"));
    if (!p)
        return false;
    const html::ImageFn img = [&pg](const QString &src, const QString &) { return imageRef(pg, src); };

    if (const html::Node *s = p->selectOne(QStringLiteral(".post_subreddit")))
        pg.subreddit = s->textContent().trimmed().remove(QRegularExpression(QStringLiteral("^r/")));
    pg.created = titleOrText(p->selectOne(QStringLiteral(".post_header .created")));
    if (const html::Node *u = doc.selectOne(QStringLiteral("#post_url")))
        pg.linkUrl = base.resolved(QUrl(u->attr(QStringLiteral("href")))).toString();

    QSet<QString> seen;
    for (const html::Node *m : p->select(QStringLiteral(
             ".post_media_content img, .post_media_content image, .gallery img, .post_media_content video"))) {
        QString src = m->name == QLatin1String("video") ? m->attr(QStringLiteral("poster"))
                                                         : m->attr(QStringLiteral("src"), m->attr(QStringLiteral("href")));
        if (src.isEmpty())
            continue;
        src = base.resolved(QUrl(src)).toString();
        if (seen.contains(src))
            continue;
        seen.insert(src);
        pg.mediaHtml += imageRef(pg, src) + QStringLiteral("<br>");
    }
    if (pg.linkUrl.isEmpty())
        if (const html::Node *v = p->selectOne(QStringLiteral(".post_media_content video source, .post_media_content video")))
            if (!v->attr(QStringLiteral("src")).isEmpty())
                pg.linkUrl = base.resolved(QUrl(v->attr(QStringLiteral("src")))).toString();

    pg.bodyHtml = html::toSafeHtml(p->selectOne(QStringLiteral(".post_body")), base, img);
    for (const html::Node *c : doc.select(QStringLiteral("div.thread > div.comment")))
        addRedlibComment(c, 0, pg, base, img);
    return true;
}

const html::Node *childWithClass(const html::Node *n, const QString &cls)
{
    if (n)
        for (const html::Node *c : n->childElements())
            if (c->hasClass(cls))
                return c;
    return nullptr;
}

void addOldComment(const html::Node *c, int depth, PostPage &pg, const QUrl &base, const html::ImageFn &img)
{
    if (c->hasClass(QStringLiteral("morechildren"))) {
        pg.comments.append(noteComment(depth, c->innerText().isEmpty() ? QStringLiteral("More comments on the site")
                                                                        : c->innerText()));
        return;
    }
    if (!c->hasClass(QStringLiteral("comment")))
        return;
    Comment cm;
    cm.depth = depth;
    cm.author = c->attr(QStringLiteral("data-author"), QStringLiteral("[deleted]"));
    if (const html::Node *entry = childWithClass(c, QStringLiteral("entry"))) {
        if (const html::Node *sc = entry->selectOne(QStringLiteral(".tagline .score.unvoted")))
            cm.score = sc->attr(QStringLiteral("title"), sc->textContent()).trimmed();
        else if (entry->selectOne(QStringLiteral(".tagline .score-hidden")))
            cm.score = QStringLiteral("[score hidden]");
        cm.created = titleOrText(entry->selectOne(QStringLiteral(".tagline time")));
        cm.bodyHtml = html::toSafeHtml(entry->selectOne(QStringLiteral(".usertext-body .md")), base, img);
    }
    pg.comments.append(cm);
    const html::Node *list = childWithClass(childWithClass(c, QStringLiteral("child")), QStringLiteral("sitetable"));
    if (list)
        for (const html::Node *child : list->childElements())
            if (child->hasClass(QStringLiteral("thing")))
                addOldComment(child, depth + 1, pg, base, img);
}

bool parseOldPost(const html::Node &doc, PostPage &pg, const QUrl &base)
{
    const html::Node *t = doc.selectOne(QStringLiteral("#siteTable div.thing.link"));
    if (!t)
        return false;
    const html::ImageFn img = [&pg](const QString &src, const QString &) { return imageRef(pg, src); };
    pg.subreddit = t->attr(QStringLiteral("data-subreddit"));
    pg.created = titleOrText(t->selectOne(QStringLiteral("p.tagline time")));
    const QString dataUrl = t->attr(QStringLiteral("data-url"));
    if (!dataUrl.isEmpty() && !dataUrl.contains(QLatin1String("/comments/"))) {
        pg.linkUrl = base.resolved(QUrl(dataUrl)).toString();
        static const QRegularExpression imageRe(QStringLiteral("\\.(jpe?g|png|gif)(\\?|$)|//i\\.redd\\.it/"),
                                                QRegularExpression::CaseInsensitiveOption);
        if (imageRe.match(pg.linkUrl).hasMatch())
            pg.mediaHtml = imageRef(pg, pg.linkUrl);
    }
    pg.bodyHtml = html::toSafeHtml(t->selectOne(QStringLiteral(".expando .usertext-body .md")), base, img);
    if (const html::Node *table = doc.selectOne(QStringLiteral(".commentarea > .sitetable")))
        for (const html::Node *c : table->childElements())
            if (c->hasClass(QStringLiteral("thing")))
                addOldComment(c, 0, pg, base, img);
    return true;
}

} // namespace

PostPage RedditScraper::loadPostHtml(const Post &post, bool manyComments)
{
    QString url = post.url;
    if (manyComments && siteModeName == QLatin1String("old"))
        url += (url.contains(QLatin1Char('?')) ? QStringLiteral("&") : QStringLiteral("?")) + QStringLiteral("limit=500");
    const QString ready = siteModeName == QLatin1String("old") ? QStringLiteral(".commentarea")
                                                                : QStringLiteral("div.post, #error");
    const Loaded page = loadHtml(url, ready);

    PostPage pg;
    pg.title = post.title;
    pg.author = post.author;
    pg.score = post.score;
    pg.numComments = post.comments;
    pg.pageUrl = page.resp.url.toString();
    const bool ok = siteModeName == QLatin1String("old") ? parseOldPost(*page.doc, pg, page.resp.url)
                                                          : parseRedlibPost(*page.doc, pg, page.resp.url);
    if (!ok)
        throw ScrapeError(QStringLiteral("Couldn't find the post on %1").arg(pg.pageUrl));
    return pg;
}

// pdf -------------------------------------------------------------------------
void RedditScraper::downloadImages(PostPage &page)
{
    const int maxImages = 40;
    const qint64 maxBytes = 15 * 1024 * 1024;
    int failed = 0;
    page.images.clear();
    for (int i = 0; i < page.imageUrls.size(); ++i) {
        QImage img;
        if (i < maxImages) {
            const HttpResponse r = http->get(QUrl(page.imageUrls[i]), {}, 30000, maxBytes);
            if (r.status == 200)
                img.loadFromData(r.body);
        }
        if (img.isNull())
            ++failed;
        page.images.append(img);
    }
    if (failed)
        log(QStringLiteral("  %1 of %2 image(s) couldn't be embedded; linked instead.")
                .arg(failed).arg(page.imageUrls.size()));
}

QString RedditScraper::savePdf(const Post &post, const QString &outDir, const SaveOptions &o)
{
    if (!QDir().mkpath(outDir))
        throw ScrapeError(QStringLiteral("Can't create folder %1").arg(outDir));
    const QString dest = QDir(outDir).filePath(pdfFileName(post));
    PostPage page = siteModeName == QLatin1String("api") ? loadPostApi(post, o.manyComments)
                                                           : loadPostHtml(post, o.manyComments);
    if (o.images)
        downloadImages(page);
    checkCancel();
    renderPostPdf(page, dest, o.paper, o.headerFooter);
    return dest;
}
