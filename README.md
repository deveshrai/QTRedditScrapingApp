# Reddit → PDF — Qt 6 port (no Chromium)

A C++/Qt 6 port of [RedditPDFScraperForClaude](https://github.com/deveshrai/RedditPDFScraperForClaude) (Python + Playwright), as two separate programs built with qmake:

| Program | Project file | Port of |
|---|---|---|
| `RedditPDF` (window) | `gui/RedditPDFGui.pro` | `main.py` |
| `redditpdf-cli` (terminal) | `cli/RedditPDFCli.pro` | `cli.py` |

Both share the core in `common/` (included through `common/common.pri`). No browser engine is used: pages come over plain HTTP, and the PDFs are laid out by Qt (`QTextDocument` printed to `QPdfWriter`).

## Build

You need Qt 6 with the **Core, Gui, Network and Widgets** modules (Widgets only for the GUI). Qt WebEngine and Chromium are not needed.

```bash
cd QTRedditScrapingApp
mkdir build && cd build
qmake6 ../RedditPDF.pro      # builds both; or qmake6 ../gui/RedditPDFGui.pro / ../cli/RedditPDFCli.pro
make -j8
./gui/RedditPDF
./cli/redditpdf-cli --help
```

Each `.pro` also opens on its own in Qt Creator. On macOS use `qmake` from your Qt install, and on Windows use `nmake` or `jom` in place of `make`.

## Where the posts come from

Every public Redlib mirror (including the old default, safereddit.com) now sits behind a JavaScript bot check (Anubis and similar). old.reddit.com redirects to a login page, and Reddit's `.json` URLs return 403. Without a browser those checks can't be passed, and this port doesn't try to get around them. Instead it supports three sources:

| Site setting | What it is | What you need |
|---|---|---|
| `reddit-api` (default) | Reddit's official OAuth API | Your own free app: client ID (+ secret) |
| a Redlib URL, e.g. `http://localhost:8080` | Same HTML parsing as the Python version | A Redlib instance without a bot check, e.g. [self-hosted](https://github.com/redlib-org/redlib) |
| `https://old.reddit.com` | Same HTML parsing as the Python version | The Cookie header of a logged-in browser session |

If a site answers with a bot check or a login wall, you get a clear error, and the page is saved to `debug/last_listing.html`.

### Getting Reddit API credentials

1. Open <https://www.reddit.com/prefs/apps> and choose **create another app**.
2. Pick **script** (or **installed app**, which has no secret), give it any name, and set the redirect URI to `http://localhost:8080`.
3. The client ID is the string under the app name. Enter it and the secret in the GUI's *API client ID* / *Secret* fields, or pass them to the CLI with `--client-id` / `--client-secret` (or set `REDDIT_CLIENT_ID` / `REDDIT_CLIENT_SECRET`).

**Built-in credentials (optional).** Copy `reddit_credentials.pri.example` to `reddit_credentials.pri` and fill in the ID and secret. qmake turns it into `builtin_credentials.h` in the build folder, and both programs use those values whenever no other credentials are given. The order is: `--client-id`/`--client-secret`, then the environment variables, then the GUI fields, then the built-in values. Re-run qmake after editing the file. Git ignores `reddit_credentials.pri`. The secret ends up readable inside the compiled binaries, so don't share builds made with it.

Reddit may require you to accept its developer terms, or approve new apps, before they work. The app uses application-only OAuth, so it never asks for your Reddit password. The GUI remembers the ID and secret with `QSettings`, in plain text, in your user settings.

## Differences from the Python version

- **PDF look.** Chrome's print-to-PDF used to copy the web page. Qt now lays out its own clean document: title, metadata, link, images, post text (with code, tables, quotes and lists), then the comment tree with coloured indentation and an **OP** tag. The Chrome-style header and footer (date, title, URL, `n/N`), Letter/A4 paper and the 0.4in margins are kept.
- **Images** are downloaded and embedded (up to 40 per post). Formats Qt can't decode, such as WebP without the qtimageformats plugin, become links. Turn images off with the *Images* checkbox or `--no-images`.
- **"500 comments"** now also works with the API.
- **Browser options are gone.** *Show browser window*, `--show-browser`, `--browser`, `--chrome-path` and `--window-size` have no effect; the CLI still accepts them and prints a note, so old scripts keep running.
- **New CLI options:** `--api`, `--site-mode api`, `--client-id`, `--client-secret`, `--cookie`, `--no-images`. The default `--site` is `reddit-api`.
- **Debug snapshot** is the HTML only (`last_listing.html`), not a screenshot.
- **Flaired Redlib posts.** The Python version could pick up a post's flair as its title. Here the parser takes the link that points at the comments page.

Everything else carries over unchanged: sorts, time filters, in-subreddit search, the pick and batch modes, rank-numbered file names (`NNN - Title [id].pdf`), folder names (`r_<sub>_<tag>`), `--skip-existing`, `--flat`, `-i` interactive picking, `--list`, the polite delay, retry/429 handling, cancel (one Ctrl-C stops after the current step, a second quits), and the exit codes 0/1/2/3/130.

## Files

- `common/scraper.h/.cpp`: constants, helpers, retries, Redlib + old.reddit parsing, saving (port of `scraper.py`)
- `common/redditapi.cpp`: the Reddit API backend
- `common/htmldom.h/.cpp`: a small, forgiving HTML parser with CSS selectors (replaces the JavaScript that ran in Chrome)
- `common/http.h/.cpp`: blocking HTTP on top of `QNetworkAccessManager`
- `common/pdfrenderer.h/.cpp`: `QTextDocument` → `QPdfWriter`, with header and footer
- `gui/mainwindow.h/.cpp`, `gui/main.cpp`: the window
- `cli/main.cpp`: the command-line tool

`REDDITPDF_API_HOST` / `REDDITPDF_AUTH_HOST` point the API backend at a test server or proxy instead of oauth.reddit.com / www.reddit.com.
