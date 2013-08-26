/*
 * MIT/X Consortium License
 *
 * © 2013 Jakub Klinkovský <kuba.klinkovsky at gmail dot com>
 * © 2010-2011 Ben Ruijl
 * © 2006-2008 Anselm R Garbe <garbeam at gmail dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdarg.h>     // variable arguments number
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>   // mlock()
#include <ctype.h>      // iscntrl()
#include <errno.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xrandr.h>
#include <security/pam_appl.h>


typedef struct Dpms {
    BOOL state;
    CARD16 level;  // why?
    CARD16 standby, suspend, off;
} Dpms;

typedef struct WindowPositionInfo {
    int display_width, display_height;
    int output_x, output_y;
    int output_width, output_height;
} WindowPositionInfo;

static int conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr);


/* need globals for signal handling */
Display *dpy;
Dpms dpms_original = { .state = True, .level = 0, .standby = 600, .suspend = 600, .off = 600 };  // holds original values
int dpms_timeout = 10;  // dpms timeout until program exits
Bool using_dpms;

pam_handle_t *pam_handle;
struct pam_conv conv = { conv_callback, NULL };

/* Holds the password you enter */
static char password[256];


static void
die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    fprintf(stderr, "%s: ", PROGNAME);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/*
 * Clears the memory which stored the password to be a bit safer against
 * cold-boot attacks.
 *
 */
static void
clear_password_memory(void) {
    /* A volatile pointer to the password buffer to prevent the compiler from
     * optimizing this out. */
    volatile char *vpassword = password;
    for (int c = 0; c < sizeof(password); c++)
        /* rewrite with random values */
        vpassword[c] = rand();
}

/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int
conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr) {
    if (num_msgs == 0)
        return PAM_BUF_ERR;

    // PAM expects an array of responses, one for each message
    if ((*resp = calloc(num_msgs, sizeof(struct pam_message))) == NULL)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msgs; i++) {
        if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF &&
            msg[i]->msg_style != PAM_PROMPT_ECHO_ON)
            continue;

        // return code is currently not used but should be set to zero
        resp[i]->resp_retcode = 0;
        if ((resp[i]->resp = strdup(password)) == NULL) {
            free(*resp);
            return PAM_BUF_ERR;
        }
    }

    return PAM_SUCCESS;
}

void
handle_signal(int sig) {
    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    die("Caught signal; dying\n");
}

void
main_loop(Window w, GC gc, XFontStruct* font, WindowPositionInfo* info, char passdisp[256], char* username, XColor black, XColor white, XColor red) {
    XEvent event;
    KeySym ksym;

    unsigned int len = 0;
    Bool running = True;
    Bool sleepmode = False;
    Bool failed = False;

    XSync(dpy, False);

    /* define base coordinates - middle of screen */
    int base_x = info->output_x + info->output_width / 2;
    int base_y = info->output_y + info->output_height / 2;    /* y-position of the line */

    /* not changed in the loop */
    int line_x_left = base_x - info->output_width / 8;
    int line_x_right = base_x + info->output_width / 8;

    /* font properties */
    int ascent, descent;
    {
        int dir;
        XCharStruct overall;
        XTextExtents(font, passdisp, strlen(username), &dir, &ascent, &descent, &overall);
    }

    /* main event loop */
    while(running && !XNextEvent(dpy, &event)) {
        if (sleepmode && using_dpms)
            DPMSForceLevel(dpy, DPMSModeOff);

        /* update window if no events pending */
        if (!XPending(dpy)) {
            int x;
            /* draw username and line */
            x = base_x - XTextWidth(font, username, strlen(username)) / 2;
            XDrawString(dpy, w, gc, x, base_y - 10, username, strlen(username));
            XDrawLine(dpy, w, gc, line_x_left, base_y, line_x_right, base_y);

            /* clear old passdisp */
            XClearArea(dpy, w, info->output_x, base_y + 20, info->output_width, ascent + descent, False);

            /* draw new passdisp or 'auth failed' */
            if (failed) {
                x = base_x - XTextWidth(font, "authentication failed", 21) / 2;
                XSetForeground(dpy, gc, red.pixel);
                XDrawString(dpy, w, gc, x, base_y + ascent + 20, "authentication failed", 21);
                XSetForeground(dpy, gc, white.pixel);
            } else {
                x = base_x - XTextWidth(font, passdisp, len) / 2;
                XDrawString(dpy, w, gc, x, base_y + ascent + 20, passdisp, len);
            }
        }

        if (event.type == MotionNotify) {
            sleepmode = False;
            failed = False;
        }

        if (event.type == KeyPress) {
            sleepmode = False;
            failed = False;

            char inputChar = 0;
            XLookupString(&event.xkey, &inputChar, sizeof(inputChar), &ksym, 0);

            switch (ksym) {
                case XK_Return:
                case XK_KP_Enter:
                    password[len] = 0;
                    if (pam_authenticate(pam_handle, 0) == PAM_SUCCESS) {
                        clear_password_memory();
                        running = False;
                    } else {
                        failed = True;
                    }
                    len = 0;
                    break;
                case XK_Escape:
                    len = 0;
                    sleepmode = True;
                    break;
                case XK_BackSpace:
                    if (len)
                        --len;
                    break;
                default:
                    if (isprint(inputChar) && (len + sizeof(inputChar) < sizeof password)) {
                        memcpy(password + len, &inputChar, sizeof(inputChar));
                        len += sizeof(inputChar);
                    }
                    break;
            }
        }
    }
}

int
main(int argc, char **argv) {
    char passdisp[256];
    int screen_num;
    WindowPositionInfo info;

    Cursor invisible;
    Window root, w;
    XColor black, red, white;
    XFontStruct* font;
    GC gc;

    // defaults
    char* passchar = "*";
    char* fontname = "-*-dejavu sans-bold-r-*-*-*-420-100-100-*-*-iso8859-1";
    char* username = "";

    if ((username = getenv("USER")) == NULL)
        die("USER environment variable not set, please set it.\n");

    /* register signal handler function */
    if (signal (SIGINT, handle_signal) == SIG_IGN)
        signal (SIGINT, SIG_IGN);
    if (signal (SIGHUP, handle_signal) == SIG_IGN)
        signal (SIGHUP, SIG_IGN);
    if (signal (SIGTERM, handle_signal) == SIG_IGN)
        signal (SIGTERM, SIG_IGN);

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 < argc)
                passchar = argv[i + 1];
            else
                die("error: no password character given.\n");
        } else
        if (!strcmp(argv[i], "-f")) {
            if (i + 1 < argc)
                fontname = argv[i + 1];
            else
                die("error: font not specified.\n");
        } else
        if (!strcmp(argv[i], "-v"))
            die(PROGNAME"-"VERSION", © 2013 Jakub Klinkovský\n");
        else
        if (!strcmp(argv[i], "?"))
            die("usage: "PROGNAME" [-v] [-c passchars] [-f fontname]\n");
    }

    /* fill with password characters */
    for (int i = 0; i < sizeof passdisp; i += strlen(passchar))
        for (int j = 0; j < strlen(passchar); j++)
            passdisp[i + j] = passchar[j];

    /* initialize random number generator */
    srand(time(NULL));

    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open dpy\n");

    if (!(font = XLoadQueryFont(dpy, fontname)))
        die("error: could not find font. Try using a full description.\n");

    screen_num = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* get display/output size and position */
    {
        XRRScreenResources* screen = XRRGetScreenResources (dpy, DefaultRootWindow(dpy));
        RROutput primary = XRRGetOutputPrimary(dpy, DefaultRootWindow(dpy));
        XRROutputInfo* output_info = XRRGetOutputInfo(dpy, screen, primary);
        XRRCrtcInfo* crtc_info = XRRGetCrtcInfo (dpy, screen, output_info->crtc);

        info.output_x = crtc_info->x;
        info.output_y = crtc_info->y;
        info.output_width = crtc_info->width;
        info.output_height = crtc_info->height;
        info.display_width = DisplayWidth(dpy, screen_num);
        info.display_height = DisplayHeight(dpy, screen_num);

        XRRFreeScreenResources(screen);
        XRRFreeOutputInfo(output_info);
        XRRFreeCrtcInfo(crtc_info);
    }

    /* allocate colors */
    {
        XColor dummy;
        Colormap cmap = DefaultColormap(dpy, screen_num);
        XAllocNamedColor(dpy, cmap, "orange red", &red, &dummy);
        XAllocNamedColor(dpy, cmap, "black", &black, &dummy);
        XAllocNamedColor(dpy, cmap, "white", &white, &dummy);
    }

    /* create window */
    {
        XSetWindowAttributes wa;
        wa.override_redirect = 1;
        wa.background_pixel = black.pixel;
        w = XCreateWindow(dpy, root, 0, 0, info.display_width, info.display_height,
                0, DefaultDepth(dpy, screen_num), CopyFromParent,
                DefaultVisual(dpy, screen_num), CWOverrideRedirect | CWBackPixel, &wa);
        XMapRaised(dpy, w);
    }

    /* define cursor */
    {
        char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
        Pixmap pmap = XCreateBitmapFromData(dpy, w, curs, 8, 8);
        invisible = XCreatePixmapCursor(dpy, pmap, pmap, &black, &black, 0, 0);
        XDefineCursor(dpy, w, invisible);
        XFreePixmap(dpy, pmap);
    }

    /* create Graphics Context */
    {
        XGCValues values;
        gc = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gc, font->fid);
        XSetForeground(dpy, gc, white.pixel);
    }

    /* grab pointer and keyboard */
    int len = 1000;
    while (len-- > 0) {
        if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    while (len-- > 0) {
        if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    if (len <= 0)
        die("Cannot grab pointer/keyboard");

    /* set up PAM */
    {
        int ret = pam_start("login", username, &conv, &pam_handle);
        if (ret != PAM_SUCCESS)
            die("PAM: %s\n", pam_strerror(pam_handle, ret));
    }

    /* Lock the area where we store the password in memory, we don’t want it to
     * be swapped to disk. Since Linux 2.6.9, this does not require any
     * privileges, just enough bytes in the RLIMIT_MEMLOCK limit. */
    if (mlock(password, sizeof(password)) != 0)
        die("Could not lock page in memory, check RLIMIT_MEMLOCK");

    /* handle dpms */
    using_dpms = DPMSCapable(dpy);
    if (using_dpms) {
        /* save dpms timeouts to restore on exit */
        DPMSGetTimeouts(dpy, &dpms_original.standby, &dpms_original.suspend, &dpms_original.off);
        DPMSInfo(dpy, &dpms_original.level, &dpms_original.state);

        /* set program specific dpms timeouts */
        DPMSSetTimeouts(dpy, dpms_timeout, dpms_timeout, dpms_timeout);

        /* force dpms enabled until exit */
        DPMSEnable(dpy);
    }

    /* run main loop */
    main_loop(w, gc, font, &info, passdisp, username, black, white, red);

    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
    return 0;
}
