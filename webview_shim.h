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
 * The JS↔C bridge uses on_invoke callback (JS→C), bindings (JS→C with return),
 * and eval (C→JS).
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

	/* Called when JS calls window.external.invoke(json_str).
	 * Note: this is NOT called for bind()-registered invocations. */
	void (*on_invoke)(struct webview_shim *w, const char *arg);

	/* Private state — do not touch */
	webview_shim_priv *priv;
} webview_shim_t;

/* ── Callback type for bound JS→C functions ───────────────────────────
 *   id:    unique request ID — pass to webview_shim_return()
 *   req:   JSON array of arguments from the JS call site
 *   arg:   user data registered in webview_shim_bind()
 */
typedef void (*webview_shim_bind_fn)(const char *id, const char *req, void *arg);

/* ── Core API (existing) ────────────────────────────────────────────── */
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

/* ── New API ────────────────────────────────────────────────────────── */

/* Navigate to a URL. Returns 0 on success, -1 on failure. */
int  webview_shim_navigate(webview_shim_t *w, const char *url);

/* Schedule fn(w, arg) to run on the main UI thread.
 * Always returns immediately. Thread-safe. */
void webview_shim_dispatch(webview_shim_t *w,
                           void (*fn)(struct webview_shim *w, void *arg),
                           void *arg);

/* Change the window title. Returns 0 on success, -1 on failure. */
int  webview_shim_set_title(webview_shim_t *w, const char *title);

/* Change the window size / constraints.
 *   hints: 0=none, 1=min, 2=max, 3=fixed
 * Returns 0 on success, -1 on failure. */
int  webview_shim_set_size(webview_shim_t *w, int width, int height, int hints);

/* Bind a named JS function on `window[name]` to a C callback.
 * The callback receives (id, args_json, arg) and must eventually call
 * webview_shim_return() to resolve/reject the JS Promise.
 * Returns 0 on success, -1 on failure. */
int  webview_shim_bind(webview_shim_t *w, const char *name,
                       webview_shim_bind_fn fn, void *arg);

/* Unbind a previously bound function. Returns 0 on success, -1 if not found. */
int  webview_shim_unbind(webview_shim_t *w, const char *name);

/* Return a value from a bind callback, resolving the JS Promise.
 *   id:      the request id passed to the callback
 *   success: non-zero to resolve, zero to reject
 *   result:  JSON-serializable string result */
void webview_shim_return(webview_shim_t *w, const char *id,
                         int success, const char *result);

/* Inject a JavaScript initialization string that runs before any page
 * content loads (at document start). Returns 0 on success, -1 on failure.
 * Call this before or after webview_shim_init() — scripts are queued. */
int  webview_shim_init_script(webview_shim_t *w, const char *js);

/* Replace the currently displayed HTML content entirely. */
int  webview_shim_set_html(webview_shim_t *w, const char *html);

/* Enter (non-zero) or leave (zero) fullscreen mode.
 * Returns 0 on success, -1 if unsupported. */
int  webview_shim_set_fullscreen(webview_shim_t *w, int fullscreen);

/* Forcefully terminate the event loop. Can be called from any thread. */
void webview_shim_terminate(webview_shim_t *w);

#ifdef __cplusplus
}
#endif

#endif /* WEBVIEW_SHIM_H */
