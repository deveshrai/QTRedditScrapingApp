#pragma once
// mainwindow.h - the Reddit -> PDF window (Qt Widgets port of main.py).

#include "scraper.h"

#include <QMainWindow>
#include <QMetaType>
#include <QPointer>
#include <QSet>
#include <atomic>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

Q_DECLARE_METATYPE(Post)

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(class QCloseEvent *event) override;

signals:
    // Emitted from the worker thread; delivered to the UI thread (queued).
    void workerLog(const QString &msg);
    void workerStatus(const QString &msg);
    void workerProgress(int done, int total);
    void workerPosts(const QList<Post> &posts);
    void workerDone(const QString &msg);
    void workerError(const QString &msg);

private:
    // ui
    void buildUi();
    QWidget *buildPickTab();
    QWidget *buildBatchTab();
    void loadSettings();
    void saveSettings() const;

    // helpers
    QString query() const;
    void onSortChange();
    void onQueryChange();
    void onSiteChange();
    void browse();
    void appendLog(const QString &msg);
    void setBusy(bool busy);
    QString validateSource();
    QString outDir(const QString &sub) const;
    ScraperOptions scraperOptions() const;
    SaveOptions saveOptions() const;
    void startWorker(std::function<void()> job);
    void cancel();

    // list / checkboxes
    void populate(const QList<Post> &posts);
    void setAll(bool on);
    void invert();
    void updateSelLabel();

    // actions
    void loadPosts();
    void downloadSelected();
    void downloadBatch();

    // worker-thread code: talks to the UI only through the worker* signals
    RedditScraper::LogFn workerLogger();
    void downloadWith(RedditScraper &s, const QList<Post> &chosen, const QString &dir, const SaveOptions &save);

    void onDone(const QString &msg);
    void onError(const QString &msg);

    // widgets
    QLineEdit *edSub = nullptr, *edQuery = nullptr, *edOut = nullptr;
    QLineEdit *edClientId = nullptr, *edSecret = nullptr, *edCookie = nullptr;
    QComboBox *cbSort = nullptr, *cbTime = nullptr, *cbSite = nullptr, *cbPaper = nullptr, *cbLoadN = nullptr;
    QCheckBox *chkHeader = nullptr, *chkComments = nullptr, *chkImages = nullptr;
    QWidget *apiRow = nullptr, *cookieRow = nullptr;
    QTabWidget *tabs = nullptr;
    QTreeWidget *tree = nullptr;
    QLabel *lblSel = nullptr, *lblStatus = nullptr;
    QPushButton *btnLoad = nullptr, *btnDlSel = nullptr, *btnBatch = nullptr, *btnCancel = nullptr;
    QRadioButton *rb25 = nullptr, *rb50 = nullptr, *rb100 = nullptr, *rbCustom = nullptr;
    QSpinBox *spFrom = nullptr, *spTo = nullptr;
    QProgressBar *progress = nullptr;
    QPlainTextEdit *logView = nullptr;

    // state
    bool sortTouched = false, timeTouched = false;
    QList<Post> posts;
    QPointer<QThread> worker;
    std::atomic_bool cancelFlag{false};
};
