/* webview_shim.h — Runtime-dispatched cross-platform webview for cosmopolitan
 *
 * This header defines a clean C API that compiles under cosmocc with ZERO
 * system headers. All platform-specific code lives in webview_shim.c and is
 * resolved at RUNTIME via dlopen/dlsym (or LoadLibrary/GetProcAddress).
 *
 * Usage:
 *   1. #include "webview_shim.h"
 *   2. Call webview_shim_init(&w) to create the native window
 *   3. Call webview_shim_loop(&w, 1) in a while loop
 *   4. Call webview_shim_exit(&w) to clean up
 *
 * The JS↔C bridge uses the on_invoke callback (JS→C) and eval (C→JS).
 */
#ifndef WEBVIEW_SHIM_H
#define WEBVIEW_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ─────────────────────────────────────────────────── */
typedef struct webview_shim_priv webview_shim_priv;

typedef struct webview_shim {
	const char *app_name;       /* application name (macOS menu bar, Windows, etc.) */
	int         width;          /* initial width */
	int         height;         /* initial height */
	int         resizable;      /* 1 = resizable, 0 = fixed */
	int         context_menu;   /* 0 = disable right-click context menu */
	const char *html;           /* HTML string to load (NULL = blank) */

	/* Called when JS calls window.external.invoke(json_str). */
	void (*on_invoke)(struct webview_shim *w, const char *arg);

	/* Private state — do not touch */
	webview_shim_priv *priv;
} webview_shim_t;

/* ── API ───────────────────────────────────────────────────────────── */
/* Allocate & initialize the native window. Returns 0 on success. */
int  webview_shim_init(webview_shim_t *w);

/* Process one frame of the event loop. Returns 0 to keep looping, non-zero
 * when the window was closed. Pass blocking=1 to wait for an event,
 * blocking=0 to poll. */
int  webview_shim_loop(webview_shim_t *w, int blocking);

/* Execute JavaScript in the webview. Fire-and-forget. */
void webview_shim_eval(webview_shim_t *w, const char *js);

/* Destroy the native window and free all resources. */
void webview_shim_exit(webview_shim_t *w);

#ifdef __cplusplus
}
#endif

#endif /* WEBVIEW_SHIM_H */
