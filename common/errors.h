#pragma once
// Exceptions shared by the scraper, the GUI and the CLI (scraper.py's
// ScrapeError / Cancelled).

#include <QString>
#include <stdexcept>

class ScrapeError : public std::runtime_error {
public:
    explicit ScrapeError(const QString &message)
        : std::runtime_error(message.toStdString()), msg(message) {}
    QString message() const { return msg; }

private:
    QString msg;
};

class Cancelled : public std::exception {
public:
    const char *what() const noexcept override { return "cancelled"; }
};
