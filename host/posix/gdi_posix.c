/* (linux) The bit of GDI the Insert menu and the FPS counter draw with (gpu/vk/vk_menu.cpp): a 32-bit DIB section,
   solid rectangles and lines of text, the text rasterised by stb_truetype (third_party/stb) from the system's sans
   font (fontconfig's choice: fc-match; else a few usual paths). Built into hookshot_vk.so. */
#define _GNU_SOURCE
#include <windows.h>

#include <math.h>
#include <pthread.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

enum { G_DC = 0x47444301, G_BITMAP, G_FONT, G_BRUSH };
typedef struct { int type; } GObj;
typedef struct { GObj o; int w, h; uint32_t *bits; } GBitmap;
typedef struct { GObj o; int px, bold, aa; } GFont;
typedef struct { GObj o; COLORREF c; } GBrush;
typedef struct { GObj o; GBitmap *bmp; GFont *font; COLORREF text; UINT align; int bk; } GDc;

/* ---- the fonts: regular and bold, loaded once ---- */
typedef struct { unsigned char *data; stbtt_fontinfo info; int ok; } Face;
static Face faces[2];
static pthread_once_t faces_once = PTHREAD_ONCE_INIT;

static unsigned char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = n > 0 ? (unsigned char *)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) free(d), d = NULL;
    fclose(f);
    return d;
}
static int face_load(Face *face, const char *path) {
    unsigned char *d = read_all(path);
    if (!d) return 0;
    const int off = stbtt_GetFontOffsetForIndex(d, 0);
    if (off < 0 || !stbtt_InitFont(&face->info, d, off)) { free(d); return 0; }
    face->data = d, face->ok = 1;
    return 1;
}
static int fc_match(const char *pattern, char *out, size_t cap) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s' 2>/dev/null", pattern);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    const size_t n = fread(out, 1, cap - 1, p);
    pclose(p);
    out[n] = 0;
    return n > 0;
}
static void faces_load(void) {
    static const char *regular[] = {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                    "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                                    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"};
    static const char *bold[] = {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                 "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                 "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                 "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                                 "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"};
    char path[4096];
    /* (a font beside the game first: fonts/regular.ttf, fonts/bold.ttf) */
    GetModuleFileNameA(NULL, path, sizeof path);
    char *slash = strrchr(path, '/');
    if (slash) {
        snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "fonts/regular.ttf");
        face_load(&faces[0], path);
        snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "fonts/bold.ttf");
        face_load(&faces[1], path);
    }
    if (!faces[0].ok && fc_match("sans-serif", path, sizeof path)) face_load(&faces[0], path);
    if (!faces[1].ok && fc_match("sans-serif:bold", path, sizeof path)) face_load(&faces[1], path);
    for (size_t i = 0; !faces[0].ok && i < sizeof regular / sizeof *regular; i++) face_load(&faces[0], regular[i]);
    for (size_t i = 0; !faces[1].ok && i < sizeof bold / sizeof *bold; i++) face_load(&faces[1], bold[i]);
    if (!faces[1].ok && faces[0].ok) faces[1] = faces[0];
    if (!faces[0].ok && faces[1].ok) faces[0] = faces[1];
    if (!faces[0].ok) fprintf(stderr, "[vk] no font found (fontconfig's sans-serif): the Insert menu has no text\n");
}
static Face *face_of(const GFont *f) {
    pthread_once(&faces_once, faces_load);
    Face *face = &faces[f && f->bold ? 1 : 0];
    return face->ok ? face : NULL;
}

/* ---- objects ---- */
HDC CreateCompatibleDC(HDC dc) {
    (void)dc;
    GDc *d = (GDc *)calloc(1, sizeof *d);
    d->o.type = G_DC, d->bk = OPAQUE;
    return d;
}
BOOL DeleteDC(HDC dc) { free(dc); return TRUE; }
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset) {
    (void)dc, (void)usage, (void)section, (void)offset;
    const int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    if (bi->bmiHeader.biBitCount != 32 || w <= 0 || h <= 0) return NULL;
    GBitmap *b = (GBitmap *)calloc(1, sizeof *b);
    b->o.type = G_BITMAP, b->w = w, b->h = h;
    b->bits = (uint32_t *)calloc((size_t)w * h, 4);   /* (top-down: the menu asks for a negative height) */
    *bits = b->bits;
    return b;
}
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj) {
    GDc *d = (GDc *)hdc;
    if (!d || !obj) return NULL;
    void *old = NULL;
    switch (((GObj *)obj)->type) {
    case G_BITMAP: old = d->bmp, d->bmp = (GBitmap *)obj; break;
    case G_FONT: old = d->font, d->font = (GFont *)obj; break;
    }
    return old;
}
BOOL DeleteObject(HGDIOBJ obj) {
    if (!obj) return FALSE;
    if (((GObj *)obj)->type == G_BITMAP) free(((GBitmap *)obj)->bits);
    free(obj);
    return TRUE;
}
BOOL GdiFlush(void) { return TRUE; }
HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strikeout, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality, DWORD pitch,
                  LPCSTR face) {
    (void)width, (void)escapement, (void)orientation, (void)italic, (void)underline, (void)strikeout, (void)charset;
    (void)out_precision, (void)clip_precision, (void)pitch, (void)face;
    GFont *f = (GFont *)calloc(1, sizeof *f);
    f->o.type = G_FONT;
    f->px = height < 0 ? -height : height;   /* (negative: the character height; positive: the cell's, taken the same) */
    f->bold = weight >= FW_SEMIBOLD;
    f->aa = quality != NONANTIALIASED_QUALITY;
    return f;
}
HBRUSH CreateSolidBrush(COLORREF c) {
    GBrush *b = (GBrush *)calloc(1, sizeof *b);
    b->o.type = G_BRUSH, b->c = c;
    return b;
}
static uint32_t bgrx(COLORREF c) { return (uint32_t)(c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
int FillRect(HDC hdc, const RECT *r, HBRUSH hb) {
    GDc *d = (GDc *)hdc;
    if (!d || !d->bmp || !hb) return 0;
    const uint32_t px = bgrx(((GBrush *)hb)->c);
    const int x0 = r->left < 0 ? 0 : r->left, y0 = r->top < 0 ? 0 : r->top;
    const int x1 = r->right > d->bmp->w ? d->bmp->w : r->right, y1 = r->bottom > d->bmp->h ? d->bmp->h : r->bottom;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) d->bmp->bits[(size_t)y * d->bmp->w + x] = px;
    return 1;
}
int SetBkMode(HDC hdc, int mode) {
    GDc *d = (GDc *)hdc;
    const int old = d->bk;
    d->bk = mode;
    return old;
}
COLORREF SetTextColor(HDC hdc, COLORREF c) {
    GDc *d = (GDc *)hdc;
    const COLORREF old = d->text;
    d->text = c;
    return old;
}
UINT SetTextAlign(HDC hdc, UINT align) {
    GDc *d = (GDc *)hdc;
    const UINT old = d->align;
    d->align = align;
    return old;
}

/* ---- text ---- */
static float scale_of(Face *face, const GFont *f) { return stbtt_ScaleForMappingEmToPixels(&face->info, (float)(f ? f->px : 16)); }
static int text_width(Face *face, const GFont *f, LPCSTR s, int n) {
    const float sc = scale_of(face, f);
    float w = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&face->info, (unsigned char)s[i], &adv, &lsb);
        w += adv * sc;
        if (i + 1 < n) w += sc * stbtt_GetCodepointKernAdvance(&face->info, (unsigned char)s[i], (unsigned char)s[i + 1]);
    }
    return (int)ceilf(w);
}
BOOL GetTextExtentPoint32A(HDC hdc, LPCSTR s, int n, SIZE *size) {
    GDc *d = (GDc *)hdc;
    Face *face = face_of(d->font);
    size->cx = size->cy = 0;
    if (!face) return TRUE;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&face->info, &asc, &desc, &gap);
    size->cx = text_width(face, d->font, s, n);
    size->cy = (LONG)ceilf((asc - desc) * scale_of(face, d->font));
    return TRUE;
}
BOOL TextOutA(HDC hdc, int x, int y, LPCSTR s, int n) {
    GDc *d = (GDc *)hdc;
    Face *face = face_of(d->font);
    if (!d->bmp || !face || n <= 0) return TRUE;
    const float sc = scale_of(face, d->font);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&face->info, &asc, &desc, &gap);
    if ((d->align & TA_CENTER) == TA_RIGHT) x -= text_width(face, d->font, s, n);
    else if ((d->align & TA_CENTER) == TA_CENTER) x -= text_width(face, d->font, s, n) / 2;
    const int baseline = (d->align & TA_BASELINE) == TA_BASELINE ? y
                       : (d->align & TA_BOTTOM) ? y + (int)lroundf(desc * sc) : y + (int)lroundf(asc * sc);
    const int aa = !d->font || d->font->aa;
    const uint32_t tr = d->text & 0xFF, tg = d->text >> 8 & 0xFF, tb = d->text >> 16 & 0xFF;
    float pen = (float)x;
    for (int i = 0; i < n; i++) {
        const int cp = (unsigned char)s[i];
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&face->info, cp, &adv, &lsb);
        const float sub = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&face->info, cp, sc, sc, sub, 0, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *g = (unsigned char *)malloc((size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&face->info, g, gw, gh, gw, sc, sc, sub, 0, cp);
            const int ox = (int)floorf(pen) + x0, oy = baseline + y0;
            for (int yy = 0; yy < gh; yy++) {
                const int py = oy + yy;
                if (py < 0 || py >= d->bmp->h) continue;
                for (int xx = 0; xx < gw; xx++) {
                    const int px = ox + xx;
                    if (px < 0 || px >= d->bmp->w) continue;
                    unsigned a = g[yy * gw + xx];
                    if (!aa) a = a >= 128 ? 255 : 0;
                    if (!a) continue;
                    uint32_t *p = &d->bmp->bits[(size_t)py * d->bmp->w + px];
                    const uint32_t ob = *p & 0xFF, og = *p >> 8 & 0xFF, orr = *p >> 16 & 0xFF;
                    *p = ((orr * (255 - a) + tr * a) / 255) << 16 | ((og * (255 - a) + tg * a) / 255) << 8 |
                         ((ob * (255 - a) + tb * a) / 255);
                }
            }
            free(g);
        }
        pen += adv * sc;
        if (i + 1 < n) pen += sc * stbtt_GetCodepointKernAdvance(&face->info, cp, (unsigned char)s[i + 1]);
    }
    return TRUE;
}
