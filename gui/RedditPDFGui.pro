# RedditPDFGui.pro - the windowed app (port of main.py).
#   qmake6 RedditPDFGui.pro && make

TEMPLATE = app
TARGET   = RedditPDF
QT      += widgets

include(../common/common.pri)

HEADERS += mainwindow.h runhistory.h
SOURCES += main.cpp mainwindow.cpp

win32: CONFIG += windows

# App icon (source: icon/RedditPDF.svg)
macx: ICON = $$PWD/icon/RedditPDF.icns
