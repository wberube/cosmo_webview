/* webview_shim.c — Runtime-dlopen webview for cosmopolitan libc
 *
 * Compiles under cosmocc with ZERO system headers.
 * At RUNTIME detects the host OS and dlopen's the native GUI:
 *   macOS  → Cocoa/WebKit  (via ObjC runtime)
 *   Linux  → GTK3 + WebKit2GTK
 *   Windows → Edge WebView2 (via COM)
 *
 * The JS↔C bridge uses window.external.invoke(json):
 *   JS→C:  window.external.invoke(json) — intercepted via WKNavigationDelegate
 *          (macOS), WebKit user content manager (Linux), or NavigationStarting
 *          (Windows), all using an about:blank?__invoke= URL-based protocol
 *   C→JS:  webview_shim_eval(w, js)
 */

#define _GNU_SOURCE /* for RTLD_DEFAULT */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Debug printing — only active when compiled with -DWEBVIEW_DEBUG */
#ifdef WEBVIEW_DEBUG
#define DBG_PRINT(fmt, ...) do { fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); } while(0)
#else
#define DBG_PRINT(fmt, ...) ((void)0)
#endif

/* {{{ Cosmopolitan platform detection — always available */
#if defined(__COSMOPOLITAN__)
  #include <cosmo.h>
#else
  #include <stdbool.h>
  static inline int IsLinux(void)   {
  #if defined(__linux__)
    return 1;
  #else
    return 0;
  #endif
  }
  static inline int IsXnu(void)     {
  #if defined(__APPLE__)
    return 1;
  #else
    return 0;
  #endif
  }
  static inline int IsWindows(void) {
  #if defined(_WIN32)
    return 1;
  #else
    return 0;
  #endif
  }
  static inline int IsFreebsd(void) {
  #if defined(__FreeBSD__)
    return 1;
  #else
    return 0;
  #endif
  }
#endif
/* }}} */

#if defined(__COSMOPOLITAN__)
  extern void *cosmo_dlopen(const char *, int);
  extern void *cosmo_dlsym(void *, const char *);
  extern int   cosmo_dlclose(void *);
  /* cosmopolitan ships proper __stdcall wrappers for Win32 windowing APIs.
     Do NOT call RegisterClassExW/CreateWindowExW via cosmo_dlsym — the raw
     function-pointer ABI mismatches and SIGSEGVs inside user32. Use these. */
  #include "libc/nt/dll.h"
  #include "libc/nt/windows.h"
  #include "libc/nt/struct/wndclass.h"
#endif

/* cosmo_dlsym returns pointers whose arguments are corrupted by cosmocc's
   ABI bridge UNLESS the function pointer is declared with __ms_abi__ (the
   Microsoft x64 calling convention). Without it, WINAPI functions taking
   arguments (CoInitializeEx, CoCreateInstance, CreateCoreWebView2Environment-
   WithOptions, etc.) receive garbage and either return bogus HRESULTs or
   SIGSEGV. Declare every dlsym-resolved WINAPI fn pointer with MSABI. */
#if defined(__COSMOPOLITAN__)
  #define MSABI __attribute__((__ms_abi__))
#else
  #define MSABI
#endif

#include "webview_shim.h"

/* ── Forward declarations ──────────────────────────────────────────── */
static int  backend_init(webview_shim_t *w);
static int  backend_loop(webview_shim_t *w, int blocking);
static void backend_eval(webview_shim_t *w, const char *js);
static void backend_exit(webview_shim_t *w);

/* ── Internal structures ───────────────────────────────────────────── */
#define WV_MAX_BINDINGS 64
#define WV_MAX_INIT_SCRIPTS 32

typedef struct {
    char name[64];
    webview_shim_bind_fn fn;
    void *arg;
} wv_binding_t;

typedef struct wv_dispatch_item {
    void (*fn)(webview_shim_t *w, void *arg);
    void *arg;
    struct wv_dispatch_item *next;
} wv_dispatch_item_t;

/* Simple spinlock helpers for dispatch queue */
static inline int wv_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) { /* spin */ }
    return 0;
}
static inline void wv_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

/* ── Per-platform private state ─────────────────────────────────────── */
struct webview_shim_priv {
	int   (*init)(webview_shim_t *w);
	int   (*loop)(webview_shim_t *w, int blocking);
	void  (*eval)(webview_shim_t *w, const char *js);
	void  (*exit)(webview_shim_t *w);
	void  *handle;       /* dlopen handle for cleanup */
	int    platform;     /* 0=macOS, 1=Linux, 2=Windows */
	int    running;      /* 1 while window is open */
	void  *native;       /* platform-specific native window pointer */
	void  *nswindow;     /* macOS: NSWindow*; Linux: GtkWindow* */
	void  *webview;      /* macOS: WKWebView*; Linux: WebKitWebView* */

	/* Binding table */
	wv_binding_t bindings[WV_MAX_BINDINGS];
	int          bindings_count;

	/* Dispatch queue (spinlock-protected) */
	wv_dispatch_item_t *dispatch_head;
	wv_dispatch_item_t *dispatch_tail;
	volatile int        dispatch_lock;

	/* Init scripts (runs at document start) */
	char *init_scripts[WV_MAX_INIT_SCRIPTS];
	int   init_scripts_count;

	/* macOS cached ObjC runtime + msgSend variants */
	void  *objc_lib;                /* cosmo_dlopen("libobjc.A.dylib") handle */
	void  *ms_objc_getClass;        /* objc_getClass */
	void  *ms_sel_registerName;     /* sel_registerName */
	void  *fn_objc_msgSend;         /* objc_msgSend (variadic, void return) */
	void  *fn_objc_msgSend_id;      /* objc_msgSend returning id */
	void  *fn_objc_msgSend_id_id;   /* objc_msgSend(self, sel, id) -> id */

	/* Linux cached library handles */
	void  *gtk_lib;           /* cosmo_dlopen("libgtk-3.so.0") handle */
	void  *webkit_lib;        /* cosmo_dlopen("libwebkit2gtk-*") handle */
	/* Linux cached function pointers */
	void  *fn_gtk_main_iteration;
	void  *fn_gtk_main_quit;
	void  *fn_gtk_events_pending;
	void  *fn_gtk_main;
	void  *fn_wk_evaluate_javascript;
	void  *fn_wk_get_child;
	void  *fn_gtk_init;
	void  *fn_gtk_window_new;
	void  *fn_gtk_window_set_title;
	void  *fn_gtk_window_set_default_size;
	void  *fn_gtk_container_add;
	void  *fn_gtk_widget_show_all;
	void  *fn_gtk_window_set_resizable;
	void  *fn_g_signal_connect_data;
	void  *fn_g_object_unref;
	void  *fn_g_free;
	void  *fn_wk_new;
	void  *fn_wk_load_html;
	void  *fn_wk_load_uri;
	void  *fn_wk_get_settings;
	void  *fn_wk_set_enable_scripts;
	void  *fn_wk_set_enable_developer_extras;
	void  *fn_wk_get_ucm;
	void  *fn_wk_ucm_reg_msg;
	void  *fn_wk_user_script_new;
	void  *fn_wk_ucm_add_script;
	void  *fn_wk_js_result_get_js_value;
	void  *fn_wk_jsc_value_to_string;

	/* Windows cached user32 function pointers */
	void  *fn_PeekMessageW;
	void  *fn_GetMessageW;
	void  *fn_TranslateMessage;
	void  *fn_DispatchMessageW;
	void  *fn_CoUninitialize;
	void  *fn_PostMessageW;
};

/* ── Internal dispatch: drain pending dispatch items ────────────── */
static void
wv_drain_dispatch(webview_shim_priv *p, webview_shim_t *w)
{
    /* Swap out the queue under lock */
    wv_dispatch_item_t *item = NULL;
    wv_lock(&p->dispatch_lock);
    item = p->dispatch_head;
    p->dispatch_head = NULL;
    p->dispatch_tail = NULL;
    wv_unlock(&p->dispatch_lock);

    while (item) {
        wv_dispatch_item_t *next = item->next;
        if (item->fn) item->fn(w, item->arg);
        free(item);
        item = next;
    }
}

/* ── Internal invoke router: bindings first, then on_invoke ──────── */
static void
wv_handle_invoke(webview_shim_t *w, const char *arg)
{
    if (!w || !w->priv || !arg) return;

    webview_shim_priv *p = w->priv;

    /* Check for bind() call: {"__bind__":"name","__id__":"id","__args__":[...]} */
    if (arg[0] == '{' && strstr(arg, "\"__bind__\"")) {
        char bname[64] = {0};
        char bid[128]  = {0};
        const char *bkey, *pstr;

        /* Extract __bind__ value */
        bkey = strstr(arg, "\"__bind__\"");
        if (bkey) {
            pstr = strchr(bkey + 10, '"'); /* skip past "__bind__": */
            if (pstr) {
                pstr++;
                const char *q = strchr(pstr, '"');
                if (q) {
                    size_t n = (size_t)(q - pstr);
                    if (n < sizeof(bname)) { memcpy(bname, pstr, n); bname[n] = 0; }
                }
            }
        }

        /* Extract __id__ value */
        bkey = strstr(arg, "\"__id__\"");
        if (bkey) {
            pstr = strchr(bkey + 8, '"');
            if (pstr) {
                pstr++;
                const char *q = strchr(pstr, '"');
                if (q) {
                    size_t n = (size_t)(q - pstr);
                    if (n < sizeof(bid)) { memcpy(bid, pstr, n); bid[n] = 0; }
                }
            }
        }

        /* Extract __args__ value — pass the JSON array as-is to callback */
        if (bname[0] && bid[0]) {
            char args_json[4096] = {0};
            bkey = strstr(arg, "\"__args__\"");
            if (bkey) {
                pstr = bkey + 10; /* skip "__args__": */
                /* Find the start of array/object — skip whitespace */
                while (*pstr && *pstr != '[' && *pstr != '{') pstr++;
                if (*pstr) {
                    /* Match brackets — simplified: just find matching ] or } */
                    char open = *pstr;
                    char close = (open == '[') ? ']' : '}';
                    int depth = 1;
                    const char *q = pstr + 1;
                    while (*q && depth > 0 && (size_t)(q - pstr) < sizeof(args_json) - 1) {
                        if (*q == open) depth++;
                        else if (*q == close) depth--;
                        q++;
                    }
                    size_t n = (size_t)(q - pstr);
                    if (n < sizeof(args_json)) {
                        memcpy(args_json, pstr, n);
                        args_json[n] = 0;
                    }
                }
            }
            /* If we couldn't extract args, pass an empty array */
            if (!args_json[0]) {
                memcpy(args_json, "[]", 3);
            }

            /* Look up binding and call it */
            for (int i = 0; i < p->bindings_count; i++) {
                if (strcmp(p->bindings[i].name, bname) == 0) {
                    if (p->bindings[i].fn)
                        p->bindings[i].fn(bid, args_json, p->bindings[i].arg);
                    return;
                }
            }
        }
        return;
    }

    /* Not a bind call — forward to user's on_invoke */
    if (w->on_invoke)
        w->on_invoke(w, arg);
}

/* ═══════════════════════════════════════════════════════════════════════
   macOS / Cocoa + WebKit backend
   ═══════════════════════════════════════════════════════════════════════
   Uses WKUserContentController for JS→C bridge.
   ═══════════════════════════════════════════════════════════════════════ */

typedef void* (*objc_getClass_t)(const char *name);
typedef void* (*sel_registerName_t)(const char *str);
typedef void* (*objc_msgSend_t)(void);

typedef void  (*msgSend_void_fn)(void *self, void *sel, ...);
typedef void* (*msgSend_id_fn)(void *self, void *sel, ...);
typedef void* (*msgSend_id_id_fn)(void *self, void *sel, void *a);
typedef void* (*msgSend_id_id_id_fn)(void *self, void *sel, void *a, void *b);
typedef void* (*msgSend_id_4_fn)(void *self, void *sel, void *a, void *b, void *c);
typedef void* (*msgSend_id_rect_str_fn)(void *self, void *sel, double x, double y, double w, double h, unsigned long mask, unsigned long backing, int flag);
typedef void* (*msgSend_id_rect_id_fn)(void *self, void *sel, double x, double y, double w, double h, void *cfg);
typedef void  (*msgSend_void_id_fn)(void *self, void *sel, void *a);
typedef void  (*msgSend_void_fn2)(void *self, void *sel);
typedef void  (*msgSend_void_bool_fn)(void *self, void *sel, int b);

static webview_shim_t *g_active_webview = NULL;

/* ── Window close handler ────────────────────────────────────────── */
static void
window_will_close(void *self, void *sel, void *notif)
{
	(void)self; (void)sel; (void)notif;
	if (g_active_webview)
		g_active_webview->priv->running = 0;
}

/* Forward declaration for navigation delegate callback */
static void
nav_decide_policy(void *self, void *sel, void *webView,
                  void *navigationAction, void *decisionHandler);

/* Cosmopolitan APE names itself .ape-1.10 in the macOS menu bar.
   Override it via NSProcessInfo so the app name matches the title. */
#if defined(__COSMOPOLITAN__) && defined(__APPLE__)
static void macos_set_process_name(void *lib, const char *app_name)
{
    const char *name = app_name ? app_name : "WebView";
    if (!name[0]) return;
    void* (*mi2)(void*,void*) = (void* (*)(void*,void*))cosmo_dlsym(lib, "objc_msgSend");
    void* (*mi3)(void*,void*,void*) = (void* (*)(void*,void*,void*))cosmo_dlsym(lib, "objc_msgSend");
    void* (*oc)(const char*) = (void* (*)(const char*))cosmo_dlsym(lib, "objc_getClass");
    void* (*sr)(const char*) = (void* (*)(const char*))cosmo_dlsym(lib, "sel_registerName");
    if (!mi2 || !mi3 || !oc || !sr) return;
    void *info = mi2(oc("NSProcessInfo"), sr("processInfo"));
    if (!info) return;
    void *nsobj = mi3(oc("NSString"), sr("stringWithUTF8String:"), (void*)name);
    mi3(info, sr("setProcessName:"), nsobj);
}
#else
#define macos_set_process_name(l,n) ((void)0)
#endif

/* ── Shared helper: build "app_name | <title>" window title ──────── */
/* Returns a pointer into a static buf. Not reentrant. */
static const char*
build_window_title(const webview_shim_t *w)
{
    static char buf[512];
    buf[0] = 0;
    if (!w || !w->html) return buf;
    const char *ts = strstr(w->html, "<title>");
    if (!ts) return buf;
    ts += 7;
    const char *te = strstr(ts, "</title>");
    if (!te) return buf;
    size_t tl = (size_t)(te - ts);
    if (tl == 0 || tl > 256) return buf;
    char ht[256];
    memcpy(ht, ts, tl);
    ht[tl] = 0;
    if (w->app_name && w->app_name[0])
        snprintf(buf, sizeof(buf), "%s | %s", w->app_name, ht);
    else
        snprintf(buf, sizeof(buf), "%s", ht);
    return buf;
}

/* ── Create a shared delegate class with methods for window close
      (avoids objc_getProtocol issues). ──────────────────────────────── */
static void*
create_delegate_class(void *lib)
{
	objc_getClass_t    getClass = (objc_getClass_t)cosmo_dlsym(lib, "objc_getClass");
	sel_registerName_t regSel  = (sel_registerName_t)cosmo_dlsym(lib, "sel_registerName");
	void *NSObject = getClass("NSObject");
	if (!NSObject) return NULL;

	void* (*allocClass)(void*,const char*,size_t) =
	    (void* (*)(void*,const char*,size_t))cosmo_dlsym(lib, "objc_allocateClassPair");
	void *cls = allocClass(NSObject, "WebviewShimDelegate", 0);
	if (!cls) return NULL;

	int (*addMethod)(void*,void*,void*,const char*) =
	    (int (*)(void*,void*,void*,const char*))cosmo_dlsym(lib, "class_addMethod");

	addMethod(cls, regSel("windowWillClose:"),
	    (void*)window_will_close, "v@:@");
	addMethod(cls, regSel("webView:decidePolicyForNavigationAction:decisionHandler:"),
	    (void*)nav_decide_policy, "v@:@@@");

	void (*regClass)(void*) = (void (*)(void*))cosmo_dlsym(lib, "objc_registerClassPair");
	regClass(cls);
	return cls;
}

/* ── WKNavigationDelegate method — receives JS invoke messages ──── */
static void
nav_decide_policy(void *self, void *sel, void *webView, void *navigationAction, void *decisionHandler)
{
	(void)self;
	(void)sel;
	(void)webView;

	if (!g_active_webview)
		goto allow;
	webview_shim_priv *p = g_active_webview->priv;
	if (!p || !p->objc_lib || (!g_active_webview->on_invoke && p->bindings_count == 0))
		goto allow;

	msgSend_id_fn msg_id = (msgSend_id_fn)p->fn_objc_msgSend_id;
	sel_registerName_t selReg = (sel_registerName_t)p->ms_sel_registerName;
	if (!msg_id || !selReg) goto allow;

	void *request = msg_id(navigationAction, selReg("request"));
	if (!request) goto allow;
	void *url = msg_id(request, selReg("URL"));
	if (!url) goto allow;
	void *absStr = msg_id(url, selReg("absoluteString"));
	if (!absStr) goto allow;
	const char *str = (const char *)msg_id(absStr, selReg("UTF8String"));
	if (!str) goto allow;

	const char *pstr = strstr(str, "__invoke=");
	if (!pstr)
		goto allow;
	pstr += 9;

	char *decoded = NULL;
	if (strchr(pstr, '%')) {
		decoded = (char *)malloc(strlen(pstr) + 1);
		if (decoded) {
			char *d = decoded;
			while (*pstr) {
				if (*pstr == '%' && *(pstr+1) && *(pstr+2)) {
					char hex[3] = {pstr[1], pstr[2], 0};
					*d++ = (char)strtol(hex, NULL, 16);
					pstr += 3;
				} else {
					*d++ = *pstr++;
				}
			}
			*d = 0;
		}
	}

	{
		void *invoke = *(void **)((char *)decisionHandler + 16);
		((void (*)(void *, unsigned long))invoke)(decisionHandler, 0);
	}

	wv_handle_invoke(g_active_webview, decoded ? decoded : pstr);
	free(decoded);
	return;

allow:
	{
		void *invoke = *(void **)((char *)decisionHandler + 16);
		((void (*)(void *, unsigned long))invoke)(decisionHandler, 1);
	}
}

/* ── macOS backend init ──────────────────────────────────────────── */
static int
macos_backend_init(webview_shim_t *w)
{
	void *lib = cosmo_dlopen("libobjc.A.dylib", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "webview_shim: cannot load ObjC runtime\n");
		return -1;
	}
	/* Cache ObjC runtime handle + critical functions so the event loop
	   (called every frame) does NOT dlopen/dlsym repeatedly.  Doing so
	   on every iteration would stall the run loop and prevent WKWebView
	   from ever rendering content (dark grey window + beach ball). */
	w->priv->objc_lib              = lib;
	w->priv->ms_objc_getClass      = cosmo_dlsym(lib, "objc_getClass");
	w->priv->ms_sel_registerName   = cosmo_dlsym(lib, "sel_registerName");
	w->priv->fn_objc_msgSend       = cosmo_dlsym(lib, "objc_msgSend");
	w->priv->fn_objc_msgSend_id    = cosmo_dlsym(lib, "objc_msgSend");
	w->priv->fn_objc_msgSend_id_id = cosmo_dlsym(lib, "objc_msgSend");

	cosmo_dlopen("/System/Library/Frameworks/AppKit.framework/AppKit",
	       RTLD_NOW);
	cosmo_dlopen("/System/Library/Frameworks/WebKit.framework/WebKit",
	       RTLD_NOW);

	objc_getClass_t    objc_getClass    = (objc_getClass_t)w->priv->ms_objc_getClass;
	sel_registerName_t sel_registerName = (sel_registerName_t)w->priv->ms_sel_registerName;
	msgSend_id_fn      msg_id           = (msgSend_id_fn)w->priv->fn_objc_msgSend_id;
	msgSend_void_fn    msg_void         = (msgSend_void_fn)w->priv->fn_objc_msgSend;
	msgSend_id_id_fn   msg_id_id        = (msgSend_id_id_fn)w->priv->fn_objc_msgSend_id_id;

	if (!objc_getClass || !sel_registerName || !msg_id || !msg_void)
		return -1;

	/* Create shared delegate class for window close */
	void *delCls = create_delegate_class(lib);

	/* Set macOS process name so the menu bar shows app_name, not .ape-1.10 */
	macos_set_process_name(lib, w->app_name);

	/* ── 1. NSApplication ──────────────────────────────────────── */
	void *NSApp_cls    = objc_getClass("NSApplication");
	void *NSApp        = msg_id(NSApp_cls, sel_registerName("sharedApplication"));
	((msgSend_void_fn)msg_void)(NSApp, sel_registerName("setActivationPolicy:"), 0);

	/* ── 2. NSMenu / menu bar ──────────────────────────────────── */
	void *NSMenu_cls   = objc_getClass("NSMenu");
	void *sel_alloc    = sel_registerName("alloc");
	void *sel_init     = sel_registerName("init");
	void *mainMenu     = msg_id(msg_id(NSMenu_cls, sel_alloc), sel_init);

	void *NSMenuItem_cls = objc_getClass("NSMenuItem");
	void *appMenuItem  = msg_id(msg_id(NSMenuItem_cls, sel_alloc), sel_init);
	void *appMenu      = msg_id(msg_id(NSMenu_cls, sel_alloc), sel_init);
	((msgSend_void_id_fn)msg_void)(appMenuItem, sel_registerName("setSubmenu:"), appMenu);
	((msgSend_void_id_fn)msg_void)(mainMenu, sel_registerName("addItem:"), appMenuItem);

	/* Quit item — derive title from app_name */
	void *NSStr_cls     = objc_getClass("NSString");
	void *sel_strWith   = sel_registerName("stringWithUTF8String:");
	{
		char quit_label[256];
		const char *an = w->app_name;
		if (an && an[0])
			snprintf(quit_label, sizeof(quit_label), "Quit %s", an);
		else
			snprintf(quit_label, sizeof(quit_label), "Quit");
		void *quitStr = msg_id_id(NSStr_cls, sel_strWith, (void*)quit_label);
		void *keyEq   = msg_id_id(NSStr_cls, sel_strWith, "q");
		msgSend_id_4_fn msg_id_3args = (msgSend_id_4_fn)w->priv->fn_objc_msgSend;
		void *quitItem = msg_id_3args(
		    msg_id(NSMenuItem_cls, sel_alloc),
		    sel_registerName("initWithTitle:action:keyEquivalent:"),
		    quitStr, sel_registerName("terminate:"), keyEq);
		((msgSend_void_id_fn)msg_void)(appMenu, sel_registerName("addItem:"), quitItem);
	}
	((msgSend_void_id_fn)msg_void)(NSApp, sel_registerName("setMainMenu:"), mainMenu);

	/* ── 3. NSWindow ───────────────────────────────────────────── */
	void *NSWindow_cls   = objc_getClass("NSWindow");
	unsigned long styleMask = (1<<0) | (1<<1) | (1<<2);
	if (w->resizable) styleMask |= (1<<3);

	msgSend_id_rect_str_fn msg_id_rect = (msgSend_id_rect_str_fn)w->priv->fn_objc_msgSend;
	void *window = msg_id_rect(msg_id(NSWindow_cls, sel_alloc),
	    sel_registerName("initWithContentRect:styleMask:backing:defer:"),
	    0, 0, (double)w->width, (double)w->height, styleMask, 2, 0);

	/* Initial window title comes from HTML <title> later; show blank until then */
	((msgSend_void_fn2)msg_void)(window, sel_registerName("center"));
	((msgSend_void_id_fn)msg_void)(window, sel_registerName("makeKeyAndOrderFront:"), NULL);

	/* Set window delegate for close button */
	{
		void *del = msg_id(msg_id(delCls, sel_alloc), sel_init);
		((msgSend_void_id_fn)msg_void)(window, sel_registerName("setDelegate:"), del);
	}

	/* ── 4. WKWebView ──────────────────────────────────────────── */
	void *config = msg_id(msg_id(objc_getClass("WKWebViewConfiguration"), sel_alloc), sel_init);

	/* Inject context-menu blocker via user script (before any page content) */
	if (!w->context_menu) {
		void *ucc = msg_id(config, sel_registerName("userContentController"));
		void *src = msg_id_id(NSStr_cls, sel_strWith,
		    "document.addEventListener('contextmenu',function(e){e.preventDefault();});");
		/* WKUserScript.alloc.initWithSource:injectionTime:forMainFrameOnly:
		   injectionTime 0 = AtDocumentStart */
		void *usCls = objc_getClass("WKUserScript");
		void *userScript = msg_id(msg_id(usCls, sel_alloc),
		    sel_registerName("initWithSource:injectionTime:forMainFrameOnly:"),
		    src, 0, 1);
		((msgSend_void_id_fn)msg_void)(ucc, sel_registerName("addUserScript:"), userScript);
	}
	void *WKWebView_cls = objc_getClass("WKWebView");

	msgSend_id_rect_id_fn msg_id_rect_id = (msgSend_id_rect_id_fn)w->priv->fn_objc_msgSend;
	void *webview = msg_id_rect_id(msg_id(WKWebView_cls, sel_alloc),
	    sel_registerName("initWithFrame:configuration:"),
	    0, 0, (double)w->width, (double)w->height, config);

	w->priv->webview = webview;

	/* Use WKNavigationDelegate for JS→C bridge — intercept __invoke= navigations */
	{
	    /* Set navigation delegate to intercept __invoke= URL navigations */
	    ((msgSend_void_id_fn)msg_void)(webview,
	        sel_registerName("setNavigationDelegate:"),
	        msg_id(msg_id(delCls, sel_alloc), sel_init));
	    /* Inject bootstrap: binding infrastructure + window.external.invoke */
	    void *bootstrapSrc = msg_id_id(NSStr_cls, sel_strWith,
	        "window.__wv_pending={};"
	        "window.__wv_bind_resolve=function(id,ok,r){"
	        "var c=window.__wv_pending[id];if(!c)return;"
	        "try{r=JSON.parse(r)}catch(e){}"
	        "if(ok)c[0](r);else c[1](r);delete window.__wv_pending[id];};"
	        "window.__wv_add_binding=function(n){"
	        "if(window[n]&&window[n].__wv_bound)return;"
	        "window[n]=function(){var i='__wv_'+Date.now()+'_'+Math.random().toString(36).slice(2);"
	        "var a=Array.prototype.slice.call(arguments);"
	        "return new Promise(function(s,f){"
	        "window.__wv_pending[i]=[s,f];"
	        "window.external.invoke(JSON.stringify({__bind__:n,__id__:i,__args__:a}));});};"
	        "window[n].__wv_bound=true;};"
	        "window.external={invoke:function(m){"
	        "window.location.href='about:blank?__invoke='+encodeURIComponent(m)}};");
	    void *usCls = objc_getClass("WKUserScript");
	    void *bootstrapScript = msg_id(msg_id(usCls, sel_alloc),
	        sel_registerName("initWithSource:injectionTime:forMainFrameOnly:"),
	        bootstrapSrc, 0, 1);
	    void *ucc = msg_id(config, sel_registerName("userContentController"));
	((msgSend_void_id_fn)msg_void)(ucc, sel_registerName("addUserScript:"), bootstrapScript);

	    /* Inject queued init scripts at document start */
	    for (int i_ = 0; i_ < w->priv->init_scripts_count; i_++) {
	        if (!w->priv->init_scripts[i_]) continue;
	        void *isrc = msg_id_id(NSStr_cls, sel_strWith,
	            (void*)w->priv->init_scripts[i_]);
	        void *uscript = msg_id(msg_id(usCls, sel_alloc),
	            sel_registerName("initWithSource:injectionTime:forMainFrameOnly:"),
	            isrc, 0, 1);
	        ((msgSend_void_id_fn)msg_void)(ucc, sel_registerName("addUserScript:"), uscript);
	    }
	}

	((msgSend_void_id_fn)msg_void)(msg_id(window, sel_registerName("contentView")),
	    sel_registerName("addSubview:"), webview);
	/* Set autoresizing mask so WKWebView resizes with the window */
	((void (*)(void*, void*, unsigned long))msg_void)(webview,
	    sel_registerName("setAutoresizingMask:"), 18);

	/* ── 5. Load HTML ──────────────────────────────────────────── */
	if (w->html && w->html[0]) {
		/* Build window title: "app_name | <title>" */
		const char *t = build_window_title(w);
		if (t[0]) {
			void *ts = msg_id_id(NSStr_cls, sel_strWith, (void*)t);
			((msgSend_void_id_fn)msg_void)(window, sel_registerName("setTitle:"), ts);
		}
		void *htmlStr = msg_id_id(NSStr_cls, sel_strWith, (void*)w->html);
		void *baseURL = msg_id_id(
		    objc_getClass("NSURL"),
		    sel_registerName("URLWithString:"),
		    msg_id_id(NSStr_cls, sel_strWith, "about:blank"));
		void* (*msg_id_id_id)(void*, void*, void*, void*) =
		    (void* (*)(void*,void*,void*,void*))w->priv->fn_objc_msgSend;
		msg_id_id_id(webview, sel_registerName("loadHTMLString:baseURL:"), htmlStr, baseURL);
	}

	/* ── 6. Finish launching ───────────────────────────────────── */
	((msgSend_void_fn2)msg_void)(NSApp, sel_registerName("finishLaunching"));
	((msgSend_void_bool_fn)msg_void)(NSApp, sel_registerName("activateIgnoringOtherApps:"), 1);

	w->priv->nswindow = window;
	w->priv->native  = NSApp;
	w->priv->running = 1;
	g_active_webview = w;

	return 0;
}

/* ── macOS event loop ───────────────────────────────────────────── */
static int
macos_backend_loop(webview_shim_t *w, int blocking)
{
	/* Drain any pending dispatch items first */
	wv_drain_dispatch(w->priv, w);

	/* Use cached ObjC runtime handle from init — NEVER dlopen here,
	   as doing so every frame (~20 Hz) stalls the run loop and causes
	   the beach ball + dark-grey WKWebView symptom. */
	void *lib = w->priv->objc_lib;
	if (!lib) return 1;

	objc_getClass_t    objc_getClass    = (objc_getClass_t)w->priv->ms_objc_getClass;
	sel_registerName_t sel_registerName = (sel_registerName_t)w->priv->ms_sel_registerName;
	if (!objc_getClass || !sel_registerName) return 1;

	msgSend_id_fn msg_id = (msgSend_id_fn)w->priv->fn_objc_msgSend_id;
	if (!msg_id) return 1;

	void *NSApp = msg_id(
	    objc_getClass("NSApplication"),
	    sel_registerName("sharedApplication"));

	void *sel_nextEvent = sel_registerName(
	    "nextEventMatchingMask:untilDate:inMode:dequeue:");
	unsigned long maxMask = ~0UL;

	void *untilDate;
	{
		void *NSDate_cls = objc_getClass("NSDate");
		void *sel_dateWithInterval = sel_registerName(
		    "dateWithTimeIntervalSinceNow:");
		typedef void* (*msg_id_double_fn)(void*, void*, double);
		msg_id_double_fn msg_id_d = (msg_id_double_fn)w->priv->fn_objc_msgSend;
		/* blocking: wait up to 50 ms; non-blocking: return immediately */
		double interval = blocking ? 0.05 : 0.0;
		untilDate = msg_id_d(NSDate_cls, sel_dateWithInterval, interval);
	}

	typedef void* (*msg_id_id_fn_cached)(void*, void*, void*);
	msg_id_id_fn_cached msg_id_id_cached = (msg_id_id_fn_cached)w->priv->fn_objc_msgSend_id_id;
	void *runLoopMode = msg_id_id_cached(
	    objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"),
	    (void*)"kCFRunLoopDefaultMode");

	typedef void* (*msg_next_event_t)(void*, void*, unsigned long, void*, void*, int);
	msg_next_event_t msg_next = (msg_next_event_t)w->priv->fn_objc_msgSend;
	void *event = msg_next(NSApp, sel_nextEvent, maxMask, untilDate, runLoopMode, 1);

	if (event) {
		typedef void (*msg_send_event_t)(void*, void*, void*);
		msg_send_event_t msg_send = (msg_send_event_t)w->priv->fn_objc_msgSend;
		msg_send(NSApp,
		    sel_registerName("sendEvent:"), event);
	}

	if (!w->priv->running) return 1;
	return 0;
}

/* ── macOS eval ─────────────────────────────────────────────────── */
static void
macos_backend_eval(webview_shim_t *w, const char *js)
{
	if (!js || !js[0] || !w->priv || !w->priv->webview) return;

	objc_getClass_t    objc_getClass    = (objc_getClass_t)w->priv->ms_objc_getClass;
	sel_registerName_t sel_registerName = (sel_registerName_t)w->priv->ms_sel_registerName;
	if (!objc_getClass || !sel_registerName) return;

	msgSend_id_id_fn msg_id_id = (msgSend_id_id_fn)w->priv->fn_objc_msgSend_id_id;

	void *jsStr = msg_id_id(
	    objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"),
	    (void*)js);

	void* (*msg_eval_t)(void*, void*, void*, void*) =
		    (void* (*)(void*,void*,void*,void*))w->priv->fn_objc_msgSend;
	msg_eval_t(w->priv->webview,
	    sel_registerName("evaluateJavaScript:completionHandler:"),
	    jsStr, NULL);
}

/* ── macOS exit ─────────────────────────────────────────────────── */
static void
macos_backend_exit(webview_shim_t *w)
{
	void *lib = w->priv->objc_lib;
	if (!lib) { g_active_webview = NULL; w->priv->running = 0; return; }

	objc_getClass_t    objc_getClass    = (objc_getClass_t)w->priv->ms_objc_getClass;
	sel_registerName_t sel_registerName = (sel_registerName_t)w->priv->ms_sel_registerName;

	msgSend_id_fn msg_id = (msgSend_id_fn)w->priv->fn_objc_msgSend_id;
	void *NSApp = msg_id(
	    objc_getClass("NSApplication"),
	    sel_registerName("sharedApplication"));
	typedef void (*msg_terminate_t)(void*, void*, void*);
	msg_terminate_t msg_term = (msg_terminate_t)w->priv->fn_objc_msgSend;
	msg_term(NSApp, sel_registerName("terminate:"), NULL);

	g_active_webview = NULL;
	w->priv->running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Linux / GTK3 + WebKit2GTK backend (unchanged)
   ═══════════════════════════════════════════════════════════════════════ */

typedef void* (*gtk_init_fn_t)(int *argc, char ***argv);
typedef void* (*gtk_window_new_fn_t)(int type);
typedef void  (*gtk_window_set_title_fn_t)(void *window, const char *title);
typedef void  (*gtk_window_set_default_size_fn_t)(void *window, int w, int h);
typedef void  (*gtk_container_add_fn_t)(void *container, void *widget);
typedef void  (*gtk_widget_show_all_fn_t)(void *widget);
typedef void  (*gtk_main_fn_t)(void);
typedef void  (*gtk_main_quit_fn_t)(void);
typedef int   (*gtk_events_pending_fn_t)(void);
typedef int   (*gtk_main_iteration_fn_t)(int);

typedef void* (*webkit_web_view_new_fn_t)(void);
typedef void  (*webkit_web_view_load_html_fn_t)(void *webview,
    const char *html, const char *base_uri);
typedef void  (*webkit_web_view_evaluate_javascript_fn_t)(void *webview,
    const char *js, long js_len, void *world_name, void *source_uri,
    void *cancellable, void *callback, void *userdata);

typedef void  (*g_object_unref_fn_t)(void *obj);
typedef int   (*g_signal_connect_data_fn_t)(void *instance,
    const char *detailed_signal, void *c_handler, void *data,
    void *destroy_data, int connect_flags);

/* User content manager / JSC bridge typedefs */
typedef void* (*wk_get_ucm_fn_t)(void *webview);
typedef int   (*wk_ucm_reg_msg_fn_t)(void *manager, const char *name, void *error);
typedef void* (*wk_user_script_new_fn_t)(const char *source, int inject_frames,
    int inject_time, void *whitelist, void *blacklist);
typedef void  (*wk_ucm_add_script_fn_t)(void *manager, void *script);
typedef void* (*wk_js_result_get_js_value_fn_t)(void *result);
typedef char* (*jsc_value_to_string_fn_t)(void *value);
typedef void  (*set_allow_fn_t)(void *, int);
typedef void  (*gtk_window_set_resizable_fn_t)(void*, int);
typedef void* (*wk_get_settings_fn_t)(void *);

static void *g_active_webview_gtk = NULL;

static void
gtk_on_destroy(void *widget, void *user_data)
{
    (void)widget;
    (void)user_data;
    if (g_active_webview_gtk)
        ((webview_shim_t *)g_active_webview_gtk)->priv->running = 0;
}

/*
 * Callback for WebKit user content manager "script-message-received::external".
 * window.external.invoke(msg) posts a message via
 *   window.webkit.messageHandlers.external.postMessage(msg)
 * which arrives here.
 */
static void
linux_script_message_received(void *manager, void *result, void *user_data)
{
    (void)manager;
    webview_shim_t *w = (webview_shim_t *)user_data;
    if (!w || !w->priv || !w->priv->webkit_lib) return;
    if (!w->on_invoke && w->priv->bindings_count == 0) return;

    wk_js_result_get_js_value_fn_t get_js_val =
        (wk_js_result_get_js_value_fn_t)w->priv->fn_wk_js_result_get_js_value;
    if (!get_js_val) return;

    jsc_value_to_string_fn_t jsc_str =
        (jsc_value_to_string_fn_t)w->priv->fn_wk_jsc_value_to_string;
    if (!jsc_str) return;

    void *js_value = get_js_val(result);
    if (!js_value) return;

    char *str = jsc_str(js_value);
    if (!str) return;

    	wv_handle_invoke(w, str);

    typedef void (*g_free_fn_t)(void*);
    g_free_fn_t g_free = (g_free_fn_t)w->priv->fn_g_free;
    if (g_free) g_free(str);
}

static int ctx_block(void *a,void *b,void *c,int d,void *e) { (void)a;(void)b;(void)c;(void)d;(void)e; return 1; }

static int
linux_backend_init(webview_shim_t *w)
{
    void *lib_gtk = cosmo_dlopen("libgtk-3.so.0", RTLD_NOW);
    if (!lib_gtk) { fprintf(stderr, "webview_shim: cannot load libgtk-3.so.0\n\tInstall: sudo apt install libgtk-3-0\n"); return -1; }

    void *lib_wk = cosmo_dlopen("libwebkit2gtk-4.1.so.0", RTLD_NOW);
    if (!lib_wk) lib_wk = cosmo_dlopen("libwebkit2gtk-4.0.so.37", RTLD_NOW);
    if (!lib_wk) { fprintf(stderr, "webview_shim: cannot load libwebkit2gtk\n\tInstall: sudo apt install libwebkit2gtk-4.1-0\n"); return -1; }

    /* ── Cache library handles ──────────────────────────────────── */
    w->priv->gtk_lib    = lib_gtk;
    w->priv->webkit_lib = lib_wk;

    /* ── Cache ALL GTK function pointers ──────────────────────────── */
    w->priv->fn_gtk_init                    = cosmo_dlsym(lib_gtk, "gtk_init");
    w->priv->fn_gtk_window_new              = cosmo_dlsym(lib_gtk, "gtk_window_new");
    w->priv->fn_gtk_window_set_title        = cosmo_dlsym(lib_gtk, "gtk_window_set_title");
    w->priv->fn_gtk_window_set_default_size = cosmo_dlsym(lib_gtk, "gtk_window_set_default_size");
    w->priv->fn_gtk_container_add           = cosmo_dlsym(lib_gtk, "gtk_container_add");
    w->priv->fn_gtk_widget_show_all         = cosmo_dlsym(lib_gtk, "gtk_widget_show_all");
    w->priv->fn_gtk_main                    = cosmo_dlsym(lib_gtk, "gtk_main");
    w->priv->fn_gtk_main_quit               = cosmo_dlsym(lib_gtk, "gtk_main_quit");
    w->priv->fn_gtk_events_pending          = cosmo_dlsym(lib_gtk, "gtk_events_pending");
    w->priv->fn_gtk_main_iteration          = cosmo_dlsym(lib_gtk, "gtk_main_iteration");
    w->priv->fn_gtk_window_set_resizable    = cosmo_dlsym(lib_gtk, "gtk_window_set_resizable");
    w->priv->fn_g_signal_connect_data       = cosmo_dlsym(lib_gtk, "g_signal_connect_data");
    w->priv->fn_g_object_unref              = cosmo_dlsym(lib_gtk, "g_object_unref");
    w->priv->fn_g_free                      = cosmo_dlsym(lib_gtk, "g_free");

    /* ── Cache ALL WebKit function pointers ────────────────────────── */
    w->priv->fn_wk_new                         = cosmo_dlsym(lib_wk, "webkit_web_view_new");
    w->priv->fn_wk_load_html                   = cosmo_dlsym(lib_wk, "webkit_web_view_load_html");
    w->priv->fn_wk_load_uri                    = cosmo_dlsym(lib_wk, "webkit_web_view_load_uri");
    w->priv->fn_wk_evaluate_javascript         = cosmo_dlsym(lib_wk, "webkit_web_view_evaluate_javascript");
    w->priv->fn_wk_get_child                   = cosmo_dlsym(lib_wk, "gtk_bin_get_child");
    w->priv->fn_wk_get_settings                = cosmo_dlsym(lib_wk, "webkit_web_view_get_settings");
    w->priv->fn_wk_set_enable_scripts          = cosmo_dlsym(lib_wk, "webkit_settings_set_enable_scripts");
    w->priv->fn_wk_set_enable_developer_extras = cosmo_dlsym(lib_wk, "webkit_settings_set_enable_developer_extras");
    w->priv->fn_wk_get_ucm                     = cosmo_dlsym(lib_wk, "webkit_web_view_get_user_content_manager");
    w->priv->fn_wk_ucm_reg_msg                 = cosmo_dlsym(lib_wk, "webkit_user_content_manager_register_script_message_handler");
    w->priv->fn_wk_user_script_new             = cosmo_dlsym(lib_wk, "webkit_user_script_new");
    w->priv->fn_wk_ucm_add_script              = cosmo_dlsym(lib_wk, "webkit_user_content_manager_add_script");
    w->priv->fn_wk_js_result_get_js_value      = cosmo_dlsym(lib_wk, "webkit_javascript_result_get_js_value");
    w->priv->fn_wk_jsc_value_to_string         = cosmo_dlsym(lib_wk, "jsc_value_to_string");

    /* Verify essential GTK symbols */
    if (!w->priv->fn_gtk_init || !w->priv->fn_gtk_window_new || !w->priv->fn_gtk_widget_show_all) {
        fprintf(stderr, "webview_shim: missing GTK functions\n"); return -1;
    }
    /* Verify essential WebKit symbols */
    if (!w->priv->fn_wk_new) {
        fprintf(stderr, "webview_shim: missing WebKit functions\n"); return -1;
    }

    /* Cast and call gtk_init */
    ((gtk_init_fn_t)w->priv->fn_gtk_init)(NULL, NULL);

    void *window = ((gtk_window_new_fn_t)w->priv->fn_gtk_window_new)(0);

    /* Build window title: "app_name | <title>" */
    const char *t = build_window_title(w);
    ((gtk_window_set_title_fn_t)w->priv->fn_gtk_window_set_title)(window,
        t[0] ? t : (w->app_name ? w->app_name : "WebView"));
    ((gtk_window_set_default_size_fn_t)w->priv->fn_gtk_window_set_default_size)(window, w->width, w->height);
    ((g_signal_connect_data_fn_t)w->priv->fn_g_signal_connect_data)(window, "destroy",
        (void *)gtk_on_destroy, NULL, NULL, 0);

    if (!w->resizable && w->priv->fn_gtk_window_set_resizable) {
        ((gtk_window_set_resizable_fn_t)w->priv->fn_gtk_window_set_resizable)(window, 0);
    }

    void *webview = ((webkit_web_view_new_fn_t)w->priv->fn_wk_new)();
    w->priv->webview = webview;

    /* ── Enable scripts / developer extras ──────────────────────────── */
    if (w->priv->fn_wk_get_settings && w->priv->fn_wk_set_enable_scripts) {
        void *settings = ((wk_get_settings_fn_t)w->priv->fn_wk_get_settings)(webview);
        if (settings) {
            ((set_allow_fn_t)w->priv->fn_wk_set_enable_scripts)(settings, 1);
            if (w->priv->fn_wk_set_enable_developer_extras) {
                ((set_allow_fn_t)w->priv->fn_wk_set_enable_developer_extras)(settings, 1);
            }
        }
    }

    /* ── Set up user content manager for JS→C bridge ────────────────── */
    if (w->priv->fn_wk_get_ucm && w->priv->fn_wk_ucm_reg_msg) {
        void *ucm = ((wk_get_ucm_fn_t)w->priv->fn_wk_get_ucm)(webview);
        if (ucm) {
            /* Register "external" message handler */
            ((wk_ucm_reg_msg_fn_t)w->priv->fn_wk_ucm_reg_msg)(ucm, "external", NULL);
            /* Connect the script-message-received::external signal */
            ((g_signal_connect_data_fn_t)w->priv->fn_g_signal_connect_data)(ucm,
                "script-message-received::external",
                (void *)linux_script_message_received, w, NULL, 0);
        }
    }

    /* ── Inject bootstrap script for binding + window.external.invoke ─── */
    /* Inserts bind infrastructure + window.external.invoke = function(msg) {
           window.webkit.messageHandlers.external.postMessage(msg);
       } */
    if (w->priv->fn_wk_user_script_new && w->priv->fn_wk_ucm_add_script) {
        /* We need the UCM again — fetch it from webview */
        void *ucm = ((wk_get_ucm_fn_t)w->priv->fn_wk_get_ucm)(webview);
        if (ucm) {
            void *script = ((wk_user_script_new_fn_t)w->priv->fn_wk_user_script_new)(
                "window.__wv_pending={};"
                "window.__wv_bind_resolve=function(id,ok,r){"
                "var c=window.__wv_pending[id];if(!c)return;"
                "try{r=JSON.parse(r)}catch(e){}"
                "if(ok)c[0](r);else c[1](r);delete window.__wv_pending[id];};"
                "window.__wv_add_binding=function(n){"
                "if(window[n]&&window[n].__wv_bound)return;"
                "window[n]=function(){var i='__wv_'+Date.now()+'_'+Math.random().toString(36).slice(2);"
                "var a=Array.prototype.slice.call(arguments);"
                "return new Promise(function(s,f){"
                "window.__wv_pending[i]=[s,f];"
                "window.external.invoke(JSON.stringify({__bind__:n,__id__:i,__args__:a}));});};"
                "window[n].__wv_bound=true;};"
                "window.external={invoke:function(m){"
                "window.webkit.messageHandlers.external.postMessage(m)}};",
                1,  /* WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES */
                0,  /* WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START */
                NULL, NULL);
            if (script) {
                ((wk_ucm_add_script_fn_t)w->priv->fn_wk_ucm_add_script)(ucm, script);
                if (w->priv->fn_g_object_unref) {
                    ((g_object_unref_fn_t)w->priv->fn_g_object_unref)(script);
                }
            }
        }
    }

    /* Disable context menu if requested */
    if (!w->context_menu) {
        ((g_signal_connect_data_fn_t)w->priv->fn_g_signal_connect_data)(webview,
            "context-menu", (void *)ctx_block, NULL, NULL, 0);
    }

	/* Inject queued init scripts at document start */
	if (w->priv->fn_wk_get_ucm && w->priv->fn_wk_user_script_new && w->priv->fn_wk_ucm_add_script) {
	    void *ucm2 = ((wk_get_ucm_fn_t)w->priv->fn_wk_get_ucm)(webview);
	    if (ucm2) {
	        for (int i_ = 0; i_ < w->priv->init_scripts_count; i_++) {
	            if (!w->priv->init_scripts[i_]) continue;
	            void *uscript = ((wk_user_script_new_fn_t)w->priv->fn_wk_user_script_new)(
	                w->priv->init_scripts[i_], 1, 0, NULL, NULL);
	            if (uscript) {
	                ((wk_ucm_add_script_fn_t)w->priv->fn_wk_ucm_add_script)(ucm2, uscript);
	                if (w->priv->fn_g_object_unref)
	                    ((g_object_unref_fn_t)w->priv->fn_g_object_unref)(uscript);
	            }
	        }
	    }
	}

	if (w->html && w->html[0])
	    ((webkit_web_view_load_html_fn_t)w->priv->fn_wk_load_html)(webview, w->html, "about:blank");

	((gtk_container_add_fn_t)w->priv->fn_gtk_container_add)(window, webview);
    ((gtk_widget_show_all_fn_t)w->priv->fn_gtk_widget_show_all)(window);

    w->priv->nswindow = window;
    w->priv->native  = window;
    w->priv->running = 1;
    g_active_webview_gtk = w;
    return 0;
}

static int
linux_backend_loop(webview_shim_t *w, int blocking)
{
    /* Drain any pending dispatch items first */
    wv_drain_dispatch(w->priv, w);
    if (blocking) {
        gtk_main_iteration_fn_t gtk_main_iter =
            (gtk_main_iteration_fn_t)w->priv->fn_gtk_main_iteration;
        if (!gtk_main_iter) {
            gtk_main_fn_t gtk_main = (gtk_main_fn_t)w->priv->fn_gtk_main;
            if (gtk_main) { gtk_main(); return 1; }
            return 1;
        }
        gtk_main_iter(1);
        if (!w->priv->running) return 1;
        return 0;
    } else {
        gtk_events_pending_fn_t gtk_events_pending =
            (gtk_events_pending_fn_t)w->priv->fn_gtk_events_pending;
        gtk_main_iteration_fn_t gtk_main_iter =
            (gtk_main_iteration_fn_t)w->priv->fn_gtk_main_iteration;
        if (gtk_events_pending && gtk_events_pending()) {
            if (gtk_main_iter) gtk_main_iter(0);
        }
    }
    if (!w->priv->running) return 1;
    return 0;
}

static void
linux_backend_eval(webview_shim_t *w, const char *js)
{
    if (!js || !js[0]) return;

    webkit_web_view_evaluate_javascript_fn_t wk_eval =
        (webkit_web_view_evaluate_javascript_fn_t)w->priv->fn_wk_evaluate_javascript;
    if (!wk_eval) {
        typedef void (*wk_exec_fn_t)(void *, const char *);
        wk_exec_fn_t wk_exec = (wk_exec_fn_t)cosmo_dlsym(w->priv->webkit_lib, "webkit_web_view_execute_script");
        if (wk_exec && w->priv->webview) {
            wk_exec(w->priv->webview, js);
        }
        return;
    }

    if (!w->priv->webview) return;
    wk_eval(w->priv->webview, js, -1, NULL, NULL, NULL, NULL, NULL);
}

static void
linux_backend_exit(webview_shim_t *w)
{
    gtk_main_quit_fn_t gtk_main_quit =
        (gtk_main_quit_fn_t)w->priv->fn_gtk_main_quit;
    if (gtk_main_quit) gtk_main_quit();

    w->priv->running = 0;
    g_active_webview_gtk = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
   Windows / Edge WebView2 backend
   ═══════════════════════════════════════════════════════════════════════
   Everything resolved at runtime via dlopen/dlsym — no windows.h needed.
   JS→C bridge: NavigationStarting intercept + about:blank?__invoke= navigation
   (same pattern as macOS backend — uses confirmed slot 7)
   C→JS eval:   ExecuteScript
   ═══════════════════════════════════════════════════════════════════════ */

/* ── Win32 type shims ────────────────────────────────────────────── */
typedef long                HRESULT;
typedef long long           LRESULT;
typedef void*               HWND;
typedef void*               HINSTANCE;
typedef void*               HMENU;
typedef void*               HICON;
typedef void*               HCURSOR;
typedef void*               HBRUSH;
typedef unsigned            UINT;
typedef size_t              WPARAM;
typedef size_t              LPARAM;

/* ── Win32 constants ──────────────────────────────────────────────── */
#define WS_OVERLAPPEDWINDOW      0x00CF0000UL
#define WS_VISIBLE               0x10000000UL
#define CW_USEDEFAULT            ((int)0x80000000)
#define SW_SHOWDEFAULT           10
#define COLOR_WINDOW             5
#define PM_NOREMOVE              0
#define PM_REMOVE                1
#define WM_CLOSE                 0x0010
#define WM_DESTROY               0x0002
#define WM_SIZE                  0x0005
#define WM_SETTEXT               0x000C
#define WM_QUIT                  0x0012
#define WM_APP                   0x8000
#define COINIT_APARTMENTTHREADED 2
#define S_OK                     0L
#define E_NOINTERFACE            0x80004002L
#define E_FAIL                   0x80004005L

/* Variant types */
#define VT_EMPTY                 0
#define VT_BSTR                  8
#define VT_BOOL                  11
#define VT_I4                    3
#define VT_BYREF                 0x4000
#define VARIANT_TRUE             0xFFFF
#define VARIANT_FALSE            0x0000

/* ── MSG and RECT structs ─────────────────────────────────────────── */
typedef struct { int left; int top; int right; int bottom; } RECT;
typedef struct { HWND hwnd; UINT msg; WPARAM wp; LPARAM lp; unsigned long time; long x; long y; } MSG;

/* ── WNDCLASSEXW struct ───────────────────────────────────────────── */
typedef struct {
    UINT    cbSize;
    UINT    style;
    LRESULT (*lpfnWndProc)(HWND, UINT, WPARAM, LPARAM);
    int     cbClsExtra;
    int     cbWndExtra;
    HINSTANCE hInstance;
    HICON   hIcon;
    HCURSOR hCursor;
    HBRUSH  hbrBackground;
    const unsigned short *lpszMenuName;
    const unsigned short *lpszClassName;
    HICON   hIconSm;
} WNDCLASSEXW;

/* ── COM GUID typedef ──────────────────────────────────────────────── */
typedef struct { unsigned long d1; unsigned short d2; unsigned short d3; unsigned char d4[8]; } GUID;

/* ── COM basic types ──────────────────────────────────────────────── */
typedef unsigned short VARIANT_BOOL;
typedef unsigned short *BSTR;

/* ── VARIANT struct (minimal: vt + wReserved[4] + union) ──────────── */
typedef struct {
    unsigned short vt;
    unsigned short wReserved1;
    unsigned short wReserved2;
    unsigned short wReserved3;
    union {
        long     lVal;
        unsigned char  bVal;
        unsigned short uiVal;
        long     intVal;
        float    fltVal;
        double   dblVal;
        VARIANT_BOOL boolVal;
        void    *pdispVal;
        BSTR     bstrVal;
    };
} VARIANT;

/* ── __ms_abi__ function pointer typedefs ─────────────────────────────
   cosmo_dlsym returns pointers whose arguments are corrupted by cosmocc's
   ABI bridge UNLESS declared with __ms_abi__.  Define these typedefs AFTER
   the Win32/COM type shims above so that HWND, MSG, BSTR, VARIANT etc. are
   available. */
#if defined(__COSMOPOLITAN__)
  #define MSABI __attribute__((__ms_abi__))
#else
  #define MSABI
#endif
typedef MSABI long      (*msabi_hr_t)(void);
typedef MSABI long      (*msabi_coinit_t)(void*, unsigned long);
typedef MSABI void      (*msabi_couninit_t)(void);
typedef MSABI void *    (*msabi_cotaskmemfree_t)(void*);
typedef MSABI long      (*msabi_createenv_t)(const unsigned short*,
                                             const unsigned short*, void*, void*);
typedef MSABI int       (*msabi_showw_t)(HWND, int);
typedef MSABI int       (*msabi_peekw_t)(MSG*, HWND, UINT, UINT, UINT);
typedef MSABI int       (*msabi_getw_t)(MSG*, HWND, UINT, UINT);
typedef MSABI int       (*msabi_trans_t)(const MSG*);
typedef MSABI long long (*msabi_disp_t)(const MSG*);
typedef MSABI int       (*msabi_getclientrect_t)(HWND, RECT*);
typedef MSABI void      (*msabi_destroyw_t)(HWND);
typedef MSABI void      (*msabi_postquit_t)(int);
typedef MSABI long long (*msabi_defwndproc_t)(HWND, UINT, WPARAM, LPARAM);

/* ── Global Windows state (single instance) ──────────────────────── */
struct {
    webview_shim_t *w;
    HWND    hwnd;
    void    *controller;   /* ICoreWebView2Controller* */
    void    *webview;      /* ICoreWebView2* */
    int     ready;         /* 0=waiting, 1=ok, -1=error */

    void    *h_ole32, *h_user32, *h_wv2;
    void    *proc_DefWindowProcW;
    void    *proc_CoTaskMemFree;
    void    *proc_DestroyWindow;
    void    *proc_PostQuitMessage;

} g_win;

/* ── UTF-8 / UTF-16 conversion helpers ──────────────────────────── */
static unsigned short*
utf8_to_wide(const char *utf8)
{
    if (!utf8) return NULL;
    size_t len = strlen(utf8);
    /* Allocate worst-case: every byte becomes a wide char (or surrogate pair) */
    unsigned short *w = (unsigned short*)calloc(len * 2 + 2, 2);
    if (!w) return NULL;
    size_t wi = 0;
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)utf8[i];
        unsigned cp;
        if (c < 0x80) {
            cp = c; i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = ((c & 0x1F) << 6) | ((unsigned char)utf8[i+1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = ((c & 0x0F) << 12) | (((unsigned char)utf8[i+1] & 0x3F) << 6)
               | ((unsigned char)utf8[i+2] & 0x3F); i += 3;
        } else {
            cp = ((c & 0x07) << 18) | (((unsigned char)utf8[i+1] & 0x3F) << 12)
               | (((unsigned char)utf8[i+2] & 0x3F) << 6) | ((unsigned char)utf8[i+3] & 0x3F);
            i += 4;
            /* Surrogate pair for code points > U+FFFF */
            cp -= 0x10000;
            w[wi++] = (unsigned short)(0xD800 | (cp >> 10));
            w[wi++] = (unsigned short)(0xDC00 | (cp & 0x3FF));
            continue;
        }
        w[wi++] = (unsigned short)cp;
    }
    w[wi] = 0;
    return w;
}

static char*
wide_to_utf8(const unsigned short *w)
{
    if (!w) return NULL;
    size_t len = 0;
    while (w[len]) len++;
    char *u = (char*)calloc(len * 3 + 1, 1);
    if (!u) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned short cp = w[i];
        if (cp < 0x80) {
            u[pos++] = (char)cp;
        } else if (cp < 0x800) {
            u[pos++] = 0xC0 | (unsigned char)(cp >> 6);
            u[pos++] = 0x80 | (unsigned char)(cp & 0x3F);
        } else {
            u[pos++] = 0xE0 | (unsigned char)(cp >> 12);
            u[pos++] = 0x80 | (unsigned char)((cp >> 6) & 0x3F);
            u[pos++] = 0x80 | (unsigned char)(cp & 0x3F);
        }
    }
    u[pos] = 0;
    return u;
}

/* ── COM vtable helpers ──────────────────────────────────────────── */
#define VTBL(obj, idx)  ((void***)(obj))[0][idx]

/* ── IUnknown methods (shared callback objects) ──────────────────── */
static MSABI unsigned long com_AddRef(void *self) { (void)self; return 2; }
static MSABI unsigned long com_Release(void *self) { (void)self; return 1; }
static MSABI HRESULT com_QueryInterface(void *self, const void *riid, void **ppv)
{ (void)self; (void)riid; *ppv = self; return S_OK; }

/* ── NavigationStarting callback — intercepts __invoke= URLs ──── */
static MSABI HRESULT nav_on_starting(void *self, void *sender, void *args)
{
    (void)self; (void)sender;
    if (!g_win.w || !args) return S_OK;
    if (!g_win.w->on_invoke && (!g_win.w->priv || g_win.w->priv->bindings_count == 0)) return S_OK;
    unsigned short *uri = NULL;
    ((MSABI HRESULT(*)(void*, unsigned short**))VTBL(args, 3))(args, &uri);
    if (!uri) return S_OK;
    char *uri8 = wide_to_utf8(uri);
    if (g_win.proc_CoTaskMemFree)
        ((msabi_cotaskmemfree_t)g_win.proc_CoTaskMemFree)(uri);
    if (!uri8) return S_OK;

    const char *pstr = strstr(uri8, "__invoke=");
    if (pstr) {
        pstr += 9;
        /* Cancel this navigation */
        ((MSABI HRESULT(*)(void*, int))VTBL(args, 8))(args, 1);
        /* URL-decode the __invoke= parameter */
        char *decoded = NULL;
        if (strchr(pstr, '%')) {
            decoded = (char *)malloc(strlen(pstr) + 1);
            if (decoded) {
                char *d = decoded;
                while (*pstr) {
                    if (*pstr == '%' && *(pstr+1) && *(pstr+2)) {
                        char hex[3] = {pstr[1], pstr[2], 0};
                        *d++ = (char)strtol(hex, NULL, 16);
                        pstr += 3;
                    } else {
                        *d++ = *pstr++;
                    }
                }
                *d = 0;
            }
        }
        	wv_handle_invoke(g_win.w, decoded ? decoded : pstr);
        free(decoded);
    }
    free(uri8);
    return S_OK;
}
static const void *g_nav_vtbl[] = {
    (const void*)com_QueryInterface, (const void*)com_AddRef,
    (const void*)com_Release, (const void*)nav_on_starting
};
static const void *g_nav_obj[] = { g_nav_vtbl };


/* ── Controller-created callback ─────────────────────────────────── */
static MSABI HRESULT ctrl_on_created(void *self, HRESULT res, void *controller)
{
    (void)self;
    DBG_PRINT("DBG ctrl_on_created res=0x%lx controller=%p\n", (unsigned long)res, controller);
    if (res != S_OK || !controller) { g_win.ready = -1; return res; }
    g_win.controller = controller;
    ((MSABI unsigned long(*)(void*))VTBL(controller, 1))(controller);

    void *wv = NULL;
    ((MSABI HRESULT(*)(void*, void**))VTBL(controller, 25))(controller, &wv);
    DBG_PRINT("DBG get_webview=%p\n", wv);
    if (!wv) { g_win.ready = -1; return -1; }
    g_win.webview = wv;
    ((MSABI unsigned long(*)(void*))VTBL(wv, 1))(wv);

    void *settings = NULL;
    ((MSABI HRESULT(*)(void*, void**))VTBL(wv, 3))(wv, &settings);
    if (settings) {
        ((MSABI HRESULT(*)(void*, int))VTBL(settings, 4))(settings, 1); /* IsScriptEnabled */
        ((MSABI unsigned long(*)(void*))VTBL(settings, 2))(settings);
    }

    /* Register NavigationStarting to intercept __invoke= URLs (stable slot 7) */
    {
        long long nav_token;
        HRESULT hrNav = ((MSABI HRESULT(*)(void*, void*, long long*))VTBL(wv, 7))(wv, (void*)g_nav_obj, &nav_token);
        DBG_PRINT("DBG add_NavigationStarting hr=0x%lx token=0x%llx\n", (unsigned long)hrNav, (unsigned long long)nav_token);
    }

    /* Inject bootstrap for window.external.invoke -> about:blank?__invoke=
       Uses navigation-based protocol (same as macOS backend) — confirmed slot 7. */
    {
        static const char kBootJs[] =
            "window.__wv_pending={};"
            "window.__wv_bind_resolve=function(id,ok,r){"
            "var c=window.__wv_pending[id];if(!c)return;"
            "try{r=JSON.parse(r)}catch(e){}"
            "if(ok)c[0](r);else c[1](r);delete window.__wv_pending[id];};"
            "window.__wv_add_binding=function(n){"
            "if(window[n]&&window[n].__wv_bound)return;"
            "window[n]=function(){var i='__wv_'+Date.now()+'_'+Math.random().toString(36).slice(2);"
            "var a=Array.prototype.slice.call(arguments);"
            "return new Promise(function(s,f){"
            "window.__wv_pending[i]=[s,f];"
            "window.external.invoke(JSON.stringify({__bind__:n,__id__:i,__args__:a}));});};"
            "window[n].__wv_bound=true;};"
            "window.external={invoke:function(m){"
            "window.location.href='about:blank?__invoke='+encodeURIComponent(m)}};";
        unsigned short *wBoot = utf8_to_wide(kBootJs);
        if (wBoot) {
            ((MSABI HRESULT(*)(void*, const unsigned short*, void*))VTBL(wv, 27))(wv, wBoot, NULL);
            free(wBoot);
        }
        /* Inject queued init scripts */
        for (int i_ = 0; i_ < g_win.w->priv->init_scripts_count; i_++) {
            if (!g_win.w->priv->init_scripts[i_]) continue;
            unsigned short *wInit = utf8_to_wide(g_win.w->priv->init_scripts[i_]);
            if (wInit) {
                ((MSABI HRESULT(*)(void*, const unsigned short*, void*))VTBL(wv, 27))(wv, wInit, NULL);
                free(wInit);
            }
        }
    }

    /* Inject context-menu blocker if requested */
    if (g_win.w && !g_win.w->context_menu) {
        static const char kNoCtx[] =
            "document.addEventListener('contextmenu',function(e){e.preventDefault();});";
        unsigned short *wNoCtx = utf8_to_wide(kNoCtx);
        if (wNoCtx) {
            ((MSABI HRESULT(*)(void*, const unsigned short*, void*))VTBL(wv, 27))(wv, wNoCtx, NULL);
            free(wNoCtx);
        }
    }

    /* Size controller to fill the window — WM_SIZE fired before controller existed */
    {
        RECT r = {0, 0, g_win.w ? g_win.w->width : 800, g_win.w ? g_win.w->height : 600};
        msabi_getclientrect_t fnGetCR =
            (msabi_getclientrect_t)cosmo_dlsym(g_win.h_user32, "GetClientRect");
        if (fnGetCR && fnGetCR(g_win.hwnd, &r))
            ((MSABI HRESULT(*)(void*, RECT))VTBL(controller, 6))(controller, r);
    }

    if (g_win.w && g_win.w->html) {
        unsigned short *wide = utf8_to_wide(g_win.w->html);
        if (wide) {
            ((MSABI HRESULT(*)(void*, const unsigned short*))VTBL(wv, 6))(wv, wide);
            free(wide);
        }
    }

    g_win.ready = 1;
    DBG_PRINT("DBG ctrl_on_created ready=1\n");
    return S_OK;
}
static const void *g_ctrl_vtbl[] = {
    (const void*)com_QueryInterface, (const void*)com_AddRef,
    (const void*)com_Release, (const void*)ctrl_on_created
};
static const void *g_ctrl_obj[] = { g_ctrl_vtbl };

/* ── Environment-created callback ────────────────────────────────── */
static MSABI HRESULT env_on_created(void *self, HRESULT res, void *env)
{
    (void)self;
    DBG_PRINT("DBG env_on_created res=0x%lx env=%p\n", (unsigned long)res, env);
    if (res != S_OK || !env) { g_win.ready = -1; return res; }
    DBG_PRINT("DBG env_on_created calling CreateCoreWebView2Controller\n");
    ((MSABI HRESULT(*)(void*, HWND, void*))VTBL(env, 3))(env, g_win.hwnd, (void*)g_ctrl_obj);
    DBG_PRINT("DBG env_on_created CreateCoreWebView2Controller returned\n");
    return S_OK;
}
static const void *g_env_vtbl[] = {
    (const void*)com_QueryInterface, (const void*)com_AddRef,
    (const void*)com_Release, (const void*)env_on_created
};
static const void *g_env_obj[] = { g_env_vtbl };

/* ── Window procedure ────────────────────────────────────────────── */
static MSABI LRESULT wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        if (g_win.w) g_win.w->priv->running = 0;
        return 0;
    case WM_SIZE: {
        int w = (int)(lp & 0xFFFF), h = (int)((lp >> 16) & 0xFFFF);
        if (g_win.controller) {
            RECT r = {0, 0, w, h};
            ((MSABI HRESULT(*)(void*, RECT))VTBL(g_win.controller, 6))(g_win.controller, r);
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_win.proc_PostQuitMessage)
            ((msabi_postquit_t)g_win.proc_PostQuitMessage)(0);
        return 0;
    }
    if (g_win.proc_DefWindowProcW)
        return ((msabi_defwndproc_t)g_win.proc_DefWindowProcW)(hwnd,msg,wp,lp);
    return 0;
}

/* ── Window creation via cosmopolitan's proper __stdcall wrappers ──
   RegisterClassExW/CreateWindowExW called through a raw cosmo_dlsym
   pointer use the wrong (cdecl) calling convention for a WINAPI fn,
   which corrupts the stack and SIGSEGVs inside user32. The libc/nt/*
   wrappers handle the ABI correctly, so we route class registration
   and window creation through them. */
#if defined(__COSMOPOLITAN__)
static HWND win_create_window(const unsigned short *cls,
                              const unsigned short *title,
                              int width, int height)
{
    intptr_t hi = GetModuleHandle(NULL);
    struct NtWndClass wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = (NtWndProc)wndproc;
    wc.hInstance   = hi;
    wc.hbrBackground = (int64_t)(COLOR_WINDOW + 1);
    wc.lpszClassName = (const char16_t *)cls;
    RegisterClass(&wc);
    int64_t hwnd = CreateWindowEx(0, (const char16_t *)cls,
        (const char16_t *)title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        0, 0, hi, 0);
    return (HWND)(void*)(size_t)hwnd;
}
#endif

/* ── Windows init ──────────────────────────────────────────────────── */
static int
windows_backend_init(webview_shim_t *w)
{
    void *ole32, *user32, *wv2;
    msabi_coinit_t       fnCoInit;
    msabi_couninit_t     fnCoUninit;
    msabi_showw_t        fnShowW;
    msabi_peekw_t        fnPeekW;
    msabi_getw_t         fnGetW;
    msabi_trans_t        fnTrans;
    msabi_disp_t         fnDisp;
    msabi_createenv_t    fnCreateEnv;

    memset(&g_win, 0, sizeof(g_win));
    g_win.w = w;
    ole32 = cosmo_dlopen("ole32.dll", RTLD_NOW);
    user32 = cosmo_dlopen("user32.dll", RTLD_NOW);
    wv2   = cosmo_dlopen("WebView2Loader.dll", RTLD_NOW);
    if (!ole32 || !user32 || !wv2) { fprintf(stderr,"webview_shim: failed to load DLLs\n"); goto fail; }
    g_win.h_ole32 = ole32;
    g_win.h_user32 = user32;
    g_win.h_wv2 = wv2;
    fnCoInit   = (msabi_coinit_t)cosmo_dlsym(ole32,"CoInitializeEx");
    fnCoUninit = (msabi_couninit_t)cosmo_dlsym(ole32,"CoUninitialize");
    g_win.proc_CoTaskMemFree = (msabi_cotaskmemfree_t)cosmo_dlsym(ole32,"CoTaskMemFree");

    /* RegisterClassExW/CreateWindowExW go through cosmopolitan wrappers. */
    fnShowW = (msabi_showw_t)cosmo_dlsym(user32,"ShowWindow");
    fnPeekW = (msabi_peekw_t)cosmo_dlsym(user32,"PeekMessageW");
    fnGetW  = (msabi_getw_t)cosmo_dlsym(user32,"GetMessageW");
    fnTrans = (msabi_trans_t)cosmo_dlsym(user32,"TranslateMessage");
    fnDisp  = (msabi_disp_t)cosmo_dlsym(user32,"DispatchMessageW");
    	w->priv->fn_PeekMessageW = (void*)fnPeekW;
    	w->priv->fn_GetMessageW = (void*)fnGetW;
    	w->priv->fn_TranslateMessage = (void*)fnTrans;
    	w->priv->fn_DispatchMessageW = (void*)fnDisp;
    	w->priv->fn_CoUninitialize = (void*)cosmo_dlsym(ole32,"CoUninitialize");
    	w->priv->fn_PostMessageW = (void*)cosmo_dlsym(user32,"PostMessageW");
    g_win.proc_DefWindowProcW = (msabi_defwndproc_t)cosmo_dlsym(user32,"DefWindowProcW");
    g_win.proc_DestroyWindow  = (msabi_destroyw_t)cosmo_dlsym(user32,"DestroyWindow");
    g_win.proc_PostQuitMessage = (msabi_postquit_t)cosmo_dlsym(user32,"PostQuitMessage");

    fnCreateEnv = (msabi_createenv_t)
        cosmo_dlsym(wv2,"CreateCoreWebView2EnvironmentWithOptions");

    if (!fnCoInit || !fnCoUninit || !fnShowW ||
        !fnPeekW || !fnGetW || !fnTrans || !fnDisp || !fnCreateEnv ||
        !g_win.proc_DefWindowProcW) {
        fprintf(stderr,"webview_shim: failed to resolve functions\n"); goto fail;
    }

    fnCoInit(NULL, COINIT_APARTMENTTHREADED);

    /* Register class + create window via cosmopolitan __stdcall wrappers */
    static const unsigned short kCls[] = {'W','e','b','V','i','e','w',0};
    const char *wt = build_window_title(w);
    const char *wtsrc = wt[0] ? wt : (w->app_name ? w->app_name : "WebView");
    unsigned short *title_w = utf8_to_wide(wtsrc);
    static const unsigned short kFallbackTitle[] = {'W','e','b','V','i','e','w',0};
    g_win.hwnd = win_create_window(kCls, title_w ? title_w : kFallbackTitle,
         w->width, w->height);
    free(title_w);
    if (!g_win.hwnd) { fprintf(stderr,"webview_shim: CreateWindowExW failed\n"); fnCoUninit(); goto fail; }

    g_win.ready = 0;
    DBG_PRINT("DBG calling CreateCoreWebView2EnvironmentWithOptions\n");
    /* WebView2 requires a non-NULL userDataFolder on most runtimes;
       NULL yields E_POINTER. Use a per-user temp folder. */
    static unsigned short kUserData[] =
        {'C',':','\\','w','v','2','_','d','a','t','a',0};
    HRESULT envhr = fnCreateEnv(NULL, kUserData, NULL, (void*)g_env_obj);
    DBG_PRINT("DBG CreateCoreWebView2EnvironmentWithOptions returned hr=0x%lx, pumping\n", (unsigned long)envhr);
    {
        MSG msg;
        int spin = 0;
        while (!g_win.ready && spin < 500) {
            int has = fnPeekW(&msg, NULL, 0, 0, PM_REMOVE);
            if (has) { fnTrans(&msg); fnDisp(&msg); spin = 0; }
            usleep(10000);
            spin++;
        }
        if (spin >= 500) { DBG_PRINT("DBG init timeout, ready=%d\n", g_win.ready); }
        DBG_PRINT("DBG init loop done ready=%d\n", g_win.ready);
    }

    if (g_win.ready == -1 || !g_win.webview) {
        if (g_win.webview)  ((MSABI unsigned long(*)(void*))VTBL(g_win.webview,2))(g_win.webview);
        if (g_win.controller) ((MSABI unsigned long(*)(void*))VTBL(g_win.controller,2))(g_win.controller);
        ((msabi_destroyw_t)g_win.proc_DestroyWindow)(g_win.hwnd);
        fnCoUninit();
        goto fail;
    }

    w->priv->webview = g_win.webview;
    w->priv->running = 1;
    w->priv->nswindow = g_win.hwnd;
    w->priv->native  = g_win.hwnd;
    return 0;

fail:
    if (ole32) cosmo_dlclose(ole32);
    if (user32) cosmo_dlclose(user32);
    if (wv2) cosmo_dlclose(wv2);
    g_win.h_ole32 = g_win.h_user32 = g_win.h_wv2 = NULL;
    return -1;
}

/* ── Windows event loop ──────────────────────────────────────────── */
static int
windows_backend_loop(webview_shim_t *w, int blocking)
{
    /* Drain any pending dispatch items first */
    wv_drain_dispatch(w->priv, w);
    msabi_peekw_t fnPeekW = (msabi_peekw_t)w->priv->fn_PeekMessageW;
    msabi_getw_t  fnGetW  = (msabi_getw_t)w->priv->fn_GetMessageW;
    msabi_trans_t fnTrans = (msabi_trans_t)w->priv->fn_TranslateMessage;
    msabi_disp_t  fnDisp  = (msabi_disp_t)w->priv->fn_DispatchMessageW;
    if (!fnPeekW || !fnTrans || !fnDisp) return 1;
    MSG msg;
    if (blocking) {
        if (fnGetW && fnGetW(&msg,NULL,0,0)) { fnTrans(&msg); fnDisp(&msg); } else return 1;
    } else {
        if (fnPeekW(&msg,NULL,0,0,PM_REMOVE)) { if(msg.msg==WM_QUIT)return 1; fnTrans(&msg); fnDisp(&msg); }
    }
    if (!w->priv->running) return 1;
    return 0;
}

/* ── Windows eval ────────────────────────────────────────────────── */
static void
windows_backend_eval(webview_shim_t *w, const char *js)
{
    (void)w;
    if (!js || !js[0] || !g_win.webview) return;
    unsigned short *wjs = utf8_to_wide(js);
    if (!wjs) return;
    ((MSABI HRESULT(*)(void*,const unsigned short*,void*))VTBL(g_win.webview,29))(g_win.webview,wjs,NULL);
    free(wjs);
}

/* ── Windows exit ────────────────────────────────────────────────── */
static void
windows_backend_exit(webview_shim_t *w)
{
    (void)w;
    if (g_win.webview)     { ((MSABI unsigned long(*)(void*))VTBL(g_win.webview,2))(g_win.webview); g_win.webview=NULL; }
    if (g_win.controller)  { ((MSABI HRESULT(*)(void*))VTBL(g_win.controller,24))(g_win.controller); ((MSABI unsigned long(*)(void*))VTBL(g_win.controller,2))(g_win.controller); g_win.controller=NULL; }
    if (g_win.hwnd && g_win.proc_DestroyWindow) { ((msabi_destroyw_t)g_win.proc_DestroyWindow)(g_win.hwnd); g_win.hwnd=NULL; }
    if (g_win.h_ole32) {
        msabi_couninit_t CoUninit = (msabi_couninit_t)w->priv->fn_CoUninitialize;
        if (CoUninit) CoUninit();
    }
    if (g_win.h_wv2)    cosmo_dlclose(g_win.h_wv2);
    if (g_win.h_user32) cosmo_dlclose(g_win.h_user32);
    if (g_win.h_ole32)  cosmo_dlclose(g_win.h_ole32);
    g_win.w = NULL;
    w->priv->running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Platform implementations for new API
   ═══════════════════════════════════════════════════════════════════════ */

/* ── macOS implementations ───────────────────────────────────────── */
static int
macos_backend_navigate(webview_shim_t *w, const char *url)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->webview || !url || !url[0]) return -1;
    msgSend_id_id_fn msg_id_id = (msgSend_id_id_fn)p->fn_objc_msgSend_id_id;
    msgSend_void_id_fn msg_void_id = (msgSend_void_id_fn)p->fn_objc_msgSend;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    objc_getClass_t oc = (objc_getClass_t)p->ms_objc_getClass;
    if (!msg_id_id || !msg_void_id || !sr || !oc) return -1;
    void *nsStr = msg_id_id(oc("NSString"), sr("stringWithUTF8String:"), (void*)url);
    if (!nsStr) return -1;
    void *nsurl = msg_id_id(oc("NSURL"), sr("URLWithString:"), nsStr);
    if (!nsurl) return -1;
    void *req = msg_id_id(oc("NSURLRequest"), sr("requestWithURL:"), nsurl);
    if (!req) return -1;
    msg_void_id(p->webview, sr("loadRequest:"), req);
    return 0;
}

static int
macos_backend_set_title(webview_shim_t *w, const char *title)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->nswindow || !title) return -1;
    msgSend_id_id_fn msg_id_id = (msgSend_id_id_fn)p->fn_objc_msgSend_id_id;
    msgSend_void_id_fn msg_void_id = (msgSend_void_id_fn)p->fn_objc_msgSend;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    objc_getClass_t oc = (objc_getClass_t)p->ms_objc_getClass;
    if (!msg_id_id || !sr || !oc) return -1;
    void *nsstr = msg_id_id(oc("NSString"), sr("stringWithUTF8String:"), (void*)title);
    if (!nsstr) return -1;
    msg_void_id(p->nswindow, sr("setTitle:"), nsstr);
    return 0;
}

static int
macos_backend_set_size(webview_shim_t *w, int width, int height, int hints)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->nswindow) return -1;
    msgSend_void_fn msg_v = (msgSend_void_fn)p->fn_objc_msgSend;
    msgSend_id_fn msg_id = (msgSend_id_fn)p->fn_objc_msgSend_id;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    objc_getClass_t oc = (objc_getClass_t)p->ms_objc_getClass;
    if (!msg_v || !msg_id || !sr || !oc) return -1;

    if (hints == 3) {
        /* FIXED: remove resizable style mask bit */
        unsigned long style = (unsigned long)(size_t)msg_id(p->nswindow, sr("styleMask"));
        style &= ~(1UL<<3); /* NSResizableWindowMask */
        msg_v(p->nswindow, sr("setStyleMask:"), (void*)(size_t)style);
        return 0;
    }
    if (hints == 2) {
        /* MAX: set max size via setFrame: */
        typedef void (*msg_v_dddd_fn)(void*, void*, double, double, double, double, int);
        msg_v_dddd_fn msg_v4d = (msg_v_dddd_fn)p->fn_objc_msgSend;
        msg_v4d(p->nswindow, sr("setFrame:display:animate:"),
            0, 0, (double)width, (double)height, 1);
        return 0;
    }
    if (hints == 1) {
        /* MIN: set min size via setContentSize: for now */
        typedef void (*msg_v_dd_fn)(void*, void*, double, double);
        msg_v_dd_fn msg_vdd = (msg_v_dd_fn)p->fn_objc_msgSend;
        msg_vdd(p->nswindow, sr("setContentSize:"), (double)width, (double)height);
        return 0;
    }
    /* NONE: set content size (pass NSSize as two doubles via msgSend) */
    {
        typedef void (*msg_v_dd_fn)(void*, void*, double, double);
        msg_v_dd_fn msg_vdd = (msg_v_dd_fn)p->fn_objc_msgSend;
        msg_vdd(p->nswindow, sr("setContentSize:"), (double)width, (double)height);
    }
    return 0;
}

static int
macos_backend_set_html(webview_shim_t *w, const char *html)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->webview || !html) return -1;
    msgSend_id_id_fn msg_id_id = (msgSend_id_id_fn)p->fn_objc_msgSend_id_id;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    objc_getClass_t oc = (objc_getClass_t)p->ms_objc_getClass;
    if (!msg_id_id || !sr || !oc) return -1;
    void *nsstr = msg_id_id(oc("NSString"), sr("stringWithUTF8String:"), (void*)html);
    void *baseURL = msg_id_id(oc("NSURL"), sr("URLWithString:"),
        msg_id_id(oc("NSString"), sr("stringWithUTF8String:"), "about:blank"));
    typedef void (*msg_3id_fn)(void*, void*, void*, void*);
    ((msg_3id_fn)p->fn_objc_msgSend)(p->webview, sr("loadHTMLString:baseURL:"), nsstr, baseURL);
    return 0;
}

static int
macos_backend_set_fullscreen(webview_shim_t *w, int fullscreen)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->nswindow) return -1;
    msgSend_void_fn msg_v = (msgSend_void_fn)p->fn_objc_msgSend;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    if (!msg_v || !sr) return -1;
    msg_v(p->nswindow, sr("toggleFullScreen:"), NULL);
    return 0;
}

static int
macos_backend_init_script(webview_shim_t *w, const char *js)
{
    webview_shim_priv *p = w->priv;
    if (!p->objc_lib || !p->webview || !js || !js[0]) return -1;
    msgSend_id_id_fn msg_id_id = (msgSend_id_id_fn)p->fn_objc_msgSend_id_id;
    msgSend_void_id_fn msg_void_id = (msgSend_void_id_fn)p->fn_objc_msgSend;
    msgSend_id_fn msg_id = (msgSend_id_fn)p->fn_objc_msgSend_id;
    sel_registerName_t sr = (sel_registerName_t)p->ms_sel_registerName;
    objc_getClass_t oc = (objc_getClass_t)p->ms_objc_getClass;
    if (!msg_id_id || !msg_void_id || !msg_id || !sr || !oc) return -1;

    	void *jsStr = msg_id_id(oc("NSString"), sr("stringWithUTF8String:"), (void*)js);

    	/* Inject as WKUserScript (applies to future page loads) */
    	void *usCls = oc("WKUserScript");
    	void *uscript = msg_id(msg_id(usCls, sr("alloc")),
    	    sr("initWithSource:injectionTime:forMainFrameOnly:"), jsStr, 0, 1);
    	if (uscript) {
    	    msgSend_id_fn msg_id_v = (msgSend_id_fn)p->fn_objc_msgSend_id;
    	    void *config = msg_id_v(
    	        msg_id_v(p->webview, sr("configuration")),
    	        sr("userContentController"));
    	    if (config) {
    	        msg_void_id(config, sr("addUserScript:"), uscript);
    	    }
    	}

    	/* Also evaluate now (applies to current page) */
    	{
    	    void* (*msg_eval_t)(void*, void*, void*, void*) =
    	        (void* (*)(void*,void*,void*,void*))p->fn_objc_msgSend;
    	    msg_eval_t(p->webview,
    	        sr("evaluateJavaScript:completionHandler:"),
    	        jsStr, NULL);
    	}
    	return 0;
}

/* ── Linux implementations ───────────────────────────────────────── */
static int
linux_backend_navigate(webview_shim_t *w, const char *url)
{
    webview_shim_priv *p = w->priv;
    if (!p->fn_wk_load_uri || !p->webview || !url || !url[0]) return -1;
    typedef void (*wk_load_uri_fn_t)(void*, const char*);
    ((wk_load_uri_fn_t)p->fn_wk_load_uri)(p->webview, url);
    return 0;
}

static int
linux_backend_set_title(webview_shim_t *w, const char *title)
{
    webview_shim_priv *p = w->priv;
    if (!p->fn_gtk_window_set_title || !p->nswindow || !title) return -1;
    ((gtk_window_set_title_fn_t)p->fn_gtk_window_set_title)(p->nswindow, title);
    return 0;
}

static int
linux_backend_set_size(webview_shim_t *w, int width, int height, int hints)
{
    webview_shim_priv *p = w->priv;
    if (!p->nswindow) return -1;
    if (hints == 3) {
        if (!p->fn_gtk_window_set_resizable) return -1;
        ((gtk_window_set_resizable_fn_t)p->fn_gtk_window_set_resizable)(p->nswindow, 0);
        return 0;
    }
    if (!p->fn_gtk_window_set_default_size) return -1;
    ((gtk_window_set_default_size_fn_t)p->fn_gtk_window_set_default_size)(p->nswindow, width, height);
    return 0;
}

static int
linux_backend_set_html(webview_shim_t *w, const char *html)
{
    webview_shim_priv *p = w->priv;
    if (!p->fn_wk_load_html || !p->webview || !html) return -1;
    ((webkit_web_view_load_html_fn_t)p->fn_wk_load_html)(p->webview, html, "about:blank");
    return 0;
}

static int
linux_backend_set_fullscreen(webview_shim_t *w, int fullscreen)
{
    webview_shim_priv *p = w->priv;
    if (!p->nswindow) return -1;
    typedef void (*fs_fn_t)(void*);
    if (fullscreen) {
        fs_fn_t fs = (fs_fn_t)cosmo_dlsym(p->gtk_lib, "gtk_window_fullscreen");
        if (fs) { fs(p->nswindow); return 0; }
    } else {
        fs_fn_t uf = (fs_fn_t)cosmo_dlsym(p->gtk_lib, "gtk_window_unfullscreen");
        if (uf) { uf(p->nswindow); return 0; }
    }
    return -1;
}

static int
linux_backend_init_script(webview_shim_t *w, const char *js)
{
    webview_shim_priv *p = w->priv;
    if (!p->fn_wk_user_script_new || !p->fn_wk_ucm_add_script || !p->fn_wk_get_ucm
        || !p->webview || !js || !js[0]) return -1;
    void *ucm = ((wk_get_ucm_fn_t)p->fn_wk_get_ucm)(p->webview);
    if (!ucm) return -1;
    void *script = ((wk_user_script_new_fn_t)p->fn_wk_user_script_new)(js, 1, 0, NULL, NULL);
    if (!script) return -1;
    ((wk_ucm_add_script_fn_t)p->fn_wk_ucm_add_script)(ucm, script);
    if (p->fn_g_object_unref)
        ((g_object_unref_fn_t)p->fn_g_object_unref)(script);
    return 0;
}

/* ── Windows implementations ─────────────────────────────────────── */
static int
windows_backend_navigate(webview_shim_t *w, const char *url)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.webview || !url || !url[0]) return -1;
    unsigned short *wurl = utf8_to_wide(url);
    if (!wurl) return -1;
    /* ICoreWebView2::Navigate — vtable slot 5 */
    ((MSABI HRESULT(*)(void*, const unsigned short*))VTBL(g_win.webview, 5))(g_win.webview, wurl);
    free(wurl);
    return 0;
}

static int
windows_backend_set_title(webview_shim_t *w, const char *title)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.h_user32 || !p->nswindow || !title) return -1;
    typedef MSABI long (*sendw_t)(HWND, UINT, WPARAM, LPARAM);
    sendw_t fnSendW = (sendw_t)cosmo_dlsym(g_win.h_user32, "SendMessageW");
    if (!fnSendW) return -1;
    unsigned short *wtitle = utf8_to_wide(title);
    if (!wtitle) return -1;
    fnSendW((HWND)p->nswindow, WM_SETTEXT, 0, (LPARAM)(size_t)wtitle);
    free(wtitle);
    return 0;
}

static int
windows_backend_set_size(webview_shim_t *w, int width, int height, int hints)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.h_user32 || !p->nswindow) return -1;
    typedef MSABI int (*setwindowpos_t)(HWND, HWND, int, int, int, int, UINT);
    setwindowpos_t fnSWP = (setwindowpos_t)cosmo_dlsym(g_win.h_user32, "SetWindowPos");
    if (!fnSWP) return -1;
    fnSWP((HWND)p->nswindow, NULL, 0, 0, width, height,
          0x0002 | 0x0004); /* SWP_NOMOVE | SWP_NOZORDER */
    return 0;
}

static int
windows_backend_set_html(webview_shim_t *w, const char *html)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.webview || !html) return -1;
    unsigned short *wh = utf8_to_wide(html);
    if (!wh) return -1;
    /* ICoreWebView2::NavigateToString — slot 6 */
    ((MSABI HRESULT(*)(void*, const unsigned short*))VTBL(g_win.webview, 6))(g_win.webview, wh);
    free(wh);
    return 0;
}

static int
windows_backend_set_fullscreen(webview_shim_t *w, int fullscreen)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.h_user32 || !p->nswindow) return -1;
    typedef MSABI long (*sendw_t)(HWND, UINT, WPARAM, LPARAM);
    sendw_t fnSendW = (sendw_t)cosmo_dlsym(g_win.h_user32, "SendMessageW");
    if (!fnSendW) return -1;
    fnSendW((HWND)p->nswindow, 0x0112, fullscreen ? 0xF030 : 0xF120, 0);
    /* WM_SYSCOMMAND, SC_MAXIMIZE / SC_RESTORE */
    return 0;
}

static int
windows_backend_init_script(webview_shim_t *w, const char *js)
{
    webview_shim_priv *p = w->priv;
    if (!g_win.webview || !js || !js[0]) return -1;
    unsigned short *wjs = utf8_to_wide(js);
    if (!wjs) return -1;
    /* ExecuteScript — slot 27 (or AddScriptToExecuteOnDocumentCreated via slot 13) */
    ((MSABI HRESULT(*)(void*, const unsigned short*, void*))VTBL(g_win.webview, 27))(g_win.webview, wjs, NULL);
    free(wjs);
    return 0;
}

/* ── Helper: register a named binding function ──────────────────── */
static int
wv_inject_bind_js(webview_shim_t *w, const char *name)
{
    if (!w || !w->priv || !name) return -1;
    /* Use the bootstrap's __wv_add_binding helper to create window[name] */
    char js[256];
    int n = snprintf(js, sizeof(js),
        "window.__wv_add_binding('%s');", name);
    if (n <= 0 || n >= (int)sizeof(js)) return -1;
    /* Eval immediately — evaluateJavaScript queues if page hasn't loaded */
    webview_shim_eval(w, js);
    /* Also register as init script so it persists across page reloads */
    webview_shim_init_script(w, js);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════ */

int
webview_shim_init(webview_shim_t *w)
{

	struct webview_shim_priv *p;
	if (!w) return -1;

	p = (struct webview_shim_priv *)calloc(1, sizeof(*p));

	if (!p) return -1;
	w->priv = p;


	if (IsXnu()) {
		p->platform = 0; p->init = macos_backend_init; p->loop = macos_backend_loop;
		p->eval = macos_backend_eval; p->exit = macos_backend_exit;
	} else if (IsLinux() || IsFreebsd()) {
		p->platform = 1; p->init = linux_backend_init; p->loop = linux_backend_loop;
		p->eval = linux_backend_eval; p->exit = linux_backend_exit;
	} else if (IsWindows()) {

		p->platform = 2; p->init = windows_backend_init; p->loop = windows_backend_loop;
		p->eval = windows_backend_eval; p->exit = windows_backend_exit;
	} else {
		fprintf(stderr, "webview_shim: unsupported platform\n");
		free(p); w->priv = NULL; return -1;
	}
	return p->init(w);
}

int webview_shim_loop(webview_shim_t *w, int blocking) { if (!w || !w->priv) return 1; return w->priv->loop(w, blocking); }
void webview_shim_eval(webview_shim_t *w, const char *js) { if (!w || !w->priv || !js) return; w->priv->eval(w, js); }
void webview_shim_exit(webview_shim_t *w) { if (!w || !w->priv) return; w->priv->exit(w); if (w->priv->objc_lib) { cosmo_dlclose(w->priv->objc_lib); w->priv->objc_lib = NULL; } if (w->priv->handle) cosmo_dlclose(w->priv->handle); free(w->priv); w->priv = NULL; }

/* ── New public API ───────────────────────────────────────────────── */

int
webview_shim_navigate(webview_shim_t *w, const char *url)
{
    if (!w || !w->priv || !url || !url[0]) return -1;
    switch (w->priv->platform) {
    case 0: return macos_backend_navigate(w, url);
    case 1: return linux_backend_navigate(w, url);
    case 2: return windows_backend_navigate(w, url);
    default: return -1;
    }
}

void
webview_shim_dispatch(webview_shim_t *w,
                      void (*fn)(struct webview_shim *w, void *arg),
                      void *arg)
{
    if (!w || !w->priv || !fn) return;
    webview_shim_priv *p = w->priv;
    wv_dispatch_item_t *item = (wv_dispatch_item_t *)malloc(sizeof(*item));
    if (!item) return;
    item->fn = (void (*)(webview_shim_t*, void*))fn;
    item->arg = arg;
    item->next = NULL;
    wv_lock(&p->dispatch_lock);
    if (p->dispatch_tail) {
        p->dispatch_tail->next = item;
        p->dispatch_tail = item;
    } else {
        p->dispatch_head = item;
        p->dispatch_tail = item;
    }
    wv_unlock(&p->dispatch_lock);

    /* Wake the event loop on Linux and Windows */
    switch (p->platform) {
    case 0: /* macOS: loop wakes every 50ms */ break;
    case 1: /* Linux */
        if (p->gtk_lib) {
            void (*wake)(void*) = (void (*)(void*))cosmo_dlsym(
                p->gtk_lib, "g_main_context_wakeup");
            if (wake) wake(NULL);
        }
        break;
    case 2: /* Windows */
        if (p->fn_PostMessageW && p->nswindow) {
            ((MSABI int (*)(HWND, UINT, WPARAM, LPARAM))p->fn_PostMessageW)(
                (HWND)p->nswindow, WM_APP, 0, 0);
        }
        break;
    }
}

int
webview_shim_set_title(webview_shim_t *w, const char *title)
{
    if (!w || !w->priv || !title) return -1;
    switch (w->priv->platform) {
    case 0: return macos_backend_set_title(w, title);
    case 1: return linux_backend_set_title(w, title);
    case 2: return windows_backend_set_title(w, title);
    default: return -1;
    }
}

int
webview_shim_set_size(webview_shim_t *w, int width, int height, int hints)
{
    if (!w || !w->priv) return -1;
    switch (w->priv->platform) {
    case 0: return macos_backend_set_size(w, width, height, hints);
    case 1: return linux_backend_set_size(w, width, height, hints);
    case 2: return windows_backend_set_size(w, width, height, hints);
    default: return -1;
    }
}

int
webview_shim_bind(webview_shim_t *w, const char *name,
                  webview_shim_bind_fn fn, void *arg)
{
    if (!w || !w->priv || !name || !name[0] || !fn) return -1;
    webview_shim_priv *p = w->priv;
    if (p->bindings_count >= WV_MAX_BINDINGS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < p->bindings_count; i++) {
        if (strcmp(p->bindings[i].name, name) == 0) {
            p->bindings[i].fn = fn;
            p->bindings[i].arg = arg;
            return 0;
        }
    }

    int idx = p->bindings_count++;
    size_t nlen = strlen(name);
    if (nlen >= sizeof(p->bindings[idx].name)) nlen = sizeof(p->bindings[idx].name) - 1;
    memcpy(p->bindings[idx].name, name, nlen);
    p->bindings[idx].name[nlen] = 0;
    p->bindings[idx].fn = fn;
    p->bindings[idx].arg = arg;

    /* Inject JS stub for this binding */
    return wv_inject_bind_js(w, name);
}

int
webview_shim_unbind(webview_shim_t *w, const char *name)
{
    if (!w || !w->priv || !name || !name[0]) return -1;
    webview_shim_priv *p = w->priv;
    for (int i = 0; i < p->bindings_count; i++) {
        if (strcmp(p->bindings[i].name, name) == 0) {
            /* Remove by shifting */
            p->bindings_count--;
            if (i < p->bindings_count) {
                memmove(&p->bindings[i], &p->bindings[i+1],
                        sizeof(wv_binding_t) * (p->bindings_count - i));
            }
            /* Eval JS to delete the window function */
            char js[256];
            snprintf(js, sizeof(js), "delete window['%s'];", name);
            webview_shim_eval(w, js);
            return 0;
        }
    }
    return -1;
}

void
webview_shim_return(webview_shim_t *w, const char *id,
                    int success, const char *result)
{
    if (!w || !w->priv || !id || !result) return;
    /* Escape result for embedding in a JS string literal. The result is
       expected to be valid JSON. We escape backslashes, quotes, and
       control characters, then wrap in JSON.parse() so the JS side gets
       the exact JSON value (string, number, object, etc.). */
    char buf[4096];
    char *d = buf;
    const char *s = result;
    while (*s && (size_t)(d - buf) < sizeof(buf) - 4) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\')       { *d++ = '\\'; *d++ = '\\'; }
        else if (c == '\'')  { *d++ = '\\'; *d++ = '\''; }
        else if (c == '\n')  { *d++ = '\\'; *d++ = 'n'; }
        else if (c == '\r')  { *d++ = '\\'; *d++ = 'r'; }
        else if (c == '\t')  { *d++ = '\\'; *d++ = 't'; }
        else if (c < 0x20)   { *d++ = '\\'; *d++ = 'x'; /* skip control chars */ }
        else                  { *d++ = c; }
        s++;
    }
    *d = 0;

    char js[4096];
    int n = snprintf(js, sizeof(js),
        "window.__wv_bind_resolve('%s',%d,JSON.parse('%s'));",
        id, success ? 1 : 0, buf);
    if (n > 0 && n < (int)sizeof(js))
        webview_shim_eval(w, js);
}

int
webview_shim_init_script(webview_shim_t *w, const char *js)
{
    if (!w || !w->priv || !js || !js[0]) return -1;
    webview_shim_priv *p = w->priv;

    /* Store the init script */
    if (p->init_scripts_count >= WV_MAX_INIT_SCRIPTS) return -1;
    int idx = p->init_scripts_count++;
    p->init_scripts[idx] = strdup(js);
    if (!p->init_scripts[idx]) return -1;

    /* If webview already initialized, inject now */
    if (p->webview && p->running) {
        switch (p->platform) {
        case 0: return macos_backend_init_script(w, js);
        case 1: return linux_backend_init_script(w, js);
        case 2: return windows_backend_init_script(w, js);
        }
    }
    return 0;
}

int
webview_shim_set_html(webview_shim_t *w, const char *html)
{
    if (!w || !w->priv || !html) return -1;
    switch (w->priv->platform) {
    case 0: return macos_backend_set_html(w, html);
    case 1: return linux_backend_set_html(w, html);
    case 2: return windows_backend_set_html(w, html);
    default: return -1;
    }
}

int
webview_shim_set_fullscreen(webview_shim_t *w, int fullscreen)
{
    if (!w || !w->priv) return -1;
    switch (w->priv->platform) {
    case 0: return macos_backend_set_fullscreen(w, fullscreen);
    case 1: return linux_backend_set_fullscreen(w, fullscreen);
    case 2: return windows_backend_set_fullscreen(w, fullscreen);
    default: return -1;
    }
}

void
webview_shim_terminate(webview_shim_t *w)
{
    if (!w || !w->priv) return;
    w->priv->running = 0;
    /* Wake event loop on Windows */
    if (w->priv->platform == 2 && w->priv->fn_PostMessageW && w->priv->nswindow) {
        ((MSABI int (*)(HWND, UINT, WPARAM, LPARAM))w->priv->fn_PostMessageW)(
            (HWND)w->priv->nswindow, WM_QUIT, 0, 0);
    }
}

static int  backend_init(webview_shim_t *w) { return w->priv->init(w); }
static int  backend_loop(webview_shim_t *w, int b) { return w->priv->loop(w, b); }
static void backend_eval(webview_shim_t *w, const char *j) { w->priv->eval(w, j); }
static void backend_exit(webview_shim_t *w) { w->priv->exit(w); }
