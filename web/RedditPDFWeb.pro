# RedditPDFWeb.pro - WebAssembly build of the window (Qt 6.5 for WebAssembly, multithread kit).
#   see web/build.sh; the page is hosted on GitHub Pages.
# Uses the GUI sources unchanged apart from Q_OS_WASM blocks. Built-in credentials
# are never compiled in (common.pri skips reddit_credentials.pri for wasm).

TEMPLATE = app
TARGET   = RedditPDF
QT      += widgets

include(../common/common.pri)

INCLUDEPATH += ../gui
HEADERS += ../gui/mainwindow.h
SOURCES += ../gui/main.cpp ../gui/mainwindow.cpp

# the scrape runs on a QThread; each thread is a Web Worker from this pool
QT_WASM_PTHREAD_POOL_SIZE = 6
QT_WASM_INITIAL_MEMORY    = 268435456

# synchronous fetch() from the worker thread (see common/http.cpp)
QMAKE_LFLAGS += -sFETCH

# the scraper reports errors (ScrapeError, Cancelled) as C++ exceptions; emscripten ignores them by default
QMAKE_CXXFLAGS += -fexceptions
QMAKE_LFLAGS   += -fexceptions

# test builds only: qmake TEST_HOST=http://127.0.0.1:8098 points both Reddit hosts at a local mock
!isEmpty(TEST_HOST): DEFINES += REDDITPDF_TEST_HOST=\\\"$$TEST_HOST\\\"
