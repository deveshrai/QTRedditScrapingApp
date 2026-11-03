#pragma once
// storezip.h - minimal "stored" (uncompressed) ZIP writer. PDFs are already
// compressed, so this is enough to hand a whole run to the browser as one download.

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace storezip {

inline quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool ready = false;
    if (!ready) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (uchar b : data)
        c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void put16(QByteArray &o, quint16 v) { o.append(char(v & 0xFF)).append(char(v >> 8)); }
inline void put32(QByteArray &o, quint32 v) { put16(o, quint16(v & 0xFFFF)); put16(o, quint16(v >> 16)); }

// Zips the regular files directly inside `dir`, stored under "<folder name>/<file>".
inline QByteArray zipFolder(const QString &dir)
{
    const QDateTime now = QDateTime::currentDateTime();
    const quint16 dosTime = quint16((now.time().hour() << 11) | (now.time().minute() << 5) | (now.time().second() / 2));
    const quint16 dosDate = quint16(((now.date().year() - 1980) << 9) | (now.date().month() << 5) | now.date().day());
    const QString prefix = QFileInfo(dir).fileName() + QLatin1Char('/');

    QByteArray out, central;
    quint16 count = 0;
    for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files, QDir::Name)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray data = f.readAll();
        const QByteArray name = (prefix + fi.fileName()).toUtf8();
        const quint32 crc = crc32(data), offset = quint32(out.size());

        put32(out, 0x04034b50); put16(out, 20); put16(out, 0x0800); put16(out, 0);
        put16(out, dosTime); put16(out, dosDate); put32(out, crc);
        put32(out, quint32(data.size())); put32(out, quint32(data.size()));
        put16(out, quint16(name.size())); put16(out, 0);
        out.append(name).append(data);

        put32(central, 0x02014b50); put16(central, 20); put16(central, 20); put16(central, 0x0800); put16(central, 0);
        put16(central, dosTime); put16(central, dosDate); put32(central, crc);
        put32(central, quint32(data.size())); put32(central, quint32(data.size()));
        put16(central, quint16(name.size())); put16(central, 0); put16(central, 0);
        put16(central, 0); put16(central, 0); put32(central, 0); put32(central, offset);
        central.append(name);
        ++count;
    }
    const quint32 centralOffset = quint32(out.size());
    out.append(central);
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, count); put16(out, count);
    put32(out, quint32(central.size())); put32(out, centralOffset); put16(out, 0);
    return out;
}

} // namespace storezip
