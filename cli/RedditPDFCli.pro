# RedditPDFCli.pro - the command-line tool (port of cli.py).
#   qmake6 RedditPDFCli.pro && make
# Needs no display: it uses Qt's "offscreen" platform unless QT_QPA_PLATFORM is set.

TEMPLATE = app
TARGET   = redditpdf-cli
CONFIG  += console
CONFIG  -= app_bundle

include(../common/common.pri)

SOURCES += main.cpp
