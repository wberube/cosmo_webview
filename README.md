<p align="center">

<img src="icon.svg" width="160" height="160" alt="Cosmopolitan WebView icon">

</p>

<h1 align="center">Cosmopolitan WebView</h1>

<p align="center"><strong>one binary. every os. native GUI — from C, zero headers.</strong></p>

A tiny cross-platform webview library that compiles with [cosmocc](https://cosmo.zip/) into a single portable **APE** (Actually Portable Executable).

It opens a native OS window with a WebKit/WebView2 webview — no dependencies, no package managers, no install. One binary, every OS.

```c
// A complete hello-world webview app:
#include "webview_shim.h"

int main(void) {
    webview_shim_t w = {0};
    w.app_name = "Hello";
    w.html     = "<h1>hello world</h1>";

    if (webview_shim_init(&w) != 0) return 1;
    while (webview_shim_loop(&w, 1) == 0) {}
    webview_shim_exit(&w);
    return 0;
}
```

```plaintext
./build.sh               →  webview_demo  (768 KB APE)
./webview_demo           →  native window with your HTML
```

## Features
- **dlopen/dlsym at runtime** — no system headers, no linker flags. ZERO includes.
- **Portable APE** — the same binary runs on macOS, Linux, and Windows (WebView2).
- **JS↔C bridge** — `window.external.invoke(json)` intercepted via WKNavigationDelegate (macOS), NavigationStarting (Windows), or WebKit user content manager (Linux). Handled via `on_invoke`.
- **C→JS eval** — call `webview_shim_eval(w, "js code")` to inject JavaScript.
- **No malloc-churn** — one allocation, one thread, no callbacks after init.

## Quick start

```bash
git clone https://github.com/yourname/cosmo_webview.git
cd cosmo_webview
./build.sh
./webview_demo          # a window opens with a cute UI
```

## API
| Function | Description |
| --- | --- |
| webview_shim_init(w) | Create the native window + webview |
| webview_shim_loop(w, blocking) | Process one event loop frame |
| webview_shim_eval(w, js) | Execute JavaScript in the webview |
| webview_shim_exit(w) | Tear down everything |

## Structure
| File | Role |
| --- | --- |
| webview_shim.h | Public C API (zero dependencies) |
| webview_shim.c | Platform dispatch + macOS / Linux / Windows backends |
| main.c | Demo app — shows what a real app looks like |
| build.sh / build.bat | Download cosmocc and compile the APE |
