/*
 * Monatomic (C) — CEF (open Chromium) host.
 *
 * Opens a native Win32 window hosting a Chromium Embedded Framework browser that
 * renders the HTML/CSS/JS UI (in ui/). The UI talks to the C core DIRECTLY over
 * CEF's JS<->native process-message bridge (no HTTP server): JS posts JSON
 * commands, C runs the matching mn_app_* call and posts JSON state back. Album art
 * + assets load from disk via a custom scheme (monatomic://assets). Runs the
 * message loop until the window closes.
 */
#ifndef MONATOMIC_CEF_HOST_H
#define MONATOMIC_CEF_HOST_H

#include "app.h"

/* ui_dir = folder with index.html; art_dir = the album-art cache folder.
 * Returns the process exit code. */
int webview_run(mn_app *app, const char *ui_dir, const char *art_dir);

#endif /* MONATOMIC_CEF_HOST_H */
