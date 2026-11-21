/* version.h - the one place osDisplay's version is set; osDisplay -v reports it.
 * OSDISPLAY_GIT, from the Makefile, adds the commit built: +DATE.TIME.HASH. */
#ifndef OSDISPLAY_VERSION_H
#define OSDISPLAY_VERSION_H

#include "build/gitversion.h"
#define OSDISPLAY_VERSION "0.1.0" OSDISPLAY_GIT

#endif
