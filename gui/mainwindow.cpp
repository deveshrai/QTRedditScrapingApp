#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "storezip.h"

#ifdef Q_OS_WASM
#include <emscripten/val.h>

namespace {
// Browsers can't write to a chosen folder: PDFs go to an in-memory folder and
// are handed over as one .zip download at the end.
const QString WASM_OUT_DIR = QStringLiteral("/tmp/redditpdf");

QString storedClientId()
{
    const emscripten::val v = emscripten::val::global("localStorage").call<emscripten::val>("getItem", std::string("redditpdf.clientId"));
    return v.isNull() ? QString() : QString::fromStdString(v.as<std::string>());
}

// Hands bytes to the browser as a file download (Qt's saveFileContent needs a fresh user click).
void browserDownload(const QByteArray &data, const QString &name, const char *mime)
{
    using emscripten::val;
    const val view(emscripten::typed_memory_view(size_t(data.size()), reinterpret_cast<const uint8_t *>(data.constData())));
    const val copy = val::global("Uint8Array").new_(view);   // copies out of the (shared) wasm heap
    val parts = val::array();
    parts.call<void>("push", copy);
    val opts = val::object();
    opts.set("type", std::string(mime));
    const val blob = val::global("Blob").new_(parts, opts);
    const val url = val::global("URL").call<val>("createObjectURL", blob);
    val a = val::global("document").call<val>("createElement", std::string("a"));
    a.set("href", url);
    a.set("download", name.toStdString());
    val body = val::global("document")["body"];
    body.call<void>("appendChild", a);
    a.call<void>("click");
    body.call<void>("removeChild", a);
}

void storeClientId(const QString &id)
{
    emscripten::val::global("localStorage").call<void>("setItem", std::string("redditpdf.clientId"), id.toStdString());
}
} // namespace
#endif

namespace {

enum Column { ColCheck, ColRank, ColTitle, ColScore, ColComments, ColAuthor };

QString elide(const QString &s, int n)
{
    return s.left(n);
}

// Modal exec() isn't available in the browser, so there the box is shown without blocking.
void notify(QWidget *parent, QMessageBox::Icon icon, const QString &title, const QString &text)
{
#ifdef Q_OS_WASM
    auto *box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
#else
    if (icon == QMessageBox::Critical)
        QMessageBox::critical(parent, title, text);
    else
        QMessageBox::information(parent, title, text);
#endif
}

} // namespace

MainWindow::MainWindow()
{
    qRegisterMetaType<Post>();
    qRegisterMetaType<QList<Post>>();

    setWindowTitle(QStringLiteral("Reddit → PDF"));
    resize(900, 740);
    setMinimumSize(760, 600);

    buildUi();
    loadSettings();
    history = runhistory::load();
    refreshHistory();
    onQueryChange();
    onSiteChange();

    connect(this, &MainWindow::workerLog, this, &MainWindow::appendLog, Qt::QueuedConnection);
    connect(this, &MainWindow::workerStatus, lblStatus, &QLabel::setText, Qt::QueuedConnection);
    connect(this, &MainWindow::workerProgress, this, [this](int done, int total) {
        progress->setMaximum(qMax(total, 1));
        progress->setValue(done);
    }, Qt::QueuedConnection);
    connect(this, &MainWindow::workerPosts, this, &MainWindow::populate, Qt::QueuedConnection);
    connect(this, &MainWindow::workerDone, this, &MainWindow::onDone, Qt::QueuedConnection);
    connect(this, &MainWindow::workerError, this, &MainWindow::onError, Qt::QueuedConnection);

    appendLog(QStringLiteral("Qt %1 / %2 / style '%3'")
                  .arg(QString::fromLatin1(qVersion()), QGuiApplication::platformName(), style()->name()));
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (worker && worker->isRunning()) {
        cancelFlag = true;
        lblStatus->setText(QStringLiteral("Cancelling…"));
        worker->wait();
    }
    saveSettings();
    event->accept();
}

// ------------------------------------------------------------------------ ui
void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);

    // --- source ---------------------------------------------------------
    auto *src = new QGroupBox(QStringLiteral("Source"));
    auto *grid = new QGridLayout(src);
    grid->setColumnStretch(1, 1);

    grid->addWidget(new QLabel(QStringLiteral("Subreddit  r/")), 0, 0, Qt::AlignRight);
    edSub = new QLineEdit;
    grid->addWidget(edSub, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Sort")), 0, 2, Qt::AlignRight);
    cbSort = new QComboBox;
    cbSort->addItems(SORTS);
    cbSort->setMinimumContentsLength(12);
    grid->addWidget(cbSort, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("Time")), 0, 4, Qt::AlignRight);
    cbTime = new QComboBox;
    cbTime->addItems(TIME_FILTERS);
    cbTime->setCurrentText(QStringLiteral("week"));
    grid->addWidget(cbTime, 0, 5);
    connect(cbSort, &QComboBox::activated, this, [this] { sortTouched = true; onSortChange(); });
    connect(cbTime, &QComboBox::activated, this, [this] { timeTouched = true; });

    grid->addWidget(new QLabel(QStringLiteral("Search")), 1, 0, Qt::AlignRight);
    edQuery = new QLineEdit;
    grid->addWidget(edQuery, 1, 1, 1, 4);
    grid->addWidget(new QLabel(QStringLiteral("(optional, inside r/)")), 1, 5);
    connect(edQuery, &QLineEdit::textChanged, this, &MainWindow::onQueryChange);

    grid->addWidget(new QLabel(QStringLiteral("Save to")), 2, 0, Qt::AlignRight);
    edOut = new QLineEdit(defaultOutputDir());
    edOut->setToolTip(QStringLiteral("\"{date}\" becomes today's date (yyyy-MM-dd) when you download."));
    grid->addWidget(edOut, 2, 1, 1, 4);
    auto *btnBrowse = new QPushButton(QStringLiteral("Browse…"));
    connect(btnBrowse, &QPushButton::clicked, this, &MainWindow::browse);
    grid->addWidget(btnBrowse, 2, 5);
#ifdef Q_OS_WASM
    edOut->setText(QStringLiteral("One .zip download when the run finishes"));
    edOut->setEnabled(false);
    btnBrowse->setVisible(false);
#endif

    auto *opts = new QHBoxLayout;
    opts->addWidget(new QLabel(QStringLiteral("Site")));
    cbSite = new QComboBox;
    cbSite->setEditable(true);
    cbSite->addItems(SITE_PRESETS);
    cbSite->setMinimumContentsLength(22);
#ifdef Q_OS_WASM
    cbSite->setEditable(false);
    cbSite->clear();
    cbSite->addItem(API_SITE);   // other sources need cookies/no CORS; only the official API works in a browser
    cbSite->setEnabled(false);
#endif
    cbSite->setToolTip(QStringLiteral("%1 = Reddit's official API. Or a Redlib URL (e.g. self-hosted), or old.reddit.com.").arg(API_SITE));
    connect(cbSite, &QComboBox::currentTextChanged, this, &MainWindow::onSiteChange);
    opts->addWidget(cbSite);
    opts->addSpacing(10);
    opts->addWidget(new QLabel(QStringLiteral("Paper")));
    cbPaper = new QComboBox;
    cbPaper->addItems({QStringLiteral("Letter"), QStringLiteral("A4")});
    opts->addWidget(cbPaper);
    opts->addSpacing(10);
    chkHeader = new QCheckBox(QStringLiteral("Header/footer (date, title, URL, page #)"));
    chkHeader->setChecked(true);
    opts->addWidget(chkHeader);
    chkComments = new QCheckBox(QStringLiteral("500 comments"));
    chkComments->setToolTip(QStringLiteral("old.reddit.com and the Reddit API only"));
    opts->addWidget(chkComments);
    chkImages = new QCheckBox(QStringLiteral("Images"));
    chkImages->setToolTip(QStringLiteral("Download post images into the PDF (otherwise they become links)"));
    chkImages->setChecked(true);
    opts->addWidget(chkImages);
    chkZip = new QCheckBox(QStringLiteral("Also make a .zip"));
    chkZip->setToolTip(QStringLiteral("Bundle the PDFs of this run into <folder>.zip, next to the folder"));
    opts->addWidget(chkZip);
#ifdef Q_OS_WASM
    chkZip->setVisible(false);   // the browser build always downloads a zip
#endif
    opts->addStretch();
    grid->addLayout(opts, 3, 0, 1, 6);

    // Reddit API credentials (site = reddit-api)
    apiRow = new QWidget;
    auto *api = new QHBoxLayout(apiRow);
    api->setContentsMargins(0, 0, 0, 0);
    api->addWidget(new QLabel(QStringLiteral("API client ID")));
    edClientId = new QLineEdit(qEnvironmentVariable("REDDIT_CLIENT_ID"));
#ifdef Q_OS_WASM
    edClientId->setPlaceholderText(QStringLiteral("your own app's client ID from reddit.com/prefs/apps (type: installed app)"));
#else
    edClientId->setPlaceholderText(hasBuiltinCredentials() ? QStringLiteral("built-in app (leave empty)")
                                                           : QStringLiteral("from reddit.com/prefs/apps"));
#endif
    api->addWidget(edClientId, 1);
    api->addWidget(new QLabel(QStringLiteral("Secret")));
    edSecret = new QLineEdit(qEnvironmentVariable("REDDIT_CLIENT_SECRET"));
    edSecret->setEchoMode(QLineEdit::Password);
    edSecret->setPlaceholderText(QStringLiteral("empty for an \"installed app\""));
    api->addWidget(edSecret, 1);
    grid->addWidget(apiRow, 4, 0, 1, 6);

    // Cookie for HTML sites (e.g. a logged-in old.reddit session)
    cookieRow = new QWidget;
    auto *ck = new QHBoxLayout(cookieRow);
    ck->setContentsMargins(0, 0, 0, 0);
    ck->addWidget(new QLabel(QStringLiteral("Cookie")));
    edCookie = new QLineEdit(qEnvironmentVariable("REDDIT_COOKIE"));
    edCookie->setPlaceholderText(QStringLiteral("optional, e.g. reddit_session=…; token_v2=… (old.reddit needs a login now)"));
    ck->addWidget(edCookie, 1);
    grid->addWidget(cookieRow, 4, 0, 1, 6);   // same cell: only one of the two rows is shown

    root->addWidget(src);

    // --- mode tabs ------------------------------------------------------
    tabs = new QTabWidget;
    tabs->addTab(buildPickTab(), QStringLiteral("  Pick posts from a list  "));
    tabs->addTab(buildBatchTab(), QStringLiteral("  Automatic batch  "));
#ifndef Q_OS_WASM
    tabs->addTab(buildHistoryTab(), QStringLiteral("  History  "));
#endif
    root->addWidget(tabs, 1);

    // --- progress / log -------------------------------------------------
    auto *bar = new QHBoxLayout;
    progress = new QProgressBar;
    progress->setTextVisible(false);
    progress->setValue(0);
    bar->addWidget(progress, 1);
    btnCancel = new QPushButton(QStringLiteral("Cancel"));
    btnCancel->setEnabled(false);
    connect(btnCancel, &QPushButton::clicked, this, &MainWindow::cancel);
    bar->addWidget(btnCancel);
    root->addLayout(bar);

    lblStatus = new QLabel(QStringLiteral("Ready."));
    lblStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // long paths mustn't widen the window
    auto *statusRow = new QHBoxLayout;   // the version stays put while the status text changes
    statusRow->addWidget(lblStatus, 1);
    auto *lblVersion = new QLabel(QStringLiteral("v%1").arg(APP_VERSION));
    lblVersion->setToolTip(QStringLiteral("RedditPDF %1").arg(APP_VERSION));
    lblVersion->setEnabled(false);   // dimmed
    statusRow->addWidget(lblVersion, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(statusRow);

    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView->setFixedHeight(logView->fontMetrics().lineSpacing() * 7 + 12);
    root->addWidget(logView);

    setCentralWidget(central);
    edSub->setFocus();
}

QWidget *MainWindow::buildPickTab()
{
    auto *tab = new QWidget;
    auto *v = new QVBoxLayout(tab);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Load")));
    cbLoadN = new QComboBox;
    cbLoadN->addItems({QStringLiteral("25"), QStringLiteral("50"), QStringLiteral("100"), QStringLiteral("200")});
    cbLoadN->setCurrentText(QStringLiteral("50"));
    top->addWidget(cbLoadN);
    top->addWidget(new QLabel(QStringLiteral("posts")));
    btnLoad = new QPushButton(QStringLiteral("Load posts"));
    connect(btnLoad, &QPushButton::clicked, this, &MainWindow::loadPosts);
    top->addSpacing(10);
    top->addWidget(btnLoad);
    top->addStretch();
    lblSel = new QLabel(QStringLiteral("0 selected"));
    top->addWidget(lblSel);
    v->addLayout(top);

    tree = new QTreeWidget;
    tree->setColumnCount(6);
    tree->setHeaderLabels({QString(), QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Score"),
                           QStringLiteral("Comments"), QStringLiteral("Author")});
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    QHeaderView *h = tree->header();
    h->setStretchLastSection(false);
    h->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    tree->setColumnWidth(ColCheck, 36);
    tree->setColumnWidth(ColRank, 44);
    tree->setColumnWidth(ColScore, 64);
    tree->setColumnWidth(ColComments, 80);
    tree->setColumnWidth(ColAuthor, 130);
    connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *, int col) {
        if (col == ColCheck)
            updateSelLabel();
    });
    v->addWidget(tree, 1);

    auto *btns = new QHBoxLayout;
    auto *all = new QPushButton(QStringLiteral("Select all"));
    auto *none = new QPushButton(QStringLiteral("Select none"));
    auto *inv = new QPushButton(QStringLiteral("Invert"));
    connect(all, &QPushButton::clicked, this, [this] { setAll(true); });
    connect(none, &QPushButton::clicked, this, [this] { setAll(false); });
    connect(inv, &QPushButton::clicked, this, &MainWindow::invert);
    btns->addWidget(all);
    btns->addWidget(none);
    btns->addWidget(inv);
    btns->addStretch();
    btnDlSel = new QPushButton(QStringLiteral("Download selected as PDF"));
    connect(btnDlSel, &QPushButton::clicked, this, &MainWindow::downloadSelected);
    btns->addWidget(btnDlSel);
    v->addLayout(btns);
    return tab;
}

QWidget *MainWindow::buildBatchTab()
{
    auto *tab = new QWidget;
    auto *v = new QVBoxLayout(tab);
    v->setContentsMargins(12, 12, 12, 12);
    auto *intro = new QLabel(QStringLiteral("Download the top-ranked posts of the chosen sort, in order, without picking."));
    intro->setWordWrap(true);
    v->addWidget(intro);

    rb25 = new QRadioButton(QStringLiteral("Posts 1 – 25"));
    rb50 = new QRadioButton(QStringLiteral("Posts 1 – 50"));
    rb100 = new QRadioButton(QStringLiteral("Posts 1 – 100"));
    rb25->setChecked(true);
    v->addWidget(rb25);
    v->addWidget(rb50);
    v->addWidget(rb100);

    auto *custom = new QHBoxLayout;
    rbCustom = new QRadioButton(QStringLiteral("Custom range:"));
    spFrom = new QSpinBox;
    spTo = new QSpinBox;
    for (QSpinBox *sp : {spFrom, spTo}) {
        sp->setRange(1, 1000);
        connect(sp, &QSpinBox::valueChanged, this, [this] { rbCustom->setChecked(true); });
    }
    spFrom->setValue(1);
    spTo->setValue(25);
    rb25->setChecked(true);   // setting the spin boxes above ticked "custom"
    custom->addWidget(rbCustom);
    custom->addWidget(spFrom);
    custom->addWidget(new QLabel(QStringLiteral("to")));
    custom->addWidget(spTo);
    custom->addStretch();
    v->addLayout(custom);

    btnBatch = new QPushButton(QStringLiteral("Start batch download"));
    connect(btnBatch, &QPushButton::clicked, this, &MainWindow::downloadBatch);
    auto *row = new QHBoxLayout;
    row->addWidget(btnBatch);
    row->addStretch();
    v->addSpacing(8);
    v->addLayout(row);
    v->addStretch();
    return tab;
}

QWidget *MainWindow::buildHistoryTab()
{
    auto *tab = new QWidget;
    auto *v = new QVBoxLayout(tab);
    v->setContentsMargins(12, 12, 12, 12);
    auto *intro = new QLabel(QStringLiteral("Your last %1 batch requests. Tick any of them and run them together; "
                                            "each saves to its usual folder, using the options set above.").arg(runhistory::kMax));
    intro->setWordWrap(true);
    v->addWidget(intro);

    histTree = new QTreeWidget;
    histTree->setRootIsDecorated(false);
    histTree->setHeaderLabels({QString(), QStringLiteral("Subreddit"), QStringLiteral("Sort"), QStringLiteral("Search"),
                               QStringLiteral("Posts"), QStringLiteral("Last used")});
    histTree->setAlternatingRowColors(true);
    histTree->header()->setStretchLastSection(false);
    histTree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    histTree->setColumnWidth(0, 34);
    histTree->setColumnWidth(1, 150);
    histTree->setColumnWidth(2, 130);
    histTree->setColumnWidth(4, 80);
    histTree->setColumnWidth(5, 140);
    v->addWidget(histTree, 1);

    auto *row = new QHBoxLayout;
    auto *btnAll = new QPushButton(QStringLiteral("Select all"));
    auto *btnNone = new QPushButton(QStringLiteral("Select none"));
    auto *btnRemove = new QPushButton(QStringLiteral("Remove selected"));
    const auto setChecks = [this](bool on) {
        for (int i = 0; i < histTree->topLevelItemCount(); ++i)
            histTree->topLevelItem(i)->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
    };
    connect(btnAll, &QPushButton::clicked, this, [=] { setChecks(true); });
    connect(btnNone, &QPushButton::clicked, this, [=] { setChecks(false); });
    connect(btnRemove, &QPushButton::clicked, this, [this] {
        for (int i = histTree->topLevelItemCount() - 1; i >= 0; --i)
            if (histTree->topLevelItem(i)->checkState(0) == Qt::Checked)
                history.removeAt(histTree->topLevelItem(i)->data(0, Qt::UserRole).toInt());
        runhistory::save(history);
        refreshHistory();
    });
    row->addWidget(btnAll);
    row->addWidget(btnNone);
    row->addWidget(btnRemove);
    row->addStretch();
    btnRunHist = new QPushButton(QStringLiteral("Run selected"));
    btnRunHist->setToolTip(QStringLiteral("Run the ticked requests one after another"));
    connect(btnRunHist, &QPushButton::clicked, this, &MainWindow::runHistory);
    row->addWidget(btnRunHist);
    v->addLayout(row);
    return tab;
}

void MainWindow::refreshHistory()
{
    if (!histTree)
        return;
    histTree->clear();
    for (int i = 0; i < history.size(); ++i) {
        const RunPattern &p = history[i];
        const bool timed = !p.query.isEmpty() || TIMED_SORTS.contains(p.sort);
        auto *it = new QTreeWidgetItem(histTree);
        it->setCheckState(0, Qt::Unchecked);
        it->setData(0, Qt::UserRole, i);
        it->setText(1, QStringLiteral("r/") + p.sub);
        it->setText(2, timed ? QStringLiteral("%1 · %2").arg(p.sort, p.timeFilter) : p.sort);
        it->setText(3, p.query);
        it->setText(4, p.from == 1 ? QString::number(p.to) : QStringLiteral("%1–%2").arg(p.from).arg(p.to));
        it->setText(5, p.lastUsed.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
}

void MainWindow::rememberRun(const RunPattern &p)
{
    runhistory::remember(history, p);
    runhistory::save(history);
    refreshHistory();
}

void MainWindow::runHistory()
{
    QList<RunPattern> picked;
    for (int i = 0; i < histTree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *it = histTree->topLevelItem(i);
        if (it->checkState(0) == Qt::Checked)
            picked.append(history[it->data(0, Qt::UserRole).toInt()]);
    }
    if (picked.isEmpty()) {
        notify(this, QMessageBox::Information, QStringLiteral("Nothing selected"), QStringLiteral("Tick at least one request first."));
        return;
    }
    QStringList dirs;
    for (const RunPattern &p : picked)
        dirs << outDir(p.sub, p.sort, p.timeFilter, p.query);
    const ScraperOptions so = scraperOptions();
    const SaveOptions save = saveOptions();
    for (int i = picked.size() - 1; i >= 0; --i)   // keep their relative order at the top
        rememberRun(picked[i]);

    startWorker([=] {
        RedditScraper s(so, workerLogger(), &cancelFlag);
        int ok = 0, failed = 0, brokenRuns = 0;
        for (int i = 0; i < picked.size(); ++i) {
            if (cancelFlag)
                throw Cancelled();
            const RunPattern &p = picked[i];
            const QString what = QStringLiteral("r/%1 %2%3 (%4–%5)")
                                     .arg(p.sub, p.sort, p.query.isEmpty() ? QString() : QStringLiteral(" \"%1\"").arg(p.query))
                                     .arg(p.from).arg(p.to);
            emit workerLog(QStringLiteral("── Request %1/%2: %3").arg(i + 1).arg(picked.size()).arg(what));
            emit workerStatus(QStringLiteral("Request %1/%2: fetching listing…").arg(i + 1).arg(picked.size()));
            try {
                const QList<Post> found = s.fetchPosts(p.sub, p.sort, p.timeFilter, p.to,
                                                       [this](int a, int b) { emit workerProgress(a, b); }, p.query);
                const QList<Post> chosen = found.mid(p.from - 1, p.to - p.from + 1);
                if (chosen.isEmpty())
                    throw ScrapeError(QStringLiteral("r/%1 only has %2 posts; nothing in %3–%4.")
                                          .arg(p.sub).arg(found.size()).arg(p.from).arg(p.to));
                const Tally t = downloadFolder(s, chosen, dirs[i], save);
                ok += t.ok;
                failed += t.failed;
                emit workerLog(QStringLiteral("   → %1%2").arg(QDir::toNativeSeparators(dirs[i]), zipNote(dirs[i], save, t.ok)));
            } catch (const Cancelled &) {
                throw;
            } catch (const ScrapeError &e) {
                emit workerLog(QStringLiteral("✘ %1 — %2").arg(what, e.message()));
                ++brokenRuns;
            } catch (const std::exception &e) {
                emit workerLog(QStringLiteral("✘ %1 — %2").arg(what, QString::fromLocal8Bit(e.what())));
                ++brokenRuns;
            }
            if (i + 1 < picked.size())
                s.sleep(s.delay());
        }
        emit workerDone(QStringLiteral("Finished %1 request%2: %3 saved, %4 failed%5")
                            .arg(picked.size()).arg(picked.size() == 1 ? "" : "s").arg(ok).arg(failed)
                            .arg(brokenRuns ? QStringLiteral(", %1 request%2 couldn't be loaded").arg(brokenRuns).arg(brokenRuns == 1 ? "" : "s")
                                            : QString()));
    });
}

void MainWindow::loadSettings()
{
#ifdef Q_OS_WASM
    if (edClientId->text().isEmpty())
        edClientId->setText(storedClientId());
    return;
#endif
    QSettings s;
    const QString savedOut = s.value(QStringLiteral("output")).toString();
    if (!savedOut.isEmpty() && QDir::cleanPath(savedOut) != QDir::homePath() + QStringLiteral("/Documents/RedditPDFs"))
        edOut->setText(savedOut);   // the old fixed default gives way to the dated one
    chkZip->setChecked(s.value(QStringLiteral("zip"), false).toBool());
    cbSite->setCurrentText(s.value(QStringLiteral("site"), DEFAULT_BASE).toString());
    cbPaper->setCurrentText(s.value(QStringLiteral("paper"), QStringLiteral("Letter")).toString());
    if (edClientId->text().isEmpty())
        edClientId->setText(s.value(QStringLiteral("clientId")).toString());
    if (edSecret->text().isEmpty())
        edSecret->setText(s.value(QStringLiteral("clientSecret")).toString());
}

void MainWindow::saveSettings() const
{
#ifdef Q_OS_WASM
    storeClientId(edClientId->text().trimmed());   // the secret is never stored
    return;
#endif
    QSettings s;
    s.setValue(QStringLiteral("output"), edOut->text());
    s.setValue(QStringLiteral("zip"), chkZip->isChecked());
    s.setValue(QStringLiteral("site"), cbSite->currentText());
    s.setValue(QStringLiteral("paper"), cbPaper->currentText());
    s.setValue(QStringLiteral("clientId"), edClientId->text());
    s.setValue(QStringLiteral("clientSecret"), edSecret->text());
}

// ------------------------------------------------------------------- helpers
QString MainWindow::query() const
{
    return edQuery->text().trimmed();
}

void MainWindow::onSortChange()
{
    cbTime->setEnabled(!query().isEmpty() || TIMED_SORTS.contains(cbSort->currentText()));
}

void MainWindow::onQueryChange()
{
    // Switch the Sort/Time choices between browsing and searching.
    const bool searching = !query().isEmpty();
    const QStringList allowed = searching ? SEARCH_SORTS : SORTS;
    const QString current = cbSort->currentText();
    QSignalBlocker block(cbSort);
    cbSort->clear();
    cbSort->addItems(allowed);
    if (!sortTouched || !allowed.contains(current))
        cbSort->setCurrentText(searching ? QStringLiteral("relevance") : QStringLiteral("hot"));
    else
        cbSort->setCurrentText(current);
    if (!timeTouched)
        cbTime->setCurrentText(searching ? QStringLiteral("all") : QStringLiteral("week"));
    onSortChange();
}

void MainWindow::onSiteChange()
{
    const bool api = siteMode(normalizeSite(cbSite->currentText())) == QLatin1String("api");
    apiRow->setVisible(api);
    cookieRow->setVisible(!api);
}

void MainWindow::browse()
{
    QString start = expandOutputDir(edOut->text());   // today's folder may not exist yet: start at its parent
    while (!QDir(start).exists() && start != QDir::rootPath())
        start = QFileInfo(start).absolutePath();
    const QString d = QFileDialog::getExistingDirectory(this, QString(), start);
    if (!d.isEmpty())
        edOut->setText(QDir::toNativeSeparators(d));
}

void MainWindow::appendLog(const QString &msg)
{
    logView->appendPlainText(msg);
    logView->verticalScrollBar()->setValue(logView->verticalScrollBar()->maximum());
}

void MainWindow::setBusy(bool busy)
{
    for (QPushButton *b : {btnLoad, btnDlSel, btnBatch, btnRunHist})
        if (b)
            b->setEnabled(!busy);
    btnCancel->setEnabled(busy);
    if (!busy)
        progress->setValue(0);
}

QString MainWindow::validateSource()
{
    try {
        return cleanSubreddit(edSub->text());
    } catch (const ScrapeError &e) {
        notify(this, QMessageBox::Critical, QStringLiteral("Subreddit"), e.message());
        return QString();
    }
}

QString MainWindow::outDir(const QString &sub) const
{
    return outDir(sub, cbSort->currentText(), cbTime->currentText(), query());
}

QString MainWindow::outDir(const QString &sub, const QString &sort, const QString &tf, const QString &q) const
{
    QString base = expandOutputDir(edOut->text());
#ifdef Q_OS_WASM
    base = WASM_OUT_DIR;
#endif
    return QDir(base).filePath(QStringLiteral("r_%1_%2").arg(sub, runTag(sort, tf, q)));
}

ScraperOptions MainWindow::scraperOptions() const
{
    ScraperOptions o;
    o.base = normalizeSite(cbSite->currentText());
    o.clientId = edClientId->text().trimmed();
    o.clientSecret = edSecret->text().trimmed();
    o.cookies = edCookie->text().trimmed();
    return o;
}

SaveOptions MainWindow::saveOptions() const
{
    SaveOptions o;
    o.paper = cbPaper->currentText();
    o.headerFooter = chkHeader->isChecked();
    o.manyComments = chkComments->isChecked();
    o.images = chkImages->isChecked();
    o.zip = chkZip->isChecked();
    return o;
}

void MainWindow::startWorker(std::function<void()> job)
{
    if (worker && worker->isRunning())
        return;
    cancelFlag = false;
    setBusy(true);
    worker = QThread::create([this, job] {
        try {
            job();
        } catch (const Cancelled &) {
            emit workerDone(QStringLiteral("Cancelled."));
        } catch (const ScrapeError &e) {
            emit workerError(e.message());
        } catch (const std::exception &e) {
            emit workerError(QString::fromLocal8Bit(e.what()));
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::cancel()
{
    cancelFlag = true;
    lblStatus->setText(QStringLiteral("Cancelling…"));
}

// ---------------------------------------------------- list / checkboxes
void MainWindow::populate(const QList<Post> &list)
{
    posts = list;
    QSignalBlocker block(tree);
    tree->clear();
    for (const Post &p : list) {
        auto *item = new QTreeWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(ColCheck, Qt::Unchecked);
        item->setData(ColCheck, Qt::UserRole, p.fullname);
        item->setText(ColRank, QString::number(p.rank));
        item->setText(ColTitle, (p.stickied ? QStringLiteral("📌 ") : QString()) + p.title);
        item->setToolTip(ColTitle, p.title);
        item->setText(ColScore, p.score);
        item->setText(ColComments, QString::number(p.comments));
        item->setText(ColAuthor, p.author);
        for (int c : {ColRank, ColScore, ColComments})
            item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        tree->addTopLevelItem(item);
    }
    block.unblock();
    updateSelLabel();
}

void MainWindow::setAll(bool on)
{
    QSignalBlocker block(tree);
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        tree->topLevelItem(i)->setCheckState(ColCheck, on ? Qt::Checked : Qt::Unchecked);
    block.unblock();
    updateSelLabel();
}

void MainWindow::invert()
{
    QSignalBlocker block(tree);
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = tree->topLevelItem(i);
        it->setCheckState(ColCheck, it->checkState(ColCheck) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
    block.unblock();
    updateSelLabel();
}

void MainWindow::updateSelLabel()
{
    int n = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        n += tree->topLevelItem(i)->checkState(ColCheck) == Qt::Checked;
    lblSel->setText(QStringLiteral("%1 of %2 selected").arg(n).arg(posts.size()));
}

// --------------------------------------------------------------- actions
void MainWindow::loadPosts()
{
    const QString sub = validateSource();
    if (sub.isEmpty())
        return;
    const QString q = query();
    lblStatus->setText(q.isEmpty() ? QStringLiteral("Loading r/%1…").arg(sub)
                                   : QStringLiteral("Searching r/%1 for \"%2\"…").arg(sub, q));
    const QString sort = cbSort->currentText(), tf = cbTime->currentText();
    const int count = cbLoadN->currentText().toInt();
    const ScraperOptions so = scraperOptions();
    startWorker([=] {
        RedditScraper s(so, workerLogger(), &cancelFlag);
        const QList<Post> found = s.fetchPosts(sub, sort, tf, count,
                                               [this](int a, int b) { emit workerProgress(a, b); }, q);
        emit workerPosts(found);
        emit workerDone(QStringLiteral("Loaded %1 posts from r/%2.").arg(found.size()).arg(sub));
    });
}

void MainWindow::downloadSelected()
{
    const QString sub = validateSource();
    if (sub.isEmpty())
        return;
    QSet<QString> checked;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = tree->topLevelItem(i);
        if (it->checkState(ColCheck) == Qt::Checked)
            checked.insert(it->data(ColCheck, Qt::UserRole).toString());
    }
    QList<Post> chosen;
    for (const Post &p : posts)
        if (checked.contains(p.fullname))
            chosen.append(p);
    if (chosen.isEmpty()) {
        notify(this, QMessageBox::Information, QStringLiteral("Nothing selected"), QStringLiteral("Tick at least one post first."));
        return;
    }
    const QString dir = outDir(sub);
    const ScraperOptions so = scraperOptions();
    const SaveOptions save = saveOptions();
    startWorker([=] {
        RedditScraper s(so, workerLogger(), &cancelFlag);
        downloadWith(s, chosen, dir, save);
    });
}

void MainWindow::downloadBatch()
{
    const QString sub = validateSource();
    if (sub.isEmpty())
        return;
    int n1 = 1, n2 = 25;
    if (rbCustom->isChecked()) {
        n1 = spFrom->value();
        n2 = spTo->value();
    } else {
        n2 = rb100->isChecked() ? 100 : rb50->isChecked() ? 50 : 25;
    }
    if (n1 < 1 || n2 < n1) {
        notify(this, QMessageBox::Critical, QStringLiteral("Range"), QStringLiteral("Range must satisfy 1 ≤ from ≤ to."));
        return;
    }
    const QString sort = cbSort->currentText(), tf = cbTime->currentText(), q = query();
    const QString dir = outDir(sub);
    const ScraperOptions so = scraperOptions();
    const SaveOptions save = saveOptions();
    RunPattern pattern;
    pattern.sub = sub;
    pattern.sort = sort;
    pattern.timeFilter = tf;
    pattern.query = q;
    pattern.from = n1;
    pattern.to = n2;
    rememberRun(pattern);
    startWorker([=] {
        RedditScraper s(so, workerLogger(), &cancelFlag);
        emit workerStatus(QStringLiteral("Fetching listing up to post %1…").arg(n2));
        const QList<Post> found = s.fetchPosts(sub, sort, tf, n2,
                                               [this](int a, int b) { emit workerProgress(a, b); }, q);
        const QList<Post> chosen = found.mid(n1 - 1, n2 - n1 + 1);
        if (chosen.isEmpty())
            throw ScrapeError(QStringLiteral("r/%1 only has %2 posts; nothing in %3–%4.")
                                  .arg(sub).arg(found.size()).arg(n1).arg(n2));
        downloadWith(s, chosen, dir, save);
    });
}

// --------------------------------------------------- worker thread code
RedditScraper::LogFn MainWindow::workerLogger()
{
    return [this](const QString &msg) { emit workerLog(msg); };
}

MainWindow::Tally MainWindow::downloadFolder(RedditScraper &s, const QList<Post> &chosen, const QString &dir, const SaveOptions &save)
{
    const int total = chosen.size();
    int ok = 0, failed = 0;
    emit workerProgress(0, total);
    for (int i = 1; i <= total; ++i) {
        const Post &p = chosen[i - 1];
        if (cancelFlag)
            throw Cancelled();
        emit workerStatus(QStringLiteral("[%1/%2] %3").arg(QString::number(i), QString::number(total), elide(p.title, 70)));
        try {
            const QString path = s.savePdf(p, dir, save);
            emit workerLog(QStringLiteral("✔ ") + QFileInfo(path).fileName());
            ++ok;
        } catch (const Cancelled &) {
            throw;
        } catch (const ScrapeError &e) {
            emit workerLog(QStringLiteral("✘ #%1 %2 — %3").arg(QString::number(p.rank), elide(p.title, 60), e.message()));
            ++failed;
        } catch (const std::exception &e) {
            emit workerLog(QStringLiteral("✘ #%1 %2 — %3")
                               .arg(QString::number(p.rank), elide(p.title, 60), QString::fromLocal8Bit(e.what())));
            ++failed;
        }
        emit workerProgress(i, total);
        if (i < total)
            s.sleep(s.delay());
    }
    return {ok, failed};
}

QString MainWindow::zipNote(const QString &dir, const SaveOptions &save, int ok)
{
    if (!save.zip || ok <= 0)
        return QString();
    const QString zip = storezip::writeZip(dir);
    if (zip.isEmpty()) {
        emit workerLog(QStringLiteral("✘ Couldn't write the .zip"));
        return QString();
    }
    return QStringLiteral("  (+ %1)").arg(QFileInfo(zip).fileName());
}

void MainWindow::downloadWith(RedditScraper &s, const QList<Post> &chosen, const QString &dir, const SaveOptions &save)
{
    const Tally t = downloadFolder(s, chosen, dir, save);
    const int ok = t.ok, failed = t.failed;
#ifdef Q_OS_WASM
    if (ok > 0) {
        const QByteArray zip = storezip::zipFolder(dir);
        const QString zipName = QFileInfo(dir).fileName() + QStringLiteral(".zip");
        QDir(dir).removeRecursively();   // free the in-memory copy
        QMetaObject::invokeMethod(this, [zip, zipName] { browserDownload(zip, zipName, "application/zip"); }, Qt::QueuedConnection);
        emit workerDone(QStringLiteral("Finished: %1 saved, %2 failed → downloading %3").arg(QString::number(ok), QString::number(failed), zipName));
        return;
    }
#endif
    emit workerDone(QStringLiteral("Finished: %1 saved, %2 failed → %3%4")
                        .arg(QString::number(ok), QString::number(failed), QDir::toNativeSeparators(dir), zipNote(dir, save, ok)));
}

void MainWindow::onDone(const QString &msg)
{
    lblStatus->setText(msg);
    appendLog(msg);
    setBusy(false);
}

void MainWindow::onError(const QString &msg)
{
    lblStatus->setText(QStringLiteral("Error."));
    appendLog(QStringLiteral("ERROR: ") + msg);
    setBusy(false);
    notify(this, QMessageBox::Critical, QStringLiteral("Error"), msg);
}
