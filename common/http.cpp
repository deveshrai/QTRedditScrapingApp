#include "http.h"

#include "errors.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

QByteArray HttpResponse::header(const QByteArray &name) const
{
    for (const auto &h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0)
            return h.second;
    return {};
}

SyncHttp::SyncHttp(const QString &ua, const std::atomic_bool *cancelFlag)
    : userAgent(ua), cancel(cancelFlag)
{
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

QNetworkRequest SyncHttp::request(const QUrl &url, const Headers &headers) const
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    return req;
}

HttpResponse SyncHttp::get(const QUrl &url, const Headers &headers, int timeoutMs, qint64 maxBytes)
{
    if (cancel && *cancel)
        throw Cancelled();
    return finish(nam.get(request(url, headers)), timeoutMs, maxBytes);
}

HttpResponse SyncHttp::post(const QUrl &url, const QByteArray &body, const Headers &headers, int timeoutMs)
{
    if (cancel && *cancel)
        throw Cancelled();
    return finish(nam.post(request(url, headers), body), timeoutMs, -1);
}

HttpResponse SyncHttp::finish(QNetworkReply *reply, int timeoutMs, qint64 maxBytes)
{
    bool cancelled = false, timedOut = false, tooBig = false;
    QElapsedTimer clock;
    clock.start();

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(100);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancel && *cancel) {
            cancelled = true;
            reply->abort();
        } else if (clock.elapsed() > timeoutMs) {
            timedOut = true;
            reply->abort();
        } else if (maxBytes > 0 && reply->bytesAvailable() > maxBytes) {
            tooBig = true;
            reply->abort();
        }
    });
    poll.start();
    if (!reply->isFinished())
        loop.exec();
    poll.stop();

    HttpResponse r;
    r.url = reply->url();
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.headers = reply->rawHeaderPairs();
    if (!cancelled && !timedOut && !tooBig)
        r.body = reply->readAll();
    if (timedOut)
        r.error = QStringLiteral("Timeout after %1 s").arg(timeoutMs / 1000);
    else if (tooBig)
        r.error = QStringLiteral("Response larger than %1 bytes").arg(maxBytes);
    else if (r.status == 0)
        r.error = reply->errorString();
    if (timedOut || tooBig)
        r.status = 0;
    delete reply;

    if (cancelled)
        throw Cancelled();
    return r;
}

void SyncHttp::setCookie(const QString &name, const QString &value, const QUrl &forUrl)
{
    QNetworkCookie c(name.toUtf8(), value.toUtf8());
    c.setPath(QStringLiteral("/"));
    nam.cookieJar()->setCookiesFromUrl({c}, forUrl);
}

void SyncHttp::addCookieHeader(const QString &cookies, const QUrl &forUrl)
{
    for (const QString &part : cookies.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        setCookie(part.left(eq).trimmed(), part.mid(eq + 1).trimmed(), forUrl);
    }
}
