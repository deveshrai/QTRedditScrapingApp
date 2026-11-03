// main.cpp - Reddit -> PDF window (Qt port of main.py).

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("RedditPDF"));
    QApplication::setApplicationName(QStringLiteral("RedditPDF"));
    QApplication::setApplicationVersion(APP_VERSION);

    MainWindow w;
    w.show();
    return app.exec();
}
