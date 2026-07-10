#!/usr/bin/env bash
# build.sh — Download cosmocc, compile webview_shim + main.c into a portable APE
#
# The resulting binary detects the host OS AT RUNTIME and dlopen's the
# correct native GUI library (Cocoa/WebKit on macOS, GTK/WebKit2GTK on
# Linux, Edge WebView2 on Windows). Zero system headers at compile time.
#
# Usage:
#   chmod +x build.sh
#   ./build.sh
#
# Output: ./webview_demo.com (single-file APE binary)
set -euo pipefail

COSMOCC_URL="https://cosmo.zip/pub/cosmocc/cosmocc.zip"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/cosmocc"
COSMOCC_BIN="${CACHE_DIR}/bin/cosmocc"
OUTPUT="webview_demo.com"

download() {
	local u="$1" d="$2"
	if command -v curl &>/dev/null; then
		curl -fSL --progress-bar -o "$d" "$u"
	elif command -v wget &>/dev/null; then
		wget -q --show-progress -O "$d" "$u"
	else
		echo "ERROR: Need curl or wget." >&2; exit 1
	fi
}

echo "==> Checking for cosmocc in ${CACHE_DIR}..."
if [[ ! -x "${COSMOCC_BIN}" ]]; then
	echo "    Not found. Downloading latest cosmocc..."
	mkdir -p "${CACHE_DIR}"
	rm -rf "${CACHE_DIR}/bin" "${CACHE_DIR}/libexec" \
	       "${CACHE_DIR}/lib" "${CACHE_DIR}/include" \
	       "${CACHE_DIR}/share" 2>/dev/null || true
	ZIP="${CACHE_DIR}/cosmocc-latest.zip"
	download "${COSMOCC_URL}" "${ZIP}"
	echo "    Extracting into ${CACHE_DIR}..."
	unzip -qo "${ZIP}" -d "${CACHE_DIR}"
	if [[ -d "${CACHE_DIR}/cosmocc" ]]; then
		shopt -s dotglob 2>/dev/null || true
		for item in "${CACHE_DIR}/cosmocc"/*; do
			[[ -e "$item" ]] || continue
			b="$(basename "$item")"
			rm -rf "${CACHE_DIR:?}/${b}"
			mv "$item" "${CACHE_DIR}/"
		done
		rmdir "${CACHE_DIR}/cosmocc"
	fi
	chmod +x "${COSMOCC_BIN}" 2>/dev/null || true
	rm -f "${ZIP}"
	echo "    cosmocc installed to ${CACHE_DIR}"
else
	echo "    Found cached cosmocc."
fi

for f in main.c webview_shim.c webview_shim.h; do
	if [[ ! -f "$f" ]]; then
		echo "ERROR: $f not found in current directory." >&2; exit 1
	fi
done

echo "==> Compiling ${OUTPUT} with cosmocc (runtime-dispatched APE)..."
export PATH="${CACHE_DIR}/bin:${PATH}"

# No -DWEBVIEW_* flag needed — the shim detects the OS at runtime
"${COSMOCC_BIN}" -O2 -o "${OUTPUT}" main.c webview_shim.c

if [[ -f "${OUTPUT}" ]]; then
	SIZE=$(du -h "${OUTPUT}" | cut -f1)
	echo ""
	echo "═══════════════════════════════════════════════════"
	echo "  SUCCESS: ${OUTPUT} built (${SIZE})"
	echo ""
	echo "  This APE auto-detects the OS at runtime and uses"
	echo "  the native GUI via dlopen/dlsym."
	echo ""
	echo "  Run it: ./${OUTPUT}"
	echo ""
	echo "  Runtime prerequisites:"
	echo "    macOS:   none — built-in Cocoa/WebKit"
	echo "    Linux:   libgtk-3-0 + libwebkit2gtk-4.1-0"
	echo "    Windows: Edge WebView2 runtime"
	echo "═══════════════════════════════════════════════════"
else
	echo "ERROR: Compilation failed." >&2; exit 1
fi

