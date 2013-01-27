/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>

#include "config.h"
#include "core/bstr.h"
#include "core/options.h"
#include "core/mp_msg.h"
#include "core/mp_fifo.h"
#include "libavutil/common.h"
#include "x11_common.h"
#include "talloc.h"

#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "vo.h"
#include "aspect.h"
#include "osdep/timer.h"

#include <X11/Xmd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#ifdef CONFIG_XSS
#include <X11/extensions/scrnsaver.h>
#endif

#ifdef CONFIG_XDPMS
#include <X11/extensions/dpms.h>
#endif

#ifdef CONFIG_XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#ifdef CONFIG_XF86VM
#include <X11/extensions/xf86vmode.h>
#endif

#ifdef CONFIG_XF86XK
#include <X11/XF86keysym.h>
#endif

#include "core/input/input.h"
#include "core/input/keycodes.h"

#define vo_wm_LAYER 1
#define vo_wm_FULLSCREEN 2
#define vo_wm_STAYS_ON_TOP 4
#define vo_wm_ABOVE 8
#define vo_wm_BELOW 16
#define vo_wm_NETWM (vo_wm_FULLSCREEN | vo_wm_STAYS_ON_TOP | vo_wm_ABOVE | vo_wm_BELOW)

/* EWMH state actions, see
         http://freedesktop.org/Standards/wm-spec/index.html#id2768769 */
#define _NET_WM_STATE_REMOVE        0    /* remove/unset property */
#define _NET_WM_STATE_ADD           1    /* add/set property */
#define _NET_WM_STATE_TOGGLE        2    /* toggle property  */

#define WIN_LAYER_ONBOTTOM               2
#define WIN_LAYER_NORMAL                 4
#define WIN_LAYER_ONTOP                  6
#define WIN_LAYER_ABOVE_DOCK             10

// ----- Motif header: -------

#define MWM_HINTS_FUNCTIONS     (1L << 0)
#define MWM_HINTS_DECORATIONS   (1L << 1)
#define MWM_HINTS_INPUT_MODE    (1L << 2)
#define MWM_HINTS_STATUS        (1L << 3)

#define MWM_FUNC_ALL            (1L << 0)
#define MWM_FUNC_RESIZE         (1L << 1)
#define MWM_FUNC_MOVE           (1L << 2)
#define MWM_FUNC_MINIMIZE       (1L << 3)
#define MWM_FUNC_MAXIMIZE       (1L << 4)
#define MWM_FUNC_CLOSE          (1L << 5)

#define MWM_DECOR_ALL           (1L << 0)
#define MWM_DECOR_BORDER        (1L << 1)
#define MWM_DECOR_RESIZEH       (1L << 2)
#define MWM_DECOR_TITLE         (1L << 3)
#define MWM_DECOR_MENU          (1L << 4)
#define MWM_DECOR_MINIMIZE      (1L << 5)
#define MWM_DECOR_MAXIMIZE      (1L << 6)

#define MWM_INPUT_MODELESS 0
#define MWM_INPUT_PRIMARY_APPLICATION_MODAL 1
#define MWM_INPUT_SYSTEM_MODAL 2
#define MWM_INPUT_FULL_APPLICATION_MODAL 3
#define MWM_INPUT_APPLICATION_MODAL MWM_INPUT_PRIMARY_APPLICATION_MODAL

#define MWM_TEAROFF_WINDOW      (1L<<0)

typedef struct
{
    long flags;
    long functions;
    long decorations;
    long input_mode;
    long state;
} MotifWmHints;

static void vo_x11_update_geometry(struct vo *vo, bool update_pos);
static int vo_x11_get_fs_type(struct vo *vo);
static void saver_on(struct vo_x11_state *x11);
static void saver_off(struct vo_x11_state *x11);
static void vo_x11_selectinput_witherr(Display *display, Window w,
                                       long event_mask);
static void vo_x11_setlayer(struct vo *vo, Window vo_window, int layer);

/*
 * Sends the EWMH fullscreen state event.
 *
 * action: could be one of _NET_WM_STATE_REMOVE -- remove state
 *                         _NET_WM_STATE_ADD    -- add state
 *                         _NET_WM_STATE_TOGGLE -- toggle
 */
static void vo_x11_ewmh_fullscreen(struct vo_x11_state *x11, int action)
{
    assert(action == _NET_WM_STATE_REMOVE ||
           action == _NET_WM_STATE_ADD || action == _NET_WM_STATE_TOGGLE);

    if (x11->fs_type & vo_wm_FULLSCREEN)
    {
        XEvent xev;

        /* init X event structure for _NET_WM_FULLSCREEN client message */
        xev.xclient.type = ClientMessage;
        xev.xclient.serial = 0;
        xev.xclient.send_event = True;
        xev.xclient.message_type = x11->XA_NET_WM_STATE;
        xev.xclient.window = x11->window;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = action;
        xev.xclient.data.l[1] = x11->XA_NET_WM_STATE_FULLSCREEN;
        xev.xclient.data.l[2] = 0;
        xev.xclient.data.l[3] = 0;
        xev.xclient.data.l[4] = 0;

        /* finally send that damn thing */
        if (!XSendEvent(x11->display, DefaultRootWindow(x11->display), False,
                        SubstructureRedirectMask | SubstructureNotifyMask,
                        &xev))
        {
            mp_tmsg(MSGT_VO, MSGL_ERR, "\nX11: Couldn't send EWMH fullscreen event!\n");
        }
    }
}

static void vo_hidecursor(Display * disp, Window win)
{
    Cursor no_ptr;
    Pixmap bm_no;
    XColor black, dummy;
    Colormap colormap;
    const char bm_no_data[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    if (WinID == 0)
        return;                 // do not hide if playing on the root window

    colormap = DefaultColormap(disp, DefaultScreen(disp));
    if ( !XAllocNamedColor(disp, colormap, "black", &black, &dummy) )
    {
        return; // color alloc failed, give up
    }
    bm_no = XCreateBitmapFromData(disp, win, bm_no_data, 8, 8);
    no_ptr = XCreatePixmapCursor(disp, bm_no, bm_no, &black, &black, 0, 0);
    XDefineCursor(disp, win, no_ptr);
    XFreeCursor(disp, no_ptr);
    if (bm_no != None)
        XFreePixmap(disp, bm_no);
    XFreeColors(disp,colormap,&black.pixel,1,0);
}

static void vo_showcursor(Display * disp, Window win)
{
    if (WinID == 0)
        return;
    XDefineCursor(disp, win, 0);
}

static int x11_errorhandler(Display * display, XErrorEvent * event)
{
#define MSGLEN 60
    char msg[MSGLEN];

    XGetErrorText(display, event->error_code, (char *) &msg, MSGLEN);

    mp_msg(MSGT_VO, MSGL_ERR, "X11 error: %s\n", msg);

    mp_msg(MSGT_VO, MSGL_V,
           "Type: %x, display: %p, resourceid: %lx, serial: %lx\n",
           event->type, event->display, event->resourceid, event->serial);
    mp_msg(MSGT_VO, MSGL_V,
           "Error code: %x, request code: %x, minor code: %x\n",
           event->error_code, event->request_code, event->minor_code);

//    abort();
    return 0;
#undef MSGLEN
}

void fstype_help(void)
{
    mp_tmsg(MSGT_VO, MSGL_INFO, "Available fullscreen layer change modes:\n");
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_FULL_SCREEN_TYPES\n");

    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "none",
           "don't set fullscreen window layer");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "layer",
           "use _WIN_LAYER hint with default layer");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "layer=<0..15>",
           "use _WIN_LAYER hint with a given layer number");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "netwm",
           "force NETWM style");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "above",
           "use _NETWM_STATE_ABOVE hint if available");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "below",
           "use _NETWM_STATE_BELOW hint if available");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "fullscreen",
           "use _NETWM_STATE_FULLSCREEN hint if available");
    mp_msg(MSGT_VO, MSGL_INFO, "    %-15s %s\n", "stays_on_top",
           "use _NETWM_STATE_STAYS_ON_TOP hint if available");
    mp_msg(MSGT_VO, MSGL_INFO,
           "You can also negate the settings with simply putting '-' in the beginning");
    mp_msg(MSGT_VO, MSGL_INFO, "\n");
}

static void fstype_dump(int fstype)
{
    if (fstype)
    {
        mp_msg(MSGT_VO, MSGL_V, "[x11] Current fstype setting honours");
        if (fstype & vo_wm_LAYER)
            mp_msg(MSGT_VO, MSGL_V, " LAYER");
        if (fstype & vo_wm_FULLSCREEN)
            mp_msg(MSGT_VO, MSGL_V, " FULLSCREEN");
        if (fstype & vo_wm_STAYS_ON_TOP)
            mp_msg(MSGT_VO, MSGL_V, " STAYS_ON_TOP");
        if (fstype & vo_wm_ABOVE)
            mp_msg(MSGT_VO, MSGL_V, " ABOVE");
        if (fstype & vo_wm_BELOW)
            mp_msg(MSGT_VO, MSGL_V, " BELOW");
        mp_msg(MSGT_VO, MSGL_V, " X atoms\n");
    } else
        mp_msg(MSGT_VO, MSGL_V,
               "[x11] Current fstype setting doesn't honour any X atoms\n");
}

static int net_wm_support_state_test(struct vo_x11_state *x11, Atom atom)
{
#define NET_WM_STATE_TEST(x) { if (atom == x11->XA_NET_WM_STATE_##x) { mp_msg( MSGT_VO,MSGL_V, "[x11] Detected wm supports " #x " state.\n" ); return vo_wm_##x; } }

    NET_WM_STATE_TEST(FULLSCREEN);
    NET_WM_STATE_TEST(ABOVE);
    NET_WM_STATE_TEST(STAYS_ON_TOP);
    NET_WM_STATE_TEST(BELOW);
    return 0;
}

static int x11_get_property(struct vo_x11_state *x11, Atom type, Atom ** args,
                            unsigned long *nitems)
{
    int format;
    unsigned long bytesafter;

    return  Success ==
            XGetWindowProperty(x11->display, x11->rootwin, type, 0, 16384, False,
                               AnyPropertyType, &type, &format, nitems,
                               &bytesafter, (unsigned char **) args)
            && *nitems > 0;
}

static int vo_wm_detect(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    int i;
    int wm = 0;
    unsigned long nitems;
    Atom *args = NULL;

    if (WinID >= 0)
        return 0;

// -- supports layers
    if (x11_get_property(x11, x11->XA_WIN_PROTOCOLS, &args, &nitems))
    {
        mp_msg(MSGT_VO, MSGL_V, "[x11] Detected wm supports layers.\n");
        int metacity_hack = 0;
        for (i = 0; i < nitems; i++)
        {
            if (args[i] == x11->XA_WIN_LAYER) {
                wm |= vo_wm_LAYER;
                metacity_hack |= 1;
            } else {
                /* metacity is the only window manager I know which reports
                 * supporting only the _WIN_LAYER hint in _WIN_PROTOCOLS.
                 * (what's more support for it is broken) */
                metacity_hack |= 2;
            }
        }
        XFree(args);
        if (wm && (metacity_hack == 1))
        {
            // metacity claims to support layers, but it is not the truth :-)
            wm ^= vo_wm_LAYER;
            mp_msg(MSGT_VO, MSGL_V,
                   "[x11] Using workaround for Metacity bugs.\n");
        }
    }
// --- netwm
    if (x11_get_property(x11, x11->XA_NET_SUPPORTED, &args, &nitems))
    {
        mp_msg(MSGT_VO, MSGL_V, "[x11] Detected wm supports NetWM.\n");
        for (i = 0; i < nitems; i++)
            wm |= net_wm_support_state_test(vo->x11, args[i]);
        XFree(args);
    }

    if (wm == 0)
        mp_msg(MSGT_VO, MSGL_V, "[x11] Unknown wm type...\n");
    return wm;
}

#define XA_INIT(x) x11->XA##x = XInternAtom(x11->display, #x, False)
static void init_atoms(struct vo_x11_state *x11)
{
    XA_INIT(_NET_SUPPORTED);
    XA_INIT(_NET_WM_STATE);
    XA_INIT(_NET_WM_STATE_FULLSCREEN);
    XA_INIT(_NET_WM_STATE_ABOVE);
    XA_INIT(_NET_WM_STATE_STAYS_ON_TOP);
    XA_INIT(_NET_WM_STATE_BELOW);
    XA_INIT(_NET_WM_PID);
    XA_INIT(_NET_WM_NAME);
    XA_INIT(_NET_WM_ICON_NAME);
    XA_INIT(_WIN_PROTOCOLS);
    XA_INIT(_WIN_LAYER);
    XA_INIT(_WIN_HINTS);
    XA_INIT(WM_PROTOCOLS);
    XA_INIT(WM_DELETE_WINDOW);
    XA_INIT(UTF8_STRING);
    char buf[50];
    sprintf(buf, "_NET_WM_CM_S%d", x11->screen);
    x11->XA_NET_WM_CM = XInternAtom(x11->display, buf, False);
}

void vo_x11_update_screeninfo(struct vo *vo) {
    struct MPOpts *opts = vo->opts;
    xinerama_x = xinerama_y = 0;
#ifdef CONFIG_XINERAMA
    if (xinerama_screen >= -1 && XineramaIsActive(vo->x11->display))
    {
        int screen = xinerama_screen;
        XineramaScreenInfo *screens;
        int num_screens;

        screens = XineramaQueryScreens(vo->x11->display, &num_screens);
        if (screen >= num_screens)
            screen = num_screens - 1;
        if (screen == -1) {
            int x = vo->dx + vo->dwidth / 2;
            int y = vo->dy + vo->dheight / 2;
            for (screen = num_screens - 1; screen > 0; screen--) {
               int left = screens[screen].x_org;
               int right = left + screens[screen].width;
               int top = screens[screen].y_org;
               int bottom = top + screens[screen].height;
               if (left <= x && x <= right && top <= y && y <= bottom)
                   break;
            }
        }
        if (screen < 0)
            screen = 0;
        opts->vo_screenwidth = screens[screen].width;
        opts->vo_screenheight = screens[screen].height;
        xinerama_x = screens[screen].x_org;
        xinerama_y = screens[screen].y_org;

        XFree(screens);
    }
#endif
    aspect_save_screenres(vo, opts->vo_screenwidth, opts->vo_screenheight);
}

int vo_x11_init(struct vo *vo)
{
    struct MPOpts *opts = vo->opts;
    char *dispName;

    assert(!vo->x11);

    struct vo_x11_state *x11 = talloc_ptrtype(NULL, x11);
    *x11 = (struct vo_x11_state){
        .olddecor = MWM_DECOR_ALL,
        .oldfuncs = MWM_FUNC_MOVE | MWM_FUNC_CLOSE | MWM_FUNC_MINIMIZE |
                    MWM_FUNC_MAXIMIZE | MWM_FUNC_RESIZE,
        .old_gravity = NorthWestGravity,
        .fs_layer = WIN_LAYER_ABOVE_DOCK,
    };
    vo->x11 = x11;

    if (vo_rootwin)
        WinID = 0; // use root window

    XSetErrorHandler(x11_errorhandler);

    dispName = XDisplayName(NULL);

    mp_msg(MSGT_VO, MSGL_V, "X11 opening display: %s\n", dispName);

    x11->display = XOpenDisplay(dispName);
    if (!x11->display)
    {
        mp_msg(MSGT_VO, MSGL_ERR,
               "vo: couldn't open the X11 display (%s)!\n", dispName);
        talloc_free(x11);
        vo->x11 = NULL;
        return 0;
    }
    x11->screen = DefaultScreen(x11->display);  // screen ID
    x11->rootwin = RootWindow(x11->display, x11->screen);   // root window ID

    x11->xim = XOpenIM(x11->display, NULL, NULL, NULL);

    init_atoms(vo->x11);

#ifdef CONFIG_XF86VM
    {
        int clock;
        XF86VidModeModeLine modeline;

        XF86VidModeGetModeLine(x11->display, x11->screen, &clock, &modeline);
        if (!opts->vo_screenwidth)
            opts->vo_screenwidth = modeline.hdisplay;
        if (!opts->vo_screenheight)
            opts->vo_screenheight = modeline.vdisplay;
    }
#endif
    {
        if (!opts->vo_screenwidth)
            opts->vo_screenwidth = DisplayWidth(x11->display, x11->screen);
        if (!opts->vo_screenheight)
            opts->vo_screenheight = DisplayHeight(x11->display, x11->screen);
    }

// XCloseDisplay( mDisplay );
/* slightly improved local display detection AST */
    if (strncmp(dispName, "unix:", 5) == 0)
        dispName += 4;
    else if (strncmp(dispName, "localhost:", 10) == 0)
        dispName += 9;
    if (*dispName == ':' && atoi(dispName + 1) < 10)
        x11->display_is_local = 1;
    else
        x11->display_is_local = 0;
    mp_msg(MSGT_VO, MSGL_V, "vo: X11 running at %dx%d (\"%s\" => %s display)\n",
           opts->vo_screenwidth, opts->vo_screenheight, dispName,
           x11->display_is_local ? "local" : "remote");

    x11->wm_type = vo_wm_detect(vo);

    x11->fs_type = vo_x11_get_fs_type(vo);

    fstype_dump(x11->fs_type);

    if (opts->vo_stop_screensaver)
        saver_off(x11);

    return 1;
}

static const struct mp_keymap keymap[] = {
    // special keys
    {XK_Pause, KEY_PAUSE}, {XK_Escape, KEY_ESC}, {XK_BackSpace, KEY_BS},
    {XK_Tab, KEY_TAB}, {XK_Return, KEY_ENTER},
    {XK_Menu, KEY_MENU}, {XK_Print, KEY_PRINT},

    // cursor keys
    {XK_Left, KEY_LEFT}, {XK_Right, KEY_RIGHT}, {XK_Up, KEY_UP}, {XK_Down, KEY_DOWN},

    // navigation block
    {XK_Insert, KEY_INSERT}, {XK_Delete, KEY_DELETE}, {XK_Home, KEY_HOME}, {XK_End, KEY_END},
    {XK_Page_Up, KEY_PAGE_UP}, {XK_Page_Down, KEY_PAGE_DOWN},

    // F-keys
    {XK_F1, KEY_F+1}, {XK_F2, KEY_F+2}, {XK_F3, KEY_F+3}, {XK_F4, KEY_F+4},
    {XK_F5, KEY_F+5}, {XK_F6, KEY_F+6}, {XK_F7, KEY_F+7}, {XK_F8, KEY_F+8},
    {XK_F9, KEY_F+9}, {XK_F10, KEY_F+10}, {XK_F11, KEY_F+11}, {XK_F12, KEY_F+12},

    // numpad independent of numlock
    {XK_KP_Subtract, '-'}, {XK_KP_Add, '+'}, {XK_KP_Multiply, '*'}, {XK_KP_Divide, '/'},
    {XK_KP_Enter, KEY_KPENTER},

    // numpad with numlock
    {XK_KP_0, KEY_KP0}, {XK_KP_1, KEY_KP1}, {XK_KP_2, KEY_KP2},
    {XK_KP_3, KEY_KP3}, {XK_KP_4, KEY_KP4}, {XK_KP_5, KEY_KP5},
    {XK_KP_6, KEY_KP6}, {XK_KP_7, KEY_KP7}, {XK_KP_8, KEY_KP8},
    {XK_KP_9, KEY_KP9}, {XK_KP_Decimal, KEY_KPDEC},
    {XK_KP_Separator, KEY_KPDEC},

    // numpad without numlock
    {XK_KP_Insert, KEY_KPINS}, {XK_KP_End, KEY_KP1}, {XK_KP_Down, KEY_KP2},
    {XK_KP_Page_Down, KEY_KP3}, {XK_KP_Left, KEY_KP4}, {XK_KP_Begin, KEY_KP5},
    {XK_KP_Right, KEY_KP6}, {XK_KP_Home, KEY_KP7}, {XK_KP_Up, KEY_KP8},
    {XK_KP_Page_Up, KEY_KP9}, {XK_KP_Delete, KEY_KPDEL},

#ifdef XF86XK_AudioPause
    {XF86XK_MenuKB, KEY_MENU},
    {XF86XK_AudioPlay, KEY_PLAY}, {XF86XK_AudioPause, KEY_PAUSE}, {XF86XK_AudioStop, KEY_STOP},
    {XF86XK_AudioPrev, KEY_PREV}, {XF86XK_AudioNext, KEY_NEXT},
    {XF86XK_AudioMute, KEY_MUTE}, {XF86XK_AudioLowerVolume, KEY_VOLUME_DOWN}, {XF86XK_AudioRaiseVolume, KEY_VOLUME_UP},
#endif

    {0, 0}
};

static int vo_x11_lookupkey(int key)
{
    static const char *passthrough_keys = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";
    int mpkey = 0;
    if ((key >= 'a' && key <= 'z') ||
        (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        mpkey = key;

    if (!mpkey)
        mpkey = lookup_keymap_table(keymap, key);

    return mpkey;
}

static void vo_x11_decoration(struct vo *vo, int d)
{
    struct vo_x11_state *x11 = vo->x11;
    Atom mtype;
    int mformat;
    unsigned long mn, mb;
    Atom vo_MotifHints;
    MotifWmHints vo_MotifWmHints;

    if (!WinID)
        return;

    if (vo_fsmode & 8)
    {
        XSetTransientForHint(x11->display, x11->window,
                             RootWindow(x11->display, x11->screen));
    }

    vo_MotifHints = XInternAtom(x11->display, "_MOTIF_WM_HINTS", 0);
    if (vo_MotifHints != None)
    {
        if (!d)
        {
            MotifWmHints *mhints = NULL;

            XGetWindowProperty(x11->display, x11->window,
                               vo_MotifHints, 0, 20, False,
                               vo_MotifHints, &mtype, &mformat, &mn,
                               &mb, (unsigned char **) &mhints);
            if (mhints)
            {
                if (mhints->flags & MWM_HINTS_DECORATIONS)
                    x11->olddecor = mhints->decorations;
                if (mhints->flags & MWM_HINTS_FUNCTIONS)
                    x11->oldfuncs = mhints->functions;
                XFree(mhints);
            }
        }

        memset(&vo_MotifWmHints, 0, sizeof(MotifWmHints));
        vo_MotifWmHints.flags =
            MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS;
        if (d)
        {
            vo_MotifWmHints.functions = x11->oldfuncs;
            d = x11->olddecor;
        }
#if 0
        vo_MotifWmHints.decorations =
            d | ((vo_fsmode & 2) ? 0 : MWM_DECOR_MENU);
#else
        vo_MotifWmHints.decorations =
            d | ((vo_fsmode & 2) ? MWM_DECOR_MENU : 0);
#endif
        XChangeProperty(x11->display, x11->window, vo_MotifHints,
                        vo_MotifHints, 32,
                        PropModeReplace,
                        (unsigned char *) &vo_MotifWmHints,
                        (vo_fsmode & 4) ? 4 : 5);
    }
}

static void vo_x11_classhint(struct vo *vo, Window window, const char *name)
{
    struct MPOpts *opts = vo->opts;
    struct vo_x11_state *x11 = vo->x11;
    XClassHint wmClass;
    pid_t pid = getpid();

    wmClass.res_name = opts->vo_winname ? opts->vo_winname : (char *)name;
    wmClass.res_class = "mpv";
    XSetClassHint(x11->display, window, &wmClass);
    XChangeProperty(x11->display, window, x11->XA_NET_WM_PID, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *) &pid, 1);
}

void vo_x11_uninit(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    assert(x11);

    saver_on(x11);
    if (x11->window != None)
        vo_showcursor(x11->display, x11->window);

    if (x11->f_gc != None)
        XFreeGC(vo->x11->display, x11->f_gc);
    if (x11->vo_gc != None)
        XFreeGC(vo->x11->display, x11->vo_gc);
    if (x11->window != None) {
        XClearWindow(x11->display, x11->window);
        if (WinID < 0) {
            XEvent xev;

            XUnmapWindow(x11->display, x11->window);
            XSelectInput(x11->display, x11->window, StructureNotifyMask);
            XDestroyWindow(x11->display, x11->window);
            do {
                XNextEvent(x11->display, &xev);
            } while (xev.type != DestroyNotify ||
                     xev.xdestroywindow.event != x11->window);
        }
    }
    if (x11->xic)
        XDestroyIC(x11->xic);
    vo_fs = 0;

    mp_msg(MSGT_VO, MSGL_V, "vo: uninit ...\n");
    if (x11->xim)
        XCloseIM(x11->xim);
    XSetErrorHandler(NULL);
    XCloseDisplay(x11->display);

    talloc_free(x11);
    vo->x11 = NULL;
}

static int check_resize(struct vo *vo)
{
    int old_w = vo->dwidth, old_h = vo->dheight;
    int old_x = vo->dx, old_y = vo->dy;
    int rc = 0;
    vo_x11_update_geometry(vo, true);
    if (vo->dwidth != old_w || vo->dheight != old_h)
        rc |= VO_EVENT_RESIZE;
    if (vo->dx     != old_x || vo->dy      != old_y)
        rc |= VO_EVENT_MOVE;
    return rc;
}

int vo_x11_check_events(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    struct MPOpts *opts = vo->opts;
    Display *display = vo->x11->display;
    int ret = 0;
    XEvent Event;

    if (x11->mouse_waiting_hide && opts->cursor_autohide_delay != -1 &&
        (GetTimerMS() - x11->mouse_timer >= opts->cursor_autohide_delay)) {
        vo_hidecursor(display, x11->window);
        x11->mouse_waiting_hide = 0;
    }

    if (WinID > 0)
        ret |= check_resize(vo);
    while (XPending(display))
    {
        XNextEvent(display, &Event);
//       printf("\rEvent.type=%X  \n",Event.type);
        switch (Event.type)
        {
            case Expose:
                ret |= VO_EVENT_EXPOSE;
                break;
            case ConfigureNotify:
                if (x11->window == None)
                    break;
                ret |= check_resize(vo);
                break;
            case KeyPress:
                {
                    char buf[100];
                    KeySym keySym = 0;
                    int modifiers = 0;
                    if (Event.xkey.state & ShiftMask)
                        modifiers |= KEY_MODIFIER_SHIFT;
                    if (Event.xkey.state & ControlMask)
                        modifiers |= KEY_MODIFIER_CTRL;
                    if (Event.xkey.state & Mod1Mask)
                        modifiers |= KEY_MODIFIER_ALT;
                    if (Event.xkey.state & Mod4Mask)
                        modifiers |= KEY_MODIFIER_META;
                    if (x11->xic) {
                        Status status;
                        int len = Xutf8LookupString(x11->xic, &Event.xkey, buf,
                                                    sizeof(buf), &keySym,
                                                    &status);
                        int mpkey = vo_x11_lookupkey(keySym);
                        if (mpkey) {
                            mplayer_put_key(vo->key_fifo, mpkey | modifiers);
                        } else if (status == XLookupChars
                                   || status == XLookupBoth)
                        {
                            struct bstr t = { buf, len };
                            mplayer_put_key_utf8(vo->key_fifo, modifiers, t);
                        }
                    } else {
                        XLookupString(&Event.xkey, buf, sizeof(buf), &keySym,
                                      &x11->compose_status);
                        int mpkey = vo_x11_lookupkey(keySym);
                        if (mpkey)
                            mplayer_put_key(vo->key_fifo, mpkey | modifiers);
                    }
                    ret |= VO_EVENT_KEYPRESS;
                }
                break;
            case MotionNotify:
                    vo_mouse_movement(vo, Event.xmotion.x, Event.xmotion.y);

                if (opts->cursor_autohide_delay > -2) {
                    vo_showcursor(display, x11->window);
                    x11->mouse_waiting_hide = 1;
                    x11->mouse_timer = GetTimerMS();
                }
                break;
            case ButtonPress:
                if (opts->cursor_autohide_delay > -2) {
                    vo_showcursor(display, x11->window);
                    x11->mouse_waiting_hide = 1;
                    x11->mouse_timer = GetTimerMS();
                }
                mplayer_put_key(vo->key_fifo,
                                (MOUSE_BTN0 + Event.xbutton.button - 1)
                                | MP_KEY_DOWN);
                break;
            case ButtonRelease:
                if (opts->cursor_autohide_delay > -2) {
                    vo_showcursor(display, x11->window);
                    x11->mouse_waiting_hide = 1;
                    x11->mouse_timer = GetTimerMS();
                }
                mplayer_put_key(vo->key_fifo,
                                MOUSE_BTN0 + Event.xbutton.button - 1);
                break;
            case PropertyNotify:
                {
                    char *name =
                        XGetAtomName(display, Event.xproperty.atom);

                    if (!name)
                        break;

//          fprintf(stderr,"[ws] PropertyNotify ( 0x%x ) %s ( 0x%x )\n",vo_window,name,Event.xproperty.atom );

                    XFree(name);
                }
                break;
            case MapNotify:
                x11->vo_hint.win_gravity = x11->old_gravity;
                XSetWMNormalHints(display, x11->window, &x11->vo_hint);
                x11->fs_flip = 0;
                break;
            case DestroyNotify:
                mp_msg(MSGT_VO, MSGL_WARN, "Our window was destroyed, exiting\n");
                mplayer_put_key(vo->key_fifo, KEY_CLOSE_WIN);
                break;
	    case ClientMessage:
                if (Event.xclient.message_type == x11->XAWM_PROTOCOLS &&
                    Event.xclient.data.l[0] == x11->XAWM_DELETE_WINDOW)
                    mplayer_put_key(vo->key_fifo, KEY_CLOSE_WIN);
                break;
        default:
                if (Event.type == x11->ShmCompletionEvent)
                    if (x11->ShmCompletionWaitCount > 0)
                        x11->ShmCompletionWaitCount--;
                break;
        }
    }
    return ret;
}

static void vo_x11_sizehint(struct vo *vo, int x, int y, int width, int height,
                            int max)
{
    struct vo_x11_state *x11 = vo->x11;
    x11->vo_hint.flags = 0;
    if (vo_keepaspect)
    {
        x11->vo_hint.flags |= PAspect;
        x11->vo_hint.min_aspect.x = width;
        x11->vo_hint.min_aspect.y = height;
        x11->vo_hint.max_aspect.x = width;
        x11->vo_hint.max_aspect.y = height;
    }

    x11->vo_hint.flags |= PPosition | PSize;
    x11->vo_hint.x = x;
    x11->vo_hint.y = y;
    x11->vo_hint.width = width;
    x11->vo_hint.height = height;
    if (max)
    {
        x11->vo_hint.flags |= PMaxSize;
        x11->vo_hint.max_width = width;
        x11->vo_hint.max_height = height;
    } else
    {
        x11->vo_hint.max_width = 0;
        x11->vo_hint.max_height = 0;
    }

    // Set minimum height/width to 4 to avoid off-by-one errors.
    x11->vo_hint.flags |= PMinSize;
    x11->vo_hint.min_width = x11->vo_hint.min_height = 4;

    // Set the base size. A window manager might display the window
    // size to the user relative to this.
    // Setting these to width/height might be nice, but e.g. fluxbox can't handle it.
    x11->vo_hint.flags |= PBaseSize;
    x11->vo_hint.base_width = 0 /*width*/;
    x11->vo_hint.base_height = 0 /*height*/;

    x11->vo_hint.flags |= PWinGravity;
    x11->vo_hint.win_gravity = StaticGravity;
    XSetWMNormalHints(x11->display, x11->window, &x11->vo_hint);
}

/**
 * \brief sets the size and position of the non-fullscreen window.
 */
static void vo_x11_nofs_sizepos(struct vo *vo, int x, int y,
                                int width, int height)
{
    struct vo_x11_state *x11 = vo->x11;
    if (width == x11->last_video_width && height == x11->last_video_height) {
        if (!vo->opts->force_window_position && !x11->size_changed_during_fs)
            return;
    } else if (vo_fs)
        x11->size_changed_during_fs = true;
    x11->last_video_height = height;
    x11->last_video_width = width;
    vo_x11_sizehint(vo, x, y, width, height, 0);
  if (vo_fs) {
    x11->vo_old_x = x;
    x11->vo_old_y = y;
    x11->vo_old_width = width;
    x11->vo_old_height = height;
  }
  else
  {
    vo->dwidth = width;
    vo->dheight = height;
    if (vo->opts->force_window_position)
        XMoveResizeWindow(vo->x11->display, vo->x11->window, x, y, width,
                          height);
    else
        XResizeWindow(vo->x11->display, vo->x11->window, width, height);
  }
}

static int vo_x11_get_gnome_layer(struct vo_x11_state *x11, Window win)
{
    Atom type;
    int format;
    unsigned long nitems;
    unsigned long bytesafter;
    unsigned short *args = NULL;

    if (XGetWindowProperty(x11->display, win, x11->XA_WIN_LAYER, 0, 16384,
                           False, AnyPropertyType, &type, &format, &nitems,
                           &bytesafter,
                           (unsigned char **) &args) == Success
        && nitems > 0 && args)
    {
        mp_msg(MSGT_VO, MSGL_V, "[x11] original window layer is %d.\n",
               *args);
        return *args;
    }
    return WIN_LAYER_NORMAL;
}

// set a X text property that expects a UTF8_STRING type
static void vo_x11_set_property_utf8(struct vo *vo, Atom name, const char *t)
{
    struct vo_x11_state *x11 = vo->x11;

    XChangeProperty(x11->display, x11->window, name, x11->XAUTF8_STRING, 8,
                    PropModeReplace, t, strlen(t));
}

// set a X text property that expects a STRING or COMPOUND_TEXT type
static void vo_x11_set_property_string(struct vo *vo, Atom name, const char *t)
{
    struct vo_x11_state *x11 = vo->x11;
    XTextProperty prop = {0};

    if (Xutf8TextListToTextProperty(x11->display, (char **)&t, 1,
                                    XStdICCTextStyle, &prop) == Success)
    {
        XSetTextProperty(x11->display, x11->window, &prop, name);
    } else {
        // Strictly speaking this violates the ICCCM, but there's no way we
        // can do this correctly.
        vo_x11_set_property_utf8(vo, name, t);
    }

    if (prop.value)
        XFree(prop.value);
}

static void vo_x11_update_window_title(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;

    const char *title = vo_get_window_title(vo);
    vo_x11_set_property_string(vo, XA_WM_NAME, title);
    vo_x11_set_property_string(vo, XA_WM_ICON_NAME, title);
    vo_x11_set_property_utf8(vo, x11->XA_NET_WM_NAME, title);
    vo_x11_set_property_utf8(vo, x11->XA_NET_WM_ICON_NAME, title);
}

//
static Window vo_x11_create_smooth_window(struct vo_x11_state *x11, Window mRoot,
                                   Visual * vis, int x, int y,
                                   unsigned int width, unsigned int height,
                                   int depth, Colormap col_map)
{
    unsigned long xswamask = CWBorderPixel;
    XSetWindowAttributes xswa;
    Window ret_win;

    if (col_map != CopyFromParent)
    {
        xswa.colormap = col_map;
        xswamask |= CWColormap;
    }
    xswa.background_pixel = 0;
    xswa.border_pixel = 0;
    xswa.backing_store = NotUseful;
    xswa.bit_gravity = StaticGravity;

    ret_win =
        XCreateWindow(x11->display, x11->rootwin, x, y, width, height, 0, depth,
                      CopyFromParent, vis, xswamask, &xswa);
    XSetWMProtocols(x11->display, ret_win, &x11->XAWM_DELETE_WINDOW, 1);
    if (x11->f_gc == None)
        x11->f_gc = XCreateGC(x11->display, ret_win, 0, 0);
    XSetForeground(x11->display, x11->f_gc, 0);

    return ret_win;
}

/**
 * \brief create and setup a window suitable for display
 * \param vis Visual to use for creating the window
 * \param x x position of window
 * \param y y position of window
 * \param width width of window
 * \param height height of window
 * \param flags flags for window creation.
 *              Only VOFLAG_FULLSCREEN is supported so far.
 * \param col_map Colourmap for window or CopyFromParent if a specific colormap isn't needed
 * \param classname name to use for the classhint
 *
 * This also does the grunt-work like setting Window Manager hints etc.
 * If vo_window is already set it just moves and resizes it.
 */
void vo_x11_create_vo_window(struct vo *vo, XVisualInfo *vis, int x, int y,
                             unsigned int width, unsigned int height, int flags,
                             Colormap col_map, const char *classname)
{
  struct MPOpts *opts = vo->opts;
  struct vo_x11_state *x11 = vo->x11;
  Display *mDisplay = vo->x11->display;
  bool force_change_xy = opts->vo_geometry.xy_valid || xinerama_screen >= 0;
  if (WinID >= 0) {
    vo_fs = flags & VOFLAG_FULLSCREEN;
    x11->window = WinID ? (Window)WinID : x11->rootwin;
    if (col_map != CopyFromParent) {
      unsigned long xswamask = CWColormap;
      XSetWindowAttributes xswa;
      xswa.colormap = col_map;
      XChangeWindowAttributes(mDisplay, x11->window, xswamask, &xswa);
      XInstallColormap(mDisplay, col_map);
    }
    if (WinID) {
        // Expose events can only really be handled by us, so request them.
        vo_x11_selectinput_witherr(mDisplay, x11->window, ExposureMask);
    } else
        // Do not capture events since it might break the parent application
        // if it relies on events being forwarded to the parent of WinID.
        // It also is consistent with the w32_common.c code.
        vo_x11_selectinput_witherr(mDisplay, x11->window,
          StructureNotifyMask | KeyPressMask | PointerMotionMask |
          ButtonPressMask | ButtonReleaseMask | ExposureMask);

    vo_x11_update_geometry(vo, true);
    goto final;
  }
  if (x11->window == None) {
    vo_fs = 0;
    vo->dwidth = width;
    vo->dheight = height;
    x11->window = vo_x11_create_smooth_window(x11, x11->rootwin, vis->visual,
                      x, y, width, height, vis->depth, col_map);
    x11->window_state = VOFLAG_HIDDEN;
  }
  if (flags & VOFLAG_HIDDEN)
    goto final;
  if (x11->window_state & VOFLAG_HIDDEN) {
    XSizeHints hint;
    x11->window_state &= ~VOFLAG_HIDDEN;
    vo_x11_classhint(vo, x11->window, classname);
    vo_hidecursor(mDisplay, x11->window);
    XSelectInput(mDisplay, x11->window, StructureNotifyMask);
    hint.x = x; hint.y = y;
    hint.width = width; hint.height = height;
    hint.flags = PSize;
    if (force_change_xy)
      hint.flags |= PPosition;
    XSetWMNormalHints(mDisplay, x11->window, &hint);
    if (!vo_border) vo_x11_decoration(vo, 0);
    // map window
    x11->xic = XCreateIC(x11->xim,
                         XNInputStyle, XIMPreeditNone | XIMStatusNone,
                         XNClientWindow, x11->window,
                         XNFocusWindow, x11->window,
                         NULL);
    XSelectInput(mDisplay, x11->window, NoEventMask);
    vo_x11_selectinput_witherr(mDisplay, x11->window,
          StructureNotifyMask | KeyPressMask | PointerMotionMask |
          ButtonPressMask | ButtonReleaseMask | ExposureMask);
    XMapWindow(mDisplay, x11->window);
    vo_x11_clearwindow(vo, x11->window);
  }
  vo_x11_update_window_title(vo);
  if (opts->vo_ontop) vo_x11_setlayer(vo, x11->window, opts->vo_ontop);
  vo_x11_update_geometry(vo, !force_change_xy);
  vo_x11_nofs_sizepos(vo, vo->dx, vo->dy, width, height);
  if (!!vo_fs != !!(flags & VOFLAG_FULLSCREEN))
    vo_x11_fullscreen(vo);
  else if (vo_fs) {
    // if we are already in fullscreen do not switch back and forth, just
    // set the size values right.
    vo->dwidth  = vo->opts->vo_screenwidth;
    vo->dheight = vo->opts->vo_screenheight;
  }
final:
  if (x11->vo_gc != None)
    XFreeGC(mDisplay, x11->vo_gc);
  x11->vo_gc = XCreateGC(mDisplay, x11->window, 0, NULL);

  XSync(mDisplay, False);
  vo->event_fd = ConnectionNumber(x11->display);
}

void vo_x11_clearwindow_part(struct vo *vo, Window vo_window,
                             int img_width, int img_height)
{
    struct vo_x11_state *x11 = vo->x11;
    Display *mDisplay = vo->x11->display;
    int u_dheight, u_dwidth, left_ov, left_ov2;

    if (x11->f_gc == None)
        return;

    u_dheight = vo->dheight;
    u_dwidth = vo->dwidth;
    if ((u_dheight <= img_height) && (u_dwidth <= img_width))
        return;

    left_ov = (u_dheight - img_height) / 2;
    left_ov2 = (u_dwidth - img_width) / 2;

    XFillRectangle(mDisplay, vo_window, x11->f_gc, 0, 0, u_dwidth, left_ov);
    XFillRectangle(mDisplay, vo_window, x11->f_gc, 0, u_dheight - left_ov - 1,
                   u_dwidth, left_ov + 1);

    if (u_dwidth > img_width)
    {
        XFillRectangle(mDisplay, vo_window, x11->f_gc, 0, left_ov, left_ov2,
                       img_height);
        XFillRectangle(mDisplay, vo_window, x11->f_gc, u_dwidth - left_ov2 - 1,
                       left_ov, left_ov2 + 1, img_height);
    }

    XFlush(mDisplay);
}

void vo_x11_clearwindow(struct vo *vo, Window vo_window)
{
    struct vo_x11_state *x11 = vo->x11;
    struct MPOpts *opts = vo->opts;
    if (x11->f_gc == None)
        return;
    XFillRectangle(x11->display, vo_window, x11->f_gc, 0, 0,
                   opts->vo_screenwidth, opts->vo_screenheight);
    //
    XFlush(x11->display);
}


static void vo_x11_setlayer(struct vo *vo, Window vo_window, int layer)
{
    struct vo_x11_state *x11 = vo->x11;
    if (WinID >= 0)
        return;

    if (x11->fs_type & vo_wm_LAYER)
    {
        XClientMessageEvent xev;

        if (!x11->orig_layer)
            x11->orig_layer = vo_x11_get_gnome_layer(x11, vo_window);

        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.display = x11->display;
        xev.window = vo_window;
        xev.message_type = x11->XA_WIN_LAYER;
        xev.format = 32;
        // if not fullscreen, stay on default layer
        xev.data.l[0] = layer ? x11->fs_layer : x11->orig_layer;
        xev.data.l[1] = CurrentTime;
        mp_msg(MSGT_VO, MSGL_V,
               "[x11] Layered style stay on top (layer %ld).\n",
               xev.data.l[0]);
        XSendEvent(x11->display, x11->rootwin, False, SubstructureNotifyMask,
                   (XEvent *) & xev);
    } else if (x11->fs_type & vo_wm_NETWM)
    {
        XClientMessageEvent xev;
        char *state;

        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.message_type = x11->XA_NET_WM_STATE;
        xev.display = x11->display;
        xev.window = vo_window;
        xev.format = 32;
        xev.data.l[0] = layer;

        if (x11->fs_type & vo_wm_STAYS_ON_TOP)
            xev.data.l[1] = x11->XA_NET_WM_STATE_STAYS_ON_TOP;
        else if (x11->fs_type & vo_wm_ABOVE)
            xev.data.l[1] = x11->XA_NET_WM_STATE_ABOVE;
        else if (x11->fs_type & vo_wm_FULLSCREEN)
            xev.data.l[1] = x11->XA_NET_WM_STATE_FULLSCREEN;
        else if (x11->fs_type & vo_wm_BELOW)
            // This is not fallback. We can safely assume that the situation
            // where only NETWM_STATE_BELOW is supported doesn't exist.
            xev.data.l[1] = x11->XA_NET_WM_STATE_BELOW;

        XSendEvent(x11->display, x11->rootwin, False, SubstructureRedirectMask,
                   (XEvent *) & xev);
        state = XGetAtomName(x11->display, xev.data.l[1]);
        mp_msg(MSGT_VO, MSGL_V,
               "[x11] NET style stay on top (layer %d). Using state %s.\n",
               layer, state);
        XFree(state);
    }
}

static int vo_x11_get_fs_type(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    int type = x11->wm_type;
    char **fstype_list = vo->opts->vo_fstype_list;
    int i;

    if (fstype_list)
    {
        for (i = 0; fstype_list[i]; i++)
        {
            int neg = 0;
            char *arg = fstype_list[i];

            if (fstype_list[i][0] == '-')
            {
                neg = 1;
                arg = fstype_list[i] + 1;
            }

            if (!strncmp(arg, "layer", 5))
            {
                if (!neg && (arg[5] == '='))
                {
                    char *endptr = NULL;
                    int layer = strtol(fstype_list[i] + 6, &endptr, 10);

                    if (endptr && *endptr == '\0' && layer >= 0
                        && layer <= 15)
                        x11->fs_layer = layer;
                }
                if (neg)
                    type &= ~vo_wm_LAYER;
                else
                    type |= vo_wm_LAYER;
            } else if (!strcmp(arg, "above"))
            {
                if (neg)
                    type &= ~vo_wm_ABOVE;
                else
                    type |= vo_wm_ABOVE;
            } else if (!strcmp(arg, "fullscreen"))
            {
                if (neg)
                    type &= ~vo_wm_FULLSCREEN;
                else
                    type |= vo_wm_FULLSCREEN;
            } else if (!strcmp(arg, "stays_on_top"))
            {
                if (neg)
                    type &= ~vo_wm_STAYS_ON_TOP;
                else
                    type |= vo_wm_STAYS_ON_TOP;
            } else if (!strcmp(arg, "below"))
            {
                if (neg)
                    type &= ~vo_wm_BELOW;
                else
                    type |= vo_wm_BELOW;
            } else if (!strcmp(arg, "netwm"))
            {
                if (neg)
                    type &= ~vo_wm_NETWM;
                else
                    type |= vo_wm_NETWM;
            } else if (!strcmp(arg, "none"))
                type = 0; // clear; keep parsing
        }
    }

    return type;
}

// update vo->dx, vo->dy, vo->dwidth and vo->dheight with current values of vo->x11->window
static void vo_x11_update_geometry(struct vo *vo, bool update_pos)
{
    struct vo_x11_state *x11 = vo->x11;
    unsigned w, h, dummy_uint;
    int dummy_int;
    Window dummy_win;
    XGetGeometry(x11->display, x11->window, &dummy_win, &dummy_int, &dummy_int,
                 &w, &h, &dummy_int, &dummy_uint);
    if (w <= INT_MAX && h <= INT_MAX) {
        vo->dwidth = w;
        vo->dheight = h;
    }
    if (update_pos)
        XTranslateCoordinates(x11->display, x11->window, x11->rootwin, 0, 0,
                              &vo->dx, &vo->dy, &dummy_win);
}

void vo_x11_fullscreen(struct vo *vo)
{
    struct MPOpts *opts = vo->opts;
    struct vo_x11_state *x11 = vo->x11;
    int x, y, w, h;
    x = x11->vo_old_x;
    y = x11->vo_old_y;
    w = x11->vo_old_width;
    h = x11->vo_old_height;

    if (WinID >= 0) {
        vo_fs = !vo_fs;
        return;
    }
    if (x11->fs_flip)
        return;

    if (vo_fs)
    {
        vo_x11_ewmh_fullscreen(x11, _NET_WM_STATE_REMOVE);   // removes fullscreen state if wm supports EWMH
        vo_fs = VO_FALSE;
        if (x11->size_changed_during_fs && (x11->fs_type & vo_wm_FULLSCREEN))
            vo_x11_nofs_sizepos(vo, vo->dx, vo->dy, x11->last_video_width,
                                x11->last_video_height);
        x11->size_changed_during_fs = false;
    } else
    {
        // win->fs
        vo_x11_ewmh_fullscreen(x11, _NET_WM_STATE_ADD);      // sends fullscreen state to be added if wm supports EWMH

        vo_fs = VO_TRUE;
        if ( ! (x11->fs_type & vo_wm_FULLSCREEN) ) // not needed with EWMH fs
        {
            x11->vo_old_x = vo->dx;
            x11->vo_old_y = vo->dy;
            x11->vo_old_width = vo->dwidth;
            x11->vo_old_height = vo->dheight;
        }
            vo_x11_update_screeninfo(vo);
            x = xinerama_x;
            y = xinerama_y;
            w = opts->vo_screenwidth;
            h = opts->vo_screenheight;
    }
    {
        long dummy;

        XGetWMNormalHints(x11->display, x11->window, &x11->vo_hint, &dummy);
        if (!(x11->vo_hint.flags & PWinGravity))
            x11->old_gravity = NorthWestGravity;
        else
            x11->old_gravity = x11->vo_hint.win_gravity;
    }
    if (x11->wm_type == 0 && !(vo_fsmode & 16))
    {
        XUnmapWindow(x11->display, x11->window);      // required for MWM
        XWithdrawWindow(x11->display, x11->window, x11->screen);
        x11->fs_flip = 1;
    }

    if ( ! (x11->fs_type & vo_wm_FULLSCREEN) ) // not needed with EWMH fs
    {
        vo_x11_decoration(vo, vo_border && !vo_fs);
        vo_x11_sizehint(vo, x, y, w, h, 0);
        vo_x11_setlayer(vo, x11->window, vo_fs);


        XMoveResizeWindow(x11->display, x11->window, x, y, w, h);
    }
    /* some WMs lose ontop after fullscreen */
    if ((!(vo_fs)) & opts->vo_ontop)
        vo_x11_setlayer(vo, x11->window, opts->vo_ontop);

    XMapRaised(x11->display, x11->window);
    if ( ! (x11->fs_type & vo_wm_FULLSCREEN) ) // some WMs change window pos on map
        XMoveResizeWindow(x11->display, x11->window, x, y, w, h);
    XRaiseWindow(x11->display, x11->window);
    XFlush(x11->display);
}

void vo_x11_ontop(struct vo *vo)
{
    struct MPOpts *opts = vo->opts;
    opts->vo_ontop = !opts->vo_ontop;

    vo_x11_setlayer(vo, vo->x11->window, opts->vo_ontop);
}

void vo_x11_border(struct vo *vo)
{
    vo_border = !vo_border;
    vo_x11_decoration(vo, vo_border && !vo_fs);
}

/*
 * XScreensaver stuff
 */

void xscreensaver_heartbeat(struct vo_x11_state *x11)
{
    unsigned int time = GetTimerMS();

    if (x11->display && x11->screensaver_off &&
        (time - x11->screensaver_time_last) > 30000)
    {
        x11->screensaver_time_last = time;

        XResetScreenSaver(x11->display);
    }
}

static int xss_suspend(Display *mDisplay, Bool suspend)
{
#ifndef CONFIG_XSS
    return 0;
#else
    int event, error, major, minor;
    if (XScreenSaverQueryExtension(mDisplay, &event, &error) != True ||
        XScreenSaverQueryVersion(mDisplay, &major, &minor) != True)
        return 0;
    if (major < 1 || (major == 1 && minor < 1))
        return 0;
    XScreenSaverSuspend(mDisplay, suspend);
    return 1;
#endif
}

/*
 * End of XScreensaver stuff
 */

static void saver_on(struct vo_x11_state *x11)
{
    Display *mDisplay = x11->display;
    if (!x11->screensaver_off)
        return;
    x11->screensaver_off = 0;
    if (xss_suspend(mDisplay, False))
        return;
#ifdef CONFIG_XDPMS
    if (x11->dpms_disabled) {
        int nothing;
        if (DPMSQueryExtension(mDisplay, &nothing, &nothing)) {
            if (!DPMSEnable(mDisplay)) {    // restoring power saving settings
                mp_msg(MSGT_VO, MSGL_WARN, "DPMS not available?\n");
            } else {
                // DPMS does not seem to be enabled unless we call DPMSInfo
                BOOL onoff;
                CARD16 state;

                DPMSForceLevel(mDisplay, DPMSModeOn);
                DPMSInfo(mDisplay, &state, &onoff);
                if (onoff)
                {
                    mp_msg(MSGT_VO, MSGL_V,
                           "Successfully enabled DPMS\n");
                } else
                {
                    mp_msg(MSGT_VO, MSGL_WARN, "Could not enable DPMS\n");
                }
            }
        }
        x11->dpms_disabled = 0;
    }
#endif
}

static void saver_off(struct vo_x11_state *x11)
{
    Display *mDisplay = x11->display;
    int nothing;

    if (x11->screensaver_off)
        return;
    x11->screensaver_off = 1;
    if (xss_suspend(mDisplay, True))
        return;
#ifdef CONFIG_XDPMS
    if (DPMSQueryExtension(mDisplay, &nothing, &nothing))
    {
        BOOL onoff;
        CARD16 state;

        DPMSInfo(mDisplay, &state, &onoff);
        if (onoff)
        {
            Status stat;

            mp_msg(MSGT_VO, MSGL_V, "Disabling DPMS\n");
            x11->dpms_disabled = 1;
            stat = DPMSDisable(mDisplay);       // monitor powersave off
            mp_msg(MSGT_VO, MSGL_V, "DPMSDisable stat: %d\n", stat);
        }
    }
#endif
}

static void vo_x11_selectinput_witherr(Display *display, Window w,
                                       long event_mask)
{
    if (vo_nomouse_input)
        event_mask &= ~(ButtonPressMask | ButtonReleaseMask);

    // NOTE: this can raise BadAccess, which should be ignored by the X error
    //       handler; also see below
    XSelectInput(display, w, event_mask);

    // Test whether setting the event mask failed (with a BadAccess X error,
    // although we don't know whether this really happened).
    // This is needed for obscure situations like using --rootwin with a window
    // manager active.
    XWindowAttributes a;
    if (XGetWindowAttributes(display, w, &a)) {
        long bad = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
        if ((event_mask & bad) && (a.all_event_masks & bad) &&
            ((a.your_event_mask & bad) != (event_mask & bad)))
        {
            mp_msg(MSGT_VO, MSGL_ERR, "X11 error: error during XSelectInput "
                   "call, trying without mouse events\n");
            XSelectInput(display, w, event_mask & ~bad);
        }
    }
}

#ifdef CONFIG_XF86VM
void vo_vm_switch(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    struct MPOpts *opts = vo->opts;
    Display *mDisplay = x11->display;
    int vm_event, vm_error;
    int vm_ver, vm_rev;
    int i, j, have_vm = 0;
    int X = vo->dwidth, Y = vo->dheight;
    int modeline_width, modeline_height;

    if (XF86VidModeQueryExtension(mDisplay, &vm_event, &vm_error))
    {
        XF86VidModeQueryVersion(mDisplay, &vm_ver, &vm_rev);
        mp_msg(MSGT_VO, MSGL_V, "XF86VidMode extension v%i.%i\n", vm_ver,
               vm_rev);
        have_vm = 1;
    } else {
        mp_msg(MSGT_VO, MSGL_WARN,
               "XF86VidMode extension not available.\n");
    }

    if (have_vm) {
        int modecount = 0;
        XF86VidModeModeInfo **vidmodes = NULL;
        XF86VidModeGetAllModeLines(mDisplay, x11->screen, &modecount, &vidmodes);
        j = 0;
        modeline_width = vidmodes[0]->hdisplay;
        modeline_height = vidmodes[0]->vdisplay;

        for (i = 1; i < modecount; i++) {
            if ((vidmodes[i]->hdisplay >= X)
                && (vidmodes[i]->vdisplay >= Y))
            {
                if ((vidmodes[i]->hdisplay <= modeline_width)
                    && (vidmodes[i]->vdisplay <= modeline_height))
                {
                    modeline_width = vidmodes[i]->hdisplay;
                    modeline_height = vidmodes[i]->vdisplay;
                    j = i;
                }
            }
        }

        mp_tmsg(MSGT_VO, MSGL_INFO, "XF86VM: Selected video mode %dx%d for image size %dx%d.\n",
               modeline_width, modeline_height, X, Y);
        XF86VidModeLockModeSwitch(mDisplay, x11->screen, 0);
        XF86VidModeSwitchToMode(mDisplay, x11->screen, vidmodes[j]);
        XF86VidModeSwitchToMode(mDisplay, x11->screen, vidmodes[j]);

        // FIXME: all this is more of a hack than proper solution
        X = (opts->vo_screenwidth - modeline_width) / 2;
        Y = (opts->vo_screenheight - modeline_height) / 2;
        XF86VidModeSetViewPort(mDisplay, x11->screen, X, Y);
        vo->dx = X;
        vo->dy = Y;
        vo->dwidth = modeline_width;
        vo->dheight = modeline_height;
        aspect_save_screenres(vo, modeline_width, modeline_height);

        x11->vm_set = 1;
        free(vidmodes);
    }
}

void vo_vm_close(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    Display *dpy = x11->display;
    struct MPOpts *opts = vo->opts;
    if (x11->vm_set) {
        int modecount = 0;
        XF86VidModeModeInfo **vidmodes = NULL;
        int i;

        XF86VidModeGetAllModeLines(dpy, x11->screen, &modecount, &vidmodes);
        for (i = 0; i < modecount; i++)
            if ((vidmodes[i]->hdisplay == opts->vo_screenwidth)
                && (vidmodes[i]->vdisplay == opts->vo_screenheight))
            {
                mp_msg(MSGT_VO, MSGL_INFO,
                       "Returning to original mode %dx%d\n",
                       opts->vo_screenwidth, opts->vo_screenheight);
                break;
            }

        XF86VidModeSwitchToMode(dpy, x11->screen, vidmodes[i]);
        XF86VidModeSwitchToMode(dpy, x11->screen, vidmodes[i]);
        free(vidmodes);
        modecount = 0;
    }
}

double vo_vm_get_fps(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    int clock;
    XF86VidModeModeLine modeline;
    if (!XF86VidModeGetModeLine(x11->display, x11->screen, &clock, &modeline))
        return 0;
    if (modeline.privsize)
        XFree(modeline.private);
    return 1e3 * clock / modeline.htotal / modeline.vtotal;
}
#endif


Colormap vo_x11_create_colormap(struct vo *vo, XVisualInfo *vinfo)
{
    struct vo_x11_state *x11 = vo->x11;
    unsigned k, r, g, b, ru, gu, bu, m, rv, gv, bv, rvu, gvu, bvu;

    if (vinfo->class != DirectColor)
        return XCreateColormap(x11->display, x11->rootwin, vinfo->visual,
                               AllocNone);

    /* can this function get called twice or more? */
    if (x11->cmap)
        return x11->cmap;
    x11->cm_size = vinfo->colormap_size;
    x11->red_mask = vinfo->red_mask;
    x11->green_mask = vinfo->green_mask;
    x11->blue_mask = vinfo->blue_mask;
    ru = (x11->red_mask & (x11->red_mask - 1)) ^ x11->red_mask;
    gu = (x11->green_mask & (x11->green_mask - 1)) ^ x11->green_mask;
    bu = (x11->blue_mask & (x11->blue_mask - 1)) ^ x11->blue_mask;
    rvu = 65536ull * ru / (x11->red_mask + ru);
    gvu = 65536ull * gu / (x11->green_mask + gu);
    bvu = 65536ull * bu / (x11->blue_mask + bu);
    r = g = b = 0;
    rv = gv = bv = 0;
    m = DoRed | DoGreen | DoBlue;
    for (k = 0; k < x11->cm_size; k++)
    {
        int t;

        x11->cols[k].pixel = r | g | b;
        x11->cols[k].red = rv;
        x11->cols[k].green = gv;
        x11->cols[k].blue = bv;
        x11->cols[k].flags = m;
        t = (r + ru) & x11->red_mask;
        if (t < r)
            m &= ~DoRed;
        r = t;
        t = (g + gu) & x11->green_mask;
        if (t < g)
            m &= ~DoGreen;
        g = t;
        t = (b + bu) & x11->blue_mask;
        if (t < b)
            m &= ~DoBlue;
        b = t;
        rv += rvu;
        gv += gvu;
        bv += bvu;
    }
    x11->cmap = XCreateColormap(x11->display, x11->rootwin, vinfo->visual,
                                AllocAll);
    XStoreColors(x11->display, x11->cmap, x11->cols, x11->cm_size);
    return x11->cmap;
}

static int transform_color(float val,
                           float brightness, float contrast, float gamma) {
    float s = pow(val, gamma);
    s = (s - 0.5) * contrast + 0.5;
    s += brightness;
    if (s < 0)
        s = 0;
    if (s > 1)
        s = 1;
    return (unsigned short) (s * 65535);
}

uint32_t vo_x11_set_equalizer(struct vo *vo, const char *name, int value)
{
    struct vo_x11_state *x11 = vo->x11;
    float gamma, brightness, contrast;
    float rf, gf, bf;
    int k;
    int red_mask = x11->red_mask;
    int green_mask = x11->green_mask;
    int blue_mask = x11->blue_mask;

    /*
     * IMPLEMENTME: consider using XF86VidModeSetGammaRamp in the case
     * of TrueColor-ed window but be careful:
     * Unlike the colormaps, which are private for the X client
     * who created them and thus automatically destroyed on client
     * disconnect, this gamma ramp is a system-wide (X-server-wide)
     * setting and _must_ be restored before the process exits.
     * Unforunately when the process crashes (or gets killed
     * for some reason) it is impossible to restore the setting,
     * and such behaviour could be rather annoying for the users.
     */
    if (x11->cmap == None)
        return VO_NOTAVAIL;

    if (!strcasecmp(name, "brightness"))
        x11->vo_brightness = value;
    else if (!strcasecmp(name, "contrast"))
        x11->vo_contrast = value;
    else if (!strcasecmp(name, "gamma"))
        x11->vo_gamma = value;
    else
        return VO_NOTIMPL;

    brightness = 0.01 * x11->vo_brightness;
    contrast = tan(0.0095 * (x11->vo_contrast + 100) * M_PI / 4);
    gamma = pow(2, -0.02 * x11->vo_gamma);

    rf = (float) ((red_mask & (red_mask - 1)) ^ red_mask) / red_mask;
    gf = (float) ((green_mask & (green_mask - 1)) ^ green_mask) /
        green_mask;
    bf = (float) ((blue_mask & (blue_mask - 1)) ^ blue_mask) / blue_mask;

    /* now recalculate the colormap using the newly set value */
    for (k = 0; k < x11->cm_size; k++)
    {
        x11->cols[k].red   = transform_color(rf * k, brightness, contrast, gamma);
        x11->cols[k].green = transform_color(gf * k, brightness, contrast, gamma);
        x11->cols[k].blue  = transform_color(bf * k, brightness, contrast, gamma);
    }

    XStoreColors(vo->x11->display, x11->cmap, x11->cols, x11->cm_size);
    XFlush(vo->x11->display);
    return VO_TRUE;
}

uint32_t vo_x11_get_equalizer(struct vo *vo, const char *name, int *value)
{
    struct vo_x11_state *x11 = vo->x11;
    if (x11->cmap == None)
        return VO_NOTAVAIL;
    if (!strcasecmp(name, "brightness"))
        *value = x11->vo_brightness;
    else if (!strcasecmp(name, "contrast"))
        *value = x11->vo_contrast;
    else if (!strcasecmp(name, "gamma"))
        *value = x11->vo_gamma;
    else
        return VO_NOTIMPL;
    return VO_TRUE;
}

bool vo_x11_screen_is_composited(struct vo *vo)
{
    struct vo_x11_state *x11 = vo->x11;
    return XGetSelectionOwner(x11->display, x11->XA_NET_WM_CM) != None;
}
