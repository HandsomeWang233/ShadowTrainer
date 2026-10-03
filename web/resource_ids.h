#pragma once
// One numeric id per embedded web asset. A plain C header so rc.exe (through
// webui.rc) and the C++ asset table in src/ui/webview_host.cpp read the same
// numbers. Ids are stable: new assets are appended rather than inserted, so
// this list and the table need not agree about order.
#define IDR_WEB_INDEX 101
#define IDR_WEB_APP_CSS 102
#define IDR_WEB_FORMAT_JS 103
#define IDR_WEB_BRIDGE_JS 104
#define IDR_WEB_GATING_JS 105
#define IDR_WEB_HEXVIEW_JS 106
#define IDR_WEB_APP_JS 107
#define IDR_WEB_CHROME_JS 108
#define IDR_WEB_SCAN_JS 109
#define IDR_WEB_ADDRESS_JS 110
#define IDR_WEB_MEMORY_JS 111
#define IDR_WEB_VIEWS_JS 112
#define IDR_WEB_POINTER_JS 113
#define IDR_WEB_BRAND_PNG 114
// Not a web asset: the window class and WM_SETICON read this one directly.
#define IDR_APP_ICON 115
#define IDR_WEB_BOOT_JS 116
#define IDR_WEB_SORT_JS 117
#define IDR_WEB_MODAL_JS 118
