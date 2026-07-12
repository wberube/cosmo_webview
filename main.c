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

/* ── Global counter for bind() demo ─────────────────────────────────── */
static int g_counter = 0;

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

	} else if (strcmp(fn, "set_title") == 0) {
		webview_shim_set_title(w, "Title Changed!");
		webview_shim_eval(w,
		    "document.getElementById('result').textContent = "
		    "'Window title changed to Title Changed!';");

	} else if (strcmp(fn, "navigate") == 0) {
		webview_shim_eval(w,
		    "document.getElementById('result').textContent = "
		    "'Navigate to https://example.com...';");
		webview_shim_navigate(w, "https://example.com");

	} else if (strcmp(fn, "set_html") == 0) {
		webview_shim_eval(w,
		    "document.getElementById('result').textContent = "
		    "'Reloading HTML...';");
		webview_shim_set_html(w,
		    "<html><body style='background:#0f0c29;color:#e0e0e0;"
		    "display:flex;align-items:center;justify-content:center;"
		    "height:100vh;font-family:sans-serif'>"
		    "<h1>HTML Replaced via webview_shim_set_html()</h1>"
		    "</body></html>");
	}

	#undef EXTRACT
}

/* ── bind() callback: window.count() returns a Promise ──────────────── */
static void
on_count(const char *id, const char *req, void *arg)
{
	(void)req;
	webview_shim_t *w = (webview_shim_t *)arg;
	g_counter++;

	char result[64];
	snprintf(result, sizeof(result), "%d", g_counter);
	webview_shim_return(w, id, 1, result);
}

/* ── bind() callback: window.echo("msg") returns the string ─────────── */
static void
on_echo(const char *id, const char *req, void *arg)
{
	webview_shim_t *w = (webview_shim_t *)arg;
	/* req is a JSON array like ["hello"] — pass it back as the value */
	webview_shim_return(w, id, 1, req);
}

/* ── Embedded HTML (now uses bind()-registered window.count etc.) ──── */
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
    ".btn-sm{font-size:0.8rem;padding:8px;margin-top:6px}"
    ".row{display:flex;gap:6px;margin-top:8px}"
    "pre{font-size:0.75rem;overflow-x:auto}"
    "#result{margin-top:20px;min-height:44px;padding:12px 16px;"
    "border-radius:10px;background:rgba(255,255,255,0.04);"
    "font-size:0.95rem;word-break:break-word;color:#c4b5fd;"
    "border:1px solid rgba(167,139,250,0.2)}"
    "#platform{font-size:0.78rem;color:#64748b;margin-top:18px}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"card\">"
    "<h1>&#9670; Cosmopolitan WebView</h1>"
    "<p class=\"sub\">One binary. Every OS. Native GUI.</p>"
    "<input id=\"name\" type=\"text\" placeholder=\"Your name&hellip;\" autofocus>"
    "<button onclick=\"doGreet()\">Greet from C</button>"
    "<div id=\"result\"></div>"
    "<hr style=\"margin:16px 0;border:none;border-top:1px solid rgba(255,255,255,0.08)\">"
    "<p style=\"font-size:0.85rem;margin-bottom:8px;color:#a78bfa\">&#9733; New bind() API &#9733;</p>"
    "<button onclick=\"window.count().then(function(v){document.getElementById('result').textContent='Count: '+v+', next...'});\">"
    "Count via bind()</button>"
    "<div class=\"row\">"
    "<button style=\"flex:1\" class=\"btn-sm\" onclick=\"window.echo('Hello from JS!').then(function(v){document.getElementById('result').textContent='Echo: '+v[0];})\">"
    "Echo via bind()</button>"
    "<button style=\"flex:1\" class=\"btn-sm\" onclick=\"webview_shim_set_title_demo()\">Set Title</button>"
    "</div>"
    "<div class=\"row\">"
    "<button style=\"flex:1\" class=\"btn-sm\" onclick=\"webview_shim_navigate_demo()\">Navigate</button>"
    "<button style=\"flex:1\" class=\"btn-sm\" onclick=\"webview_shim_set_html_demo()\">Reload HTML</button>"
    "</div>"
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
    "function webview_shim_set_title_demo(){"
    "  invoke(JSON.stringify({fn:'set_title'}));"
    "}"
    "function webview_shim_navigate_demo(){"
    "  invoke(JSON.stringify({fn:'navigate'}));"
    "}"
    "function webview_shim_set_html_demo(){"
    "  invoke(JSON.stringify({fn:'set_html'}));"
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
	w.app_name     = "Cosmopolitan WebView";
	w.width        = 800;
	w.height       = 600;
	w.resizable    = 1;
	w.context_menu = 0;
	w.html         = kHTML;
	w.on_invoke    = on_invoke;

	if (webview_shim_init(&w) != 0) {
		fprintf(stderr, "FATAL: could not initialize webview.\n");
		return 1;
	}

	/* ── Demo: init_script() — inject a script that runs before page load ── */
	webview_shim_init_script(&w,
	    "console.log('This init script runs at document start!');"
	    "document.documentElement.setAttribute('data-init','ok');");

	/* ── Demo: bind() — register named JS→C functions ────────────── */
	webview_shim_bind(&w, "count", on_count, &w);
	webview_shim_bind(&w, "echo",  on_echo,  &w);

	/* ── Demo: set_title() after init via a delayed dispatch ──────── */
	webview_shim_set_title(&w, "Cosmo WebView — API Demo");

	/* ── Event loop — blocks until the window is closed ──────────── */
	while (webview_shim_loop(&w, 1) == 0) {
		/* The dispatch queue is drained inside webview_shim_loop */
	}

	/* ── Demo: terminate() can be called from any thread ──────────── */
	webview_shim_terminate(&w);
	webview_shim_exit(&w);
	return 0;
}
