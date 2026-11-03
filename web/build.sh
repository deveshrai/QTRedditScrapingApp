#!/bin/zsh
# Build the WebAssembly site into <outdir> (default web/site), ready for GitHub Pages.
# Needs: Qt 6.5.x for WebAssembly (multithread kit) + the matching host Qt, and Emscripten 3.1.25
# (the version Qt 6.5 expects). Install both once, e.g.:
#   pip install aqtinstall
#   aqt install-qt mac desktop 6.5.1 clang_64 -O ~/Qt-wasm   (host tools)
#   aqt install-qt mac desktop 6.5.1 wasm_multithread -O ~/Qt-wasm
#   git clone https://github.com/emscripten-core/emsdk && ./emsdk/emsdk install 3.1.25 && ./emsdk/emsdk activate 3.1.25
# Then:  QT_WASM=~/Qt-wasm/6.5.1/wasm_multithread EMSDK=~/emsdk web/build.sh [outdir]
# (EMSDK_PYTHON must point at Python >= 3.10 for emsdk.)
set -e
HERE=${0:A:h}
QT_WASM=${QT_WASM:?set QT_WASM to the wasm_multithread kit dir}
EMSDK=${EMSDK:?set EMSDK to the emsdk checkout}
OUT=${1:-$HERE/site}
source $EMSDK/emsdk_env.sh >/dev/null 2>&1
B=$(mktemp -d); cd $B
$QT_WASM/bin/qmake $HERE/RedditPDFWeb.pro >/dev/null
make -j8 2>&1 | grep -E "error|undefined" && exit 1
rm -rf $OUT && mkdir -p $OUT
cp RedditPDF.js RedditPDF.wasm RedditPDF.worker.js qtloader.js qtlogo.svg $HERE/coi-serviceworker.js $OUT/
# the service worker makes GitHub Pages cross-origin isolated (needed for wasm threads)
sed -e 's#<head>#<head>\n    <script src="coi-serviceworker.js"></script>#' \
    -e 's#<title>[^<]*</title>#<title>Reddit to PDF</title>#' \
    -e 's#Qt for WebAssembly: RedditPDF#Reddit to PDF (loading)…#' RedditPDF.html > $OUT/index.html
touch $OUT/.nojekyll
echo "built $OUT"; rm -rf $B
