// i18n.h - gettext wrapper. All user-visible strings go through _(). Catalogs (en default, uk, ru) are added in po/.
#pragma once
#include <libintl.h>

#define _(s) gettext(s)
#define GETTEXT_DOMAIN "icmon"
#define APP_NAME "ICmon"  // the program's name: never translated; change here for other builds
