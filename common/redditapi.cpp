// redditapi.cpp - the "api" mode of RedditScraper: Reddit's official OAuth API.
//
// Uses application-only OAuth (no Reddit login): a "script" or "web" app needs
// client ID + secret, an "installed app" only the client ID. Create one at
// https://www.reddit.com/prefs/apps.

#include "scraper.h"

#include "htmldom.h"
#include "http.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QtMath>

namespace {

// REDDITPDF_API_HOST / REDDITPDF_AUTH_HOST point the API at a test server or proxy.
const QString kApiHost = qEnvironmentVariable("REDDITPDF_API_HOST", QStringLiteral("https://oauth.reddit.com"));
const QString kAuthHost = qEnvironmentVariable("REDDITPDF_AUTH_HOST", QStringLiteral("https://www.reddit.com"));
const QString kPublicHost = QStringLiteral("https://www.reddit.com");

QString apiHelp()
{
    return QStringLiteral(
        "The Reddit API needs your own (free) app credentials.\n"
        "1. Open https://www.reddit.com/prefs/apps and choose \"create another app\".\n"
        "2. Pick \"script\" (or \"installed app\"), any name, redirect URI http://localhost:8080.\n"
        "3. Enter the client ID (the string under the app name) and the secret\n"
        "   (CLI: --client-id / --client-secret, or REDDIT_CLIENT_ID / REDDIT_CLIENT_SECRET).");
}

QString formatTime(double utcSeconds)
{
    if (utcSeconds <= 0)
        return QString();
    return QDateTime::fromSecsSinceEpoch(qint64(utcSeconds)).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString safeBody(const QString &bodyHtml, PostPage &pg)
{
    if (bodyHtml.isEmpty())
        return QString();
    const auto doc = html::parse(bodyHtml);
    return html::toSafeHtml(doc.get(), QUrl(kPublicHost),
                            [&pg](const QString &src, const QString &) { return imageRef(pg, src); });
}

void walkComments(const QJsonArray &children, int depth, PostPage &pg)
{
    for (const QJsonValue &v : children) {
        const QJsonObject child = v.toObject();
        const QString kind = child.value(QLatin1String("kind")).toString();
        const QJsonObject d = child.value(QLatin1String("data")).toObject();
        if (kind == QLatin1String("more")) {
            const int n = d.value(QLatin1String("count")).toInt();
            Comment note;
            note.depth = depth;
            note.note = true;
            note.bodyHtml = n > 0 ? QStringLiteral("%1 more repl%2 not loaded").arg(n).arg(n == 1 ? "y" : "ies")
                                  : QStringLiteral("Thread continues on Reddit");
            pg.comments.append(note);
            continue;
        }
        if (kind != QLatin1String("t1"))
            continue;
        Comment c;
        c.depth = depth;
        c.author = d.value(QLatin1String("author")).toString(QStringLiteral("[deleted]"));
        c.score = d.value(QLatin1String("score_hidden")).toBool()
                      ? QStringLiteral("[score hidden]")
                      : QString::number(d.value(QLatin1String("score")).toInteger());
        c.created = formatTime(d.value(QLatin1String("created_utc")).toDouble());
        c.bodyHtml = safeBody(d.value(QLatin1String("body_html")).toString(), pg);
        pg.comments.append(c);
        const QJsonValue replies = d.value(QLatin1String("replies"));   // "" when there are none
        if (replies.isObject())
            walkComments(replies.toObject().value(QLatin1String("data")).toObject()
                             .value(QLatin1String("children")).toArray(),
                         depth + 1, pg);
    }
}

} // namespace

void RedditScraper::ensureToken()
{
    if (!token.isEmpty() && QDateTime::currentMSecsSinceEpoch() < tokenExpiresMs)
        return;
    if (opt.clientId.trimmed().isEmpty())
        throw ScrapeError(apiHelp());

    QList<QPair<QString, QString>> form;
    if (opt.clientSecret.isEmpty())
        form = {{"grant_type", "https://oauth.reddit.com/grants/installed_client"},
                {"device_id", "DO_NOT_TRACK_THIS_DEVICE"}};
    else
        form = {{"grant_type", "client_credentials"}};
    const QByteArray basic = (opt.clientId.trimmed() + QLatin1Char(':') + opt.clientSecret.trimmed()).toUtf8().toBase64();

    log(QStringLiteral("Getting a Reddit API token..."));
    const HttpResponse r = http->post(QUrl(kAuthHost + QStringLiteral("/api/v1/access_token")),
                                      encodeQuery(form).toUtf8(),
                                      {{"Authorization", "Basic " + basic},
                                       {"Content-Type", "application/x-www-form-urlencoded"}});
    if (r.status == 0)
        throw ScrapeError(QStringLiteral("Couldn't reach Reddit to get an API token (%1).").arg(r.error));
    if (r.status == 401 || r.status == 403)
        throw ScrapeError(QStringLiteral("Reddit rejected the API client ID/secret (HTTP %1).\n").arg(r.status) + apiHelp());
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    token = o.value(QLatin1String("access_token")).toString().toUtf8();
    if (token.isEmpty()) {
        const QString err = o.value(QLatin1String("error")).toString();
        throw ScrapeError(QStringLiteral("Reddit didn't give an API token (HTTP %1%2).\n")
                              .arg(QString::number(r.status), err.isEmpty() ? QString() : QStringLiteral(", ") + err)
                          + apiHelp());
    }
    const int expires = o.value(QLatin1String("expires_in")).toInt(3600);
    tokenExpiresMs = QDateTime::currentMSecsSinceEpoch() + qMax(60, expires - 60) * 1000LL;
}

QJsonDocument RedditScraper::apiGet(const QString &path, const QList<QPair<QString, QString>> &params)
{
    QList<QPair<QString, QString>> q = params;
    q.append(qMakePair(QStringLiteral("raw_json"), QStringLiteral("1")));
    const QUrl url(kApiHost + path + QLatin1Char('?') + encodeQuery(q));
    for (int attempt = 0; attempt < 2; ++attempt) {
        ensureToken();
        const HttpResponse r = getWithRetry(url, {{"Authorization", "bearer " + token}});
        if (r.status == 401 && attempt == 0) {   // token expired early: get a new one
            token.clear();
            continue;
        }
        if (r.status == 403)
            throw ScrapeError(QStringLiteral("HTTP 403 for %1 (private, quarantined or banned subreddit?)").arg(url.toString()));
        if (r.status != 200)
            throw ScrapeError(QStringLiteral("HTTP %1 for %2").arg(QString::number(r.status), url.toString()));

        // Stay inside Reddit's rate limit (about 100 requests a minute).
        bool okRemaining = false, okReset = false;
        const double remaining = r.header("x-ratelimit-remaining").toDouble(&okRemaining);
        const double reset = r.header("x-ratelimit-reset").toDouble(&okReset);
        if (okRemaining && okReset && remaining < 1.0) {
            log(QStringLiteral("  API rate limit reached. Waiting %1s...").arg(qCeil(reset)));
            sleep(reset + 1);
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(r.body, &err);
        if (err.error != QJsonParseError::NoError)
            throw ScrapeError(QStringLiteral("Reddit sent something that isn't JSON for %1 (%2)").arg(url.toString(), err.errorString()));
        return doc;
    }
    throw ScrapeError(QStringLiteral("Reddit keeps rejecting the API token for %1").arg(url.toString()));
}

QList<Post> RedditScraper::fetchPostsApi(const QString &sub, const QString &sort, const QString &tf,
                                         int count, const ProgressFn &progress, const QString &query)
{
    QList<Post> posts;
    QSet<QString> seen;
    QString after;
    const QString limit = QString::number(qMin(100, qMax(count, 25)));
    const QString path = query.isEmpty() ? QStringLiteral("/r/%1/%2").arg(sub, sort)
                                         : QStringLiteral("/r/%1/search").arg(sub);
    while (posts.size() < count) {
        QList<QPair<QString, QString>> params = {{"limit", limit}};
        if (!query.isEmpty()) {
            params << qMakePair(QStringLiteral("q"), query) << qMakePair(QStringLiteral("restrict_sr"), QStringLiteral("on"))
                   << qMakePair(QStringLiteral("sort"), sort) << qMakePair(QStringLiteral("type"), QStringLiteral("link"));
            if (!tf.isEmpty() && tf != QLatin1String("all"))
                params << qMakePair(QStringLiteral("t"), tf);
        } else if (TIMED_SORTS.contains(sort)) {
            params << qMakePair(QStringLiteral("t"), tf);
        }
        if (!after.isEmpty())
            params << qMakePair(QStringLiteral("after"), after);
        log(QStringLiteral("Loading listing: %1%2?%3").arg(kApiHost, path, encodeQuery(params)));

        const QJsonObject data = apiGet(path, params).object().value(QLatin1String("data")).toObject();
        const QJsonArray children = data.value(QLatin1String("children")).toArray();
        int added = 0;
        for (const QJsonValue &v : children) {
            const QJsonObject child = v.toObject();
            if (child.value(QLatin1String("kind")).toString() != QLatin1String("t3"))
                continue;
            const QJsonObject d = child.value(QLatin1String("data")).toObject();
            const QString fullname = d.value(QLatin1String("name")).toString();
            const QString permalink = d.value(QLatin1String("permalink")).toString();
            if (fullname.isEmpty() || permalink.isEmpty() || seen.contains(fullname))
                continue;
            seen.insert(fullname);
            Post p;
            p.rank = posts.size() + 1;
            p.fullname = fullname;
            p.title = d.value(QLatin1String("title")).toString();
            p.author = d.value(QLatin1String("author")).toString(QStringLiteral("[deleted]"));
            p.score = QString::number(d.value(QLatin1String("score")).toInteger());
            p.comments = d.value(QLatin1String("num_comments")).toInt();
            p.url = kPublicHost + permalink;
            p.stickied = d.value(QLatin1String("stickied")).toBool();
            posts.append(p);
            ++added;
            if (posts.size() >= count)
                break;
        }
        if (!added && posts.isEmpty())
            throw ScrapeError(QStringLiteral("No posts found for %1 (%2) through the Reddit API.\n"
                                             "Check the subreddit name; it may also be empty or private.")
                                  .arg(query.isEmpty() ? QStringLiteral("r/") + sub
                                                       : QStringLiteral("search \"%1\" in r/%2").arg(query, sub),
                                       sort));
        if (!added)
            break;
        if (progress)
            progress(posts.size(), count);
        after = data.value(QLatin1String("after")).toString();
        if (after.isEmpty())
            break;
        if (posts.size() < count)
            sleep(opt.delay);
    }
    return posts;
}

PostPage RedditScraper::loadPostApi(const Post &post, bool manyComments)
{
    const QJsonArray arr = apiGet(QStringLiteral("/comments/") + post.id(),
                                  {{"limit", manyComments ? "500" : "200"}, {"sort", "confidence"}})
                               .array();
    const QJsonObject d = arr.at(0).toObject().value(QLatin1String("data")).toObject()
                              .value(QLatin1String("children")).toArray().at(0).toObject()
                              .value(QLatin1String("data")).toObject();
    if (d.isEmpty())
        throw ScrapeError(QStringLiteral("Reddit returned no post for %1").arg(post.url));

    PostPage pg;
    pg.title = d.value(QLatin1String("title")).toString(post.title);
    pg.author = d.value(QLatin1String("author")).toString(post.author);
    pg.score = QString::number(d.value(QLatin1String("score")).toInteger());
    pg.numComments = d.value(QLatin1String("num_comments")).toInt(post.comments);
    pg.subreddit = d.value(QLatin1String("subreddit")).toString();
    pg.created = formatTime(d.value(QLatin1String("created_utc")).toDouble());
    pg.pageUrl = post.url;

    const QString url = d.value(QLatin1String("url_overridden_by_dest")).toString(d.value(QLatin1String("url")).toString());
    if (!d.value(QLatin1String("is_self")).toBool() && !url.isEmpty())
        pg.linkUrl = url.startsWith(QLatin1Char('/')) ? kPublicHost + url : url;

    // images: gallery > direct image > preview picture
    auto addMedia = [&pg](const QString &u) { pg.mediaHtml += imageRef(pg, u) + QStringLiteral("<br>"); };
    if (d.value(QLatin1String("is_gallery")).toBool()) {
        const QJsonObject meta = d.value(QLatin1String("media_metadata")).toObject();
        for (const QJsonValue &item : d.value(QLatin1String("gallery_data")).toObject().value(QLatin1String("items")).toArray()) {
            const QJsonObject s = meta.value(item.toObject().value(QLatin1String("media_id")).toString())
                                      .toObject().value(QLatin1String("s")).toObject();
            const QString u = s.value(QLatin1String("u")).toString(s.value(QLatin1String("gif")).toString());
            if (!u.isEmpty())
                addMedia(u);
        }
    } else {
        static const QRegularExpression imageRe(QStringLiteral("\\.(jpe?g|png|gif)(\\?|$)"),
                                                QRegularExpression::CaseInsensitiveOption);
        const QString preview = d.value(QLatin1String("preview")).toObject().value(QLatin1String("images")).toArray()
                                    .at(0).toObject().value(QLatin1String("source")).toObject()
                                    .value(QLatin1String("url")).toString();
        if (!pg.linkUrl.isEmpty() && imageRe.match(pg.linkUrl).hasMatch())
            addMedia(pg.linkUrl);
        else if (!preview.isEmpty())
            addMedia(preview);
    }

    pg.bodyHtml = safeBody(d.value(QLatin1String("selftext_html")).toString(), pg);
    walkComments(arr.at(1).toObject().value(QLatin1String("data")).toObject()
                     .value(QLatin1String("children")).toArray(),
                 0, pg);
    return pg;
}
