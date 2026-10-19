// main.cpp - command-line interface for the Reddit -> PDF tool (port of cli.py).
//
//     redditpdf-cli SUBREDDIT [options]
//
// Run with --help for every option.

#include "scraper.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>

#ifdef Q_OS_WIN
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO 0
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

const QString PROG = QStringLiteral("redditpdf-cli");
enum ExitCode { EXIT_OK = 0, EXIT_PARTIAL = 1, EXIT_USAGE = 2, EXIT_FATAL = 3, EXIT_CANCELLED = 130 };

std::atomic_bool g_cancel{false};

extern "C" void onSigint(int)
{
#ifndef Q_OS_WIN
    if (g_cancel) {
        const char msg[] = "Interrupted.\n";
        (void)!write(2, msg, sizeof msg - 1);
        _exit(EXIT_CANCELLED);
    }
    const char msg[] = "\nStopping after the current step... (Ctrl-C again to force quit)\n";
    (void)!write(2, msg, sizeof msg - 1);
#endif
    g_cancel = true;
}

// --------------------------------------------------------------------------- //
// output helpers
// --------------------------------------------------------------------------- //

void eprint(const QString &msg)
{
    QTextStream err(stderr);
    err << msg << Qt::endl;
}

void print(const QString &msg)
{
    QTextStream out(stdout);
    out << msg << Qt::endl;
}

int terminalColumns()
{
    bool ok = false;
    const int env = qEnvironmentVariableIntValue("COLUMNS", &ok);
    if (ok && env > 0)
        return env;
#ifndef Q_OS_WIN
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
#endif
    return 100;
}

[[noreturn]] void usageError(const QString &msg)
{
    eprint(QStringLiteral("usage: %1 SUBREDDIT [options]\n%1: error: %2").arg(PROG, msg));
    std::exit(EXIT_USAGE);
}

// --------------------------------------------------------------------------- //
// argument types
// --------------------------------------------------------------------------- //

int positiveInt(const QString &opt, const QString &text)
{
    bool ok = false;
    const int n = text.trimmed().toInt(&ok);
    if (!ok)
        usageError(QStringLiteral("argument %1: '%2' is not a whole number").arg(opt, text));
    if (n < 1)
        usageError(QStringLiteral("argument %1: must be 1 or more").arg(opt));
    return n;
}

double nonNegativeFloat(const QString &opt, const QString &text)
{
    bool ok = false;
    const double x = text.trimmed().toDouble(&ok);
    if (!ok)
        usageError(QStringLiteral("argument %1: '%2' is not a number").arg(opt, text));
    if (x < 0)
        usageError(QStringLiteral("argument %1: must be 0 or more").arg(opt));
    return x;
}

// '10-30' -> (10, 30);  '25' -> (1, 25).
QPair<int, int> rangeArg(const QString &opt, const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d+)\\s*(?:[-:]\\s*(\\d+))?\\s*$"));
    const auto m = re.match(text);
    if (!m.hasMatch())
        usageError(QStringLiteral("argument %1: '%2' is not a range like 10-30").arg(opt, text));
    const int a = m.captured(2).isEmpty() ? 1 : m.captured(1).toInt();
    const int b = m.captured(2).isEmpty() ? m.captured(1).toInt() : m.captured(2).toInt();
    if (a < 1 || b < a)
        usageError(QStringLiteral("argument %1: range must satisfy 1 <= START <= END").arg(opt));
    return {a, b};
}

// Parse '1,3,5-8' into sorted unique ranks; false if malformed.
bool parseRanks(const QString &text, QList<int> *ranks)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)(?:\\s*-\\s*(\\d+))?$"));
    std::set<int> out;
    for (const QString &raw : text.split(QLatin1Char(','))) {
        const QString part = raw.trimmed();
        if (part.isEmpty())
            continue;
        const auto m = re.match(part);
        if (!m.hasMatch())
            return false;
        const int a = m.captured(1).toInt();
        const int b = m.captured(2).isEmpty() ? a : m.captured(2).toInt();
        if (a < 1 || b < a || b - a > 100000)
            return false;
        for (int r = a; r <= b; ++r)
            out.insert(r);
    }
    ranks->clear();
    for (int r : out)
        ranks->append(r);
    return true;
}

// '1,3,5-8' -> [1, 3, 5, 6, 7, 8].
QList<int> ranksArg(const QString &opt, const QString &text)
{
    QList<int> ranks;
    if (!parseRanks(text, &ranks) || ranks.isEmpty())
        usageError(QStringLiteral("argument %1: '%2' is not a list like 1,3,5-8").arg(opt, text));
    return ranks;
}

QString choice(const QString &opt, const QString &value, const QStringList &choices)
{
    if (!choices.contains(value))
        usageError(QStringLiteral("argument %1: invalid choice: '%2' (choose from %3)")
                       .arg(opt, value, choices.join(QStringLiteral(", "))));
    return value;
}

// --------------------------------------------------------------------------- //
// help
// --------------------------------------------------------------------------- //

QString helpText()
{
    const QString d = DEFAULT_BASE;
    return QStringLiteral(
R"(usage: %1 SUBREDDIT [options]

Save posts from a subreddit as PDFs. Reads Reddit through its official API by
default (needs your own free app client ID); a self-hosted Redlib or
old.reddit.com (with a login cookie) also work. No browser is needed: Qt lays
out and prints the PDFs itself.

positional arguments:
  SUBREDDIT                   subreddit name: python, r/python, or a reddit.com/r/... URL

what to fetch:
  -S, --search, --query TEXT  search inside the subreddit instead of browsing it
                              (/r/<sub>/search?q=TEXT&restrict_sr=on). Quote multi-word searches
  -s, --sort SORT             browsing: %2 (default: hot).
                              With --search: %3 (default: relevance)
  -t, --time PERIOD           time window: %4. Browsing: used by top/controversial
                              (default: week). With --search: any sort (default: all)

which posts (choose at most one):
  -n, --count N               posts 1 to N (e.g. -n 25, -n 50, -n 100)
  -r, --range N1-N2           posts N1 to N2, e.g. 26-60
  -p, --pick LIST             specific post numbers, e.g. 1,3,5-8
  -i, --interactive           load a list, show it, and ask which posts to save
  --load N                    how many posts -i loads and shows (default: 50)
  -l, --list                  only print the selected posts; don't create any PDFs

site:
  --site URL                  where to read from (default: %5 = Reddit's official API);
                              or a Redlib URL, or https://old.reddit.com
  --api                       shortcut for --site %5
  --old-reddit                shortcut for --site https://old.reddit.com
  --site-mode MODE            page layout to parse: auto, api, old, redlib (default: auto -
                              'api' for %5, 'old' for old.reddit.com, 'redlib' otherwise)
  --client-id ID              Reddit API app client ID (default: $REDDIT_CLIENT_ID, then the
                              app built in from reddit_credentials.pri, if any)
  --client-secret SECRET      Reddit API app secret; leave out for an "installed app"
                              (default: $REDDIT_CLIENT_SECRET)
  --cookie STRING             Cookie header for HTML sites, e.g. a logged-in old.reddit
                              session (default: $REDDIT_COOKIE)

output:
  -o, --output DIR            folder for the PDFs (default: ~/Documents/RedditPDFs)
  --flat                      write PDFs straight into DIR instead of DIR/r_<sub>_<sort>/
  --paper {Letter,A4}         page size (default: Letter)
  --no-header-footer          leave out the date/title/URL/page-number header and footer
  --many-comments             old.reddit and API: load up to 500 comments
  --no-images                 don't download post images (they become links)
  --skip-existing             don't re-download a post whose PDF is already in the folder

network:
  --user-agent STRING         User-Agent header to send (default: a desktop Chrome for HTML
                              sites, a descriptive app name for the API)
  --wait-timeout SEC          keep re-fetching a page this long while it has no content
                              (default: 45)
  --debug-dir DIR             where to save last_listing.html when a listing comes back
                              empty or blocked (default: the 'debug' folder next to the program)

pacing:
  --delay SEC                 pause between page loads, to be polite to the site (default: 1.5)

other:
  -q, --quiet                 print only errors and the final summary
  -v, --verbose               also print every page URL as it is loaded
  --version                   show program's version number and exit
  -h, --help                  show this help and exit

examples:
  %1 python                          first 25 hot posts of r/python
  %1 python -s top -t month -n 50    top 50 posts of the month
  %1 python -s new -r 26-60          posts 26 to 60 of "new"
  %1 python -p 1,4,10-12             only posts #1, #4 and #10 through #12
  %1 python -i                       list posts, then choose which to save
  %1 python -n 100 --list            just print the top 100 titles
  %1 python -S "async await"         search r/python (relevance, all time)
  %1 python -S visiteur -s top -t year -n 20   top 20 search hits of the past year
  %1 python -n 25 -o ~/Desktop/pdfs --paper A4 --no-header-footer
  %1 python --site http://localhost:8080        a self-hosted Redlib

selecting posts:
  Use at most one of -n / -r / -p / -i. With none of them you get posts 1-25.
  Post numbers are positions in the listing (stickied posts count).

exit status:
  0  every selected post was saved (or skipped as already present)
  1  some posts failed, the rest were saved
  2  bad command line
  3  could not load the listing (blocked, wrong name, no network, ...)
  130  interrupted with Ctrl-C

browser options from the Python version (--show-browser, --browser, --chrome-path,
--window-size) are accepted but do nothing: this version has no browser.)")
        .arg(PROG, SORTS.join(QStringLiteral(", ")), SEARCH_SORTS.join(QStringLiteral(", ")),
             TIME_FILTERS.join(QStringLiteral(", ")), d);
}

// --------------------------------------------------------------------------- //
// table / interactive selection
// --------------------------------------------------------------------------- //

void printTable(const QList<Post> &posts)
{
    const int cols = terminalColumns();
    QTextStream out(stdout);
    out << QStringLiteral("%1  %2 %3  title").arg(QStringLiteral("#"), 4).arg(QStringLiteral("score"), 7).arg(QStringLiteral("cmts"), 6)
        << Qt::endl;
    bool anySticky = false;
    for (const Post &p : posts) {
        anySticky |= p.stickied;
        const QString head = QStringLiteral("%1%2 %3 %4  ")
                                 .arg(p.rank, 4)
                                 .arg(p.stickied ? QStringLiteral("*") : QStringLiteral(" "))
                                 .arg(p.score, 7)
                                 .arg(p.comments, 6);
        const QString tail = QStringLiteral("  (u/%1)").arg(p.author);
        const int room = std::max<int>(20, cols - int(head.size()) - int(tail.size()));
        const QString title = p.title.size() <= room ? p.title : p.title.left(room - 1) + QStringLiteral("…");
        out << head << title << tail << Qt::endl;
    }
    if (anySticky)
        out << "(* = stickied)" << Qt::endl;
}

QList<Post> askSelection(const QList<Post> &posts)
{
    QMap<int, Post> byRank;
    for (const Post &p : posts)
        byRank.insert(p.rank, p);
    const int top = byRank.lastKey();
    QTextStream out(stdout);
    out << Qt::endl;
    for (;;) {
        out << QStringLiteral("Which posts? e.g. 1,3,5-8 | all | q to quit  [1-%1]: ").arg(top) << Qt::flush;
        std::string line;
        if (!std::getline(std::cin, line))
            return {};
        const QString ans = QString::fromStdString(line).trimmed().toLower();
        if (ans.isEmpty() || ans == QLatin1String("q") || ans == QLatin1String("quit")
            || ans == QLatin1String("exit") || ans == QLatin1String("none"))
            return {};
        if (ans == QLatin1String("all"))
            return posts;
        QList<int> ranks;
        if (!parseRanks(ans, &ranks)) {
            out << "  Couldn't read that. Try something like: 1,3,5-8" << Qt::endl;
            continue;
        }
        QStringList missing;
        for (int r : ranks)
            if (!byRank.contains(r) && missing.size() < 8)
                missing << QString::number(r);
        if (!missing.isEmpty()) {
            out << QStringLiteral("  No such post number(s): %1").arg(missing.join(QStringLiteral(", "))) << Qt::endl;
            continue;
        }
        QList<Post> chosen;
        for (int r : ranks)
            chosen.append(byRank.value(r));
        return chosen;
    }
}

// --------------------------------------------------------------------------- //
// main
// --------------------------------------------------------------------------- //

int run(const QStringList &arguments)
{
    QCommandLineParser p;
    p.setSingleDashWordOptionMode(QCommandLineParser::ParseAsCompactedShortOptions);
    p.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsOptions);

    const QCommandLineOption oSearch(QStringList{"S", "search", "query"}, QString(), "TEXT");
    const QCommandLineOption oSort(QStringList{"s", "sort"}, QString(), "SORT");
    const QCommandLineOption oTime(QStringList{"t", "time"}, QString(), "PERIOD");
    const QCommandLineOption oCount(QStringList{"n", "count"}, QString(), "N");
    const QCommandLineOption oRange(QStringList{"r", "range"}, QString(), "N1-N2");
    const QCommandLineOption oPick(QStringList{"p", "pick"}, QString(), "LIST");
    const QCommandLineOption oInteractive(QStringList{"i", "interactive"});
    const QCommandLineOption oLoad(QStringList{"load"}, QString(), "N", "50");
    const QCommandLineOption oList(QStringList{"l", "list"});
    const QCommandLineOption oSite(QStringList{"site"}, QString(), "URL", DEFAULT_BASE);
    const QCommandLineOption oApi("api");
    const QCommandLineOption oOld("old-reddit");
    const QCommandLineOption oSiteMode(QStringList{"site-mode"}, QString(), "MODE", "auto");
    const QCommandLineOption oClientId(QStringList{"client-id"}, QString(), "ID", qEnvironmentVariable("REDDIT_CLIENT_ID"));
    const QCommandLineOption oClientSecret(QStringList{"client-secret"}, QString(), "SECRET", qEnvironmentVariable("REDDIT_CLIENT_SECRET"));
    const QCommandLineOption oCookie(QStringList{"cookie"}, QString(), "STRING", qEnvironmentVariable("REDDIT_COOKIE"));
    const QCommandLineOption oOutput(QStringList{"o", "output"}, QString(), "DIR", QDir::homePath() + "/Documents/RedditPDFs");
    const QCommandLineOption oFlat("flat");
    const QCommandLineOption oPaper(QStringList{"paper"}, QString(), "PAPER", "Letter");
    const QCommandLineOption oNoHeader("no-header-footer");
    const QCommandLineOption oMany("many-comments");
    const QCommandLineOption oNoImages("no-images");
    const QCommandLineOption oSkip("skip-existing");
    const QCommandLineOption oUserAgent(QStringList{"user-agent"}, QString(), "STRING");
    const QCommandLineOption oWait(QStringList{"wait-timeout"}, QString(), "SEC", "45");
    const QCommandLineOption oDebug(QStringList{"debug-dir"}, QString(), "DIR");
    const QCommandLineOption oDelay(QStringList{"delay"}, QString(), "SEC", "1.5");
    const QCommandLineOption oQuiet(QStringList{"q", "quiet"});
    const QCommandLineOption oVerbose(QStringList{"v", "verbose"});
    const QCommandLineOption oVersion("version");
    const QCommandLineOption oHelp(QStringList{"h", "help"});
    // Kept so scripts written for the Python version still run.
    const QCommandLineOption oShow(QStringList{"show-browser", "headed"});
    const QCommandLineOption oBrowser(QStringList{"browser"}, QString(), "KIND");
    const QCommandLineOption oChrome(QStringList{"chrome-path"}, QString(), "FILE");
    const QCommandLineOption oWindow(QStringList{"window-size"}, QString(), "WxH");

    p.addOptions({oSearch, oSort, oTime, oCount, oRange, oPick, oInteractive, oLoad, oList, oSite, oApi,
                  oOld, oSiteMode, oClientId, oClientSecret, oCookie, oOutput, oFlat, oPaper, oNoHeader,
                  oMany, oNoImages, oSkip, oUserAgent, oWait, oDebug, oDelay, oQuiet, oVerbose, oVersion,
                  oHelp, oShow, oBrowser, oChrome, oWindow});

    if (!p.parse(arguments))
        usageError(p.errorText());
    if (p.isSet(oHelp)) {
        print(helpText());
        return EXIT_OK;
    }
    if (p.isSet(oVersion)) {
        print(QStringLiteral("%1 %2").arg(PROG, APP_VERSION));
        return EXIT_OK;
    }
    const QStringList pos = p.positionalArguments();
    if (pos.isEmpty())
        usageError(QStringLiteral("the following arguments are required: SUBREDDIT"));
    if (pos.size() > 1)
        usageError(QStringLiteral("unrecognized arguments: ") + pos.mid(1).join(QLatin1Char(' ')));

    QString sub;
    try {
        sub = cleanSubreddit(pos.first());
    } catch (const ScrapeError &e) {
        usageError(e.message());
    }

    // which posts (at most one selector)
    const QList<QPair<const QCommandLineOption *, QString>> selectors = {
        {&oCount, "-n/--count"}, {&oRange, "-r/--range"}, {&oPick, "-p/--pick"}, {&oInteractive, "-i/--interactive"}};
    QString firstSel;
    for (const auto &s : selectors) {
        if (!p.isSet(*s.first))
            continue;
        if (!firstSel.isEmpty())
            usageError(QStringLiteral("argument %1: not allowed with argument %2").arg(s.second, firstSel));
        firstSel = s.second;
    }
    const bool interactive = p.isSet(oInteractive);
    const bool listOnly = p.isSet(oList);
    const int load = positiveInt("--load", p.value(oLoad));
    QPair<int, int> range{0, 0};
    QList<int> pick;
    int count = 0;
    if (p.isSet(oRange))
        range = rangeArg("-r/--range", p.value(oRange));
    if (p.isSet(oPick))
        pick = ranksArg("-p/--pick", p.value(oPick));
    if (p.isSet(oCount))
        count = positiveInt("-n/--count", p.value(oCount));

    const double delay = nonNegativeFloat("--delay", p.value(oDelay));
    const double waitTimeout = nonNegativeFloat("--wait-timeout", p.value(oWait));
    const QString paper = choice("--paper", p.value(oPaper), {"Letter", "A4"});
    const QString siteModeArg = choice("--site-mode", p.value(oSiteMode), {"auto", "api", "old", "redlib"});
    for (const auto *o : {&oShow, &oBrowser, &oChrome, &oWindow})
        if (p.isSet(*o))
            eprint(QStringLiteral("note: --%1 has no effect in the Qt version (there is no browser).").arg(o->names().constFirst()));

    QString site = p.isSet(oOld) ? QStringLiteral("https://old.reddit.com")
                   : p.isSet(oApi) ? API_SITE
                                   : p.value(oSite);
    site = normalizeSite(site);
    const QString mode = siteModeArg == QLatin1String("auto") ? QString() : siteModeArg;
    const QString effMode = mode.isEmpty() ? siteMode(site) : mode;

    if (interactive && !isatty(STDIN_FILENO))
        usageError(QStringLiteral("--interactive needs a terminal (stdin is not a TTY)"));
    if (p.isSet(oMany) && effMode == QLatin1String("redlib"))
        eprint(QStringLiteral("note: --many-comments only applies to old.reddit.com and the API; ignoring it."));

    // How many posts must be loaded, and which of them are wanted.
    int fetchN = 25;
    if (p.isSet(oRange))
        fetchN = range.second;
    else if (!pick.isEmpty())
        fetchN = pick.last();
    else if (interactive)
        fetchN = load;
    else if (count)
        fetchN = count;

    const bool quiet = p.isSet(oQuiet), verbose = p.isSet(oVerbose);

    // Search vs. browse decides which sorts are valid and the default sort/time.
    QString query;
    if (p.isSet(oSearch)) {
        query = p.value(oSearch).trimmed();
        if (query.isEmpty())
            usageError(QStringLiteral("--search needs some text to look for"));
    }
    QStringList allSorts = SORTS;
    for (const QString &s : SEARCH_SORTS)
        if (!allSorts.contains(s))
            allSorts << s;
    QString sort = p.isSet(oSort) ? choice("-s/--sort", p.value(oSort), allSorts)
                                  : (query.isEmpty() ? QStringLiteral("hot") : QStringLiteral("relevance"));
    const QStringList valid = query.isEmpty() ? SORTS : SEARCH_SORTS;
    if (!valid.contains(sort))
        usageError(QStringLiteral("sort '%1' isn't available %2; choose from: %3")
                       .arg(sort, query.isEmpty() ? QStringLiteral("when browsing") : QStringLiteral("with --search"),
                            valid.join(QStringLiteral(", "))));
    const QString tf = p.isSet(oTime) ? choice("-t/--time", p.value(oTime), TIME_FILTERS)
                                      : (query.isEmpty() ? QStringLiteral("week") : QStringLiteral("all"));

    auto log = [quiet, verbose](const QString &msg) {
        if (quiet)
            return;
        if (msg.startsWith(QLatin1String("Loading listing")) && !verbose)
            return;
        eprint(msg);
    };
    auto info = [quiet](const QString &msg) {
        if (!quiet)
            eprint(msg);
    };

    std::signal(SIGINT, onSigint);

    const QString tag = runTag(sort, tf, query);
    QString outDir = p.value(oOutput);
    if (outDir.startsWith(QLatin1String("~/")) || outDir == QLatin1String("~"))
        outDir = QDir::homePath() + outDir.mid(1);
    outDir = QDir(outDir).absolutePath();
    if (!p.isSet(oFlat))
        outDir = QDir(outDir).filePath(QStringLiteral("r_%1_%2").arg(sub, tag));

    ScraperOptions so;
    so.base = site;
    so.mode = mode;
    so.delay = delay;
    so.userAgent = p.value(oUserAgent);
    so.readyTimeout = waitTimeout;
    so.debugDir = p.value(oDebug);
    so.clientId = p.value(oClientId);
    so.clientSecret = p.value(oClientSecret);
    so.cookies = p.value(oCookie);

    SaveOptions save;
    save.paper = paper;
    save.headerFooter = !p.isSet(oNoHeader);
    save.manyComments = p.isSet(oMany);
    save.images = !p.isSet(oNoImages);

    int ok = 0, failed = 0, skipped = 0;
    try {
        RedditScraper s(so, log, &g_cancel);
        const QString what = query.isEmpty() ? QStringLiteral("r/") + sub
                                             : QStringLiteral("search \"%1\" in r/%2").arg(query, sub);
        const QString from = effMode == QLatin1String("api") ? QStringLiteral("the Reddit API") : site;
        info(QStringLiteral("Loading %1 (%2%3) from %4 - up to %5 posts...")
                 .arg(what, sort, tf == QLatin1String("all") ? QString() : QStringLiteral(", ") + tf, from,
                      QString::number(fetchN)));
        const QList<Post> posts = s.fetchPosts(sub, sort, tf, fetchN, {}, query);
        info(QStringLiteral("Loaded %1 posts.").arg(posts.size()));

        QList<Post> chosen;
        if (p.isSet(oRange)) {
            for (const Post &post : posts)
                if (post.rank >= range.first && post.rank <= range.second)
                    chosen.append(post);
            if (chosen.isEmpty())
                throw ScrapeError(QStringLiteral("r/%1 only has %2 posts; nothing in %3-%4.")
                                      .arg(sub).arg(posts.size()).arg(range.first).arg(range.second));
        } else if (!pick.isEmpty()) {
            QSet<int> have;
            for (const Post &post : posts)
                have.insert(post.rank);
            QStringList gone;
            for (int r : pick)
                if (!have.contains(r))
                    gone << QString::number(r);
            if (!gone.isEmpty())
                eprint(QStringLiteral("warning: no post number %1 (only %2 loaded).")
                           .arg(gone.join(QStringLiteral(", ")), QString::number(posts.size())));
            const QSet<int> wanted(pick.begin(), pick.end());
            for (const Post &post : posts)
                if (wanted.contains(post.rank))
                    chosen.append(post);
            if (chosen.isEmpty())
                throw ScrapeError(QStringLiteral("none of the requested post numbers exist."));
        } else if (interactive) {
            printTable(posts);
            if (listOnly)
                return EXIT_OK;
            chosen = askSelection(posts);
            if (chosen.isEmpty()) {
                info(QStringLiteral("Nothing selected."));
                return EXIT_OK;
            }
        } else {
            chosen = posts.mid(0, fetchN);
        }

        if (listOnly) {
            printTable(chosen);
            return EXIT_OK;
        }

        const int total = chosen.size();
        const int width = QString::number(total).size();
        info(QStringLiteral("Saving %1 PDF%2 to %3").arg(QString::number(total), total != 1 ? "s" : "", outDir));
        const QLocale en(QLocale::English);
        for (int i = 1; i <= total; ++i) {
            const Post &post = chosen[i - 1];
            if (g_cancel)
                throw Cancelled();
            const QString label = QStringLiteral("[%1/%2]").arg(i, width).arg(total);
            const QString rank = QStringLiteral("%1").arg(post.rank, 3, 10, QLatin1Char('0'));
            if (p.isSet(oSkip) && QFileInfo(outDir).isDir()
                && !QDir(outDir).entryList({QStringLiteral("*[[]%1].pdf").arg(post.id())}, QDir::Files).isEmpty()) {
                ++skipped;
                info(QStringLiteral("%1 SKIP #%2 %3 (already saved)").arg(label, rank, post.title.left(70)));
                continue;
            }
            try {
                const QString path = s.savePdf(post, outDir, save);
                ++ok;
                const double kb = QFileInfo(path).size() / 1024.0;
                info(QStringLiteral("%1 OK   %2  (%3 KB)").arg(label, QFileInfo(path).fileName(), en.toString(kb, 'f', 0)));
            } catch (const Cancelled &) {
                throw;
            } catch (const std::exception &e) {
                ++failed;
                const auto *se = dynamic_cast<const ScrapeError *>(&e);
                const QString msg = se ? se->message() : QString::fromLocal8Bit(e.what());
                eprint(QStringLiteral("%1 FAIL #%2 %3 - %4").arg(label, rank, post.title.left(60), msg.section(QLatin1Char('\n'), 0, 0)));
            }
            if (i < total)
                s.sleep(delay);
        }
    } catch (const Cancelled &) {
        eprint(QStringLiteral("Cancelled. Saved %1, failed %2, skipped %3.").arg(ok).arg(failed).arg(skipped));
        return EXIT_CANCELLED;
    } catch (const ScrapeError &e) {
        eprint(QStringLiteral("error: ") + e.message());
        return EXIT_FATAL;
    }

    QString summary = QStringLiteral("Done: %1 saved, %2 failed").arg(ok).arg(failed);
    if (skipped)
        summary += QStringLiteral(", %1 skipped").arg(skipped);
    print(QStringLiteral("%1. Folder: %2").arg(summary, outDir));
    return failed ? EXIT_PARTIAL : EXIT_OK;
}

} // namespace

int main(int argc, char *argv[])
{
    // Fonts and PDF painting need a QGuiApplication, but not a display.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(PROG);
    QGuiApplication::setApplicationVersion(APP_VERSION);
    return run(QGuiApplication::arguments());
}
