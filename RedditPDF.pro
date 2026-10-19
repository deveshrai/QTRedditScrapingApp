# RedditPDF.pro - builds both projects:  qmake6 && make
# Each one also builds on its own (open gui/RedditPDFGui.pro or cli/RedditPDFCli.pro).

TEMPLATE = subdirs
SUBDIRS  = gui cli
gui.file = gui/RedditPDFGui.pro
cli.file = cli/RedditPDFCli.pro
