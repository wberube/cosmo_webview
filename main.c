/* main.c — Cosmopolitan + webview_shim demo application
 *
 * Compiles under cosmocc with ZERO system headers. All GUI is resolved
 * at runtime via dlopen/dlsym (webview_shim.c).
 *
 * Build:
 *   ./build.sh        (macOS/Linux/BSD)
 *   build.bat          (Windows)
 *
 * Required files: main.c, webview_shim.h, webview_shim.c
 * Output: webview-demo (portable APE binary)
 */
#include "webview_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── JS→C bridge: called when JS does window.external.invoke(json) ─ */
static void
on_invoke(webview_shim_t *w, const char *arg)
{
	char fn[64]   = {0};
	char name[256] = "World";

	/* Simple JSON field parser */
	#define EXTRACT(field, dest, dsize) do { \
		const char *k = "\"" field "\""; \
		const char *p = strstr(arg, k); \
		if (p) { \
			p = strchr(p + (int)strlen(k), '"'); \
			if (p) { p++; \
				const char *q = strchr(p, '"'); \
				if (q) { \
					size_t n = (size_t)(q - p); \
					if (n < (size_t)(dsize)) { \
						memcpy(dest, p, n); \
						dest[n] = 0; \
					} \
				} \
			} \
		} \
	} while(0)

	EXTRACT("fn",   fn,   sizeof(fn));
	EXTRACT("name", name, sizeof(name));

	if (strcmp(fn, "greet") == 0) {
		char js[512];
		snprintf(js, sizeof(js),
		    "document.getElementById('result').textContent = "
		    "'Hello, %s! (from pure C on cosmopolitan libc)';",
		    name);
		webview_shim_eval(w, js);

	} else if (strcmp(fn, "platform") == 0) {
		webview_shim_eval(w,
		    "document.getElementById('platform').textContent = "
		    "'Running on cosmopolitan libc (portable APE).';");
	}

	#undef EXTRACT
}

/* ── Embedded HTML ────────────────────────────────────────────────── */
static const char kHTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Demo Page</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;"
    "background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);"
    "color:#e0e0e0;display:flex;align-items:center;justify-content:center;"
    "height:100vh;overflow:hidden}"
    ".card{background:rgba(255,255,255,0.06);backdrop-filter:blur(16px);"
    "border:1px solid rgba(255,255,255,0.1);border-radius:16px;"
    "padding:40px 32px;width:90vw;max-width:420px;text-align:center;"
    "box-shadow:0 25px 60px rgba(0,0,0,0.4)}"
    "h1{font-size:1.6rem;font-weight:600;margin-bottom:6px;"
    "background:linear-gradient(90deg,#a78bfa,#60a5fa);"
    "-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    ".sub{font-size:0.85rem;color:#94a3b8;margin-bottom:24px}"
    "input{width:100%;padding:12px 16px;border-radius:10px;border:1px solid "
    "rgba(255,255,255,0.15);background:rgba(255,255,255,0.05);color:#f1f5f9;"
    "font-size:0.95rem;outline:none;transition:border .2s}"
    "input:focus{border-color:#818cf8}"
    "button{width:100%;margin-top:14px;padding:12px;border:none;"
    "border-radius:10px;background:linear-gradient(135deg,#6366f1,#8b5cf6);"
    "color:#fff;font-size:1rem;font-weight:600;cursor:pointer;"
    "transition:transform .15s,box-shadow .15s}"
    "button:hover{transform:translateY(-1px);box-shadow:0 8px 20px "
    "rgba(99,102,241,0.35)}"
    "button:active{transform:translateY(0)}"
    "#result{margin-top:20px;min-height:44px;padding:12px 16px;"
    "border-radius:10px;background:rgba(255,255,255,0.04);"
    "font-size:0.95rem;word-break:break-word;color:#c4b5fd;"
    "border:1px solid rgba(167,139,250,0.2)}"
    "#platform{font-size:0.78rem;color:#64748b;margin-top:18px}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"card\">"
    "<svg viewBox=\"0 0 160 160\" width=\"64\" height=\"64\" "
    "style=\"margin-bottom:12px\">"
    "<rect x=\"16\" y=\"36\" width=\"128\" height=\"96\" rx=\"12\" "
    "fill=\"none\" stroke=\"#6366f1\" stroke-width=\"5\"/>"
    "<circle cx=\"28\" cy=\"48\" r=\"4\" fill=\"#6366f1\"/>"
    "<circle cx=\"42\" cy=\"48\" r=\"4\" fill=\"#6366f1\"/>"
    "<circle cx=\"56\" cy=\"48\" r=\"4\" fill=\"#6366f1\"/>"
    "<line x1=\"72\" y1=\"48\" x2=\"136\" y2=\"48\" "
    "stroke=\"#6366f1\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
    "<line x1=\"36\" y1=\"72\" x2=\"124\" y2=\"72\" "
    "stroke=\"#a5b4fc\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
    "<line x1=\"36\" y1=\"90\" x2=\"100\" y2=\"90\" "
    "stroke=\"#a5b4fc\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
    "<line x1=\"36\" y1=\"108\" x2=\"80\" y2=\"108\" "
    "stroke=\"#a5b4fc\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
    "</svg>"
    "<h1>&#9670; Cosmopolitan WebView</h1>"
    "<p class=\"sub\">One binary. Every OS. Native GUI.</p>"
    "<input id=\"name\" type=\"text\" placeholder=\"Your name&hellip;\" autofocus>"
    "<button onclick=\"doGreet()\">Greet from C</button>"
    "<div id=\"result\"></div>"
    "<div id=\"platform\">Detecting platform&#8230;</div>"
    "</div>"
    "<script>"
    "function invoke(msg){"
    "  window.external.invoke(msg);"
    "}"
    "function doGreet(){"
    "  var name=document.getElementById('name').value||'World';"
    "  invoke(JSON.stringify({fn:'greet',name:name}));"
    "}"
    "window.addEventListener('load',function(){"
    "  invoke(JSON.stringify({fn:'platform'}));"
    "});"
    "</script>"
    "</body>"
    "</html>";

/* ── Entry point ──────────────────────────────────────────────────── */
int
main(void)
{
	webview_shim_t w;

	memset(&w, 0, sizeof(w));
	w.app_name = "Cosmopolitan WebView";
	w.width     = 800;
	w.height    = 600;
	w.resizable = 1;
	w.context_menu = 0;
	w.html      = kHTML;
	w.on_invoke = on_invoke;

	if (webview_shim_init(&w) != 0) {
		fprintf(stderr, "FATAL: could not initialize webview.\n");
		return 1;
	}

	/* Event loop — blocks until the window is closed */
	while (webview_shim_loop(&w, 1) == 0) {
		/* spin */
	}

	webview_shim_exit(&w);
	return 0;
}
