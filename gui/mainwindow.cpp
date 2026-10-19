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

namespace {

enum Column { ColCheck, ColRank, ColTitle, ColScore, ColComments, ColAuthor };

QString elide(const QString &s, int n)
{
    return s.left(n);
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
    edOut = new QLineEdit(QDir::homePath() + QStringLiteral("/Documents/RedditPDFs"));
    grid->addWidget(edOut, 2, 1, 1, 4);
    auto *btnBrowse = new QPushButton(QStringLiteral("Browse…"));
    connect(btnBrowse, &QPushButton::clicked, this, &MainWindow::browse);
    grid->addWidget(btnBrowse, 2, 5);

    auto *opts = new QHBoxLayout;
    opts->addWidget(new QLabel(QStringLiteral("Site")));
    cbSite = new QComboBox;
    cbSite->setEditable(true);
    cbSite->addItems(SITE_PRESETS);
    cbSite->setMinimumContentsLength(22);
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
    opts->addStretch();
    grid->addLayout(opts, 3, 0, 1, 6);

    // Reddit API credentials (site = reddit-api)
    apiRow = new QWidget;
    auto *api = new QHBoxLayout(apiRow);
    api->setContentsMargins(0, 0, 0, 0);
    api->addWidget(new QLabel(QStringLiteral("API client ID")));
    edClientId = new QLineEdit(qEnvironmentVariable("REDDIT_CLIENT_ID"));
    edClientId->setPlaceholderText(hasBuiltinCredentials() ? QStringLiteral("built-in app (leave empty)")
                                                           : QStringLiteral("from reddit.com/prefs/apps"));
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
    root->addWidget(lblStatus);

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

void MainWindow::loadSettings()
{
    QSettings s;
    edOut->setText(s.value(QStringLiteral("output"), edOut->text()).toString());
    cbSite->setCurrentText(s.value(QStringLiteral("site"), DEFAULT_BASE).toString());
    cbPaper->setCurrentText(s.value(QStringLiteral("paper"), QStringLiteral("Letter")).toString());
    if (edClientId->text().isEmpty())
        edClientId->setText(s.value(QStringLiteral("clientId")).toString());
    if (edSecret->text().isEmpty())
        edSecret->setText(s.value(QStringLiteral("clientSecret")).toString());
}

void MainWindow::saveSettings() const
{
    QSettings s;
    s.setValue(QStringLiteral("output"), edOut->text());
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
    const QString d = QFileDialog::getExistingDirectory(this, QString(), edOut->text().isEmpty() ? QDir::homePath() : edOut->text());
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
    for (QPushButton *b : {btnLoad, btnDlSel, btnBatch})
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
        QMessageBox::critical(this, QStringLiteral("Subreddit"), e.message());
        return QString();
    }
}

QString MainWindow::outDir(const QString &sub) const
{
    QString base = edOut->text().trimmed();
    if (base == QLatin1String("~") || base.startsWith(QLatin1String("~/")))
        base = QDir::homePath() + base.mid(1);
    const QString tag = runTag(cbSort->currentText(), cbTime->currentText(), query());
    return QDir(base).filePath(QStringLiteral("r_%1_%2").arg(sub, tag));
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
        QMessageBox::information(this, QStringLiteral("Nothing selected"), QStringLiteral("Tick at least one post first."));
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
        QMessageBox::critical(this, QStringLiteral("Range"), QStringLiteral("Range must satisfy 1 ≤ from ≤ to."));
        return;
    }
    const QString sort = cbSort->currentText(), tf = cbTime->currentText(), q = query();
    const QString dir = outDir(sub);
    const ScraperOptions so = scraperOptions();
    const SaveOptions save = saveOptions();
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

void MainWindow::downloadWith(RedditScraper &s, const QList<Post> &chosen, const QString &dir, const SaveOptions &save)
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
    emit workerDone(QStringLiteral("Finished: %1 saved, %2 failed → %3")
                        .arg(QString::number(ok), QString::number(failed), QDir::toNativeSeparators(dir)));
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
    QMessageBox::critical(this, QStringLiteral("Error"), msg);
}
