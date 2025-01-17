#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/xpm.h>
#include <X11/Xft/Xft.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/sync.h>

#include "util.h"
#include "widget.h"
#include "winicon.data"         /* window icon, for the window manager */

/* default theme configuration */
#define DEF_FONT        "monospace:size=9"
#define DEF_BG          "#0A0A0A"
#define DEF_FG          "#FFFFFF"
#define DEF_SELBG       "#121212"
#define DEF_SELFG       "#707880"

/* ellipsis has two dots rather than three; the third comes from the extension */
#define ELLIPSIS        ".."

/* what to display when item's status is unknown */
#define STATUS_UNKNOWN  "?"

/* opacity of drag-and-drop mini window */
#define DND_OPACITY     0x7FFFFFFF

/* constants to check a .ppm file */
#define PPM_HEADER      "P6\n"
#define PPM_COLOR       "255\n"

#define ALARMFLAGS      (XSyncCACounter | XSyncCAValue | XSyncCAValueType | XSyncCATestType | XSyncCADelta)
#define DEPTH(d)        (DefaultDepth((d), DefaultScreen((d))))
#define VISUAL(d)       (DefaultVisual((d), DefaultScreen((d))))
#define COLORMAP(d)     (DefaultColormap((d), DefaultScreen((d))))
#define WIDTH(d)        (DisplayWidth((d), DefaultScreen((d))))
#define HEIGHT(d)       (DisplayHeight((d), DefaultScreen((d))))
#define ROOT(d)         (DefaultRootWindow((d)))
#define FLAG(f, b)      (((f) & (b)) == (b))
#define ATOI(c)         (((c) >= '0' && (c) <= '9') ? (c) - '0' : -1)

/* how much to scroll on PageUp/PageDown (half the window height) */
#define PAGE_STEP(w)    ((w)->h / 2)

enum {
	/* number of members on a the .data.l[] array of a XClientMessageEvent */
	NCLIENTMSG_DATA = 5,

	/* drag and drop miniwindow */
	DND_DISTANCE    = 8,                    /* distance from pointer to dnd miniwindow */

	/* distance the cursor must move to be considered a drag */
	DRAG_THRESHOLD  = 8,
	DRAG_SQUARE     = (DRAG_THRESHOLD * DRAG_THRESHOLD),

	/* XDND protocol version */
	XDND_VERSION    = 5,

	/* buttons not defined by X.h */
	BUTTON8         = 8,
	BUTTON9         = 9,

	/* one byte was 8 bits in size last time I checked */
	BYTE            = 8,

	/* color depths */
	CLIP_DEPTH      = 1,                    /* 0/1 */
	PPM_DEPTH       = 3,                    /* RGB */
	DATA_DEPTH      = 4,                    /* BGRA */

	/* sizes of string buffers */
	TITLE_BUFSIZE   = 1028,                 /* title buffer size */
	STATUS_BUFSIZE  = 64,                   /* status buffer size */
	RES_BUFSIZE     = 512,                  /* resource buffer size */

	/* hardcoded object sizes in pixels */
	/* there's no ITEM_HEIGHT for it is computed at runtime from font height */
	THUMBSIZE       = 64,                   /* maximum thumbnail size */
	ITEM_WIDTH      = (THUMBSIZE * 2),      /* width of an item (icon + margins) */
	MIN_WIDTH       = ITEM_WIDTH,           /* minimum window width */
	MIN_HEIGHT      = (THUMBSIZE * 3),      /* minimum window height */
	DEF_WIDTH       = 600,                  /* default window width */
	DEF_HEIGHT      = 460,                  /* default window height */
	STIPPLE_SIZE    = 2,                    /* size of stipple pattern */
	MARGIN          = 16,                   /* top margin above first row */

	/* constants for parsing .ppm files */
	PPM_HEADER_SIZE = (sizeof(PPM_HEADER) - 1),
	PPM_COLOR_SIZE  = (sizeof(PPM_COLOR) - 1),
	PPM_BUFSIZE     = 8,

	/* draw up to NLINES lines of label; each one up to LABELWIDTH pixels long */
	NLINES          = 2,                    /* change it for more lines below icons */
	LABELWIDTH      = ITEM_WIDTH - 16,      /* save 8 pixels each side around label */

	/* times in milliseconds */
	DOUBLECLICK     = 250,                  /* time of a doubleclick, in milliseconds */
	MOTION_TIME     = 32,                   /* update time rate for rectangular selection */

	/* scrolling */
	SCROLL_STEP     = 32,                   /* pixels per scroll */
	SCROLLER_SIZE   = 32,                   /* size of the scroller */
	SCROLLER_MIN    = 16,                   /* min lines to scroll for the scroller to change */
	HANDLE_MAX_SIZE = (SCROLLER_SIZE - 2),  /* max size of the scroller handle */
};

enum {
	SELECT_NOT,
	SELECT_YES,
	SELECT_LAST,
};

enum {
	COLOR_BG,
	COLOR_FG,
	COLOR_LAST,
};

enum {
	/* xdnd window properties */
	XDND_AWARE,
	XDND_TYPELIST,
	XDND_DIRECT_SAVE,

	/* xdnd selections */
	XDND_SELECTION,

	/* xdnd client messages */
	XDND_ENTER,
	XDND_POSITION,
	XDND_STATUS,
	XDND_LEAVE,
	XDND_DROP,
	XDND_FINISHED,

	/* xdnd actions */
	XDND_ACTION_COPY,
	XDND_ACTION_MOVE,
	XDND_ACTION_LINK,
	XDND_ACTION_ASK,
	XDND_ACTION_PRIVATE,
	XDND_ACTION_DIRECT_SAVE,

	/* xdnd types we accept */
	XDND_URI_LIST,
	TEXT_URI_LIST,
	TEXT_PLAIN,

	/* selection targets */
	UTF8_STRING,
	COMPOUND_TEXT,
	DELETE,
	INCR,
	MULTIPLE,
	TARGETS,
	TEXT,
	TIMESTAMP,

	/* selection properties */
	ATOM_PAIR,

	/* window protocols */
	WM_PROTOCOLS,
	WM_DELETE_WINDOW,

	/* window properties */
	_NET_WM_ICON,
	_NET_WM_NAME,
	_NET_WM_PID,
	_NET_WM_WINDOW_TYPE,
	_NET_WM_WINDOW_TYPE_DND,
	_NET_WM_WINDOW_TYPE_NORMAL,
	_NET_WM_WINDOW_OPACITY,

	/* others */
	_NULL,

	ATOM_LAST,
};

struct Icon {
	Pixmap pix, mask;
};

struct Thumb {
	struct Thumb *next;
	int w, h;
	XImage *img;
};

struct Selection {
	struct Selection *prev, *next;
	int index;
};

struct Widget {
	char *progname;
	int start;
	int redraw;

	/* X11 stuff */
	Display *dpy;
	Atom atoms[ATOM_LAST];
	GC stipgc, gc;
	Cursor cursors[CURSOR_LAST];    /* for the hourglass cursor, when loading */
	Window win;
	XftColor colors[SELECT_LAST][COLOR_LAST];
	XftFont *font;
	Atom dropaction;
	Window dropsrc;

	Pixmap pix;                     /* the pixmap of the window */
	Pixmap namepix;                 /* temporary pixmap for the labels */
	Pixmap stipple;                 /* stipple for painting icons of selected items */
	Pixmap rectbord;                /* rectangular selection border */

	/*
	 * TODO
	 */
	pthread_mutex_t lock;

	/*
	 * .items is an array of arrays of strings.
	 * For each index i, .items[i] contains (at least) three elements:
	 * - .items[i][ITEM_NAME]   -- The label displayed for the item.
	 * - .items[i][ITEM_PATH]   -- The path given in PRIMARY selection.
	 * - .items[i][ITEM_STATUS] -- The status string displayed on the
	 *                             titlebar when the item is selected.
	 */
	char ***items;                  /* see comment above */
	int nitems;                     /* number of items */
	int *linelen;                   /* for each item, the lengths of its largest label line */
	int *nlines;                    /* for each item, the number of label lines */

	/*
	 * Items can be selected with the mouse and the Control and Shift modifiers.
	 *
	 * We keep track of selections in a list of selections, which is
	 * essentially a doubly-linked list of indices.  It's kept as a
	 * doubly-linked list for easily adding and removing any
	 * element.
	 *
	 * We also maintain an array of pointers to selections, so we
	 * can easily access a selection in the list, and remove it for
	 * example, given the index of an item.
	 */
	struct Selection *sel;          /* list of selections */
	struct Selection *rectsel;      /* list of selections by rectsel */
	struct Selection **issel;       /* array of pointers to Selections */
	Time seltime;

	/*
	 * We keep track of thumbnails in a list of thumbnails, which is
	 * essentially a singly-linked list of XImages.  It's kept as a
	 * singly-linked list just so we can traverse them one-by-one at
	 * the end for freeing them.
	 *
	 * We also maintain an array of pointers to thumbnails, so we
	 * can easily access a thumbnail in the list, given the index
	 * of an item.
	 */
	struct Thumb *thumbhead;
	struct Thumb **thumbs;

	/*
	 * Geometry of the window and its contents.
	 *
	 * WARNING:
	 * - .nrows is the number of rows in the pixmap, which is
	 *   approximately the number of rows visible in the window
	 *   (that is, those that are not hidden by scrolling) plus 2.
	 * - .ncols is also the number of rows in the pixmap, which is
	 *   exactly the number of columns visible in the window.
	 *
	 * I call "screenful" what is being visible at a given time on
	 * the window.
	 */
	int w, h;                       /* window size */
	int pixw, pixh;                 /* pixmap size */
	int itemw, itemh;               /* size of a item (margin + icon + label) */
	int ydiff;                      /* how much the pixmap is scrolled up */
	int ncols, nrows;               /* number of columns and rows visible at a time */
	int nscreens;                   /* maximum number of screenfuls we can scroll */
	int row;                        /* index of first row visible in the current screenful */
	int fonth;                      /* font height */
	int x0;                         /* position of first column after the left margin */
	int ellipsisw;                  /* width of the ellipsis we draw on long labels */

	/*
	 * We use icons for items that do not have a thumbnail.
	 */
	struct Icon *icons;             /* array of icons set by the user */
	int nicons;                     /* number of icons set by the user */

	/*
	 * This array, with .nitem members, defines the index in the
	 * .icons array for the icon of each item.
	 */
	int *itemicons;

	/* Strings used to build the title bar. */
	const char *title;
	const char *class;

	/*
	 * Index of highlighted item (usually the last item clicked by
	 * the user); or -1 if none.
	 */
	int highlight;

	/*
	 * The scroller how this code calls the widget that replaces the
	 * scrollbar.  It is a little pop-up window that appears after a
	 * middle-click.  It can be either controlled as a scrollbar, by
	 * dragging its inner manipulable object (called the "handler"),
	 * or by moving the pointer up and down the scroller itself.
	 *
	 * The scroller is based on a Firefox's hidden feature called
	 * autoScroll.
	 *
	 * TIP: To enable this feature in Firefox, set the option
	 * "general.autoScroll" to True in about:config.
	 *
	 * We use the XSync extension for the scroller.
	 * See https://nrk.neocities.org/articles/x11-timeout-with-xsyncalarm.html
	 */
	Window scroller;                /* the scroller popup window */
	int handlew;                    /* size of scroller handle */
	XSyncAlarmAttributes syncattr;
	int syncevent;

	/*
	 * We can be either in the main loop (see pollwidget), in the
	 * rectangular selection sub-loop (see rectmotion), or in the
	 * scroller widget sub-loop (see scrollmotion).  Some routines
	 * depend on the loop we're in, so we mark its current state
	 * with this enum.
	 *
	 * NOTE: whenever implementing a new sub-loop, always set its
	 * state at beginning and reset it to STATE_NORMAL at the end
	 * of the function.
	 */
	enum {
		STATE_NORMAL,
		STATE_SELECTING,
		STATE_SCROLLING,
		STATE_DRAGGING,
	} state;
};

static char *atomnames[ATOM_LAST] = {
	[XDND_AWARE]                 = "XdndAware",
	[XDND_TYPELIST]              = "XdndTypeList",
	[XDND_DIRECT_SAVE]           = "XdndDirectSave",
	[XDND_SELECTION]             = "XdndSelection",
	[XDND_ENTER]                 = "XdndEnter",
	[XDND_POSITION]              = "XdndPosition",
	[XDND_STATUS]                = "XdndStatus",
	[XDND_LEAVE]                 = "XdndLeave",
	[XDND_DROP]                  = "XdndDrop",
	[XDND_FINISHED]              = "XdndFinished",
	[XDND_ACTION_COPY]           = "XdndActionCopy",
	[XDND_ACTION_MOVE]           = "XdndActionMove",
	[XDND_ACTION_LINK]           = "XdndActionLink",
	[XDND_ACTION_ASK]            = "XdndActionAsk",
	[XDND_ACTION_PRIVATE]        = "XdndActionPrivate",
	[XDND_ACTION_DIRECT_SAVE]    = "XdndActionDirectSave",
	[XDND_URI_LIST]              = "XdndUriList",
	[TEXT_URI_LIST]              = "text/uri-list",
	[TEXT_PLAIN]                 = "text/plain",
	[ATOM_PAIR]                  = "ATOM_PAIR",
	[COMPOUND_TEXT]              = "COMPOUND_TEXT",
	[UTF8_STRING]                = "UTF8_STRING",
	[WM_PROTOCOLS]               = "WM_PROTOCOLS",
	[WM_DELETE_WINDOW]           = "WM_DELETE_WINDOW",
	[DELETE]                     = "DELETE",
	[INCR]                       = "INCR",
	[MULTIPLE]                   = "MULTIPLE",
	[TARGETS]                    = "TARGETS",
	[TEXT]                       = "TEXT",
	[TIMESTAMP]                  = "TIMESTAMP",
	[_NET_WM_ICON]               = "_NET_WM_ICON",
	[_NET_WM_NAME]               = "_NET_WM_NAME",
	[_NET_WM_PID]                = "_NET_WM_PID",
	[_NET_WM_WINDOW_TYPE]        = "_NET_WM_WINDOW_TYPE",
	[_NET_WM_WINDOW_TYPE_DND]    = "_NET_WM_WINDOW_TYPE_DND",
	[_NET_WM_WINDOW_TYPE_NORMAL] = "_NET_WM_WINDOW_TYPE_NORMAL",
	[_NET_WM_WINDOW_OPACITY] =  "_NET_WM_WINDOW_OPACITY",
	[_NULL]                      = "NULL",
};

static int
createwin(Widget wid, const char *class, const char *name, const char *geom, int argc, char *argv[], unsigned long *icon, size_t iconsize)
{
	Atom version = XDND_VERSION;    /* yes, version is an Atom */
	unsigned int dw, dh;
	int x, y;
	int dx, dy;
	int flags, sizehints;
	pid_t pid;

	x = y = 0;
	wid->w = DEF_WIDTH;
	wid->h = DEF_HEIGHT;
	sizehints = 0;
	pid = getpid();
	if (geom != NULL) {
		flags = XParseGeometry(geom, &dx, &dy, &dw, &dh);
		dw = max(MIN_WIDTH, dw);
		dh = max(MIN_HEIGHT, dh);
		if (FLAG(flags, WidthValue)) {
			wid->w = dw;
			sizehints |= USSize;
		}
		if (FLAG(flags, HeightValue)) {
			wid->h = dh;
			sizehints |= USSize;
		}
		if (FLAG(flags, XValue | XNegative)) {
			x = WIDTH(wid->dpy) - wid->w - (dx > 0 ? dx : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			x = dx;
			sizehints |= USPosition;
		}
		if (FLAG(flags, YValue | YNegative)) {
			y = HEIGHT(wid->dpy) - wid->h - (dy > 0 ? dy : 0);
			sizehints |= USPosition;
		} else if (FLAG(flags, XValue)) {
			y = dy;
			sizehints |= USPosition;
		}
	}
	wid->win = XCreateWindow(
		wid->dpy, ROOT(wid->dpy),
		x, y, wid->w, wid->h, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = COLORMAP(wid->dpy),
			.background_pixel = wid->colors[SELECT_NOT][COLOR_BG].pixel,
			.event_mask = StructureNotifyMask | ExposureMask
			            | KeyPressMask | PointerMotionMask
			            | ButtonReleaseMask | ButtonPressMask,
		}
	);
	if (wid->win == None)
		return RET_ERROR;
	wid->namepix = XCreatePixmap(
		wid->dpy,
		wid->win,
		LABELWIDTH,
		wid->fonth,
		DEPTH(wid->dpy)
	);
	if (wid->namepix == None)
		return RET_ERROR;
	XmbSetWMProperties(
		wid->dpy, wid->win,
		class, class,
		argv, argc,
		&(XSizeHints){ .flags = sizehints, },
		NULL,
		&(XClassHint){ .res_class = (char *)class, .res_name = (char *)name, }
	);
	XSetWMProtocols(wid->dpy, wid->win, &wid->atoms[WM_DELETE_WINDOW], 1);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING], 8, PropModeReplace,
		(unsigned char *)class,
		strlen(class) + 1
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&wid->atoms[_NET_WM_WINDOW_TYPE_NORMAL],
		1
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_ICON],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)icon, iconsize
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[_NET_WM_PID],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&pid,
		1
	);
	XChangeProperty(
		wid->dpy, wid->win,
		wid->atoms[XDND_AWARE],
		XA_ATOM, 32,
		PropModeReplace,
		(unsigned char *)&version,
		1
	);
	return RET_OK;
}

static int
ealloccolor(Widget wid, const char *s, XftColor *color)
{
	if(!XftColorAllocName(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), s, color))
		return RET_ERROR;
	return RET_OK;
}

static int
eallocfont(Display *dpy, const char *s, XftFont **font)
{
	if ((*font = XftFontOpenXlfd(dpy, DefaultScreen(dpy), s)) == NULL)
		if ((*font = XftFontOpenName(dpy, DefaultScreen(dpy), s)) == NULL)
			return RET_ERROR;
	return RET_OK;
}

static char *
getresource(XrmDatabase xdb, const char *class, const char *name, const char *resource)
{
	XrmValue xval;
	char *type;
	char classbuf[RES_BUFSIZE], namebuf[RES_BUFSIZE];

	if (xdb == NULL)
		return NULL;
	(void)snprintf(classbuf, RES_BUFSIZE, "%s.%s", class, resource);
	(void)snprintf(namebuf, RES_BUFSIZE, "%s.%s", name, resource);
	if (XrmGetResource(xdb, namebuf, classbuf, &type, &xval) == True)
		return xval.addr;
	return NULL;
}

static int
textwidth(Widget wid, const char *text, int len)
{
	XGlyphInfo box;

	XftTextExtentsUtf8(wid->dpy, wid->font, (const FcChar8 *)text, len, &box);
	return box.width;
}

static int
inittheme(Widget wid, const char *class, const char *name)
{
	XrmDatabase xdb;
	int i, j, goterror;;
	char *xrm, *s;
	char *resources[SELECT_LAST][COLOR_LAST] = {
		[SELECT_NOT][COLOR_BG] = "background",
		[SELECT_NOT][COLOR_FG] = "foreground",
		[SELECT_YES][COLOR_BG] = "selbackground",
		[SELECT_YES][COLOR_FG] = "selforeground",
	};
	char *defvalue[SELECT_LAST][COLOR_LAST] = {
		[SELECT_NOT][COLOR_BG] = DEF_BG,
		[SELECT_NOT][COLOR_FG] = DEF_FG,
		[SELECT_YES][COLOR_BG] = DEF_SELBG,
		[SELECT_YES][COLOR_FG] = DEF_SELFG,
	};
	int colorerror[SELECT_LAST][COLOR_LAST] = { { FALSE, FALSE }, { FALSE, FALSE } };
	int fonterror = FALSE;

	xdb = NULL;
	goterror = FALSE;
	if ((xrm = XResourceManagerString(wid->dpy)) != NULL)
		xdb = XrmGetStringDatabase(xrm);
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			s = getresource(xdb, class, name, resources[i][j]);
			if (s == NULL) {
				/* could not found resource; use default value */
				s = defvalue[i][j];
			} else if (ealloccolor(wid, s, &wid->colors[i][j]) == RET_ERROR) {
				/* resource found, but allocation failed; use default value */
				warnx("\"%s\": could not load color (falling back to \"%s\")", s, defvalue[i][j]);
				s = defvalue[i][j];
			} else {
				/* resource found and successfully allocated */
				continue;
			}
			if (ealloccolor(wid, s, &wid->colors[i][j]) == RET_ERROR) {
				warnx("\"%s\": could not load color", s);
				colorerror[i][j] = TRUE;
				goterror = TRUE;
			}
		}
	}
	s = getresource(xdb, class, name, "faceName");
	if (s == NULL) {
		/* could not found resource; use default value */
		s = DEF_FONT;
	} else if (eallocfont(wid->dpy, s, &wid->font) == RET_ERROR) {
		/* resource found, but allocation failed; use default value */
		warnx("\"%s\": could not open font (falling back to \"%s\")", s, DEF_FONT);
		s = DEF_FONT;
	} else {
		goto done;
	}
	if (eallocfont(wid->dpy, s, &wid->font) == RET_ERROR) {
		warnx("\"%s\": could not open font", s);
		fonterror = TRUE;
		goterror = TRUE;
	}
done:
	if (goterror)
		goto error;
	wid->fonth = wid->font->height;
	wid->itemw = ITEM_WIDTH;
	wid->itemh = THUMBSIZE + (NLINES + 1) * wid->fonth;
	wid->ellipsisw = textwidth(wid, ELLIPSIS, strlen(ELLIPSIS));
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RET_OK;
error:
	for (i = 0; i < SELECT_LAST; i++) {
		for (j = 0; j < COLOR_LAST; j++) {
			if (colorerror[i][j])
				continue;
			XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->colors[i][j]);
		}
	}
	if (!fonterror)
		XftFontClose(wid->dpy, wid->font);
	if (xdb != NULL)
		XrmDestroyDatabase(xdb);
	return RET_ERROR;
}

static int
calcsize(Widget wid, int w, int h)
{
	int ncols, nrows, ret;
	double d;

	ret = 0;
	if (wid->w == w && wid->h == h)
		return 0;
	etlock(&wid->lock);
	ncols = wid->ncols;
	nrows = wid->nrows;
	if (w > 0 && h > 0) {
		wid->w = w;
		wid->h = h;
		wid->ydiff = 0;
	}
	wid->ncols = max(wid->w / wid->itemw, 1);
	wid->nrows = max(wid->h / wid->itemh + (wid->h % wid->itemh ? 2 : 1), 1);
	wid->x0 = max((wid->w - wid->ncols * wid->itemw) / 2, 0);
	wid->nscreens = wid->nitems / wid->ncols - wid->h / wid->itemh + (wid->nitems % wid->ncols != 0 ? 1 : 0);
	wid->nscreens = max(wid->nscreens, 1);
	d = (double)wid->nscreens / SCROLLER_MIN;
	d = (d < 1.0 ? 1.0 : d);
	wid->handlew = max(SCROLLER_SIZE / d - 2, 1);
	wid->handlew = min(wid->handlew, HANDLE_MAX_SIZE);
	if (wid->handlew == HANDLE_MAX_SIZE && wid->nscreens > 1)
		wid->handlew = HANDLE_MAX_SIZE - 1;
	if (ncols != wid->ncols || nrows != wid->nrows) {
		if (wid->pix != None)
			XFreePixmap(wid->dpy, wid->pix);
		if (wid->rectbord != None)
			XFreePixmap(wid->dpy, wid->rectbord);
		wid->pixw = wid->ncols * wid->itemw;
		wid->pixh = wid->nrows * wid->itemh;
		wid->pix = XCreatePixmap(wid->dpy, wid->win, wid->pixw, wid->pixh, DEPTH(wid->dpy));
		wid->rectbord = XCreatePixmap(wid->dpy, wid->win, wid->w, wid->h, CLIP_DEPTH);
		ret = 1;
	}
	etunlock(&wid->lock);
	return ret;
}

static int
isbreakable(char c)
{
	return c == '.' || c == '-' || c == '_';
}

static void
drawtext(Widget wid, Drawable pix, XftColor *color, int x, const char *text, int len)
{
	XftDraw *draw;
	int w;

	w = textwidth(wid, text, len);
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->namepix, wid->gc, 0, 0, LABELWIDTH, wid->fonth);
	XSetForeground(wid->dpy, wid->gc, color[COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->namepix, wid->gc, x, 0, w, wid->fonth);
	draw = XftDrawCreate(wid->dpy, pix, VISUAL(wid->dpy), COLORMAP(wid->dpy));
	XftDrawStringUtf8(draw, &color[COLOR_FG], wid->font, x, wid->font->ascent, (const FcChar8 *)text, len);
	XftDrawDestroy(draw);
}

static void
setrow(Widget wid, int row)
{
	etlock(&wid->lock);
	wid->row = row;
	etunlock(&wid->lock);
}

static void
drawicon(Widget wid, int index, int x, int y)
{
	XGCValues val;
	Pixmap pix, mask;
	int icon;

	icon = wid->itemicons[index];
	pix = wid->icons[icon].pix;
	mask = wid->icons[icon].mask;
	if (wid->thumbs != NULL && wid->thumbs[index] != NULL) {
		/* draw thumbnail */
		XPutImage(
			wid->dpy,
			wid->pix,
			wid->gc,
			wid->thumbs[index]->img,
			0, 0,
			x + (wid->itemw - wid->thumbs[index]->w) / 2,
			y + (THUMBSIZE - wid->thumbs[index]->h) / 2,
			wid->thumbs[index]->w,
			wid->thumbs[index]->h
		);
		if (wid->issel[index]) {
			XSetFillStyle(wid->dpy, wid->gc, FillStippled);
			XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				wid->dpy,
				wid->pix,
				wid->gc,
				x + (wid->itemw - wid->thumbs[index]->w) / 2,
				y + (THUMBSIZE - wid->thumbs[index]->h) / 2,
				wid->thumbs[index]->w,
				wid->thumbs[index]->h
			);
			XSetFillStyle(wid->dpy, wid->gc, FillSolid);
		}
	} else {
		/* draw icon */
		val.clip_x_origin = x + (wid->itemw - THUMBSIZE) / 2;
		val.clip_y_origin = y;
		val.clip_mask = mask;
		XChangeGC(wid->dpy, wid->gc, GCClipXOrigin | GCClipYOrigin | GCClipMask, &val);
		XCopyArea(
			wid->dpy,
			pix, wid->pix,
			wid->gc,
			0, 0,
			THUMBSIZE, THUMBSIZE,
			val.clip_x_origin,
			y
		);
		if (wid->issel[index]) {
			XSetFillStyle(wid->dpy, wid->gc, FillStippled);
			XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_YES][COLOR_BG].pixel);
			XFillRectangle(
				wid->dpy,
				wid->pix,
				wid->gc,
				val.clip_x_origin, y,
				THUMBSIZE, THUMBSIZE
			);
			XSetFillStyle(wid->dpy, wid->gc, FillSolid);
		}
		val.clip_mask = None;
		XChangeGC(wid->dpy, wid->gc, GCClipMask, &val);
	}
}

static void
drawlabel(Widget wid, int index, int x, int y)
{
	XftColor *color;
	int i;
	int textx, maxw;
	int textw, w, textlen, len;
	int extensionw, extensionlen;
	char *text, *extension;

	color = wid->colors[(wid->issel != NULL && wid->issel[index]) ? SELECT_YES : SELECT_NOT];
	text = wid->items[index][ITEM_NAME];
	wid->nlines[index] = 1;
	textx = x + wid->itemw / 2 - LABELWIDTH / 2;
	extension = NULL;
	maxw = 0;
	textlen = 0;
	wid->linelen[index] = 0;
	for (i = 0; i < wid->nlines[index]; i++) {
		while (isspace(text[textlen]))
			textlen++;
		text += textlen;
		textlen = strlen(text);
		textw = textwidth(wid, text, textlen);
		if (wid->nlines[index] < NLINES && textw >= LABELWIDTH) {
			textlen = len = 0;
			w = 0;
			while (w < LABELWIDTH) {
				textlen = len;
				textw = w;
				while (isspace(text[len]))
					len++;
				while (isbreakable(text[len]))
					len++;
				while (text[len] != '\0' && !isspace(text[len]) && !isbreakable(text[len]))
					len++;
				w = textwidth(wid, text, len);
				if (text[len] == '\0') {
					break;
				}
			}
			if (textw > 0) {
				wid->nlines[index] = min(wid->nlines[index] + 1, NLINES);
			} else {
				textlen = len;
				textw = w;
			}
		}
		textw = min(LABELWIDTH, textw);
		maxw = max(textw, maxw);
		drawtext(
			wid,
			wid->namepix, color,
			max(LABELWIDTH / 2 - textw / 2, 0),
			text, textlen
		);
		textw = min(textw, LABELWIDTH);
		wid->linelen[index] = max(wid->linelen[index], textw);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			LABELWIDTH, wid->fonth,
			textx, y + wid->itemh - (NLINES - i + 0.5) * wid->fonth
		);
	}
	if (index == wid->highlight) {
		XSetForeground(wid->dpy, wid->gc, color[COLOR_FG].pixel);
		XDrawRectangle(
			wid->dpy,
			wid->pix,
			wid->gc,
			x + wid->itemw / 2 - maxw / 2 - 1,
			y + wid->itemh - (NLINES + 0.5) * wid->fonth - 1,
			maxw + 1, i * wid->fonth + 1
		);
	}
	if (textw >= LABELWIDTH &&
	    (extension = strrchr(text, '.')) != NULL &&
	    extension[1] != '\0') {
		extensionlen = strlen(extension);
		extensionw = textwidth(wid, extension, extensionlen);
	}
	if (extension != NULL) {
		/* draw ellipsis */
		drawtext(
			wid,
			wid->namepix, color,
			0,
			ELLIPSIS, strlen(ELLIPSIS)
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			wid->ellipsisw, wid->fonth,
			textx + textw - extensionw - wid->ellipsisw,
			y + wid->itemh - (NLINES + 1 - wid->nlines[index] + 0.5) * wid->fonth
		);

		/* draw extension */
		drawtext(
			wid,
			wid->namepix, color,
			0, extension, extensionlen
		);
		XCopyArea(
			wid->dpy,
			wid->namepix, wid->pix,
			wid->gc,
			0, 0,
			extensionw, wid->fonth,
			textx + textw - extensionw,
			y + wid->itemh - (NLINES + 1 - wid->nlines[index] + 0.5) * wid->fonth
		);
	}
}

static int
firstvisible(Widget wid)
{
	/* gets index of last visible item */
	return wid->row * wid->ncols;
}

static int
lastvisible(Widget wid)
{
	/* gets index of last visible item */
	return min(wid->nitems, firstvisible(wid) + wid->nrows * wid->ncols) - 1;
}

static void
drawitem(Widget wid, int index)
{
	int i, x, y, min, max;

	etlock(&wid->lock);
	min = firstvisible(wid);
	max = lastvisible(wid);
	if (index < min || index > max)
		goto done;
	i = index - min;
	x = i % wid->ncols;
	y = (i / wid->ncols) % wid->nrows;
	x *= wid->itemw;
	y *= wid->itemh;
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, x, y, wid->itemw, wid->itemh);
	drawicon(wid, index, x, y);
	drawlabel(wid, index, x, y);
done:
	etunlock(&wid->lock);
	wid->redraw = TRUE;
}

static void
drawitems(Widget wid)
{
	int i, n;

	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_BG].pixel);
	XFillRectangle(wid->dpy, wid->pix, wid->gc, 0, 0, wid->w, wid->nrows * wid->itemh);
	n = lastvisible(wid);
	for (i = wid->row * wid->ncols; i <= n; i++) {
		drawitem(wid, i);
	}
}

static void
commitdraw(Widget wid)
{
	etlock(&wid->lock);
	XClearWindow(wid->dpy, wid->win);
	XCopyArea(
		wid->dpy,
		wid->pix, wid->win,
		wid->gc,
		0, wid->ydiff - MARGIN,
		wid->pixw, wid->pixh,
		wid->x0, 0
	);
	if (wid->state != STATE_SELECTING)
		goto done;
	XChangeGC(
		wid->dpy,
		wid->gc,
		GCClipXOrigin | GCClipYOrigin | GCClipMask,
		&(XGCValues) {
			.clip_x_origin = 0,
			.clip_y_origin = 0,
			.clip_mask = wid->rectbord,
		}
	);
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_FG].pixel);
	XFillRectangle(wid->dpy, wid->win, wid->gc, 0, 0, wid->w, wid->h);
	XChangeGC(
		wid->dpy,
		wid->gc,
		GCClipMask,
		&(XGCValues) {
			.clip_mask = None,
		}
	);
	XFlush(wid->dpy);
done:
	etunlock(&wid->lock);
}

static void
settitle(Widget wid)
{
	char title[TITLE_BUFSIZE];
	char nitems[STATUS_BUFSIZE];
	char *selitem, *status;
	int scrollpct;                  /* scroll percentage */

	if (wid->row == 0 && wid->nscreens > 1)
		scrollpct = 0;
	else
		scrollpct = 100 * ((double)(wid->row + 1) / wid->nscreens);
	(void)snprintf(nitems, STATUS_BUFSIZE, "%d items", wid->nitems - 1);
	selitem = "";
	status = nitems;
	selitem = (wid->highlight > 0 ? wid->items[wid->highlight][ITEM_NAME] : "");
	if (wid->highlight <= 0)
		status = nitems;
	else if (wid->items[wid->highlight][ITEM_STATUS] == NULL)
		status = STATUS_UNKNOWN;
	else
		status = wid->items[wid->highlight][ITEM_STATUS];
	if (wid->title != NULL) {
		(void)snprintf(
			title, TITLE_BUFSIZE,
			"%s%s%s (%s) - %s (%d%%)",
			wid->title,
			(strcmp(wid->title, "/") != 0 ? "/" : ""),
			selitem,
			status,
			wid->class,
			scrollpct
		);
	}
	XmbSetWMProperties(wid->dpy, wid->win, title, title, NULL, 0, NULL, NULL, NULL);
	XChangeProperty(
		wid->dpy,
		wid->win,
		wid->atoms[_NET_WM_NAME],
		wid->atoms[UTF8_STRING],
		8,
		PropModeReplace,
		(unsigned char *)title,
		strlen(title)
	);
}

static int
gethandlepos(Widget wid)
{
	int row;

	if (wid->ydiff >= wid->itemh)
		row = wid->nscreens;
	else
		row = wid->row;
	return (HANDLE_MAX_SIZE - wid->handlew) * ((double)row / wid->nscreens);
}

static void
drawscroller(Widget wid, int y)
{
	Pixmap pix;

	if ((pix = XCreatePixmap(wid->dpy, wid->scroller, SCROLLER_SIZE, SCROLLER_SIZE, DEPTH(wid->dpy))) == None)
		return;
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_BG].pixel);
	XFillRectangle(wid->dpy, pix, wid->gc, 0, 0, SCROLLER_SIZE, SCROLLER_SIZE);
	XSetForeground(wid->dpy, wid->gc, wid->colors[SELECT_NOT][COLOR_FG].pixel);
	XFillRectangle(wid->dpy, pix, wid->gc, 1, y + 1, HANDLE_MAX_SIZE, wid->handlew);
	XSetWindowBackgroundPixmap(wid->dpy, wid->scroller, pix);
	XClearWindow(wid->dpy, wid->scroller);
	XFreePixmap(wid->dpy, pix);
}

static int
scroll(Widget wid, int y)
{
	int prevhand, newhand;          /* position of the scroller handle */
	int prevrow, newrow;

	if (y == 0)
		return FALSE;
	if (wid->nitems / wid->ncols + (wid->nitems % wid->ncols != 0 ? 2 : 1) < wid->nrows)
		return FALSE;
	prevhand = gethandlepos(wid);
	newrow = prevrow = wid->row;
	wid->ydiff += y;
	newrow += wid->ydiff / wid->itemh;
	wid->ydiff %= wid->itemh;
	if (wid->ydiff < 0) {
		wid->ydiff += wid->itemh;
		newrow--;
	}
	if (y > 0) {
		if (newrow >= wid->nscreens) {
			wid->ydiff = wid->itemh;
			newrow = wid->nscreens - 1;
		}
	} else if (y < 0) {
		if (newrow < 0) {
			wid->ydiff = 0;
			newrow = 0;
		}
	}
	setrow(wid, newrow);
	newhand = gethandlepos(wid);
	if (wid->state == STATE_SCROLLING && prevhand != newhand) {
		drawscroller(wid, newhand);
	}
	if (prevrow != newrow) {
		settitle(wid);
		return TRUE;
	}
	return FALSE;
}

static int
readsize(FILE *fp)
{
	int size, c, n, i;

	size = 0;
	for (i = 0; i < 3; i++) {
		c = fgetc(fp);
		if (c == EOF)
			return -1;
		n = ATOI(c);
		if (n == -1)
			break;
		size *= 10;
		size += n;
	}
	if (i == 0 || (i == 3 && ATOI(c) != -1))
		return -1;
	return size;
}

static int
getitem(Widget wid, int row, int ydiff, int *x, int *y)
{
	int i, w, h;

	*y -= MARGIN;
	*y += ydiff;
	*x -= wid->x0;
	if (*x < 0 || *x >= wid->ncols * wid->itemw)
		return -1;
	if (*y < 0 || *y >= wid->h + ydiff)
		return -1;
	w = *x / wid->itemw;
	h = *y / wid->itemh;
	row *= wid->ncols;
	i = row + h * wid->ncols + w;
	if (i < row)
		return -1;
	*x -= w * wid->itemw;
	*y -= h * wid->itemh;
	return i;
}

static int
getpointerclick(Widget wid, int x, int y)
{
	int iconx, textx, texty, i;

	if ((i = getitem(wid, wid->row, wid->ydiff, &x, &y)) < 0)
		return -1;
	if (i < 0 || i >= wid->nitems)
		return -1;
	iconx = (wid->itemw - THUMBSIZE) / 2;
	if (x >= iconx && x < iconx + THUMBSIZE && y >= 0 && y < THUMBSIZE + wid->fonth / 2)
		return i;
	if (wid->linelen == NULL)
		return -1;
	textx = (wid->itemw - wid->linelen[i]) / 2;
	texty = wid->itemh - (NLINES + 0.5) * wid->fonth;
	if (x >= textx && x < textx + wid->linelen[i] &&
	    y >= texty && y < texty + wid->nlines[i] * wid->fonth) {
		return i;
	}
	return -1;
}

static void
cleanwidget(Widget wid)
{
	struct Thumb *thumb;
	struct Selection *sel;
	void *tmp;

	thumb = wid->thumbhead;
	while (thumb != NULL) {
		tmp = thumb;
		thumb = thumb->next;
		XDestroyImage(((struct Thumb *)tmp)->img);
		free(tmp);
	}
	sel = wid->sel;
	while (sel != NULL) {
		tmp = sel;
		sel = sel->next;
		free(tmp);
	}
	wid->sel = NULL;
	wid->rectsel = NULL;
	free(wid->thumbs);
	wid->thumbs = NULL;
	free(wid->linelen);
	wid->linelen = NULL;
	free(wid->nlines);
	wid->nlines = NULL;
	free(wid->issel);
	wid->issel = NULL;
}

static void
selectitem(Widget wid, int index, int select, int rectsel)
{
	struct Selection *sel;
	struct Selection **header;

	if (wid->issel == NULL || index <= 0 || index >= wid->nitems)
		return;
	/*
	 * We have two lists of selections: the global list (wid->sel),
	 * and the list used by rectangular selection (wid->rectsel).
	 */
	header = rectsel ? &wid->rectsel : &wid->sel;
	if (select && wid->issel[index] == NULL) {
		if ((sel = malloc(sizeof(*sel))) == NULL)
			return;
		*sel = (struct Selection){
			.next = *header,
			.prev = NULL,
			.index = (rectsel ? -1 : 1) * index,
		};
		if (*header != NULL)
			(*header)->prev = sel;
		*header = sel;
		wid->issel[index] = sel;
	} else if (!select && wid->issel[index] != NULL) {
		sel = wid->issel[index];
		if (sel->next != NULL)
			sel->next->prev = sel->prev;
		if (sel->prev != NULL)
			sel->prev->next = sel->next;
		if (*header == sel)
			*header = sel->next;
		free(sel);
		wid->issel[index] = NULL;
	} else {
		return;
	}
	drawitem(wid, index);
}

static void
highlight(Widget wid, int index, int redraw)
{
	int prevhili;

	if (wid->highlight == index)
		return;
	prevhili = wid->highlight;
	wid->highlight = index;
	if (redraw)
		drawitem(wid, index);
	/* we still have to redraw the previous one */
	drawitem(wid, prevhili);
	settitle(wid);
}

static void
selectitems(Widget wid, int a, int b)
{
	int i, min, max;

	if (a < 0 || b < 0 || a >= wid->nitems || b >= wid->nitems)
		return;
	if (a < b) {
		min = a;
		max = b;
	} else {
		min = b;
		max = a;
	}
	for (i = min; i <= max; i++) {
		selectitem(wid, i, TRUE, 0);
	}
}

static void
unselectitems(Widget wid)
{
	while (wid->sel) {
		selectitem(wid, wid->sel->index, FALSE, 0);
	}
}

static void
ownprimary(Widget wid, Time time)
{
	if (wid->sel == NULL)
		return;
	wid->seltime = time;
	XSetSelectionOwner(wid->dpy, XA_PRIMARY, wid->win, time);
}

static unsigned long
getatomsprop(Widget wid, Window win, Atom prop, Atom *type, Atom **atoms)
{
	unsigned char *p;
	unsigned long len;
	unsigned long dl;   /* dummy variable */
	int di;             /* dummy variable */
	int state;

	p = NULL;
	state = XGetWindowProperty(
		wid->dpy, win,
		prop, 0L, 0x1FFFFFFF,
		False,
		*type, type,
		&di, &len, &dl, &p
	);
	if (state != Success || len == 0 || p == NULL) {
		*atoms = NULL;
		XFree(p);
		return 0;
	}
	*atoms = (Atom *)p;
	return len;
}

static unsigned long
getatompairs(Widget wid, Window win, Atom prop, Atom **pairs)
{
	unsigned long len;
	Atom *p;
	Atom type;
	size_t size;

	p = NULL;
	*pairs = NULL;
	type = wid->atoms[ATOM_PAIR];
	if ((len = getatomsprop(wid, win, prop, &type, &p)) == 0)
		goto error;
	if (type != wid->atoms[ATOM_PAIR])
		goto error;
	size = len * sizeof(**pairs);
	if ((*pairs = malloc(size)) == NULL)
		goto error;
	memcpy(*pairs, p, size);
	XFree(p);
	return len;
error:
	XFree(p);
	return 0;
}

static Bool
convert(Widget wid, Window requestor, Atom target, Atom property)
{
	struct Selection *sel;

	if (target == wid->atoms[MULTIPLE]) {
		/*
		 * A MULTIPLE should be handled when processing a
		 * SelectionRequest event.  We do not support nested
		 * MULTIPLE targets.
		 */
		return False;
	}
	if (target == wid->atoms[TIMESTAMP]) {
		/*
		 * According to ICCCM, to avoid some race conditions, it
		 * is important that requestors be able to discover the
		 * timestamp the owner used to acquire ownership.
		 * Requester do that by requesting sellection owners to
		 * convert to `TIMESTAMP`.  Selections owners must
		 * return the timestamp as an `XA_INTEGER`.
		 */
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			XA_INTEGER, 32,
			PropModeReplace,
			(unsigned char *)&wid->seltime,
			1
		);
		return True;
	}
	if (target == wid->atoms[TARGETS]) {
		/*
		 * According to ICCCM, when requested for the `TARGETS`
		 * target, the selection owner should return a list of
		 * atoms representing the targets for which an attempt
		 * to convert the selection will (hopefully) succeed.
		 */
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			XA_ATOM, 32,
			PropModeReplace,
			(unsigned char *)(Atom[]){
				wid->atoms[XDND_URI_LIST],
				wid->atoms[TEXT_URI_LIST],
				wid->atoms[TEXT_PLAIN],
				wid->atoms[UTF8_STRING],
				wid->atoms[DELETE],
				wid->atoms[MULTIPLE],
				wid->atoms[TARGETS],
				wid->atoms[TEXT],
				wid->atoms[TIMESTAMP],
				XA_STRING,
			},
			10      /* the 10 atoms in the preceding array */
		);
		return True;
	}
	if (target == wid->atoms[DELETE]) {
		if (wid->sel != NULL)
			unselectitems(wid);
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			wid->atoms[_NULL],
			8L,
			PropModeReplace,
			(unsigned char *)"",
			0
		);
		return True;
	}
	if (target != wid->atoms[XDND_URI_LIST] &&
	    target != wid->atoms[TEXT_URI_LIST] &&
	    target != wid->atoms[TEXT_PLAIN] &&
	    target != wid->atoms[TEXT] &&
	    target != wid->atoms[UTF8_STRING] &&
	    target != wid->atoms[COMPOUND_TEXT] &&
	    target != XA_STRING) {
		return FALSE;
	}
	XChangeProperty(
		wid->dpy,
		requestor,
		property,
		target,
		8L,
		PropModeReplace,
		NULL,
		0
	);
	for (sel = wid->sel; sel != NULL; sel = sel->next) {
		if (target == wid->atoms[XDND_URI_LIST] ||
		    target == wid->atoms[TEXT_URI_LIST]) {
			XChangeProperty(
				wid->dpy,
				requestor,
				property,
				target,
				8L,
				PropModeAppend,
				(unsigned char *)"file://",
				7
			);
		}
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			target,
			8L,
			PropModeAppend,
			(unsigned char *)wid->items[sel->index][ITEM_PATH],
			strlen(wid->items[sel->index][ITEM_PATH])
		);
		if (target != wid->atoms[XDND_URI_LIST] &&
		    target != wid->atoms[TEXT_URI_LIST] &&
		    sel->next == NULL) {
			break;
		}
		if (target == wid->atoms[XDND_URI_LIST] ||
		    target == wid->atoms[TEXT_URI_LIST]) {
			XChangeProperty(
				wid->dpy,
				requestor,
				property,
				target,
				8L,
				PropModeAppend,
				(unsigned char *)"\r",
				1
			);
		}
		XChangeProperty(
			wid->dpy,
			requestor,
			property,
			target,
			8L,
			PropModeAppend,
			(unsigned char *)"\n",
			1
		);
	}
	XChangeProperty(
		wid->dpy,
		requestor,
		property,
		target,
		8L,
		PropModeAppend,
		(unsigned char[]){ '\0' },
		1
	);
	return True;
}

static void
sendselection(Widget wid, XSelectionRequestEvent *xev)
{
	enum { PAIR_TARGET, PAIR_PROPERTY, PAIR_LAST };
	;
	XSelectionEvent sev;
	Atom *pairs;
	Atom pair[PAIR_LAST];
	unsigned long natoms, i;
	Bool success;

	sev = (XSelectionEvent){
		.type           = SelectionNotify,
		.display        = xev->display,
		.requestor      = xev->requestor,
		.selection      = xev->selection,
		.time           = xev->time,
		.target         = xev->target,
		.property       = None,
	};
	if (xev->time != CurrentTime && xev->time < wid->seltime) {
		/*
		 * According to ICCCM, the selection owner should
		 * compare the timestamp with the period it has owned
		 * the selection and, if the time is outside, refuse the
		 * `SelectionRequest` by sending the requestor window a
		 * `SelectionNotify` event with the property set to
		 * `None` (by means of a `SendEvent` request with an
		 * empty event mask).
		 */
		goto done;
	}
	if (xev->target == wid->atoms[MULTIPLE]) {
		if (xev->property == None)
			goto done;
		natoms = getatompairs(wid, xev->requestor, xev->property, &pairs);
	} else {
		pair[PAIR_TARGET] = xev->target;
		pair[PAIR_PROPERTY] = xev->property;
		pairs = pair;
		natoms = 2;
	}
	success = True;
	for (i = 0; i < natoms; i += 2) {
		if (!convert(wid, xev->requestor, pairs[i + PAIR_TARGET], pairs[i + PAIR_PROPERTY])) {
			success = False;
			pairs[i + PAIR_PROPERTY] = None;
		}
	}
	if (xev->target == wid->atoms[MULTIPLE]) {
		XChangeProperty(
			xev->display,
			xev->requestor,
			xev->property,
			wid->atoms[ATOM_PAIR],
			32, PropModeReplace,
			(unsigned char *)pairs, natoms
		);
		free(pairs);
	}
	if (success) {
		sev.property = (xev->property == None) ? xev->target : xev->property;
	}
done:
	XSendEvent(xev->display, xev->requestor, False, NoEventMask, (XEvent *)&sev);
}

static int
mouse1click(Widget wid, XButtonPressedEvent *ev)
{
	int prevhili, index;

	index = getpointerclick(wid, ev->x, ev->y);
	if (index > 0 && wid->issel[index] != NULL)
		return index;
	if (!(ev->state & (ControlMask | ShiftMask)))
		unselectitems(wid);
	if (index < 0)
		return index;
	/*
	 * If index != 0, there's no need to ask highlight() to redraw the item,
	 * as selectitem() or selectitems() will already redraw it.
	 */
	prevhili = wid->highlight;
	highlight(wid, index, (index == 0));
	if (prevhili != -1 && ev->state & ShiftMask)
		selectitems(wid, wid->highlight, prevhili);
	else
		selectitem(wid, wid->highlight, ((ev->state & ControlMask) ? wid->issel[wid->highlight] == NULL : TRUE), FALSE);
	ownprimary(wid, ev->time);
	return index;
}

static void
mouse3click(Widget wid, int x, int y)
{
	int index;

	index = getpointerclick(wid, x, y);
	if (index != -1) {
		if (wid->issel[index] == NULL) {
			highlight(wid, index, FALSE);
			unselectitems(wid);
			selectitem(wid, index, TRUE, FALSE);
		} else {
			highlight(wid, index, TRUE);
		}
	}
}

static void
rectdraw(Widget wid, int row, int ydiff, int x0, int y0, int x, int y)
{
	int w, h;

	XSetForeground(wid->dpy, wid->stipgc, 0);
	XFillRectangle(
		wid->dpy,
		wid->rectbord,
		wid->stipgc,
		0, 0,
		wid->w, wid->h
	);
	if (wid->state != STATE_SELECTING)
		return;
	if (row < wid->row) {
		y0 -= min(wid->row - row, wid->nrows) * wid->itemh;
	} else if (row > wid->row) {
		y0 += min(row - wid->row, wid->nrows) * wid->itemh;
	}
	y0 += ydiff - wid->ydiff;
	w = (x0 > x) ? x0 - x : x - x0;
	h = (y0 > y) ? y0 - y : y - y0;
	XSetForeground(wid->dpy, wid->stipgc, 1);
	XDrawRectangle(
		wid->dpy,
		wid->rectbord,
		wid->stipgc,
		min(x0, x),
		min(y0, y),
		w, h
	);
}

static int
rectselect(Widget wid, int srcrow, int srcydiff, int srcx, int srcy, int dstx, int dsty)
{
	int row, col, tmp, i;
	int changed;
	int sel, x, y;

	/* x,y positions of the vertices of the rectangular selection */
	int minx, maxx, miny;

	/*
	 * Indices of visible items.
	 */
	int vismin, vismax;

	/*
	 * Indices of items at the top left of the rectangular selection
	 * and at bottom right of the rectangular selection.
	 */
	int indexmin, indexmax, srci, dsti;

	/*
	 * First and last columns and rows of the items at the
	 * rectangular selection.
	 */
	int colmin, colmax;
	int rowmin;
	int rowsrc;

	miny = min(srcy, dsty);
	minx = min(srcx, dstx);
	maxx = max(srcx, dstx);
	if ((dstx > srcx && srcy > dsty) || (dstx < srcx && srcy < dsty)) {
		tmp = dstx;
		dstx = srcx;
		srcx = tmp;
	}
	if (dstx < wid->x0)              dstx = wid->x0;
	if (srcx < wid->x0)              srcx = wid->x0;
	if (dstx >= wid->x0 + wid->pixw) dstx = wid->x0 + wid->pixw - 1;
	if (srcx >= wid->x0 + wid->pixw) srcx = wid->x0 + wid->pixw - 1;
	if (dsty < MARGIN)               dsty = MARGIN;
	if (srcy < MARGIN)               srcy = MARGIN;
	if (dsty >= wid->h)              dsty = wid->h - 1;
	if (srcy >= wid->h)              srcy = wid->h - 1;
	if ((srci = getitem(wid, srcrow, srcydiff, &srcx, &srcy)) < 0)
		return FALSE;
	if ((dsti = getitem(wid, wid->row, wid->ydiff, &dstx, &dsty)) < 0)
		return FALSE;
	vismin = firstvisible(wid);
	vismax = lastvisible(wid);
	indexmin = min(srci, dsti);
	indexmax = max(srci, dsti);
	rowmin = indexmin / wid->ncols;
	colmin = indexmin % wid->ncols;
	colmax = indexmax % wid->ncols;
	indexmin = min(indexmin, wid->nitems - 1);
	indexmax = min(indexmax, wid->nitems - 1);
	rowsrc = srci / wid->ncols;
	changed = FALSE;
	for (i = vismin; i <= vismax; i++) {
		sel = TRUE;
		row = i / wid->ncols;
		col = i % wid->ncols;
		x = wid->x0 + col * wid->itemw + (wid->itemw - THUMBSIZE) / 2;
		y = (row - wid->row + 1) * wid->itemh -
		    (NLINES - wid->nlines[i] + 0.5) * wid->fonth +
		    MARGIN - wid->ydiff;
		if (i < indexmin || i > indexmax) {
			sel = FALSE;
		} else if ((col == colmin || col == colmax) && (minx > x + THUMBSIZE || maxx < x)) {
			sel = FALSE;
		} else if (col < colmin || col > colmax) {
			sel = FALSE;
		} else if (row == rowmin && row != rowsrc && row >= wid->row && miny > y) {
			sel = FALSE;
		}
		if (!sel && (wid->issel[i] == NULL || wid->issel[i]->index > 0))
			continue;
		if (sel && wid->issel[i] != NULL && wid->issel[i]->index > 0)
			selectitem(wid, i, FALSE, FALSE);
		selectitem(wid, i, sel, TRUE);
		changed = TRUE;
	}
	return changed;
}

static void
commitrectsel(Widget wid)
{
	struct Selection *sel, *next;

	/*
	 * We keep the items selected by rectangular selection on a
	 * temporary list.  Move them to the regular list.
	 */
	while (wid->rectsel != NULL) {
		next = wid->rectsel->next;
		sel = wid->rectsel;
		sel->next = wid->sel;
		sel->prev = NULL;
		if (sel->index < 0)
			sel->index *= -1;
		if (wid->sel != NULL)
			wid->sel->prev = sel;
		wid->sel = sel;
		wid->rectsel = next;
	}
}

static void
endevent(Widget wid)
{
	if (wid->redraw) {
		commitdraw(wid);
	}
}

static Bool
translatetoroot(Widget wid, int *x, int *y)
{
	Window win;

	return XTranslateCoordinates(wid->dpy, wid->win, ROOT(wid->dpy), 0, 0, x, y, &win);
}

static int
querypointer(Widget wid, Window win, int *retx, int *rety, Window *retwin)
{
	Window root, child;
	unsigned int mask;
	int rootx, rooty;
	int x, y;
	int retval;

	retval = XQueryPointer(
		wid->dpy,
		win,
		&root, &child,
		&rootx, &rooty,
		&x, &y,
		&mask
	);
	if (retwin != NULL)
		*retwin = child;
	if (retx != NULL)
		*retx = x;
	if (rety != NULL)
		*rety = y;
	return retval;
}

static int
scrollerpos(Widget wid)
{
	int x, y;

	if (querypointer(wid, wid->scroller, &x, &y, NULL) == True) {
		if (y > SCROLLER_SIZE)
			return y - SCROLLER_SIZE;
		if (y < 0)
			return y;
		return 0;
	}
	return 0;
}

static void
scrollerset(Widget wid, int pos)
{
	int prevrow, newrow, maxpos;

	maxpos = HANDLE_MAX_SIZE - wid->handlew;
	if (maxpos <= 0) {
		/* all files are visible, there's nothing to scroll */
		return;
	}
	pos = max(pos, 0);
	pos = min(pos, maxpos);
	newrow = pos * wid->nscreens / maxpos;
	newrow = max(newrow, 0);
	newrow = min(newrow, wid->nscreens);
	if (newrow == wid->nscreens) {
		wid->ydiff = wid->itemh;
		newrow = wid->nscreens - 1;
	} else {
		wid->ydiff = 0;
	}
	prevrow = wid->row;
	setrow(wid, newrow);
	drawscroller(wid, pos);
	if (prevrow != newrow) {
		settitle(wid);
		drawitems(wid);
	}
}

static int
checkheader(FILE *fp, char *header, size_t size)
{
	char buf[PPM_BUFSIZE];

	if (fread(buf, 1, size, fp) != size)
		return RET_ERROR;
	if (memcmp(buf, header, size) != 0)
		return RET_ERROR;
	return RET_OK;
}

static int
pixmapfromdata(Widget wid, char **data, Pixmap *pix, Pixmap *mask)
{
	XpmAttributes xa = { 0 };

	if (XpmCreatePixmapFromData(wid->dpy, wid->win, data, pix, mask, &xa) != XpmSuccess) {
		*pix = None;
		*mask = None;
		return RET_ERROR;
	}
	return RET_OK;
}

static int
fillselitems(Widget wid, int *selitems, int clicked)
{
	struct Selection *sel;
	int nitems;

	nitems = 0;
	if (clicked != -1)
		selitems[nitems++] = clicked;
	for (sel = wid->sel; sel != NULL; sel = sel->next) {
		if (sel->index == clicked)
			continue;
		selitems[nitems++] = sel->index;
	}
	return nitems;
}

static Window
getdropwin(Widget wid, Atom *version)
{
	Atom type;
	Atom *p;
	Window win;

	*version = None;
	win = ROOT(wid->dpy);
	type = XA_ATOM;
	while (querypointer(wid, win, NULL, NULL, &win)) {
		if (win == None)
			break;
		if (getatomsprop(wid, win, wid->atoms[XDND_AWARE], &type, &p) > 0) {
			*version = *p;
			XFree(p);
			return win;
		}
	}
	return None;
}

static void
clientmsg(Display *dpy, Window win, Atom atom, long d[5])
{
	XEvent ev;

	ev.xclient.type = ClientMessage;
	ev.xclient.display = dpy;
	ev.xclient.serial = 0;
	ev.xclient.send_event = True;
	ev.xclient.message_type = atom;
	ev.xclient.window = win;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d[0];
	ev.xclient.data.l[1] = d[1];
	ev.xclient.data.l[2] = d[2];
	ev.xclient.data.l[3] = d[3];
	ev.xclient.data.l[4] = d[4];
	if (!XSendEvent(dpy, win, False, 0x0, &ev)) {
		warnx("could not send event");
	}
}

static Window
createdragwin(Widget wid, int index)
{
	Window win;
	GC gc;
	unsigned long opacity;
	int icon, pix, mask;
	int xroot, yroot, w, h;

	if (index <= 0)
		return None;
	if (!querypointer(wid, ROOT(wid->dpy), &xroot, &yroot, NULL))
		return None;
	w = h = THUMBSIZE;
	xroot += DND_DISTANCE;
	yroot += DND_DISTANCE;
	if (wid->thumbs[index] != NULL) {
		w = wid->thumbs[index]->w;
		h = wid->thumbs[index]->h;
	}
	win = XCreateWindow(
		wid->dpy, ROOT(wid->dpy),
		xroot, yroot, w, h, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWOverrideRedirect| CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.border_pixel = 0,
			.colormap = COLORMAP(wid->dpy),
			.background_pixel = wid->colors[SELECT_NOT][COLOR_BG].pixel,
			.override_redirect = True
		}
	);
	if (win == None)
		return None;
	opacity = DND_OPACITY;
	XChangeProperty(
		wid->dpy, win,
		wid->atoms[_NET_WM_WINDOW_OPACITY],
		XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&opacity,
		1
	);
	XChangeProperty(
		wid->dpy, win,
		wid->atoms[_NET_WM_WINDOW_TYPE],
		XA_ATOM, 32, PropModeReplace,
		(unsigned char *)&wid->atoms[_NET_WM_WINDOW_TYPE_DND],
		1
	);
	if (wid->thumbs[index] == NULL) {
		icon = wid->itemicons[index];
		pix = wid->icons[icon].pix;
		if ((mask = XCreatePixmap(wid->dpy, win, w, h, CLIP_DEPTH)) == None) {
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		if ((gc = XCreateGC(wid->dpy, mask, 0, NULL)) == None) {
			XFreePixmap(wid->dpy, mask);
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		XSetForeground(wid->dpy, gc, 0);
		XFillRectangle(wid->dpy, mask, gc, 0, 0, w, h);
		XCopyArea(wid->dpy, wid->icons[icon].mask, mask, gc, 0, 0, w, h, 0, 0);
		XShapeCombineMask(wid->dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
		XFreePixmap(wid->dpy, mask);
		XFreeGC(wid->dpy, gc);
		XSetWindowBackgroundPixmap(wid->dpy, win, pix);
	} else {
		if ((pix = XCreatePixmap(wid->dpy, win, w, h, DEPTH(wid->dpy))) == None) {
			XDestroyWindow(wid->dpy, win);
			return None;
		}
		XPutImage(
			wid->dpy,
			pix,
			wid->gc,
			wid->thumbs[index]->img,
			0, 0, 0, 0, w, h
		);
		XSetWindowBackgroundPixmap(wid->dpy, win, pix);
		XFreePixmap(wid->dpy, pix);
	}
	XMapRaised(wid->dpy, win);
	XFlush(wid->dpy);
	return win;
}

static void
finishdrop(Widget wid)
{
	long d[NCLIENTMSG_DATA];

	d[0] = wid->win;
	d[1] = d[2] = d[3] = d[4] = 0;
	if (wid->dropsrc == None)
		return;
	clientmsg(wid->dpy, wid->dropsrc, wid->atoms[XDND_FINISHED], d);
	wid->dropaction = None;
	wid->dropsrc = None;
}

static void
tryconvertsel(Widget wid, XSelectionEvent *xev)
{
	Atom sel;

	if (xev->target == wid->atoms[TEXT_URI_LIST])
		sel = wid->atoms[UTF8_STRING];
	else if (xev->target == wid->atoms[UTF8_STRING])
		sel = wid->atoms[TEXT_PLAIN];
	else if (xev->target == wid->atoms[TEXT_PLAIN])
		sel = wid->atoms[TEXT];
	else if (xev->target == wid->atoms[TEXT])
		sel = wid->atoms[XA_STRING];
	else
		return;         /* give up */
	XConvertSelection(
		wid->dpy,
		wid->atoms[XDND_SELECTION],
		sel, sel,
		wid->win,
		xev->time
	);
}

static char *
getdroptext(Widget wid, Atom prop)
{
	char *text;
	unsigned char *p;
	unsigned long len;
	unsigned long dl;               /* dummy variable */
	int format, status;
	Atom type;

	text = NULL;
	status = XGetWindowProperty(
		wid->dpy,
		wid->win,
		prop,
		0L,
		0x1FFFFFFF,
		True,
		AnyPropertyType,
		&type, &format,
		&len, &dl,
		&p
	);
	if (status != Success || len == 0 || p == NULL) {
		goto done;
	}
	if (type == wid->atoms[INCR]) {
		/* we do not do incremental transfer */
		goto done;
	}
	if ((text = emalloc(len + 1)) == NULL) {
		goto done;
	}
	memcpy(text, p, len);
	text[len] = '\0';
done:
	XFree(p);
	finishdrop(wid);
	return text;
}

/*
 * The following routines check an event, and process then if needed.
 * They return WIDGET_NONE if the event is not processed.
 */

static WidgetEvent
keypress(Widget wid, XKeyEvent *xev, int *selitems, int *nitems)
{
	KeySym ksym;
	unsigned int state;
	int redrawall, previtem, index, row[2], newrow, n, i;

	if (!XkbLookupKeySym(wid->dpy, xev->keycode, xev->state, &state, &ksym))
		return WIDGET_NONE;
	switch (ksym) {
	case XK_KP_Enter:       ksym = XK_Return;       break;
	case XK_KP_Space:       ksym = XK_space;        break;
	case XK_KP_Home:        ksym = XK_Home;         break;
	case XK_KP_End:         ksym = XK_End;          break;
	case XK_KP_Left:        ksym = XK_Left;         break;
	case XK_KP_Right:       ksym = XK_Right;        break;
	case XK_KP_Up:          ksym = XK_Up;           break;
	case XK_KP_Down:        ksym = XK_Down;         break;
	case XK_KP_Prior:       ksym = XK_Prior;        break;
	case XK_KP_Next:        ksym = XK_Next;         break;
	default:                                        break;
	}
	switch (ksym) {
	case XK_Escape:
		if (wid->sel == NULL)
			break;
		unselectitems(wid);
		break;
	case XK_Return:
		if (wid->highlight == -1)
			break;
		*nitems = fillselitems(wid, selitems, wid->highlight);
		return WIDGET_OPEN;
	case XK_Menu:
		*nitems = fillselitems(wid, selitems, -1);
		return WIDGET_CONTEXT;
	case XK_space:
		if (wid->highlight == -1)
			break;
		selectitem(wid, wid->highlight, wid->issel[wid->highlight] == NULL, FALSE);
		break;
	case XK_Prior:
	case XK_Next:
		if (scroll(wid, (ksym == XK_Prior ? -1 : 1) * PAGE_STEP(wid)))
			drawitems(wid);
		break;
	case XK_Home:
	case XK_End:
	case XK_Up:
	case XK_Down:
	case XK_Left:
	case XK_Right:
		redrawall = TRUE;
		if (ksym == XK_Home) {
			index = 0;
			wid->ydiff = 0;
			setrow(wid, 0);
			goto draw;
		}
		if (ksym == XK_End) {
			index = wid->nitems - 1;
			wid->ydiff = 0;
			setrow(wid, wid->nscreens);
			goto draw;
		}
		if (wid->highlight == -1) {
			wid->highlight = 0;
			setrow(wid, 0);
		}
		if (ksym == XK_Up) {
			n = -wid->ncols;
		} else if (ksym == XK_Down) {
			n = wid->highlight < (wid->nitems / wid->ncols) * wid->ncols
			  ? wid->nitems - wid->highlight - 1
			  : 0;
			n = min(wid->ncols, n);
		} else if (ksym == XK_Left) {
			n = -1;
		} else {
			n = 1;
		}
		if ((index = wid->highlight + n) < 0 || index >= wid->nitems)
			break;
		row[0] = wid->highlight / wid->ncols;
		row[1] = index / wid->ncols;
		newrow = wid->row;
		for (i = 0; i < 2; i++) {
			/*
			 * Try to make both previously highlighted item
			 * and new highlighted item visible.
			 */
			if (row[i] < newrow) {
				newrow = row[i];
			} else if (row[i] >= newrow + wid->h / wid->itemh) {
				newrow = row[i] - wid->h / wid->itemh + 1;
			}
		}
		if (wid->row != newrow) {
			wid->ydiff = 0;
			setrow(wid, newrow);
			redrawall = TRUE;
		} else if (wid->row == index / wid->ncols) {
			wid->ydiff = 0;
			wid->redraw = TRUE;
		}
draw:
		previtem = wid->highlight;
		highlight(wid, index, TRUE);
		if (xev->state & ShiftMask)
			selectitems(wid, index, previtem);
		else if (xev->state & ControlMask)
			selectitem(wid, index, TRUE, 0);
		if (redrawall)
			drawitems(wid);
		break;
	case XK_F5:
		return WIDGET_REFRESH;
	case XK_BackSpace:
		return WIDGET_PARENT;
	case XK_period:
		if (xev->state & ControlMask)
			return WIDGET_TOGGLE_HIDE;
		break;
	default:
		break;
	}
	return WIDGET_NONE;
}

static WidgetEvent
processevent(Widget wid, XEvent *ev)
{
	long d[NCLIENTMSG_DATA];
	int x, y;

	wid->redraw = FALSE;
	switch (ev->type) {
	case ClientMessage:
		if (ev->xclient.message_type == wid->atoms[WM_PROTOCOLS] &&
		    (Atom)ev->xclient.data.l[0] == wid->atoms[WM_DELETE_WINDOW]) {
			return WIDGET_CLOSE;
		} else if (ev->xclient.message_type == wid->atoms[XDND_ENTER]) {
			wid->dropsrc = (Window)ev->xclient.data.l[0];
			wid->dropaction = None;
		} else if (ev->xclient.message_type == wid->atoms[XDND_LEAVE] &&
		           (Window)ev->xclient.data.l[0] != None &&
		           (Window)ev->xclient.data.l[0] == wid->dropsrc) {
			wid->dropaction = None;
			wid->dropsrc = None;
		} else if (ev->xclient.message_type == wid->atoms[XDND_DROP] &&
		           (Window)ev->xclient.data.l[0] != None &&
		           (Window)ev->xclient.data.l[0] == wid->dropsrc) {
			XConvertSelection(
				wid->dpy,
				wid->atoms[XDND_SELECTION],
				wid->atoms[TEXT_URI_LIST],
				wid->atoms[TEXT_URI_LIST],
				wid->win,
				(Time)ev->xclient.data.l[2]
			);
		} else if (ev->xclient.message_type == wid->atoms[XDND_POSITION] &&
		           (Window)ev->xclient.data.l[0] != None &&
		           (Window)ev->xclient.data.l[0] == wid->dropsrc) {
			d[0] = wid->win;
			d[1] = 0x1;
			if (translatetoroot(wid, &x, &y)) {
				d[2] = (x << 16) | (y & 0xFFFF);
				d[3] = (wid->w << 16) | (wid->h & 0xFFFF);
			} else {
				d[2] = d[3] = 0;
			}
			if ((Atom)ev->xclient.data.l[4] == wid->atoms[XDND_ACTION_COPY] ||
			    (Atom)ev->xclient.data.l[4] == wid->atoms[XDND_ACTION_MOVE] ||
			    (Atom)ev->xclient.data.l[4] == wid->atoms[XDND_ACTION_LINK] ||
			    (Atom)ev->xclient.data.l[4] == wid->atoms[XDND_ACTION_ASK]) {
				wid->dropaction = (Atom)ev->xclient.data.l[4];
			} else {
				wid->dropaction = wid->atoms[XDND_ACTION_ASK];
			}
			d[4] = wid->dropaction;
			clientmsg(wid->dpy, (Window)ev->xclient.data.l[0], wid->atoms[XDND_STATUS], d);
		} else {
			return WIDGET_NONE;
		}
		break;
	case Expose:
		if (ev->xexpose.count == 0)
			commitdraw(wid);
		break;
	case ConfigureNotify:
		if (calcsize(wid, ev->xconfigure.width, ev->xconfigure.height)) {
			if (wid->row >= wid->nscreens)
				setrow(wid, wid->nscreens - 1);
			drawitems(wid);
		}
		break;
	case SelectionRequest:
		if (ev->xselectionrequest.selection == XA_PRIMARY ||
		    ev->xselectionrequest.selection == wid->atoms[XDND_SELECTION])
			sendselection(wid, &ev->xselectionrequest);
		break;
	case SelectionClear:
		if (wid->sel == NULL)
			break;
		unselectitems(wid);
		break;
	default:
		return WIDGET_NONE;
	}
	endevent(wid);
	return WIDGET_INTERNAL;
}

/*
 * The following are the event loops we use.
 *
 * mainmode() is the main event loop; while the others are modes that
 * the widget can get in before returning to main mode.
 *
 * All event loops call processevent to handle events that are common
 * to all modes.
 */

static WidgetEvent
scrollmode(Widget wid, int x, int y)
{
	XSyncAlarm alarm;
	XEvent ev;
	int grabpos, pos, left;

	wid->state = STATE_SCROLLING;
	grabpos = wid->handlew / 2;             /* we grab the handle in its middle */
	drawscroller(wid, gethandlepos(wid));
	XMoveWindow(wid->dpy, wid->scroller, x - SCROLLER_SIZE / 2 - 1, y - SCROLLER_SIZE / 2 - 1);
	XMapRaised(wid->dpy, wid->scroller);
	alarm = XSyncCreateAlarm(wid->dpy, ALARMFLAGS, &wid->syncattr);
	left = FALSE;
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			XSyncDestroyAlarm(wid->dpy, alarm);
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if (ev.type == wid->syncevent + XSyncAlarmNotify) {
			if ((pos = scrollerpos(wid)) != 0) {
				if (scroll(wid, pos))
					drawitems(wid);
				commitdraw(wid);
			}
			XSyncChangeAlarm(wid->dpy, alarm, ALARMFLAGS, &wid->syncattr);
			continue;
		}
		switch (ev.type) {
		case MotionNotify:
			if (ev.xmotion.window == wid->scroller && (ev.xmotion.state & Button1Mask)) {
				scrollerset(wid, ev.xmotion.y - grabpos);
			} else if (ev.xmotion.window == wid->win &&
			    (diff(ev.xmotion.x, x) > SCROLLER_SIZE / 2 || diff(ev.xmotion.y, y) > SCROLLER_SIZE / 2)) {
				left = TRUE;
			}
			break;
		case ButtonRelease:
			if (left)
				goto done;
			break;
		case ButtonPress:
			if (ev.xbutton.button != Button1)
				goto done;
			if (ev.xbutton.window == wid->win)
				goto done;
			if (ev.xbutton.window == wid->scroller) {
				left = TRUE;
				pos = gethandlepos(wid);
				if (ev.xmotion.y < pos || ev.xmotion.y > pos + wid->handlew) {
					/* grab handle in the middle */
					grabpos = wid->handlew / 2;
					scrollerset(wid, ev.xmotion.y - grabpos);
				} else {
					/* grab handle in position under pointer */
					grabpos = ev.xmotion.y - pos;
				}
			}
			break;
		}
		endevent(wid);
	}
done:
	wid->state = STATE_NORMAL;
	XSyncDestroyAlarm(wid->dpy, alarm);
	XUnmapWindow(wid->dpy, wid->scroller);
	return WIDGET_NONE;
}

static WidgetEvent
selmode(Widget wid, Time lasttime, int shift, int clickx, int clicky)
{
	XEvent ev;
	XSyncAlarm alarm;
	int rectrow, rectydiff, ownsel, x, y;

	wid->state = STATE_SELECTING;
	rectrow = wid->row;
	rectydiff = wid->ydiff;
	alarm = XSyncCreateAlarm(wid->dpy, ALARMFLAGS, &wid->syncattr);
	ownsel = FALSE;
	if (!shift)
		unselectitems(wid);
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			XSyncDestroyAlarm(wid->dpy, alarm);
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		if (ev.type == wid->syncevent + XSyncAlarmNotify) {
			if (querypointer(wid, wid->win, &x, &y, NULL)) {
				if (y > wid->h)
					y -= wid->h;
				else if (y > 0)
					y = 0;
				if (scroll(wid, y)) {
					drawitems(wid);
				}
				if (y != 0) {
					commitdraw(wid);
				}
			}
			XSyncChangeAlarm(wid->dpy, alarm, ALARMFLAGS, &wid->syncattr);
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			if (ownsel)
				ownprimary(wid, ev.xbutton.time);
			rectdraw(wid, rectrow, rectydiff, clickx, clicky, ev.xbutton.x, ev.xbutton.y);
			goto done;
		case MotionNotify:
			if (ev.xmotion.time - lasttime < MOTION_TIME)
				break;
			rectdraw(wid, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y);
			if (rectselect(wid, rectrow, rectydiff, clickx, clicky, ev.xmotion.x, ev.xmotion.y))
				ownsel = TRUE;
			commitdraw(wid);
			lasttime = ev.xmotion.time;
			break;
		}
		endevent(wid);
	}
done:
	commitrectsel(wid);
	wid->state = STATE_NORMAL;
	commitdraw(wid);
	XSyncDestroyAlarm(wid->dpy, alarm);
	return WIDGET_NONE;
}

static WidgetEvent
dragmode(Widget wid, Time lasttime, int clicki, int *selitems, int *nitems)
{
	XEvent ev;
	Window lastwin, win, dragwin;
	Atom version;
	WidgetEvent retval;
	int sendposition;
	int accept, inside, x, y, w, h;
	long d[NCLIENTMSG_DATA];

	retval = WIDGET_NONE;
	lastwin = None;
	dragwin = None;
	wid->state = STATE_SELECTING;
	widgetcursor(wid, CURSOR_DRAG);
	d[0] = wid->win;
	accept = TRUE;
	sendposition = TRUE;
	x = y = w = h = 0;
	wid->seltime = lasttime;
	XSetSelectionOwner(wid->dpy, wid->atoms[XDND_SELECTION], wid->win, lasttime);
	dragwin = createdragwin(wid, clicki);
	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		switch (ev.type) {
		case ButtonPress:
		case ButtonRelease:
			if (lastwin == wid->win) {
				clicki = getpointerclick(wid, ev.xbutton.x, ev.xbutton.y);
				if (clicki < 1)
					goto done;
				*nitems = fillselitems(wid, selitems, clicki);
				if (FLAG(ev.xbutton.state, ControlMask|ShiftMask))
					retval = WIDGET_DROPLINK;
				if (FLAG(ev.xbutton.state, ShiftMask))
					retval = WIDGET_DROPMOVE;
				if (FLAG(ev.xbutton.state, ControlMask))
					retval = WIDGET_DROPCOPY;
				else
					retval = WIDGET_DROPASK;
				finishdrop(wid);
			} else if (lastwin != None) {
				d[1] = d[2] = d[3] = d[4] = 0;
				d[2] = ev.xbutton.time;
				clientmsg(wid->dpy, lastwin, wid->atoms[XDND_DROP], d);
			}
			goto done;
		case ClientMessage:
			if (ev.xclient.message_type != wid->atoms[XDND_STATUS])
				break;
			if ((Window)ev.xclient.data.l[0] != lastwin)
				break;
			accept = (ev.xclient.data.l[1] & 0x1);
			sendposition = (ev.xclient.data.l[1] & 0x2);
			widgetcursor(wid, accept ? CURSOR_DRAG : CURSOR_NODROP);
			x = ev.xclient.data.l[2] >> 16;
			y = ev.xclient.data.l[2] & 0xFFFF;
			w = ev.xclient.data.l[3] >> 16;
			h = ev.xclient.data.l[3] & 0xFFFF;
			break;
		case MotionNotify:
			if (ev.xmotion.time - lasttime < MOTION_TIME)
				break;
			XMoveWindow(wid->dpy, dragwin, ev.xmotion.x_root + DND_DISTANCE, ev.xmotion.y_root + DND_DISTANCE);
			inside = between(ev.xmotion.x, ev.xmotion.y, x, y, w, h);
			if ((sendposition || !inside) && lastwin != None) {
				if (FLAG(ev.xmotion.state, ControlMask|ShiftMask))
					d[4] = wid->atoms[XDND_ACTION_LINK];
				if (FLAG(ev.xmotion.state, ShiftMask))
					d[4] = wid->atoms[XDND_ACTION_MOVE];
				if (FLAG(ev.xmotion.state, ControlMask))
					d[4] = wid->atoms[XDND_ACTION_COPY];
				else
					d[4] = wid->atoms[XDND_ACTION_ASK];
				d[1] = 0;
				d[2] = (ev.xmotion.x_root << 16) | (ev.xmotion.y_root & 0xFFFF);
				d[3] = ev.xmotion.time;
				clientmsg(wid->dpy, lastwin, wid->atoms[XDND_POSITION], d);
				sendposition = TRUE;
			}
			lasttime = ev.xmotion.time;
			if ((win = getdropwin(wid, &version)) == lastwin)
				break;
			sendposition = TRUE;
			x = y = w = h = 0;
			if (version > XDND_VERSION)
				version = XDND_VERSION;
			if (lastwin != None && lastwin != wid->win) {
				d[1] = d[2] = d[3] = d[4] = 0;
				clientmsg(wid->dpy, lastwin, wid->atoms[XDND_LEAVE], d);
			}
			if (win != None && win != wid->win) {
				d[1] = version << 24;
				d[2] = wid->atoms[TEXT_URI_LIST];
				d[3] = d[4] = None;
				clientmsg(wid->dpy, win, wid->atoms[XDND_ENTER], d);
			}
			if (win == None)
				widgetcursor(wid, CURSOR_NODROP);
			else
				widgetcursor(wid, CURSOR_DRAG);
			lastwin = win;
			break;
		}
	}
done:
	if (dragwin != None)
		XDestroyWindow(wid->dpy, dragwin);
	widgetcursor(wid, CURSOR_NORMAL);
	wid->state = STATE_NORMAL;
	return retval;
}

static WidgetEvent
mainmode(Widget wid, int *selitems, int *nitems, char **text)
{
	XEvent ev;
	Time lasttime = 0;
	int clickx = 0;
	int clicky = 0;
	int clicki = -1;
	int x, y;
	int state;

	while (!XNextEvent(wid->dpy, &ev)) {
		switch (processevent(wid, &ev)) {
		case WIDGET_CLOSE:
			return WIDGET_CLOSE;
		case WIDGET_NONE:
			break;
		default:
			continue;
		}
		switch (ev.type) {
		case KeyPress:
			state = keypress(wid, &ev.xkey, selitems, nitems);
			if (state != WIDGET_NONE)
				return state;
			break;
		case ButtonPress:
			clickx = ev.xbutton.x;
			clicky = ev.xbutton.y;
			if (ev.xbutton.window != wid->win)
				break;
			if (ev.xbutton.button == Button1) {
				clicki = mouse1click(wid, &ev.xbutton);
			} else if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
				if (scroll(wid, (ev.xbutton.button == Button4 ? -SCROLL_STEP : +SCROLL_STEP)))
					drawitems(wid);
				commitdraw(wid);
			} else if (ev.xbutton.button == Button2) {
				state = scrollmode(wid, ev.xmotion.x, ev.xmotion.y);
				if (state != WIDGET_NONE)
					return state;
			} else if (ev.xbutton.button == Button3) {
				mouse3click(wid, ev.xbutton.x, ev.xbutton.y);
				*nitems = fillselitems(wid, selitems, -1);
				commitdraw(wid);
				XUngrabPointer(wid->dpy, ev.xbutton.time);
				XFlush(wid->dpy);
				return WIDGET_CONTEXT;
			}
			break;
		case ButtonRelease:
			if (ev.xbutton.window != wid->win)
				break;
			if (ev.xbutton.button == BUTTON8)
				return WIDGET_PREV;
			if (ev.xbutton.button == BUTTON9)
				return WIDGET_NEXT;
			if (ev.xbutton.button != Button1)
				break;
			if (clicki < 0 ||
			    (ev.xbutton.state & (ControlMask | ShiftMask)) ||
			    ev.xbutton.time - lasttime > DOUBLECLICK) {
				lasttime = ev.xbutton.time;
				break;
			}
			*nitems = fillselitems(wid, selitems, wid->highlight);
			return WIDGET_OPEN;
		case MotionNotify:
			if (ev.xmotion.window != wid->win)
				break;
			if (ev.xmotion.state != Button1Mask &&
			    ev.xmotion.state != (Button1Mask|ShiftMask) &&
			    ev.xmotion.state != (Button1Mask|ControlMask))
				break;
			if (diff(ev.xmotion.x, clickx) * diff(ev.xmotion.y, clicky) < DRAG_SQUARE)
				break;
			if (clicki == -1) {
				state = selmode(wid, ev.xmotion.time, ev.xmotion.state & (ShiftMask | ControlMask), clickx, clicky);
			} else if (clicki > 0) {
				state = dragmode(wid, ev.xmotion.time, clicki, selitems, nitems);
			}
			if (state != WIDGET_NONE)
				return state;
			break;
		case SelectionNotify:
			if (wid->dropsrc == None)
				break;
			if (ev.xselection.property == None) {
				tryconvertsel(wid, &ev.xselection);
				break;
			}
			if ((*text = getdroptext(wid, ev.xselection.property)) != NULL) {
				querypointer(wid, wid->win, &x, &y, NULL);
				*nitems = 1;
				selitems[0] = getpointerclick(wid, x, y);
				if (wid->dropaction == wid->atoms[XDND_ACTION_COPY])
					return WIDGET_DROPCOPY;
				if (wid->dropaction == wid->atoms[XDND_ACTION_MOVE])
					return WIDGET_DROPMOVE;
				if (wid->dropaction == wid->atoms[XDND_ACTION_LINK])
					return WIDGET_DROPLINK;
				return WIDGET_DROPASK;
			}
			break;
		}
		endevent(wid);
	}
	return WIDGET_CLOSE;
}

/*
 * Check widget.h for description on the interface of the following
 * public functions.  Some of them rely on the existence of objects
 * in the given addresses, during Widget's lifetime.
 */

Widget
initwidget(const char *class, const char *name, const char *geom, int argc, char *argv[])
{
	XSyncSystemCounter *counters;
	Widget wid;
	int ncounters, tmp, i;
	char *progname, *s;

	wid = NULL;
	progname = "";
	if (argc > 0 && argv[0] != NULL) {
		progname = argv[0];
		if ((s = strrchr(argv[0], '/')) != NULL) {
			progname = s + 1;
		}
	}
	if (name == NULL)
		name = progname;
	if ((wid = malloc(sizeof(*wid))) == NULL) {
		warn("malloc");
		return NULL;
	}
	*wid = (struct Widget){
		.progname = progname,
		.dpy = NULL,
		.start = FALSE,
		.redraw = FALSE,
		.win = None,
		.scroller = None,
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.thumbs = NULL,
		.thumbhead = NULL,
		.linelen = NULL,
		.nlines = NULL,
		.icons = NULL,
		.highlight = -1,
		.dropaction = None,
		.dropsrc = None,
		.title = "",
		.class = class,
		.sel = NULL,
		.rectsel = NULL,
		.issel = NULL,
		.seltime = 0,
		.pix = None,
		.rectbord = None,
		.state = STATE_NORMAL,
		.stipple = None,
		.syncattr = (XSyncAlarmAttributes){
			.trigger.counter        = None,
			.trigger.value_type     = XSyncRelative,
			.trigger.test_type      = XSyncPositiveComparison,
		},
	};
	if (!XInitThreads()) {
		warnx("XInitThreads");
		goto error;
	}
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale()) {
		warnx("could not set locale");
		goto error;
	}
	if ((wid->dpy = XOpenDisplay(NULL)) == NULL) {
		warnx("could not connect to X server");
		goto error;
	}
	if (!XSyncQueryExtension(wid->dpy, &wid->syncevent, &tmp)) {
		warnx("could not query XSync extension");
		goto error;
	}
	if (!XSyncInitialize(wid->dpy, &tmp, &tmp)) {
		warnx("could not initialize XSync extension");
		goto error;
	}
	XInternAtoms(wid->dpy, atomnames, ATOM_LAST, False, wid->atoms);
	if (fcntl(XConnectionNumber(wid->dpy), F_SETFD, FD_CLOEXEC) == -1) {
		warn("fcntl");
		goto error;
	}
	if (inittheme(wid, class, name) == -1) {
		warnx("could not set theme");
		goto error;
	}
	if (createwin(wid, class, name, geom, argc, argv, winicon_data, winicon_size) == -1) {
		warnx("could not create window");
		goto error;
	}
	if ((wid->gc = XCreateGC(wid->dpy, wid->win, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	wid->scroller = XCreateWindow(
		wid->dpy, wid->win,
		0, 0, SCROLLER_SIZE, SCROLLER_SIZE, 1,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWBackPixel | CWBorderPixel | CWEventMask | CWColormap | CWBorderPixel,
		&(XSetWindowAttributes){
			.colormap = COLORMAP(wid->dpy),
			.background_pixel = wid->colors[SELECT_NOT][COLOR_BG].pixel,
			.border_pixel = wid->colors[SELECT_NOT][COLOR_FG].pixel,
			.event_mask = ButtonPressMask | PointerMotionMask,
		}
	);
	if (wid->scroller == None) {
		warnx("could not create window");
		goto error;
	}
	if ((wid->stipple = XCreatePixmap(wid->dpy, wid->win, STIPPLE_SIZE, STIPPLE_SIZE, CLIP_DEPTH)) == None) {
		warnx("could not create pixmap");
		goto error;
	}
	if ((wid->stipgc = XCreateGC(wid->dpy, wid->stipple, GCLineStyle, &(XGCValues){.line_style = LineOnOffDash})) == None) {
		warnx("could not create graphics context");
		goto error;
	}
	XSetForeground(wid->dpy, wid->stipgc, 0);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, STIPPLE_SIZE, STIPPLE_SIZE);
	XSetForeground(wid->dpy, wid->stipgc, 1);
	XFillRectangle(wid->dpy, wid->stipple, wid->stipgc, 0, 0, 1, 1);
	XSetStipple(wid->dpy, wid->gc, wid->stipple);
	wid->cursors[CURSOR_WATCH] = XCreateFontCursor(wid->dpy, XC_watch);
	wid->cursors[CURSOR_DRAG] = XcursorLibraryLoadCursor(wid->dpy, "dnd-none");
	wid->cursors[CURSOR_NODROP] = XcursorLibraryLoadCursor(wid->dpy, "forbidden");
	if ((counters = XSyncListSystemCounters(wid->dpy, &ncounters)) != NULL) {
		for (i = 0; i < ncounters; i++) {
			if (strcmp(counters[i].name, "SERVERTIME") == 0) {
				wid->syncattr.trigger.counter = counters[i].counter;
				break;
			}
		}
		XSyncFreeSystemCounterList(counters);
	}
	if (wid->syncattr.trigger.counter == None) {
		warnx("could not use XSync extension");
		goto error;
	}
	XSyncIntToValue(&wid->syncattr.trigger.wait_value, 128);
	XSyncIntToValue(&wid->syncattr.delta, 0);
	return wid;
error:
	if (wid->stipple != None)
		XFreePixmap(wid->dpy, wid->stipple);
	if (wid->scroller != None)
		XDestroyWindow(wid->dpy, wid->scroller);
	if (wid->win != None)
		XDestroyWindow(wid->dpy, wid->win);
	if (wid->dpy != NULL)
		XCloseDisplay(wid->dpy);
	free(wid);
	return NULL;
}

int
setwidget(Widget wid, const char *title, char **items[], int itemicons[], size_t nitems, Scroll *scrl)
{
	size_t i;

	cleanwidget(wid);
	wid->items = items;
	wid->nitems = nitems;
	wid->itemicons = itemicons;
	if (scrl == NULL) {
		wid->ydiff = 0;
		wid->row = 0;
	} else {
		wid->ydiff = scrl->ydiff;
		wid->row = scrl->row;
	}
	wid->title = title;
	wid->highlight = -1;
	(void)calcsize(wid, -1, -1);
	if (scrl != NULL && wid->row >= wid->nscreens) {
		wid->ydiff = 0;
		setrow(wid, wid->nscreens);
	}
	if ((wid->issel = calloc(wid->nitems, sizeof(*wid->issel))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->linelen = calloc(wid->nitems, sizeof(*wid->linelen))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->nlines = calloc(wid->nitems, sizeof(*wid->nlines))) == NULL) {
		warn("calloc");
		goto error;
	}
	if ((wid->thumbs = malloc(nitems * sizeof(*wid->thumbs))) == NULL) {
		warn("calloc");
		goto error;
	}
	for (i = 0; i < nitems; i++)
		wid->thumbs[i] = NULL;
	wid->thumbhead = NULL;
	settitle(wid);
	drawitems(wid);
	commitdraw(wid);
	return RET_OK;
error:
	free(wid->issel);
	free(wid->linelen);
	free(wid->nlines);
	free(wid->thumbs);
	wid->issel = NULL;
	wid->linelen = NULL;
	wid->nlines = NULL;
	wid->thumbs = NULL;
	return RET_ERROR;
}

void
mapwidget(Widget wid)
{
	XMapWindow(wid->dpy, wid->win);
}

WidgetEvent
pollwidget(Widget wid, int *selitems, int *nitems, Scroll *scrl, char **text)
{
	XEvent ev;
	int retval;

	*text = NULL;
	finishdrop(wid);
	while (wid->start && XPending(wid->dpy) > 0) {
		(void)XNextEvent(wid->dpy, &ev);
		if (processevent(wid, &ev) == WIDGET_CLOSE) {
			endevent(wid);
			return WIDGET_CLOSE;
		}
	}
	wid->start = TRUE;
	retval = mainmode(wid, selitems, nitems, text);
	endevent(wid);
	scrl->ydiff = wid->ydiff;
	scrl->row = wid->row;
	return retval;
}

void
closewidget(Widget wid)
{
	int i, j;

	finishdrop(wid);
	cleanwidget(wid);
	for (i = 0; i < wid->nicons; i++) {
		if (wid->icons[i].pix != None) {
			XFreePixmap(wid->dpy, wid->icons[i].pix);
		}
		if (wid->icons[i].mask != None) {
			XFreePixmap(wid->dpy, wid->icons[i].mask);
		}
	}
	free(wid->icons);
	XFreePixmap(wid->dpy, wid->pix);
	XFreePixmap(wid->dpy, wid->rectbord);
	XFreePixmap(wid->dpy, wid->namepix);
	XFreePixmap(wid->dpy, wid->stipple);
	for (i = 0; i < SELECT_LAST; i++)
		for (j = 0; j < COLOR_LAST; j++)
			XftColorFree(wid->dpy, VISUAL(wid->dpy), COLORMAP(wid->dpy), &wid->colors[i][j]);
	XftFontClose(wid->dpy, wid->font);
	XDestroyWindow(wid->dpy, wid->scroller);
	XDestroyWindow(wid->dpy, wid->win);
	XFreeGC(wid->dpy, wid->stipgc);
	XFreeGC(wid->dpy, wid->gc);
	XCloseDisplay(wid->dpy);
	free(wid);
}

int
widopenicons(Widget wid, char **xpms[], int nxpms)
{
	int retval, i;

	wid->nicons = nxpms;
	if ((wid->icons = calloc(wid->nicons, sizeof(*wid->icons))) == NULL) {
		warn("calloc");
		return RET_ERROR;
	}
	retval = RET_OK;
	for (i = 0; i < wid->nicons; i++) {
		if (pixmapfromdata(wid, xpms[i], &wid->icons[i].pix, &wid->icons[i].mask) == RET_ERROR) {
			warnx("could not open %d-th default icon pixmap", i);
			retval = RET_ERROR;
		}
	}
	return retval;
}

void
setthumbnail(Widget wid, char *path, int item)
{
	FILE *fp;
	size_t size, i;
	int w, h;
	char buf[DATA_DEPTH];
	unsigned char *data;

	if (item < 0 || item >= wid->nitems || wid->thumbs == NULL)
		return;
	data = NULL;
	wid->thumbs[item] = NULL;
	if ((fp = fopen(path, "rb")) == NULL) {
		warn("%s", path);
		goto error;
	}
	if (checkheader(fp, PPM_HEADER, PPM_HEADER_SIZE) == -1) {
		warnx("%s: not a ppm file", path);
		goto error;
	}
	w = readsize(fp);
	h = readsize(fp);
	if (w <= 0 || w > THUMBSIZE || h <= 0 || h > THUMBSIZE) {
		warnx("%s: ppm file with invalid header", path);
		goto error;
	}
	if (checkheader(fp, PPM_COLOR, PPM_COLOR_SIZE) == -1) {
		warnx("%s: ppm file with invalid header", path);
		goto error;
	}
	size = w * h;
	if ((data = malloc(size * DATA_DEPTH)) == NULL) {
		warn("malloc");
		goto error;
	}
	for (i = 0; i < size; i++) {
		if (fread(buf, 1, PPM_DEPTH, fp) != PPM_DEPTH) {
			warn("%s", path);
			goto error;
		}
		data[i * DATA_DEPTH + 0] = buf[2];   /* B */
		data[i * DATA_DEPTH + 1] = buf[1];   /* G */
		data[i * DATA_DEPTH + 2] = buf[0];   /* R */
		data[i * DATA_DEPTH + 3] = 0xFF;     /* A */
	}
	fclose(fp);
	fp = NULL;
	if ((wid->thumbs[item] = malloc(sizeof(*wid->thumbs[item]))) == NULL) {
		warn("malloc");
		goto error;
	}
	*wid->thumbs[item] = (struct Thumb){
		.w = w,
		.h = h,
		.img = NULL,
		.next = wid->thumbhead,
	};
	wid->thumbs[item]->img = XCreateImage(
		wid->dpy,
		VISUAL(wid->dpy),
		DEPTH(wid->dpy),
		ZPixmap,
		0, (char *)data,
		w, h,
		DATA_DEPTH * BYTE,
		0
	);
	if (wid->thumbs[item]->img == NULL) {
		warnx("%s: could not allocate XImage", path);
		goto error;
	}
	XInitImage(wid->thumbs[item]->img);
	wid->thumbhead = wid->thumbs[item];
	if (item >= wid->row * wid->ncols && item < wid->row * wid->ncols + wid->nrows * wid->ncols) {
		drawitem(wid, item);
		commitdraw(wid);
	}
	return;
error:
	if (fp != NULL)
		fclose(fp);
	free(data);
	free(wid->thumbs[item]);
	wid->thumbs[item] = NULL;
}

void
widgetcursor(Widget wid, int cursor)
{
	if (cursor == CURSOR_NORMAL || cursor < 0 || cursor >= CURSOR_LAST)
		XUndefineCursor(wid->dpy, wid->win);
	else
		XDefineCursor(wid->dpy, wid->win, wid->cursors[cursor]);
	XFlush(wid->dpy);
}

unsigned long
widgetwinid(Widget wid)
{
	return (unsigned long)wid->win;
}
