#pragma once
// scraper.h - core logic for the Reddit -> PDF tool (Qt port of scraper.py).
//
// No browser engine: pages come over plain HTTP and PDFs are laid out by Qt
// (QTextDocument + QPdfWriter). Three ways to read Reddit:
//   "api"    - Reddit's official OAuth API (needs your own client ID)
//   "redlib" - a Redlib-style frontend, e.g. a self-hosted Redlib
//   "old"    - old.reddit.com (now needs a logged-in session cookie)
//
// Public API:
//   RedditScraper(options, log, cancel)
//       .fetchPosts(sub, sort, timeFilter, count, progress, query)
//       .savePdf(post, outDir, saveOptions)

#include "errors.h"

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

inline const QString APP_VERSION = QStringLiteral("1.0");

inline const QString API_SITE = QStringLiteral("reddit-api");
inline const QString DEFAULT_BASE = API_SITE;
inline const QStringList SITE_PRESETS = {
    API_SITE, "http://localhost:8080", "https://old.reddit.com", "https://safereddit.com",
};

inline const QStringList SORTS = {"hot", "new", "top", "rising", "controversial"};
inline const QStringList TIME_FILTERS = {"hour", "day", "week", "month", "year", "all"};
inline const QStringList TIMED_SORTS = {"top", "controversial"};
inline const QStringList SEARCH_SORTS = {"relevance", "hot", "top", "new", "comments"};

// Sent to Redlib / old.reddit (HTML sites).
inline const QString USER_AGENT = QStringLiteral(
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
// Reddit's API rules ask for a unique, descriptive User-Agent.
inline const QString API_USER_AGENT =
    QStringLiteral("qt6:RedditPDF:%1 (Qt port of RedditPDFScraperForClaude)").arg(APP_VERSION);

struct Post {
    int rank = 0;           // 1-based position in the listing
    QString fullname;       // e.g. t3_abc123
    QString title;
    QString author;
    QString score;
    int comments = 0;
    QString url;            // comments page on the chosen site
    bool stickied = false;

    QString id() const;
};

struct Comment {
    int depth = 0;
    QString author, score, created;
    QString bodyHtml;       // safe HTML (see html::toSafeHtml)
    bool note = false;      // "N more replies" placeholder rather than a comment
};

// Everything the PDF needs about one post.
struct PostPage {
    QString title, subreddit, author, score, created;
    int numComments = 0;
    QString pageUrl;        // shown in the footer
    QString linkUrl;        // external link of a link post
    QString mediaHtml;      // post images, as <img src="img:N"> placeholders
    QString bodyHtml;       // self text
    QList<Comment> comments;
    QStringList imageUrls;  // what img:N refers to
    QList<QImage> images;   // downloaded imageUrls (null image = not loaded)
};

struct ScraperOptions {
    QString base = DEFAULT_BASE;
    QString mode;                 // "", "api", "old" or "redlib"; "" = from base
    double delay = 1.5;           // pause between page loads (seconds)
    QString userAgent;            // "" = USER_AGENT / API_USER_AGENT
    double readyTimeout = 45.0;   // keep retrying a page this long for its content
    QString debugDir;             // "" = <program folder>/debug
    QString clientId;             // Reddit API app; "" = built-in (reddit_credentials.pri)
    QString clientSecret;         // empty for an "installed app"
    QString cookies;              // extra "a=1; b=2" for HTML sites
};

struct SaveOptions {
    QString paper = QStringLiteral("Letter");   // Letter | A4
    bool headerFooter = true;     // date / title / URL / page number
    bool manyComments = false;    // up to 500 comments (old.reddit and API)
    bool images = true;           // download and embed post images
};

// helpers ------------------------------------------------------------------
QString cleanSubreddit(const QString &name);              // throws ScrapeError
QString safeFilename(const QString &text, int maxLen = 90);
QString siteMode(const QString &base);                    // "api" | "old" | "redlib"
QString normalizeSite(const QString &site);               // adds https://, trims '/'
QString listingUrl(const QString &base, const QString &mode, const QString &sub,
                   const QString &sort, const QString &timeFilter, int limit = 100,
                   const QString &after = QString(), const QString &query = QString());
QString runTag(const QString &sort, const QString &timeFilter, const QString &query = QString());
QString pdfFileName(const Post &post);
// k=v&k=v with every value fully percent-encoded (spaces as '+').
QString encodeQuery(const QList<QPair<QString, QString>> &items);
// True when reddit_credentials.pri supplied API credentials at build time.
bool hasBuiltinCredentials();

class QJsonDocument;
class SyncHttp;
struct HttpResponse;

class RedditScraper {
public:
    using LogFn = std::function<void(const QString &)>;
    using ProgressFn = std::function<void(int, int)>;

    explicit RedditScraper(const ScraperOptions &options, LogFn log = {},
                           const std::atomic_bool *cancel = nullptr);
    ~RedditScraper();

    QList<Post> fetchPosts(const QString &sub, const QString &sort, const QString &timeFilter,
                           int count, const ProgressFn &progress = {},
                           const QString &query = QString());
    // Returns the path of the written PDF.
    QString savePdf(const Post &post, const QString &outDir, const SaveOptions &options);

    double delay() const { return opt.delay; }
    QString mode() const { return siteModeName; }
    QString base() const { return opt.base; }
    void sleep(double seconds);   // interruptible by the cancel flag

private:
    struct Loaded;

    void log(const QString &msg) const;
    void checkCancel() const;
    HttpResponse getWithRetry(const QUrl &url, const QList<QPair<QByteArray, QByteArray>> &headers);

    // HTML sites (scraper.cpp)
    Loaded loadHtml(const QString &url, const QString &ready);
    void checkBlocked(const Loaded &page);
    QString diagnose(const Loaded &page, const QString &sub, const QString &sort, const QString &query);
    QString saveDebug(const Loaded &page);
    QList<Post> fetchPostsHtml(const QString &sub, const QString &sort, const QString &tf,
                               int count, const ProgressFn &progress, const QString &query);
    PostPage loadPostHtml(const Post &post, bool manyComments);

    // Reddit API (redditapi.cpp)
    void ensureToken();
    QJsonDocument apiGet(const QString &path, const QList<QPair<QString, QString>> &params);
    QList<Post> fetchPostsApi(const QString &sub, const QString &sort, const QString &tf,
                              int count, const ProgressFn &progress, const QString &query);
    PostPage loadPostApi(const Post &post, bool manyComments);

    void downloadImages(PostPage &page);

    ScraperOptions opt;
    QString siteModeName;
    LogFn logFn;
    const std::atomic_bool *cancel;
    std::unique_ptr<SyncHttp> http;
    QByteArray token;
    qint64 tokenExpiresMs = 0;
};

// Adds `url` to page.imageUrls (once) and returns its <img src="img:N"> tag.
QString imageRef(PostPage &page, const QString &url);
