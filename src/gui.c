/*
Draw and interface with our switcher window.

Copyright 2017-2026 Alexander Kulak.
This file is part of alttab program.

alttab is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

alttab is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with alttab.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
//#include <sys/time.h>
#include "alttab.h"
#include "util.h"
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
extern Globals g;
extern Display *dpy;
extern int scr;
extern Window root;

// PRIVATE

static unsigned int tileW, tileH, iconW, iconH;
static unsigned int visualTileW, visualTileH;
static int lastPressedTile;
static int cols, rows;
static quad scrdim;
static Window uiwin;
static int uiwinW, uiwinH, uiwinX, uiwinY;
static Colormap colormap;
static Visual *visual;
//Font fontLabel;  // Xft instead
static XftFont *fontLabel;
static int selNdx;                 // current (selected) item

static Pixmap roundMask;
static Picture uiwinPic;

typedef struct {
    float x, y;
    float scale;
    float dim;
} AnimTileState;

static AnimTileState *animFrom;
static AnimTileState *animTo;
static float animProgress;
static bool animActive;
static struct timespec animStartTime;
#define ANIM_DURATION_NS 200000000

//
// allocates GC
// type is:
//   0: normal
//   1: for bg fill
//   2: for drawing frame
//
static GC create_gc(int type)
{
    GC gc;                      /* handle of newly created GC.  */
    unsigned long valuemask = 0;    /* which values in 'values' to  */
    /* check when creating the GC.  */
    XGCValues values;           /* initial values for the GC.   */
    int line_style = LineSolid;
    int cap_style = CapButt;
    int join_style = JoinMiter;

    gc = XCreateGC(dpy, root, valuemask, &values);
    if (gc < 0) {
        msg(-1, "can't create GC\n");
        return 0;
    }
    /* allocate foreground and background colors for this GC. */
    switch (type) {
    case 1:
        XSetForeground(dpy, gc, g.color[COLBG].xcolor.pixel);
        XSetBackground(dpy, gc, g.color[COLINACT].xcolor.pixel);
        XSetLineAttributes(dpy, gc, frameW, line_style, cap_style, join_style);
        break;
    case 0:
        XSetForeground(dpy, gc, g.color[COLINACT].xcolor.pixel);
        XSetBackground(dpy, gc, g.color[COLBG].xcolor.pixel);
        XSetLineAttributes(dpy, gc, 1, line_style, cap_style, join_style);
        break;
    case 2:
        XSetForeground(dpy, gc, g.color[COLFRAME].xcolor.pixel);
        XSetBackground(dpy, gc, g.color[COLBG].xcolor.pixel);
        XSetLineAttributes(dpy, gc, frameW, line_style, cap_style, join_style);
        break;
    case 3:
        XSetForeground(dpy, gc, g.color[COLMIN].xcolor.pixel);
        XSetBackground(dpy, gc, g.color[COLBG].xcolor.pixel);
        XSetLineAttributes(dpy, gc, 1, line_style, cap_style, join_style);
        break;
    default:
        msg(-1, "unknown GC type, not setting colors\n");
        break;
    }
    /* define the fill style for the GC. to be 'solid filling'. */
    XSetFillStyle(dpy, gc, FillSolid);
    return gc;
}

//
// compute tile position for any layout mode
//
static void layoutPos(int idx, int *xp, int *yp)
{
    if (g.option_layout != LAYOUT_GRID) {
        int step = tileW - (tileW > 40 ? tileW * 50 / 100 : tileW / 2);
        if (step < 1) step = 1;
        *xp = (int)uiwinW / 2 + (idx - selNdx) * step - (int)tileW / 2;
        *yp = (int)uiwinH / 2 - (int)tileH / 2;
    } else if (g.option_vertical) {
        *xp = (idx / rows) * (tileW + frameW + g.option_spacing) + frameW + g.option_winPad;
        *yp = (idx % rows) * (tileH + frameW + g.option_spacing) + frameW + g.option_winPad;
    } else {
        *xp = (idx % cols) * (tileW + frameW + g.option_spacing) + frameW + g.option_winPad;
        *yp = (idx / cols) * (tileH + frameW + g.option_spacing) + frameW + g.option_winPad;
    }
}

//
// single use helper for function below
//
static void drawFr(GC gc, int f)
{
    int tx, ty;
    layoutPos(f, &tx, &ty);
    int x = tx - frameW / 2;
    int y = ty - frameW / 2;
    int dw = tileW + frameW;
    int dh = tileH + frameW;
    if (g.option_cornerRadius > 0) {
        drawRoundedRectFrame(dpy, uiwin, gc, x, y, dw, dh,
                             g.option_cornerRadius);
    } else {
        int d = XDrawRectangle(dpy, uiwin, gc, x, y, dw, dh);
        if (!d)
            msg(-1, "can't draw frame\n");
    }
}

//
// draw selected and unselected frames around tiles
//
static void framesRedraw(void)
{
    if (g.option_layout != LAYOUT_GRID) {
        // stack: selected frame only (others drawn in renderAllTiles)
        drawFr(g.gcFrame, selNdx);
        return;
    }
    int f;
    for (f = 0; f < g.maxNdx; f++) {
        if (f == selNdx)
            continue;           // skip
        drawFr(g.gcReverse, f); // thick bg
        if (g.winlist[f].use_minimized_color) {
            drawFr(g.gcMin, f);  // minimized frame
        } else {
            drawFr(g.gcDirect, f);  // thin frame
        }
    }
// _after_ unselected draw selected, because they may overlap
    drawFr(g.gcFrame, selNdx);
}

static float easeOutQuad(float t) {
    return t * (2.0f - t);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static void computeTileState(int j, int sel, AnimTileState *s) {
    int step = tileW - (tileW > 40 ? tileW * 50 / 100 : tileW / 2);
    if (step < 1) step = 1;
    s->x = (float)((int)uiwinW / 2 + (j - sel) * step - (int)tileW / 2);
    s->y = (float)((int)uiwinH / 2 - (int)tileH / 2);
    int dist = (j == sel) ? 0 : abs(j - sel);
    if (dist == 0) {
        s->scale = 1.0f;
        s->dim = 0.0f;
    } else if (dist == 1) {
        s->scale = 0.85f;
        s->dim = 0.1875f;
    } else if (dist == 2) {
        s->scale = 0.72f;
        s->dim = 0.3125f;
    } else {
        s->scale = 0.60f;
        s->dim = 0.3125f;
    }
}

//
// given coordinates relative to our window,
// return the tile number or -1
//
static int pointedTile(int x, int y)
{
    if (g.option_layout == LAYOUT_GRID) {
        int marg = frameW / 2 + g.option_winPad;
        if (x < marg || y < marg
            || (unsigned int)x > uiwinW - marg
            || (unsigned int)y > uiwinH - marg)
            return -1;
        int pc = (x - marg) / visualTileW;
        int pr = (y - marg) / visualTileH;
        int idx;
        if (g.option_vertical)
            idx = pc * rows + pr;
        else
            idx = pr * cols + pc;
        if (idx >= 0 && idx < g.maxNdx)
            return idx;
        return -1;
    } else {
        // Stack: check tiles from closest to farthest
        int step = (tileW - (tileW > 40 ? tileW * 50 / 100 : tileW / 2));
        if (step < 1) step = 1;
        int cx = (int)uiwinW / 2;
        int cy = (int)uiwinH / 2;
        for (int d = 0; d < g.maxNdx; d++) {
            int idx = selNdx + d;
            if (idx < g.maxNdx) {
                int tx = cx + (idx - selNdx) * step - (int)tileW / 2;
                int ty = cy - (int)tileH / 2;
                if (x >= tx && x < tx + (int)tileW
                    && y >= ty && y < ty + (int)tileH)
                    return idx;
            }
            if (d > 0) {
                idx = selNdx - d;
                if (idx >= 0) {
                    int tx = cx + (idx - selNdx) * step - (int)tileW / 2;
                    int ty = cy - (int)tileH / 2;
                    if (x >= tx && x < tx + (int)tileW
                        && y >= ty && y < ty + (int)tileH)
                        return idx;
                }
            }
        }
        return -1;
    }
}

//
// combine widgets into wi->tile
// for uiShow()
//
static void prepareTile(WindowInfo * wi)
{
    XGlyphInfo ext;
    int bottW = 0, bottH = 0, bottX = 0, bottY = 0;

    wi->tile = XCreatePixmap(dpy, root, tileW, tileH, XDEPTH);
    if (!wi->tile)
        die("can't create tile");
    int fr = XFillRectangle(dpy, wi->tile, g.gcReverse, 0, 0,
                            tileW, tileH);
    if (!fr) {
        msg(-1, "can't fill tile\n");
    }

    // In-tile preview: render window content instead of icon
    if (g.option_preview) {
        unsigned int srcW = 0, srcH = 0;
        Drawable src = None;

        XSync(dpy, False);
        ee_ignored = NULL;
        ee_complain = false;
        Pixmap named = XCompositeNameWindowPixmap(dpy, wi->id);
        XSync(dpy, False);
        bool haveComposite = (!ee_ignored && named != None);
        ee_complain = true;
        ee_ignored = NULL;

        if (haveComposite) {
            Window rr;
            int xr, yr;
            unsigned int bw, depth;
            if (XGetGeometry(dpy, named, &rr, &xr, &yr, &srcW, &srcH, &bw, &depth)
                && srcW > 0 && srcH > 0) {
                src = named;
            } else {
                XFreePixmap(dpy, named);
                named = None;
            }
        }

        if (src == None) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, wi->id, &wa) && wa.map_state == IsViewable
                && wa.width > 0 && wa.height > 0) {
                srcW = wa.width;
                srcH = wa.height;
                src = wi->id;
            }
        }

        if (srcW > 0 && srcH > 0) {
            pixmapFit(src, None, wi->tile, srcW, srcH, tileW, tileH);
        }

        if (named != None) {
            XSync(dpy, False);
            ee_ignored = NULL;
            ee_complain = false;
            XFreePixmap(dpy, named);
            XSync(dpy, False);
            ee_ignored = NULL;
            ee_complain = true;
        }

        // Top info strip (drawn regardless of preview success)
        if (wi->name[0] && fontLabel) {
            XGlyphInfo te;
            XftTextExtentsUtf8(dpy, fontLabel,
                (unsigned char *)(wi->name), strlen(wi->name), &te);
            int pad = 6;
            int stripH = te.height + pad * 2;
            if (stripH < 22) stripH = 22;
            if (wi->bottom_line[0]) {
                XftTextExtentsUtf8(dpy, fontLabel,
                    (unsigned char *)(wi->bottom_line),
                    strlen(wi->bottom_line), &ext);
                int blH = ext.height + pad * 2;
                if (blH > stripH) stripH = blH;
            }
            if (stripH > tileH / 2)
                stripH = tileH / 2;
            GC tgc = XCreateGC(dpy, root, 0, NULL);
            XSetForeground(dpy, tgc, g.color[COLINACT].xcolor.pixel);
            XSetFillStyle(dpy, tgc, FillSolid);
            XFillRectangle(dpy, wi->tile, tgc, 0, 0, tileW, stripH);
            // separator line below strip
            XSetForeground(dpy, tgc, g.color[COLFRAME].xcolor.pixel);
            XFillRectangle(dpy, wi->tile, tgc, 0, stripH, tileW, 1);
            XFreeGC(dpy, tgc);
            // title text: left-aligned with padding
            int textY = pad - 2;
            if (textY < 0) textY = 0;
            drawSingleLine(wi->tile, fontLabel, &(g.color[COLFG].xftcolor),
                           wi->name, 8, textY, tileW / 2, te.height);
            // bottom line: right-aligned in same strip
            if (wi->bottom_line[0]) {
                XftTextExtentsUtf8(dpy, fontLabel,
                    (unsigned char *)(wi->bottom_line),
                    strlen(wi->bottom_line), &ext);
                if (ext.width < tileW / 2) {
                    int bx = tileW - ext.width - 8;
                    drawSingleLine(wi->tile, fontLabel, &(g.color[COLFG].xftcolor),
                                   wi->bottom_line, bx, textY, ext.width, ext.height);
                }
            }
        }

        // Skip icon if we got a preview, or if preview was attempted but failed
        goto endTile;
    }

    // place icons
    if (g.option_iconSrc == ISRC_NONE)
        goto endIcon;
    if (wi->icon_drawable) {
        if (wi->icon_w == iconW && wi->icon_h == iconH) {
            // direct copy
            msg(1, "copying icon onto tile\n");
            // prepare special GC to copy icon, with clip mask if icon_mask present
            unsigned long ic_valuemask = 0;
            XGCValues ic_values;
            GC ic_gc = XCreateGC(dpy, root, ic_valuemask,
                                 &ic_values);
            if (ic_gc < 0) {
                msg(-1, "can't create GC to draw icon\n");
                goto endIcon;
            }
            if (wi->icon_mask != None) {
                XSetClipMask(dpy, ic_gc, wi->icon_mask);
            }
            int or = XCopyArea(dpy,
                               wi->icon_drawable,
                               wi->tile,
                               ic_gc, 0, 0,
                               wi->icon_w, wi->icon_h,  // src
                               0, 0);   // dst
            if (!or) {
                msg(-1, "can't copy icon to tile\n");
            }
            XFreeGC(dpy, ic_gc);
        } else {
            // scale
            msg(1, "scaling icon onto tile\n");
            int sc = pixmapFit(wi->icon_drawable,
                               wi->icon_mask,
                               wi->tile,
                               wi->icon_w,
                               wi->icon_h,
                               iconW, iconH);
            if (!sc) {
                msg(-1, "can't scale icon to tile\n");
            }
        }
    } else {
        // draw placeholder or standalone icons from some WM
        GC gcL = create_gc(0);  // GC for thin line
        if (!gcL) {
            msg(-1, "can't create gcL\n");
        } else {
            XSetLineAttributes(dpy, gcL, 1, LineSolid, CapButt, JoinMiter);
            //XSetForeground (dpy, gcL, pixel);
            int pr = XDrawRectangle(dpy, wi->tile, gcL,
                                    0, 0, iconW, iconH);
            if (!pr) {
                msg(-1, "can't draw placeholder\n");
            }
            XFreeGC(dpy, gcL);
        }
    }
 endIcon:

    // draw bottom line if there at least the same
    // space for main label as for bottom line
    if (wi->bottom_line[0] == '\0')
        goto endBottomLine;
    XftTextExtentsUtf8(dpy, fontLabel, 
            (unsigned char *)(wi->bottom_line), strlen(wi->bottom_line), &ext);
    msg(1, "bottom line of size %dx%d requested\n", ext.width, ext.height);
    if ((!g.option_vertical && (tileH - iconH - 5) / 2 >= ext.height + 1) 
     || ( g.option_vertical && (tileW - iconW - 5) / 2 >= ext.width + 5)) {
        bottW = ext.width;
        bottH = ext.height;
        bottX = tileW - bottW - 5; // 5 to avoid overlap with frame
        bottY = tileH - bottH - 1;
        int dr = drawSingleLine(wi->tile, fontLabel,
                 &(g.color[COLFG].xftcolor),
                 wi->bottom_line,
                 bottX, bottY, bottW, bottH);
        if (dr != 1) {
            msg(-1, "can't draw bottom line '%s'\n", wi->bottom_line);
        }
        msg(1, "bottom line '%s' drawn at %dx%d+%d+%d\n",
                wi->bottom_line, bottW, bottH, bottX, bottY);
    } else {
        msg(1, "bottom line skipped\n");
    }
endBottomLine:

    // draw label
    if (wi->name[0] && fontLabel) {
        int x, y, w, h;
        if (g.option_vertical) {
            x = iconW + 5;
            y = frameW; // avoids overlapping with frames
            w = tileW - iconW - 5 - bottW;
            if (bottW > 0) w = w - 5 - 5; // avoids label too close to bottom line
            h = tileH - frameW;
        } else {
            x = 0;
            y = iconH + 5;
            w = tileW;
            h = tileH - iconH - 5 - bottH;
        }
        int dr = drawMultiLine(wi->tile, fontLabel,
                               &(g.color[COLFG].xftcolor),
                               wi->name,
                               x, y, w, h);
        if (dr != 1) {
            msg(-1, "can't draw label\n");
        }
    }
endTile:
}                               // prepareTile

//
// grab auxiliary keys: arrows, cancel, kill
// rely on pre-calculated g.ignored_modmask and g.option_modMask
//
static int grabKeysAtUiShow(bool grabUngrab)
{
    char *grabhint =
        "Error while (un)grabbing key 0x%x with mask 0x%x/0x%x.\n";
#define nkeys 10
    KeyCode key[nkeys] = {
        g.option_prevCode,
        g.option_nextCode,
        g.option_cancelCode,
        g.option_killCode,
        g.option_upCode,
        g.option_downCode,
        g.option_leftCode,
        g.option_rightCode,
        g.option_prevWsCode,
        g.option_nextWsCode
    };
    int k;

    for (k = 0; k < nkeys; k++) {
        if (key[k] != 0) {
            if (!changeKeygrab
                (root, grabUngrab, key[k], g.option_modMask,
                 g.ignored_modmask)) {
                msg(0, grabhint, key[k], g.option_modMask, g.ignored_modmask);
                return 0;
            }
        }
    }
    return 1;
}

//
// copy single tile to canvas
//
static int placeSingleTile (int j) {
    int dest_x, dest_y, r;

    if (! g.winlist[j].tile)
        return -1;
    layoutPos(j, &dest_x, &dest_y);

    // Non-grid non-selected: distance-based scaling via XRender
    if (g.option_layout != LAYOUT_GRID && j != selNdx && uiwinPic) {
        int dist = abs(j - selNdx);
        int sw, sh;
        float scale = (dist == 1) ? 0.85f
                     : (dist == 2) ? 0.72f
                     : 0.60f;
        sw = (int)(tileW * scale + 0.5f);
        sh = (int)(tileH * scale + 0.5f);
        if (sw < 16 || sh < 16) return -1;
        int ox = dest_x + ((int)tileW - sw) / 2;
        int oy = dest_y + ((int)tileH - sh) / 2;

        bool old = ee_complain;
        ee_ignored = NULL;
        ee_complain = false;

        XRenderPictFormat *srcFmt = XRenderFindStandardFormat(dpy, PictStandardRGB24);
        Picture srcPic = XRenderCreatePicture(dpy, g.winlist[j].tile, srcFmt, 0, NULL);
        if (!srcPic) { ee_complain = old; goto fallback_copy; }

        double sx0 = (double)tileW / sw;
        double sy0 = (double)tileH / sh;
        XTransform t = {{
            {XDoubleToFixed(sx0), XDoubleToFixed(0), XDoubleToFixed(0)},
            {XDoubleToFixed(0), XDoubleToFixed(sy0), XDoubleToFixed(0)},
            {XDoubleToFixed(0), XDoubleToFixed(0), XDoubleToFixed(1.0)}
        }};
        XRenderSetPictureTransform(dpy, srcPic, &t);
        XRenderSetPictureFilter(dpy, srcPic, FilterBilinear, 0, 0);
        XRenderComposite(dpy, PictOpOver, srcPic, None, uiwinPic,
                         0, 0, 0, 0, ox, oy, sw, sh);
        XRenderFreePicture(dpy, srcPic);
        XRenderColor dc = {0, 0, 0, (dist == 1) ? 0x3000 : 0x5000};
        XRenderFillRectangle(dpy, PictOpOver, uiwinPic, &dc, ox, oy, sw, sh);
        XSync(dpy, False);
        ee_complain = old;
        return 1;
    }

fallback_copy:
    msg(1, "copying tile %d to canvas at %dx%d\n", j, dest_x, dest_y);
    if (g.option_cornerRadius > 0 && roundMask) {
        XSetClipMask(dpy, g.gcDirect, roundMask);
        XSetClipOrigin(dpy, g.gcDirect, dest_x, dest_y);
        r = XCopyArea(dpy, g.winlist[j].tile, uiwin,
                g.gcDirect, 0, 0, tileW, tileH, dest_x, dest_y);
        XSetClipMask(dpy, g.gcDirect, None);
    } else {
        r = XCopyArea(dpy, g.winlist[j].tile, uiwin,
                g.gcDirect, 0, 0, tileW, tileH, dest_x, dest_y);
    }
    //XSync (dpy, false);
    msg(1, "XCopyArea returned %d\n", r);
    return r;
}

//
// render all tiles in Z-order (back-to-front for stack)
// draws unselected frames + tiles together per Z-layer
//
static void renderAllTiles(void)
{
    if (g.option_layout == LAYOUT_GRID) {
        for (int j = 0; j < g.maxNdx; j++)
            placeSingleTile(j);
    } else {
        int maxD = g.maxNdx - 1;
        for (int d = maxD; d >= 0; d--) {
            for (int s = -1; s <= 1; s += 2) {
                int j = (s == -1) ? selNdx - d : selNdx + d;
                if (j < 0 || j >= g.maxNdx || j == selNdx) continue;
                placeSingleTile(j);
            }
        }
        // selected tile last
        placeSingleTile(selNdx);
    }
}

//
// render single tile with explicit animation state (x, y, scale, dim)
//
static int placeSingleTileAnim(int j, float dest_x, float dest_y,
                                float scale, float dim)
{
    if (!g.winlist[j].tile)
        return -1;

    if (g.option_layout != LAYOUT_GRID && uiwinPic) {
        int sw = (int)(tileW * scale + 0.5f);
        int sh = (int)(tileH * scale + 0.5f);
        if (sw < 16 || sh < 16) return -1;
        int ox = (int)dest_x + ((int)tileW - sw) / 2;
        int oy = (int)dest_y + ((int)tileH - sh) / 2;

        bool old = ee_complain;
        ee_complain = false;
        ee_ignored = NULL;

        XRenderPictFormat *srcFmt = XRenderFindStandardFormat(dpy, PictStandardRGB24);
        Picture srcPic = XRenderCreatePicture(dpy, g.winlist[j].tile, srcFmt, 0, NULL);
        if (!srcPic) { ee_complain = old; goto fallback_copy_anim; }

        double sx0 = (double)tileW / sw;
        double sy0 = (double)tileH / sh;
        XTransform t = {{
            {XDoubleToFixed(sx0), XDoubleToFixed(0), XDoubleToFixed(0)},
            {XDoubleToFixed(0), XDoubleToFixed(sy0), XDoubleToFixed(0)},
            {XDoubleToFixed(0), XDoubleToFixed(0), XDoubleToFixed(1.0)}
        }};
        XRenderSetPictureTransform(dpy, srcPic, &t);
        XRenderSetPictureFilter(dpy, srcPic, FilterBilinear, 0, 0);
        XRenderComposite(dpy, PictOpOver, srcPic, None, uiwinPic,
                         0, 0, 0, 0, ox, oy, sw, sh);
        XRenderFreePicture(dpy, srcPic);

        if (dim > 0.0f) {
            unsigned short alpha = (unsigned short)(dim * 0x10000);
            XRenderColor dc = {0, 0, 0, alpha};
            XRenderFillRectangle(dpy, PictOpOver, uiwinPic, &dc, ox, oy, sw, sh);
        }

        XSync(dpy, False);
        ee_complain = old;
        return 1;
    }

fallback_copy_anim:
    msg(1, "copying tile %d to canvas at %.0fx%.0f\n", j, dest_x, dest_y);
    if (g.option_cornerRadius > 0 && roundMask) {
        XSetClipMask(dpy, g.gcDirect, roundMask);
        XSetClipOrigin(dpy, g.gcDirect, (int)dest_x, (int)dest_y);
        int r = XCopyArea(dpy, g.winlist[j].tile, uiwin,
                g.gcDirect, 0, 0, tileW, tileH, (int)dest_x, (int)dest_y);
        XSetClipMask(dpy, g.gcDirect, None);
        return r;
    }
    int r = XCopyArea(dpy, g.winlist[j].tile, uiwin,
            g.gcDirect, 0, 0, tileW, tileH, (int)dest_x, (int)dest_y);
    return r;
}

//
// render all tiles with interpolated animation state
//
static void renderAllTilesAnim(float t)
{
    int maxD = g.maxNdx - 1;
    for (int d = maxD; d >= 0; d--) {
        for (int s = -1; s <= 1; s += 2) {
            int j = (s == -1) ? selNdx - d : selNdx + d;
            if (j < 0 || j >= g.maxNdx || j == selNdx) continue;

            float x = lerpf(animFrom[j].x, animTo[j].x, t);
            float y = lerpf(animFrom[j].y, animTo[j].y, t);
            float scale = lerpf(animFrom[j].scale, animTo[j].scale, t);
            float dim = lerpf(animFrom[j].dim, animTo[j].dim, t);
            placeSingleTileAnim(j, x, y, scale, dim);
        }
    }
    // selected tile last (on top)
    int j = selNdx;
    float x = lerpf(animFrom[j].x, animTo[j].x, t);
    float y = lerpf(animFrom[j].y, animTo[j].y, t);
    float scale = lerpf(animFrom[j].scale, animTo[j].scale, t);
    float dim = lerpf(animFrom[j].dim, animTo[j].dim, t);
    placeSingleTileAnim(j, x, y, scale, dim);
}

//
// draw frame at selected tile's interpolated position
//
static void framesRedrawAnim(float t)
{
    if (g.option_layout != LAYOUT_GRID) {
        float fx = lerpf(animFrom[selNdx].x, animTo[selNdx].x, t);
        float fy = lerpf(animFrom[selNdx].y, animTo[selNdx].y, t);
        int x = (int)fx - frameW / 2;
        int y = (int)fy - frameW / 2;
        int dw = tileW + frameW;
        int dh = tileH + frameW;
        if (g.option_cornerRadius > 0) {
            drawRoundedRectFrame(dpy, uiwin, g.gcFrame, x, y, dw, dh,
                                 g.option_cornerRadius);
        } else {
            XDrawRectangle(dpy, uiwin, g.gcFrame, x, y, dw, dh);
        }
        return;
    }
    framesRedraw();
}

// PUBLIC

//
// early initialization
// called once per execution
// mostly initializes g.*

int startupGUItasks(void)
{
// if viewport is not fixed, then initialize vp* at every show
    if (g.option_vp_mode == VP_SPECIFIC) {
        g.vp = g.option_vp;
    }
    g.has_randr = randrAvailable();
// colors
    colormap = DefaultColormap(dpy, scr);
    visual = DefaultVisual(dpy, scr);
    msg(0, "early allocating colors\n");
    srand(time(NULL));
    int p;
    for (p = 0; p < NCOLORS; p++) {
        if (g.color[p].name[0]) {
            if (strncmp(g.color[p].name, "_rnd_", 5) == 0) {
                // replace in-place: 8 chars is sufficient for #rrggbb
                char r[3];
                short int rc;
                for (rc = 0; rc <= 2; rc++) {
                    r[rc] = rand() / (RAND_MAX / 0x80);
                }
                if (strncmp(g.color[p].name, "_rnd_low", 8) == 0) {
                    (void)snprintf(g.color[p].name, 8,
                                   "#%.2hhx%.2hhx%.2hhx", r[0], r[1], r[2]);
                    g.color[p].name[7] = '\0';
                } else if (strncmp(g.color[p].name, "_rnd_high", 9)
                           == 0) {
                    (void)snprintf(g.color[p].name, 9,
                                   "#%.2hhx%.2hhx%.2hhx",
                                   r[0] + 0x80, r[0] + 0x80, r[1] + 0x80);
                    g.color[p].name[7] = '\0';
                }
                msg(1,
                    "color generated: %s, RAND_MAX=%d\n",
                    g.color[p].name, RAND_MAX);
            }
            if (!XAllocNamedColor(dpy,
                                  colormap,
                                  g.color[p].name,
                                  &(g.color[p].xcolor), &(g.color[p].xcolor)))
                die("failed to allocate X color: ", g.color[p].name);
            if (!XftColorAllocName
                (dpy, visual, colormap, g.color[p].name,
                 &(g.color[p].xftcolor)))
                die("failed to allocate Xft color: ", g.color[p].name);
        }
    }

    msg(0, "early opening font\n");
//fontLabel = XLoadFont (dpy, LABELFONT);  // using Xft instead
    fontLabel = XftFontOpenName(dpy, scr, g.option_font);
    if (!fontLabel) {
        msg(-1,
            "can't allocate font: %s\ncheck installed fontconfig fonts: fc-list\n",
            g.option_font);
    }
// having colors, GC may be built
// they are required early for addWindow when transforming icon depth
    msg(0, "early building GCs\n");
    g.gcDirect = create_gc(0);
    g.gcReverse = create_gc(1);
    g.gcFrame = create_gc(2);
    g.gcMin = create_gc(3);

    return 1;
}

//
// calculate grid/carousel layout from available space
//
static void calcLayout(int avail_w, int avail_h)
{
    if (g.option_layout == LAYOUT_GRID) {
        if (g.option_vertical) {
            rows = (avail_h - frameW - 2 * g.option_winPad + g.option_spacing)
                 / (tileH + frameW + g.option_spacing);
            if (rows < 1) rows = 1;
            if (rows > g.maxNdx) rows = g.maxNdx;
            cols = (g.maxNdx + rows - 1) / rows;
        } else {
            cols = (avail_w - frameW - 2 * g.option_winPad + g.option_spacing)
                 / (tileW + frameW + g.option_spacing);
            if (cols < 1) cols = 1;
            if (cols > g.maxNdx) cols = g.maxNdx;
            rows = (g.maxNdx + cols - 1) / cols;
        }
        uiwinW = cols * tileW + (cols + 1) * frameW
                 + g.option_spacing * (cols - 1) + 2 * g.option_winPad;
        uiwinH = rows * tileH + (rows + 1) * frameW
                 + g.option_spacing * (rows - 1) + 2 * g.option_winPad;
    } else {
        int step = (tileW - (tileW > 40 ? tileW * 50 / 100 : tileW / 2));
        if (step < 1) step = 1;
        int nvis = g.maxNdx < 7 ? g.maxNdx : 7;
        uiwinW = nvis * tileW - (nvis - 1) * (tileW - step)
                 + 2 * frameW + 2 * g.option_winPad;
        uiwinH = tileH + 2 * frameW + 2 * g.option_winPad;
        cols = nvis;
        rows = 1;
    }
    if (g.option_positioning == POS_CENTER) {
        uiwinX = (g.vp.w - uiwinW) / 2 + g.vp.x;
        uiwinY = (g.vp.h - uiwinH) / 2 + g.vp.y;
    } else {
        uiwinX = g.option_posX + g.vp.x;
        uiwinY = g.option_posY + g.vp.y;
    }
    visualTileW = tileW + frameW + g.option_spacing;
    visualTileH = tileH + frameW + g.option_spacing;
}

static void setSizeHints(void)
{
    long sflags = USPosition | USSize | PPosition | PSize | PMinSize | PMaxSize | PBaseSize;
    if (g.option_positioning != POS_NONE)
        sflags |= PWinGravity;
    int max_w = g.vp.w > 0 ? g.vp.w : 8192;
    int max_h = g.vp.h > 0 ? g.vp.h : 8192;
    int gravity = (g.option_positioning == POS_CENTER && g.option_vp_mode != VP_SPECIFIC)
        ? CenterGravity : StaticGravity;
    XSizeHints hints = {
        .flags = sflags,
        .x = uiwinX, .y = uiwinY,
        .width = uiwinW, .height = uiwinH,
        .min_width = uiwinW, .min_height = uiwinH,
        .max_width = max_w, .max_height = max_h,
        .width_inc = 0, .height_inc = 0,
        .min_aspect = {0, 0}, .max_aspect = {0, 0},
        .base_width = uiwinW, .base_height = uiwinH,
        .win_gravity = gravity,
    };
    XSetWMNormalHints(dpy, uiwin, &hints);
}

static void applyWindowShapeMask(void)
{
    if (g.option_cornerRadius <= 0) return;
    int ev, err;
    if (!XShapeQueryExtension(dpy, &ev, &err)) return;
    Pixmap winMask = createRoundedRectMask(uiwinW, uiwinH, g.option_cornerRadius);
    if (!winMask) return;
    XShapeCombineMask(dpy, uiwin, ShapeBounding, 0, 0, winMask, ShapeSet);
    XFreePixmap(dpy, winMask);
}

static int initAndLayout(void)
{
    if (!initWinlist())
        return 0;
    if (!g.winlist || g.maxNdx < 1)
        return 0;
    int avail_w = g.vp.w - (g.option_positioning == POS_SPECIFIC ? g.option_posX : 0);
    int avail_h = g.vp.h - (g.option_positioning == POS_SPECIFIC ? g.option_posY : 0);
    calcLayout(avail_w, avail_h);
    for (int m = 0; m < g.maxNdx; m++)
        prepareTile(&(g.winlist[m]));
    return 1;
}

//
// called on alt-tab keypress to draw popup
// build g.winlist
// create our window
// sets g.uiShowHasRun (if set, then call uiHide to free X stuff)
//
// returns 1 if our window is ready to Expose, 0 otherwise
// direction is direction of first press: with shift or without
//
int uiShow(bool direction)
{
    msg(0, "preparing ui\n");
    g.uiShowHasRun = true;      // begin allocations
    g.viewDesktop = DESKTOP_UNKNOWN;  // start with real current desktop
// screen-related stuff is not at startup but here,
// because screen configuration may get changed at runtime
// moreover, DisplayWidth/Height aren't changed without
// reconnecting to X server, that's why root geometry is used
    XWindowAttributes ra;
    if (XGetWindowAttributes(dpy, root, &ra) != 0) {
        scrdim.x = ra.x;
        scrdim.y = ra.y;
        scrdim.w = ra.width;
        scrdim.h = ra.height;
    } else {
        msg(-1, "can't get root window attributes, using screen dimensions\n");
        scrdim.x = scrdim.y = 0;
        scrdim.w = DisplayWidth(dpy, scr);
        scrdim.h = DisplayHeight(dpy, scr);
    }
// calculate viewport.
#define VPM  g.option_vp_mode
    switch (VPM) {
    case VP_SPECIFIC:
        // initialized at startup instead
        break;
    case VP_TOTAL:
        g.vp = scrdim;
        break;
    case VP_FOCUS:
    case VP_POINTER:
        if (g.has_randr) {
            bool multihead;
            if (!randrGetViewport(&(g.vp), &multihead)) {
                msg(0,
                    "can't obtain viewport from randr, using default screen\n");
                g.vp = scrdim;
            }
            if (!multihead) {
                msg(0,
                    "randr reports single head, using default screen instead\n");
                g.vp = scrdim;
            }
        } else {
            msg(0, "no randr, using default screen as viewport\n");
            g.vp = scrdim;
        }
        break;
    default:
        msg(-1, "unknown viewport mode, using default screen\n");
        g.vp = scrdim;
    }

    XClassHint class_h = { XCLASSNAME, XCLASS };

    tileW = g.option_tileW;
    tileH = g.option_tileH;
    if (g.option_iconSrc != ISRC_NONE) {
        iconW = g.option_iconW;
        iconH = g.option_iconH;
    } else {
        iconW = iconH = 0;
    }

    if (!initAndLayout()) {
        msg(0, "initWinlist or layout failed, skipping ui initialization\n");
        return 0;
    }

    int backNdx = g.maxNdx - 1;
    int forwNdx = (0 >= (g.maxNdx - 1) || g.option_desktop == DESK_NOCURRENT) ? 0 : 1;
    selNdx = direction ? backNdx : forwNdx;
    msg(1, "Current (selected) item in winlist: %d\n", selNdx);

    if (iconW > tileW || iconH > tileH) {
        float rt = (float)tileW / (float)iconW;
        if (iconW > tileW) {
            iconW = tileW;
            iconH = rt * iconH;
        }
        if (iconH > tileH) {
            rt = (float)tileH / (float)iconH;
            iconH = tileH;
            iconW = rt * iconW;
        }
    }

    visualTileW = tileW + frameW + g.option_spacing;
    visualTileH = tileH + frameW + g.option_spacing;
    if (g.debug > 0) {
        msg(0, "got %d windows, tile %dx%d, uiwin %dx%d +%d+%d\n",
            g.maxNdx, tileW, tileH, uiwinW, uiwinH, uiwinX, uiwinY);
        if (g.debug > 1) {
            int nscr = ScreenCount(dpy);
            msg(0, "%d screen(s):", nscr);
            for (int si = 0; si < nscr; ++si) {
                Screen *s = ScreenOfDisplay(dpy, si);
                msg(0, " [%dx%d]", s->width, s->height);
            }
            msg(0, ", viewport %dx%d+%d+%d",
                g.vp.w, g.vp.h, g.vp.x, g.vp.y);
        }
        msg(0, "\n");
    }
    if (g.debug > 0) {
        for (int i = 0; i < g.maxNdx; i++) {
            msg(0,
                "%d: %lx (lvl %d, icon %lu (%dx%d)): %s\n", i,
                g.winlist[i].id, g.winlist[i].reclevel,
                g.winlist[i].icon_drawable, g.winlist[i].icon_w,
                g.winlist[i].icon_h, g.winlist[i].name);
        }
    }

// prepare our window
    unsigned long valuemask = CWBackPixel | CWBorderPixel | CWOverrideRedirect;
    XSetWindowAttributes attributes;
    attributes.background_pixel = g.color[COLBG].xcolor.pixel;
    attributes.border_pixel = g.color[COLBORDER].xcolor.pixel;
    attributes.override_redirect = 1;
    uiwin = XCreateWindow(dpy, root, uiwinX, uiwinY, uiwinW, uiwinH, g.option_borderW, // border_width
                          CopyFromParent,   // depth
                          InputOutput,  // class
                          CopyFromParent,   // visual
                          valuemask, &attributes);
    if (uiwin <= 0)
        die("can't create window");
    msg(0, "our window is 0x%lx\n", uiwin);

// set properties of our window
    XStoreName(dpy, uiwin, XWINNAME);
    XSetClassHint(dpy, uiwin, &class_h);
// warning: this overwrites any previous value.
// note: x_setCommonPropertiesForAnyWindow does the same thing for any window
    XSelectInput(dpy, uiwin, ExposureMask | KeyPressMask | KeyReleaseMask
                 | ButtonPressMask | ButtonReleaseMask);
    grabKeysAtUiShow(true);
// set window type so that WM will hopefully not resize it
// before mapping: https://specifications.freedesktop.org/wm-spec/1.3/ar01s05.html
    Atom at = XInternAtom(dpy, "ATOM", True);
    Atom wt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom td = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    if (at && wt && td)
        XChangeProperty(dpy, uiwin, wt, at, 32, PropModeReplace,
                        (unsigned char *)(&td), 1);
// disable appearance in taskbar
    Atom st = XInternAtom(dpy, "_NET_WM_STATE", True);
    Atom sk = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", True); // there is also PAGER
    if (at && st && sk)
        XChangeProperty(dpy, uiwin, st, at, 32, PropModeReplace,
                        (unsigned char *)(&sk), 1);
// xmonad ignores _NET_WM_WINDOW_TYPE_DIALOG but obeys WM_TRANSIENT_FOR
    XSetTransientForHint(dpy, uiwin, uiwin);
// disable window title and borders. works in xfwm4.
#define PROP_MOTIF_WM_HINTS_ELEMENTS 5
#define MWM_HINTS_DECORATIONS (1L << 1)
    struct {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long inputMode;
        unsigned long status;
    } hints = {
    MWM_HINTS_DECORATIONS, 0, 0,};
    Atom ma = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    if (ma) {
        XChangeProperty(dpy, uiwin, ma, ma, 32, PropModeReplace,
                        (unsigned char *)&hints, PROP_MOTIF_WM_HINTS_ELEMENTS);
    }

    XMapWindow(dpy, uiwin);

    setSizeHints();

    if (g.option_cornerRadius > 0) {
        int ev, err;
        if (!XShapeQueryExtension(dpy, &ev, &err)) {
            msg(-1, "X Shape extension not available, disabling rounded corners\n");
            g.option_cornerRadius = 0;
        } else {
            roundMask = createRoundedRectMask(tileW, tileH, g.option_cornerRadius);
            applyWindowShapeMask();
        }
    }

    if (g.option_layout != LAYOUT_GRID) {
        XRenderPictFormat *visFmt = XRenderFindVisualFormat(dpy, visual);
        if (visFmt) {
            uiwinPic = XRenderCreatePicture(dpy, uiwin, visFmt, 0, NULL);
        } else {
            uiwinPic = 0;
        }
    } else {
        uiwinPic = 0;
    }

    return 1;
}

// forward declarations for animation functions
static float advanceAnimation(void);

//
// Expose event callback
// redraw our window
//
void uiExpose(void)
{
    msg(0, "expose ui\n");
// if WM moved uiwin, here is the place
// where we first see 'bad' absolute coordinates.
// try to correct them.
    quad uwq;
    if (get_absolute_coordinates(uiwin, &uwq)) {
// debug for #54
        msg(1, "attr abs at expose: %dx%d +%d+%d\n",
            uwq.w, uwq.h, uwq.x, uwq.y);
        int xdiff = uwq.x - uiwinX;
        int ydiff = uwq.y - uiwinY;
        if (abs(xdiff) > frameW / 2 || abs(ydiff) > frameW / 2) {
            msg(1, "WM moved uiwin too far, trying to correct\n");
            XMoveWindow(dpy, uiwin, uiwinX, uiwinY);
        }
        if (uwq.w != uiwinW || uwq.h != uiwinH) {
            // WM resized our window, like
            // floating_maximum_size in #54.
            // there is little can be done here,
            // so just complain.
            msg(-1,
                "switcher window resized, expect bugs. Please configure WM to not interfere with alttab window size, for example, disable 'floating_maximum_size' in i3\n");
        }
    }
    if (g.option_cornerRadius > 0)
        XClearWindow(dpy, uiwin);
    float t = advanceAnimation();
    if (t >= 0.0f) {
        renderAllTilesAnim(t);
        framesRedrawAnim(t);
    } else {
        renderAllTiles();
        framesRedraw();
    }
}

//
// advance animation timer, return eased t (0..1) if animating,
// -1 if animation just ended (caller should render final),
// -2 if no animation active
//
static float advanceAnimation(void)
{
    if (!animActive) return -2.0f;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ns = (now.tv_sec - animStartTime.tv_sec) * 1000000000L
                    + (now.tv_nsec - animStartTime.tv_nsec);
    float p = (float)elapsed_ns / ANIM_DURATION_NS;
    if (p >= 1.0f) {
        animProgress = 1.0f;
        animActive = false;
        return -1.0f;
    }
    animProgress = p;
    return easeOutQuad(p);
}

//
// start or restart animation toward newSel
//
static void startAnimation(int newSel)
{
    if (!uiwin || g.option_layout == LAYOUT_GRID || newSel == selNdx)
        return;

    if (animFrom) free(animFrom);
    if (animTo) free(animTo);
    animFrom = malloc(g.maxNdx * sizeof(AnimTileState));
    animTo = malloc(g.maxNdx * sizeof(AnimTileState));
    if (!animFrom || !animTo) {
        if (animFrom) { free(animFrom); animFrom = NULL; }
        if (animTo) { free(animTo); animTo = NULL; }
        animActive = false;
        selNdx = newSel;
        XClearWindow(dpy, uiwin);
        renderAllTiles();
        framesRedraw();
        return;
    }

    if (animActive) {
        // snap current interpolated state as new from-state
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ns = (now.tv_sec - animStartTime.tv_sec) * 1000000000L
                        + (now.tv_nsec - animStartTime.tv_nsec);
        float p = (float)elapsed_ns / ANIM_DURATION_NS;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        float t = easeOutQuad(p);
        for (int j = 0; j < g.maxNdx; j++) {
            animFrom[j].x = lerpf(animFrom[j].x, animTo[j].x, t);
            animFrom[j].y = lerpf(animFrom[j].y, animTo[j].y, t);
            animFrom[j].scale = lerpf(animFrom[j].scale, animTo[j].scale, t);
            animFrom[j].dim = lerpf(animFrom[j].dim, animTo[j].dim, t);
        }
    } else {
        for (int j = 0; j < g.maxNdx; j++)
            computeTileState(j, selNdx, &animFrom[j]);
    }

    selNdx = newSel;
    for (int j = 0; j < g.maxNdx; j++)
        computeTileState(j, selNdx, &animTo[j]);

    animActive = true;
    animProgress = 0.0f;
    clock_gettime(CLOCK_MONOTONIC, &animStartTime);

    // render first frame
    float first_t = easeOutQuad(0.0f);
    XClearWindow(dpy, uiwin);
    renderAllTilesAnim(first_t);
    framesRedrawAnim(first_t);
    XFlush(dpy);
}

//
// render animation frame if animation is active
// called from event loop
//
void uiAnimRender(void)
{
    float t = advanceAnimation();
    if (t >= 0.0f) {
        XClearWindow(dpy, uiwin);
        renderAllTilesAnim(t);
        framesRedrawAnim(t);
        XFlush(dpy);
    } else if (t == -1.0f) {
        // animation just ended, render final state
        XClearWindow(dpy, uiwin);
        renderAllTiles();
        framesRedraw();
        XFlush(dpy);
    }
}

//
// remove ui and switch to chosen window
//
int uiHide(void)
{
    animActive = false;
    if (animFrom) { free(animFrom); animFrom = NULL; }
    if (animTo) { free(animTo); animTo = NULL; }
    grabKeysAtUiShow(false);
    // free XRender Pictures before destroying the window
    // (destroying a window auto-destroys Pictures on it)
    if (uiwinPic) {
        XRenderFreePicture(dpy, uiwinPic);
        uiwinPic = 0;
    }
    // order is important: to set focus in Metacity,
    // our window must be destroyed first
    if (uiwin) {
        msg(0, "destroying our window\n");
        XUnmapWindow(dpy, uiwin);
        XDestroyWindow(dpy, uiwin);
        uiwin = 0;
    }
    if (g.winlist) {
        msg(0, "changing focus to 0x%lx\n", g.winlist[selNdx].id);
        /*
           // save the switch moment for detecting
           // subsequent false focus event from WM
           gettimeofday(&(g.last.tv), NULL);
           g.last.prev = g.winlist[g.startNdx].id;
           g.last.to = g.winlist[selNdx].id;
         */
        setFocus(selNdx);     // before winlist destruction!
    }
    msg(0, "destroying tiles\n");
    int y;
    for (y = 0; y < g.maxNdx; y++) {
        if (g.winlist && g.winlist[y].tile) {
            XFreePixmap(dpy, g.winlist[y].tile);
            g.winlist[y].tile = 0;
        }
    }
    if (g.winlist) {
        freeWinlist();
    }
    if (roundMask) {
        XFreePixmap(dpy, roundMask);
        roundMask = 0;
    }
    g.uiShowHasRun = false;
    return 1;
}

//
// select next item in g.winlist
//
int uiNextWindow(void)
{
    if (!uiwin)
        return 0;
    int newSel = selNdx + 1;
    if (newSel >= g.maxNdx)
        newSel = 0;
    msg(0, "item %d\n", newSel);
    if (g.option_layout != LAYOUT_GRID) {
        startAnimation(newSel);
    } else {
        selNdx = newSel;
        framesRedraw();
    }
    return 1;
}

//
// select previous item in g.winlist
//
int uiPrevWindow(void)
{
    if (!uiwin)
        return 0;
    int newSel = selNdx - 1;
    if (newSel < 0)
        newSel = g.maxNdx - 1;
    msg(0, "item %d\n", newSel);
    if (g.option_layout != LAYOUT_GRID) {
        startAnimation(newSel);
    } else {
        selNdx = newSel;
        framesRedraw();
    }
    return 1;
}

//
// directional navigation: move selection up in the grid
//
static int uiMoveFocus(int dc, int dr)
{
    if (!uiwin || g.option_layout != LAYOUT_GRID)
        return 0;
    int col, row;
    if (g.option_vertical) {
        row = selNdx % rows;
        col = selNdx / rows;
    } else {
        col = selNdx % cols;
        row = selNdx / cols;
    }
    if (dc) col = (col + dc + cols) % cols;
    if (dr) row = (row + dr + rows) % rows;
    int newSel = g.option_vertical ? col * rows + row : row * cols + col;
    if (newSel >= g.maxNdx)
        newSel = g.maxNdx - 1;
    msg(0, "item %d\n", newSel);
    selNdx = newSel;
    framesRedraw();
    return 1;
}

int uiUpWindow(void)    { return uiMoveFocus(0, -1); }
int uiDownWindow(void)  { return uiMoveFocus(0,  1); }
int uiLeftWindow(void)  { return uiMoveFocus(-1, 0); }
int uiRightWindow(void) { return uiMoveFocus( 1, 0); }

// rebuild UI after workspace switch:
// free tiles, re-init winlist, recalculate layout, redraw
//
static int rebuildUi(void)
{
    for (int m = 0; m < g.maxNdx; m++) {
        if (g.winlist && g.winlist[m].tile) {
            XFreePixmap(dpy, g.winlist[m].tile);
            g.winlist[m].tile = 0;
        }
    }
    freeWinlist();

    if (!initAndLayout()) {
        uiHide();
        return 0;
    }

    selNdx = 0;

    setSizeHints();
    XMoveResizeWindow(dpy, uiwin, uiwinX, uiwinY, uiwinW, uiwinH);
    applyWindowShapeMask();

    XSync(dpy, False);
    {
        Window rroot;
        int rx, ry;
        unsigned int rw, rh, rb, rd;
        if (XGetGeometry(dpy, uiwin, &rroot, &rx, &ry, &rw, &rh, &rb, &rd)) {
            if ((int)rw != uiwinW || (int)rh != uiwinH) {
                uiwinW = rw;
                uiwinH = rh;
                if (g.option_layout == LAYOUT_GRID) {
                    if (g.option_vertical)
                        rows = (uiwinH - frameW - 2 * g.option_winPad + g.option_spacing)
                             / (tileH + frameW + g.option_spacing);
                    else
                        cols = (uiwinW - frameW - 2 * g.option_winPad + g.option_spacing)
                             / (tileW + frameW + g.option_spacing);
                    if (rows < 1) rows = 1;
                    if (rows > g.maxNdx) rows = g.maxNdx;
                    if (cols < 1) cols = 1;
                    if (cols > g.maxNdx) cols = g.maxNdx;
                }
            }
        }
    }

    XClearWindow(dpy, uiwin);
    renderAllTiles();
    framesRedraw();
    XFlush(dpy);
    return 1;
}

static int findNeighborDesktop(long *viewports, int count, unsigned long cur, bool next)
{
    long cx = viewports[cur * 2];
    int best = -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            if ((unsigned long)i == cur) continue;
            long vx = viewports[i * 2];
            if (vx == cx) continue;
            if (pass == 0 && (next ? vx <= cx : vx >= cx)) continue;
            if (best < 0 || (next ? vx < viewports[best * 2] : vx > viewports[best * 2]))
                best = i;
        }
        if (best >= 0) break;
    }
    return best;
}

static int findI3VisibleWorkspace(long *viewports, int count, long target_x)
{
    FILE *fp = popen("i3-msg -t get_workspaces 2>/dev/null", "r");
    if (!fp) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    int rc = pclose(fp);
    if (rc != 0 || n == 0) return -1;
    buf[n] = '\0';
    int idx = 0;
    char *p = buf;
    while ((p = strchr(p, '{')) != NULL) {
        char *end = strchr(p, '}');
        if (!end) break;
        char *np = strstr(p, "\"num\":");
        if (np && np < end) {
            strtol(np + 6, &np, 10);
            char *vp = strstr(np, "\"visible\":");
            char *rp = strstr(np, "\"rect\"");
            if (vp && vp < end && rp && rp < end
                && strncmp(vp + 10, "true", 4) == 0) {
                char *xp = strstr(rp, "\"x\":");
                if (xp && xp < end) {
                    long rx = strtol(xp + 4, NULL, 10);
                    if (rx == target_x && idx < count && viewports[idx * 2] == target_x)
                        return idx;
                }
            }
        }
        idx++;
        p = end + 1;
    }
    return -1;
}

static int adjacentWorkspace(bool next)
{
    int count;
    long *viewports = ewmh_getViewports(&count);
    if (!viewports) return -1;

    unsigned long cur = g.viewDesktop != DESKTOP_UNKNOWN
        ? g.viewDesktop : ewmh_getCurrentDesktop();
    if (cur >= (unsigned long)count || cur == DESKTOP_UNKNOWN) { free(viewports); return -1; }

    int target = findNeighborDesktop(viewports, count, cur, next);
    if (target < 0) { free(viewports); return -1; }

    int i3_target = findI3VisibleWorkspace(viewports, count, viewports[target * 2]);
    if (i3_target >= 0) { free(viewports); return i3_target; }

    free(viewports);
    return target;
}

static int switchWorkspace(int next)
{
    if (!uiwin)
        return 0;

    int target = adjacentWorkspace(next);
    if (target < 0) {
        unsigned long current = g.viewDesktop != DESKTOP_UNKNOWN
            ? g.viewDesktop : ewmh_getCurrentDesktop();
        unsigned long nDesktops = ewmh_getNumberOfDesktops();
        if (nDesktops < 2) return 0;
        target = next
            ? (current + 1) % nDesktops
            : (current == 0 ? nDesktops - 1 : current - 1);
    }

    g.viewDesktop = target;
    return rebuildUi();
}

int uiNextWorkspace(void) { return switchWorkspace(true);  }
int uiPrevWorkspace(void) { return switchWorkspace(false); }

//
// kill X client of current window
//
int uiKillWindow(void)
{
    Window w;
    char *n;

    if (!uiwin)
        return 0;
    WindowInfo wi = g.winlist[selNdx];
    w = wi.id;
    n = wi.name;
    msg(0, "killing client of window %d, %s\n", w, n);
    if (XKillClient(dpy, w) == BadValue) {
        msg(-1, "can't kill X client\n");
        return 0;
    }
    msg(1, "blanking tile %d\n", selNdx);
    if (! XFillRectangle(dpy, wi.tile, g.gcReverse, 0, 0,
                            tileW, tileH)) {
        msg(-1, "can't fill tile\n");
        return 0;
    }
    if (! placeSingleTile(selNdx)) {
        msg(-1, "can't copy tile to canvas\n");
        return 0;
    }
    return 1;
}

//
// select item in g.winlist
//
int uiSelectWindow(int ndx)
{
    if (!uiwin)
        return 0;               // kb events may trigger it even when no window drawn yet
    if (ndx < 0 || ndx >= g.maxNdx) {
        return 0;
    }
    msg(0, "item %d\n", ndx);
    if (g.option_layout != LAYOUT_GRID) {
        startAnimation(ndx);
    } else {
        selNdx = ndx;
        framesRedraw();
    }
    return 1;
}

//
// mouse press/release handler
//
void uiButtonEvent(XButtonEvent e)
{
    if (!uiwin)
        return;
    if (e.type == ButtonPress) {
        switch (e.button) {
        case 1:
            lastPressedTile = pointedTile(e.x, e.y);
            if (lastPressedTile != -1)
                uiSelectWindow(lastPressedTile);
            break;
        case 4:
            uiPrevWindow();
            break;
        case 5:
            uiNextWindow();
            break;
        }
    }
    if (e.type == ButtonRelease && e.button == 1) {
        if (lastPressedTile != -1 && lastPressedTile == pointedTile(e.x, e.y))
            uiHide();
    }
}

//
// our window
//
Window getUiwin(void)
{
    return uiwin;
}

void shutdownGUI(void)
{
    int p;

    for (p=0; p<NCOLORS; p++) {
        XftColorFree(dpy,
                     DefaultVisual(dpy,0),
                     DefaultColormap(dpy,0),
                     &g.color[p].xftcolor);
            // XFreeColors ?
    }

    //XftFontClose(dpy, fontLabel); // actually, not needed

    if (g.gcDirect)
        XFreeGC(dpy, g.gcDirect);
    if (g.gcMin)
        XFreeGC(dpy, g.gcMin);
    if (g.gcReverse)
        XFreeGC(dpy, g.gcReverse);
    if (g.gcFrame)
        XFreeGC (dpy, g.gcFrame);
}
