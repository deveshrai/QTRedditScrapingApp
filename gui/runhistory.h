#pragma once
// runhistory.h - the last few "batch" requests (e.g. 100 new from r/python), kept in QSettings
// so they can be ticked and re-run together later.

#include "scraper.h"

#include <QDateTime>
#include <QList>
#include <QSettings>
#include <QString>

struct RunPattern {
    QString sub, sort, timeFilter, query;
    int from = 1, to = 25;        // post range of the batch
    QDateTime lastUsed;

    // The time filter only matters for timed sorts and searches; ignore it otherwise.
    QString key() const
    {
        const bool timed = !query.isEmpty() || TIMED_SORTS.contains(sort);
        return QStringList{sub.toLower(), sort, timed ? timeFilter : QString(), query.trimmed().toLower(),
                           QString::number(from), QString::number(to)}.join(QLatin1Char('|'));
    }
};

namespace runhistory {

inline constexpr int kMax = 10;

inline QList<RunPattern> load()
{
    QList<RunPattern> out;
    QSettings s;
    const int n = s.beginReadArray(QStringLiteral("history"));
    for (int i = 0; i < n && out.size() < kMax; ++i) {
        s.setArrayIndex(i);
        RunPattern p;
        p.sub = s.value(QStringLiteral("sub")).toString();
        p.sort = s.value(QStringLiteral("sort")).toString();
        p.timeFilter = s.value(QStringLiteral("time")).toString();
        p.query = s.value(QStringLiteral("query")).toString();
        p.from = s.value(QStringLiteral("from"), 1).toInt();
        p.to = s.value(QStringLiteral("to"), 25).toInt();
        p.lastUsed = s.value(QStringLiteral("last")).toDateTime();
        if (!p.sub.isEmpty() && !p.sort.isEmpty() && p.from >= 1 && p.to >= p.from)
            out.append(p);
    }
    s.endArray();
    return out;
}

inline void save(const QList<RunPattern> &list)
{
    QSettings s;
    s.beginWriteArray(QStringLiteral("history"), list.size());
    for (int i = 0; i < list.size(); ++i) {
        s.setArrayIndex(i);
        const RunPattern &p = list[i];
        s.setValue(QStringLiteral("sub"), p.sub);
        s.setValue(QStringLiteral("sort"), p.sort);
        s.setValue(QStringLiteral("time"), p.timeFilter);
        s.setValue(QStringLiteral("query"), p.query);
        s.setValue(QStringLiteral("from"), p.from);
        s.setValue(QStringLiteral("to"), p.to);
        s.setValue(QStringLiteral("last"), p.lastUsed);
    }
    s.endArray();
}

// Puts `p` at the top (stamped now), dropping an equal older entry and anything past kMax.
inline void remember(QList<RunPattern> &list, RunPattern p)
{
    p.lastUsed = QDateTime::currentDateTime();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list[i].key() == p.key())
            list.removeAt(i);
    list.prepend(p);
    while (list.size() > kMax)
        list.removeLast();
}

} // namespace runhistory
