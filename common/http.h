#pragma once
// http.h - blocking HTTP on top of QNetworkAccessManager.
//
// Each call spins a local event loop until the reply finishes, so scraper code
// can stay sequential like the Python version. Create and use a SyncHttp on a
// single thread (the GUI's worker thread, or the CLI's main thread).

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QString>
#include <QUrl>
#include <atomic>

struct HttpResponse {
    int status = 0;          // 0 = no HTTP response (network error / timeout)
    QByteArray body;
    QUrl url;                // final URL after redirects
    QString error;           // set when status == 0
    QList<QPair<QByteArray, QByteArray>> headers;

    QByteArray header(const QByteArray &name) const;
};

class SyncHttp {
public:
    using Headers = QList<QPair<QByteArray, QByteArray>>;

    SyncHttp(const QString &userAgent, const std::atomic_bool *cancel);

    // Throws Cancelled if the cancel flag is raised while waiting.
    HttpResponse get(const QUrl &url, const Headers &headers = {},
                     int timeoutMs = 60000, qint64 maxBytes = -1);
    HttpResponse post(const QUrl &url, const QByteArray &body,
                      const Headers &headers = {}, int timeoutMs = 60000);

    void setCookie(const QString &name, const QString &value, const QUrl &forUrl);
    // "a=1; b=2" as copied from a browser's Cookie header.
    void addCookieHeader(const QString &cookies, const QUrl &forUrl);

private:
    QNetworkRequest request(const QUrl &url, const Headers &headers) const;
    HttpResponse finish(class QNetworkReply *reply, int timeoutMs, qint64 maxBytes);

    QNetworkAccessManager nam;
    QString userAgent;
    const std::atomic_bool *cancel;
};
