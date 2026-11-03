#include "http.h"

#include "errors.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#ifdef Q_OS_WASM
#include <emscripten/fetch.h>
#include <vector>

// QNetworkAccessManager doesn't work from a QThread in the browser. The scraper
// runs on a worker thread and wants blocking calls anyway, which is exactly what
// Emscripten's synchronous fetch offers there. Browsers forbid setting User-Agent /
// Accept-Language and follow redirects themselves; cancelling mid-request isn't possible
// (the flag is checked between requests; timeouts are enforced by the fetch).
static HttpResponse wasmFetch(const char *method, const QUrl &url, const QByteArray &body,
                              const SyncHttp::Headers &headers, int timeoutMs, qint64 maxBytes)
{
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    qstrcpy(attr.requestMethod, method);
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
    attr.timeoutMSecs = timeoutMs;

    std::vector<QByteArray> store;
    for (const auto &h : headers) {
        store.push_back(h.first);
        store.push_back(h.second);
    }
    std::vector<const char *> hdrs;
    for (const QByteArray &b : store)
        hdrs.push_back(b.constData());
    hdrs.push_back(nullptr);
    attr.requestHeaders = hdrs.data();
    if (!body.isEmpty()) {
        attr.requestData = body.constData();
        attr.requestDataSize = size_t(body.size());
    }

    const QByteArray u = url.toEncoded();
    emscripten_fetch_t *f = emscripten_fetch(&attr, u.constData());

    HttpResponse r;
    r.url = url;
    r.status = f->status;
    if (f->status != 0) {
        r.body = QByteArray(f->data, int(f->numBytes));
        const size_t n = emscripten_fetch_get_response_headers_length(f);
        QByteArray raw(int(n) + 1, '\0');
        emscripten_fetch_get_response_headers(f, raw.data(), n + 1);
        for (const QByteArray &line : raw.split('\n')) {
            const int colon = line.indexOf(':');
            if (colon > 0)
                r.headers.append({line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
        }
    }
    emscripten_fetch_close(f);

    if (r.status == 0) {
        r.error = QStringLiteral("Network error (blocked by the browser, offline, or timed out)");
    } else if (maxBytes > 0 && r.body.size() > maxBytes) {
        r.status = 0;
        r.body.clear();
        r.error = QStringLiteral("Response larger than %1 bytes").arg(maxBytes);
    }
    return r;
}
#endif

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
#ifndef Q_OS_WASM   // forbidden header names in browsers
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    req.setRawHeader("Accept-Language", "en-US,en;q=0.9");
#endif
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    return req;
}

HttpResponse SyncHttp::get(const QUrl &url, const Headers &headers, int timeoutMs, qint64 maxBytes)
{
    if (cancel && *cancel)
        throw Cancelled();
#ifdef Q_OS_WASM
    return wasmFetch("GET", url, {}, headers, timeoutMs, maxBytes);
#else
    return finish(nam.get(request(url, headers)), timeoutMs, maxBytes);
#endif
}

HttpResponse SyncHttp::post(const QUrl &url, const QByteArray &body, const Headers &headers, int timeoutMs)
{
    if (cancel && *cancel)
        throw Cancelled();
#ifdef Q_OS_WASM
    return wasmFetch("POST", url, body, headers, timeoutMs, -1);
#else
    return finish(nam.post(request(url, headers), body), timeoutMs, -1);
#endif
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
