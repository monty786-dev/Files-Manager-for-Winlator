/*
 * WinShell File Manager  -  native C (Win32 / GDI) port of FilesV3_1.py
 *
 * Runs on Windows and under Wine/Winlator (Android).  No runtime dependencies
 * other than the Win32 DLLs that ship with Wine.  GDI+ is loaded dynamically
 * (optional: used only for image thumbnails/preview).
 *
 * Build (MinGW-w64, 32-bit  -> works in every Winlator container):
 *   i686-w64-mingw32-gcc -O2 -municode -mwindows winshell_fm.c -o WinShellFM.exe \
 *       -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32 -lole32 -luuid -lshlwapi -ladvapi32 -lmsimg32 -static
 * 64-bit:  x86_64-w64-mingw32-gcc  (same flags)
 *
 * Config file is the same JSON file the Python version uses
 * (%PROGRAMDATA%\WinShell\fm_config.json); unknown keys are preserved.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#define _WIN32_IE 0x0600
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <math.h>

#define FM_TYPES
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef unsigned short u16;
#define ARRLEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define CFG_UNSET (-32768)

/* ========================================================================== */
/* small utilities                                                            */
/* ========================================================================== */
static void scpy(WCHAR *d, int cap, const WCHAR *s) { lstrcpynW(d, s ? s : L"", cap); }
static void scat(WCHAR *d, int cap, const WCHAR *s) {
    int l = lstrlenW(d); if (l < cap - 1) lstrcpynW(d + l, s, cap - l);
}
static void wfmt(WCHAR *d, int cap, const WCHAR *f, ...) {
    va_list a; va_start(a, f); _vsnwprintf(d, cap, f, a); va_end(a); d[cap - 1] = 0;
}
static WCHAR *wdup(const WCHAR *s) {
    size_t n = lstrlenW(s) + 1; WCHAR *p = (WCHAR *)malloc(n * sizeof(WCHAR)); memcpy(p, s, n * sizeof(WCHAR)); return p;
}
static int wieq(const WCHAR *a, const WCHAR *b) { return lstrcmpiW(a, b) == 0; }

/* string builder */
typedef struct { WCHAR *s; int n, cap; } SB;
static void sb_addn(SB *b, const WCHAR *s, int n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256; b->s = (WCHAR *)realloc(b->s, b->cap * sizeof(WCHAR));
    }
    memcpy(b->s + b->n, s, n * sizeof(WCHAR)); b->n += n; b->s[b->n] = 0;
}
static void sb_add(SB *b, const WCHAR *s) { sb_addn(b, s, lstrlenW(s)); }
static void sb_addc(SB *b, WCHAR c) { sb_addn(b, &c, 1); }
static void sb_addf(SB *b, const WCHAR *f, ...) {
    WCHAR t[512]; va_list a; va_start(a, f); _vsnwprintf(t, 512, f, a); va_end(a); t[511] = 0; sb_add(b, t);
}
static void sb_jstr(SB *b, const WCHAR *s) {
    sb_addc(b, '"');
    for (; *s; s++) {
        WCHAR c = *s;
        if (c == '"' || c == '\\') { sb_addc(b, '\\'); sb_addc(b, c); }
        else if (c == '\n') sb_add(b, L"\\n");
        else if (c == '\r') sb_add(b, L"\\r");
        else if (c == '\t') sb_add(b, L"\\t");
        else if (c < 32 || c > 126) { WCHAR t[12]; wfmt(t, 12, L"\\u%04x", (unsigned)c); sb_add(b, t); }
        else sb_addc(b, c);
    }
    sb_addc(b, '"');
}

/* string list */
typedef struct { WCHAR **v; int n, cap; } SL;
static void sl_add(SL *l, const WCHAR *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = (WCHAR **)realloc(l->v, l->cap * sizeof(WCHAR *)); }
    l->v[l->n++] = wdup(s);
}
static int sl_find(const SL *l, const WCHAR *s) {
    for (int i = 0; i < l->n; i++) if (wieq(l->v[i], s)) return i;
    return -1;
}
static void sl_del(SL *l, int i) {
    free(l->v[i]); memmove(l->v + i, l->v + i + 1, (l->n - i - 1) * sizeof(WCHAR *)); l->n--;
}
static void sl_clear(SL *l) { for (int i = 0; i < l->n; i++) free(l->v[i]); l->n = 0; }
static void sl_front(SL *l, const WCHAR *s, int max) {
    int i = sl_find(l, s); if (i >= 0) sl_del(l, i);
    sl_add(l, s); WCHAR *x = l->v[l->n - 1];
    memmove(l->v + 1, l->v, (l->n - 1) * sizeof(WCHAR *)); l->v[0] = x;
    while (l->n > max) sl_del(l, l->n - 1);
}

/* ========================================================================== */
/* logging + Wine detection                                                   */
/* ========================================================================== */
/* ---- portable document / archive engine ---- */
/* ==========================================================================
   d_zip.h - portable archive engine (no Win32): inflate, deflate, zip, tar, gzip, ISO9660
   ========================================================================== */
#ifndef FM_TYPES
#define FM_TYPES
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned long long u64; typedef unsigned int u32; typedef unsigned char u8; typedef unsigned short u16;
#endif

/* ---- growable byte / char buffer ---- */
typedef struct { char *s; size_t n, cap; } CBuf;
static void cb_reserve(CBuf *b, size_t add) {
    if (b->n + add + 1 > b->cap) { size_t c = b->cap ? b->cap : 256; while (c < b->n + add + 1) c *= 2; b->s = (char *)realloc(b->s, c); b->cap = c; }
}
static void cb_add(CBuf *b, const char *p, size_t n) { cb_reserve(b, n); memcpy(b->s + b->n, p, n); b->n += n; b->s[b->n] = 0; }
static void cb_adds(CBuf *b, const char *s) { cb_add(b, s, strlen(s)); }
static void cb_addc(CBuf *b, char c) { cb_add(b, &c, 1); }
static void cb_utf8(CBuf *b, u32 cp) {
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 63)); t[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { /* 4-byte */ char q[4]; q[0] = (char)(0xF0 | (cp >> 18)); q[1] = (char)(0x80 | ((cp >> 12) & 63)); q[2] = (char)(0x80 | ((cp >> 6) & 63)); q[3] = (char)(0x80 | (cp & 63)); cb_add(b, q, 4); return; }
    cb_add(b, t, n);
}

/* ---- random-access reader ---- */
typedef struct Rd { int (*read)(struct Rd *, u64 off, void *buf, size_t n); u64 size; void *ctx; } Rd;
typedef struct { const u8 *p; } MemCtx;
static int mem_read(Rd *r, u64 off, void *buf, size_t n) {
    if (off + n > r->size) return -1; memcpy(buf, ((MemCtx *)r->ctx)->p + off, n); return 0;
}
static void rd_mem(Rd *r, MemCtx *m, const void *p, size_t n) { m->p = (const u8 *)p; r->read = mem_read; r->size = n; r->ctx = m; }
typedef struct { int (*write)(void *, const void *, size_t); void *ctx; } Sink;

/* ---- CRC32 ---- */
static u32 g_crc_t[256]; static int g_crc_init;
static u32 crc32_upd(u32 c, const u8 *p, size_t n) {
    if (!g_crc_init) { for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; g_crc_t[i] = x; } g_crc_init = 1; }
    c = ~c; while (n--) c = g_crc_t[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}

/* ==========================================================================
   INFLATE (puff-style)
   ========================================================================== */
#define INF_MAXBITS 15
typedef struct { short count[INF_MAXBITS + 1]; short symbol[288]; } InfHuff;
typedef struct { const u8 *in; size_t inlen, incnt; u32 bitbuf; int bitcnt; u8 *out; size_t outcap, outn, limit; } Inf;
static int inf_bits(Inf *s, int need) {
    u32 val = s->bitbuf;
    while (s->bitcnt < need) { if (s->incnt >= s->inlen) return -1; val |= (u32)s->in[s->incnt++] << s->bitcnt; s->bitcnt += 8; }
    s->bitbuf = val >> need; s->bitcnt -= need; return (int)(val & ((1u << need) - 1));
}
static int inf_put(Inf *s, u8 c) {
    if (s->outn >= s->outcap) { if (s->outcap >= s->limit) return -1; size_t nc = s->outcap ? s->outcap * 2 : 65536; if (nc > s->limit) nc = s->limit; u8 *t = (u8 *)realloc(s->out, nc); if (!t) return -1; s->out = t; s->outcap = nc; }
    s->out[s->outn++] = c; return 0;
}
static int inf_decode(Inf *s, const InfHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= INF_MAXBITS; len++) {
        int b = inf_bits(s, 1); if (b < 0) return -1;
        code |= b; int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -2;
}
static int inf_build(InfHuff *h, const short *length, int n) {
    short offs[INF_MAXBITS + 1]; memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; s++) h->count[length[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1; for (int len = 1; len <= INF_MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0; for (int len = 1; len < INF_MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int s = 0; s < n; s++) if (length[s]) h->symbol[offs[length[s]]++] = (short)s;
    return left;
}
static int inf_codes(Inf *s, const InfHuff *lc, const InfHuff *dc) {
    static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int sym;
    do {
        sym = inf_decode(s, lc); if (sym < 0) return 1;
        if (sym < 256) { if (inf_put(s, (u8)sym)) return 2; }
        else if (sym > 256) {
            sym -= 257; if (sym >= 29) return 3;
            int len = lens[sym]; int b = inf_bits(s, lext[sym]); if (b < 0) return 1; len += b;
            sym = inf_decode(s, dc); if (sym < 0 || sym >= 30) return 3;
            u32 dist = dists[sym]; b = inf_bits(s, dext[sym]); if (b < 0) return 1; dist += b;
            if (dist > s->outn) return 4;
            while (len--) { if (inf_put(s, s->out[s->outn - dist])) return 2; }
        }
    } while (sym != 256);
    return 0;
}
static int inf_fixed(Inf *s) {
    static InfHuff lc, dc; static int built;
    if (!built) {
        short l[288]; int i; for (i = 0; i < 144; i++) l[i] = 8; for (; i < 256; i++) l[i] = 9; for (; i < 280; i++) l[i] = 7; for (; i < 288; i++) l[i] = 8;
        inf_build(&lc, l, 288); for (i = 0; i < 30; i++) l[i] = 5; inf_build(&dc, l, 30); built = 1;
    }
    return inf_codes(s, &lc, &dc);
}
static int inf_dynamic(Inf *s) {
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short lengths[320]; InfHuff lc, dc;
    int nlen = inf_bits(s, 5), ndist = inf_bits(s, 5), ncode = inf_bits(s, 4); if (ncode < 0) return 1;
    nlen += 257; ndist += 1; ncode += 4; if (nlen > 286 || ndist > 30) return 3;
    int idx; for (idx = 0; idx < ncode; idx++) { int b = inf_bits(s, 3); if (b < 0) return 1; lengths[order[idx]] = (short)b; }
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (inf_build(&lc, lengths, 19) != 0) return 5;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = inf_decode(s, &lc); if (sym < 0) return 1;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return 6; len = lengths[idx - 1]; rep = 3 + inf_bits(s, 2); }
            else if (sym == 17) rep = 3 + inf_bits(s, 3); else rep = 11 + inf_bits(s, 7);
            if (idx + rep > nlen + ndist) return 6; while (rep--) lengths[idx++] = (short)len;
        }
    }
    if (lengths[256] == 0) return 9;
    int e = inf_build(&lc, lengths, nlen); if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return 7;
    e = inf_build(&dc, lengths + nlen, ndist); if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return 8;
    return inf_codes(s, &lc, &dc);
}
/* raw deflate -> malloc'd buffer. limit = max output size. returns 0 on success */
static int inflate_raw(const u8 *in, size_t inlen, u8 **out, size_t *outn, size_t hint, size_t limit) {
    Inf s; memset(&s, 0, sizeof s); s.in = in; s.inlen = inlen; s.limit = limit ? limit : (size_t)0x7FFFFFFF;
    if (hint) { if (hint > s.limit) hint = s.limit; s.out = (u8 *)malloc(hint ? hint : 1); s.outcap = hint; }
    int last, err = 0;
    do {
        last = inf_bits(&s, 1); int type = inf_bits(&s, 2); if (last < 0 || type < 0) { err = 1; break; }
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) { err = 2; break; }
            unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8), nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8); s.incnt += 4;
            if (len != (~nlen & 0xFFFF)) { err = 3; break; }
            if (s.incnt + len > s.inlen) { err = 2; break; }
            for (unsigned i = 0; i < len; i++) if (inf_put(&s, s.in[s.incnt++])) { err = 4; break; }
        } else if (type == 1) err = inf_fixed(&s); else if (type == 2) err = inf_dynamic(&s); else err = -1;
    } while (!last && !err);
    if (err) { free(s.out); *out = NULL; *outn = 0; return err; }
    *out = s.out; *outn = s.outn; return 0;
}
static int inflate_zlib(const u8 *in, size_t n, u8 **out, size_t *outn, size_t limit) {
    if (n < 2) return 1; return inflate_raw(in + 2, n - 2, out, outn, n * 4, limit);
}

/* ==========================================================================
   DEFLATE (LZ77 + fixed Huffman, block-wise so it can stream)
   ========================================================================== */
typedef struct { u8 *buf; size_t n, cap; u32 acc; int nb; } BW;
static void bw_put(BW *w, u32 v, int nbits) {
    w->acc |= v << w->nb; w->nb += nbits;
    while (w->nb >= 8) { if (w->n >= w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->buf = (u8 *)realloc(w->buf, w->cap); } w->buf[w->n++] = (u8)w->acc; w->acc >>= 8; w->nb -= 8; }
}
static void bw_flush(BW *w) { if (w->nb > 0) bw_put(w, 0, 8 - w->nb); }
static u32 bit_rev(u32 v, int n) { u32 r = 0; while (n--) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void bw_lit(BW *w, int sym) {
    if (sym < 144) bw_put(w, bit_rev(0x30 + sym, 8), 8);
    else if (sym < 256) bw_put(w, bit_rev(0x190 + sym - 144, 9), 9);
    else if (sym < 280) bw_put(w, bit_rev(sym - 256, 7), 7);
    else bw_put(w, bit_rev(0xC0 + sym - 280, 8), 8);
}
static const short DF_LEN[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short DF_LEXT[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DF_DIST[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DF_DEXT[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void bw_match(BW *w, int len, int dist) {
    int i = 28; while (DF_LEN[i] > len) i--;
    bw_lit(w, 257 + i); if (DF_LEXT[i]) bw_put(w, len - DF_LEN[i], DF_LEXT[i]);
    int d = 29; while (DF_DIST[d] > dist) d--;
    bw_put(w, bit_rev(d, 5), 5); if (DF_DEXT[d]) bw_put(w, dist - DF_DIST[d], DF_DEXT[d]);
}
/* compress one block (data[0..n)), matches stay inside the block. final: set BFINAL */
static void deflate_block(BW *w, const u8 *d, size_t n, int final) {
    enum { HB = 15, HS = 1 << HB };
    int *head = (int *)malloc(HS * sizeof(int)); int *prev = (int *)malloc((n ? n : 1) * sizeof(int));
    for (int i = 0; i < HS; i++) head[i] = -1;
    bw_put(w, final ? 1 : 0, 1); bw_put(w, 1, 2);
    size_t i = 0;
    while (i < n) {
        int bl = 0, bd = 0;
        if (i + 3 <= n) {
            u32 h = ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & (HS - 1);
            int c = head[h], chain = 0;
            while (c >= 0 && chain++ < 24) {
                size_t dist = i - c; if (dist > 32768) break;
                int l = 0; while (i + l < n && l < 258 && d[c + l] == d[i + l]) l++;
                if (l > bl) { bl = l; bd = (int)dist; if (l == 258) break; }
                c = prev[c];
            }
            prev[i] = head[h]; head[h] = (int)i;
        }
        if (bl >= 3) {
            bw_match(w, bl, bd);
            for (int k = 1; k < bl; k++) { size_t p = i + k; if (p + 3 <= n) { u32 h = ((d[p] << 10) ^ (d[p + 1] << 5) ^ d[p + 2]) & (HS - 1); prev[p] = head[h]; head[h] = (int)p; } }
            i += bl;
        } else { bw_lit(w, d[i]); i++; }
    }
    bw_lit(w, 256);
    free(head); free(prev);
}

/* ==========================================================================
   ZIP reader
   ========================================================================== */
typedef struct { char *name; u64 size, csize, off; u32 crc; int method, isdir; u32 dostime; } AEnt;
typedef struct { AEnt *e; int n; int kind; Rd *rd; u64 dataoff_iso; } FArc;
enum { ARC_NONE, ARC_ZIP, ARC_TAR, ARC_TGZ, ARC_GZ, ARC_ISO, ARC_EXT /* needs 7z */ };
static void arc_free(FArc *a) { for (int i = 0; i < a->n; i++) free(a->e[i].name); free(a->e); memset(a, 0, sizeof *a); }
static AEnt *arc_push(FArc *a, int *cap, const char *name, size_t nl) {
    if (a->n >= *cap) { *cap = *cap ? *cap * 2 : 256; a->e = (AEnt *)realloc(a->e, *cap * sizeof(AEnt)); }
    AEnt *e = &a->e[a->n++]; memset(e, 0, sizeof *e); e->name = (char *)malloc(nl + 1); memcpy(e->name, name, nl); e->name[nl] = 0;
    for (size_t i = 0; i < nl; i++) if (e->name[i] == '\\') e->name[i] = '/';
    return e;
}
static u32 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 rd64(const u8 *p) { return rd32(p) | ((u64)rd32(p + 4) << 32); }
static int zip_open(FArc *a, Rd *rd) {
    memset(a, 0, sizeof *a); a->rd = rd; a->kind = ARC_ZIP;
    if (rd->size < 22) return -1;
    size_t tail = rd->size < 70000 ? (size_t)rd->size : 70000; u8 *t = (u8 *)malloc(tail);
    if (rd->read(rd, rd->size - tail, t, tail)) { free(t); return -1; }
    long eo = -1; for (long i = (long)tail - 22; i >= 0; i--) if (rd32(t + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { free(t); return -2; }
    u64 total = rd16(t + eo + 10), cdsize = rd32(t + eo + 12), cdoff = rd32(t + eo + 16);
    if (total == 0xFFFF || cdoff == 0xFFFFFFFFu || cdsize == 0xFFFFFFFFu) {
        if (eo >= 20 && rd32(t + eo - 20) == 0x07064b50) {
            u64 o64 = rd64(t + eo - 12); u8 b[56];
            if (!rd->read(rd, o64, b, 56) && rd32(b) == 0x06064b50) { total = rd64(b + 32); cdsize = rd64(b + 40); cdoff = rd64(b + 48); }
        }
    }
    free(t);
    if (cdsize > ((u64)1 << 29) || cdoff + cdsize > rd->size) return -3;
    u8 *cd = (u8 *)malloc((size_t)cdsize + 1); if (rd->read(rd, cdoff, cd, (size_t)cdsize)) { free(cd); return -4; }
    int cap = 0; size_t p = 0;
    while (p + 46 <= cdsize && rd32(cd + p) == 0x02014b50) {
        u32 flags = rd16(cd + p + 8), method = rd16(cd + p + 10), tm = rd32(cd + p + 12), crc = rd32(cd + p + 16);
        u64 cs = rd32(cd + p + 20), us = rd32(cd + p + 24); u32 nl = rd16(cd + p + 28), el = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        u64 lo = rd32(cd + p + 42); if (p + 46 + nl + el + cl > cdsize) break;
        const u8 *ex = cd + p + 46 + nl; size_t ep = 0;
        while (ep + 4 <= el) { u32 id = rd16(ex + ep), sz = rd16(ex + ep + 2); if (id == 1) { size_t q = ep + 4; if (us == 0xFFFFFFFFu && q + 8 <= el) { us = rd64(ex + q); q += 8; } if (cs == 0xFFFFFFFFu && q + 8 <= el) { cs = rd64(ex + q); q += 8; } if (lo == 0xFFFFFFFFu && q + 8 <= el) { lo = rd64(ex + q); } } ep += 4 + sz; }
        AEnt *e = arc_push(a, &cap, (const char *)cd + p + 46, nl);
        e->size = us; e->csize = cs; e->off = lo; e->crc = crc; e->method = (int)method; e->dostime = tm;
        e->isdir = nl && (cd[p + 46 + nl - 1] == '/' || cd[p + 46 + nl - 1] == '\\'); (void)flags;
        p += 46 + nl + el + cl; if (a->n > 200000) break;
    }
    free(cd); return 0;
}
/* data offset of an entry (reads the local header) */
static int zip_data_off(FArc *a, AEnt *e, u64 *off) {
    u8 h[30]; if (a->rd->read(a->rd, e->off, h, 30) || rd32(h) != 0x04034b50) return -1;
    *off = e->off + 30 + rd16(h + 26) + rd16(h + 28); return 0;
}
/* extract one entry into a malloc'd buffer (cap bytes) */
static int zip_read_entry(FArc *a, int idx, u8 **out, size_t *outn, size_t cap) {
    AEnt *e = &a->e[idx]; *out = NULL; *outn = 0; u64 off;
    if (e->isdir) return 0;
    if (e->size > cap || e->csize > cap) return -10;
    if (zip_data_off(a, e, &off)) return -1;
    u8 *c = (u8 *)malloc((size_t)e->csize + 1); if (a->rd->read(a->rd, off, c, (size_t)e->csize)) { free(c); return -2; }
    if (e->method == 0) { *out = c; *outn = (size_t)e->csize; return 0; }
    if (e->method != 8) { free(c); return -3; }
    int r = inflate_raw(c, (size_t)e->csize, out, outn, (size_t)e->size, cap); free(c); return r;
}
static int zip_find(FArc *a, const char *name) {
    for (int i = 0; i < a->n; i++) if (!strcmp(a->e[i].name, name)) return i;
    for (int i = 0; i < a->n; i++) {   /* case-insensitive fallback */
        const char *x = a->e[i].name, *y = name; while (*x && *y && ((*x | 32) == (*y | 32))) { x++; y++; } if (!*x && !*y) return i; }
    return -1;
}
/* ---- gzip wrapper ---- */
static int gzip_unpack(const u8 *in, size_t n, u8 **out, size_t *outn, size_t limit) {
    if (n < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
    int fl = in[3]; size_t p = 10;
    if (fl & 4) { if (p + 2 > n) return -1; p += 2 + rd16(in + p); }
    if (fl & 8) { while (p < n && in[p]) p++; p++; }
    if (fl & 16) { while (p < n && in[p]) p++; p++; }
    if (fl & 2) p += 2;
    if (p >= n) return -1;
    size_t hint = n >= 4 ? rd32(in + n - 4) : 0; if (hint > limit) hint = 0;
    return inflate_raw(in + p, n - p - 8 > n ? 0 : n - p - 8, out, outn, hint, limit);
}

/* ==========================================================================
   TAR (in a memory buffer, possibly decompressed from gzip)
   ========================================================================== */
static u64 tar_oct(const u8 *p, int n) {
    u64 v = 0; if (p[0] & 0x80) { for (int i = 1; i < n; i++) v = (v << 8) | p[i]; return v; }
    for (int i = 0; i < n && p[i]; i++) if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (p[i] - '0'); return v;
}
/* entries' off = byte offset of data inside buf */
static int tar_scan(FArc *a, const u8 *buf, size_t n) {
    int cap = 0; size_t p = 0; char longname[1024]; int havelong = 0;
    while (p + 512 <= n) {
        const u8 *h = buf + p; if (!h[0]) break;
        u64 sz = tar_oct(h + 124, 12); int type = h[156]; p += 512;
        char name[512]; size_t nl = 0; if (havelong) { snprintf(name, sizeof name, "%s", longname); havelong = 0; }
        else { char pre[160] = ""; if (!memcmp(h + 257, "ustar", 5) && h[345]) { memcpy(pre, h + 345, 155); pre[155] = 0; } char nm[101]; memcpy(nm, h, 100); nm[100] = 0; snprintf(name, sizeof name, "%s%s%s", pre, pre[0] ? "/" : "", nm); }
        nl = strlen(name);
        if (type == 'L') { size_t c = sz < 1023 ? (size_t)sz : 1023; if (p + c <= n) { memcpy(longname, buf + p, c); longname[c] = 0; havelong = 1; } }
        else if (type == '0' || type == 0 || type == '5' || type == '7') {
            AEnt *e = arc_push(a, &cap, name, nl); e->size = sz; e->csize = sz; e->off = p; e->isdir = (type == '5') || (nl && name[nl - 1] == '/');
        }
        if (p + sz > n && type != 'L') { if (a->n) a->e[a->n - 1].size = a->e[a->n - 1].csize = n > p ? n - p : 0; break; }
        p += (size_t)((sz + 511) & ~(u64)511); if (a->n > 200000) break;
    }
    return 0;
}

/* ==========================================================================
   ISO9660
   ========================================================================== */
static int iso_walk(FArc *a, int *cap, Rd *rd, u32 lba, u32 len, const char *prefix, int depth) {
    if (depth > 12 || len > (64u << 20)) return 0;
    u8 *d = (u8 *)malloc(len); if (rd->read(rd, (u64)lba * 2048, d, len)) { free(d); return -1; }
    u32 p = 0;
    while (p < len) {
        u32 rl = d[p]; if (!rl) { p = (p / 2048 + 1) * 2048; continue; } if (p + rl > len || rl < 34) break;
        u32 ext = rd32(d + p + 2), sz = rd32(d + p + 10), fl = d[p + 25], nl = d[p + 32]; const u8 *nm = d + p + 33;
        if (!(nl == 1 && (nm[0] == 0 || nm[0] == 1))) {
            char name[256]; u32 k = 0; for (u32 i = 0; i < nl && k < 250; i++) name[k++] = (char)nm[i]; name[k] = 0;
            char *semi = strchr(name, ';'); if (semi) *semi = 0; k = (u32)strlen(name); if (k && name[k - 1] == '.') name[k - 1] = 0;
            char full[1024]; snprintf(full, sizeof full, "%s%s%s", prefix, name, (fl & 2) ? "/" : "");
            AEnt *e = arc_push(a, cap, full, strlen(full)); e->size = e->csize = sz; e->off = (u64)ext * 2048; e->isdir = (fl & 2) ? 1 : 0;
            if ((fl & 2) && a->n < 100000) { char sub[1024]; snprintf(sub, sizeof sub, "%s", full); int idx = a->n - 1; (void)idx; iso_walk(a, cap, rd, ext, sz, sub, depth + 1); }
        }
        p += rl;
    }
    free(d); return 0;
}
static int iso_open(FArc *a, Rd *rd) {
    memset(a, 0, sizeof *a); a->rd = rd; a->kind = ARC_ISO; u8 v[2048];
    if (rd->size < 2048 * 17 || rd->read(rd, 16 * 2048, v, 2048) || memcmp(v + 1, "CD001", 5)) return -1;
    int cap = 0; return iso_walk(a, &cap, rd, rd32(v + 158), rd32(v + 166), "", 0);
}

/* ==========================================================================
   ZIP writer (streaming; deflate blocks of 256 KiB)
   ========================================================================== */
typedef struct { char *name; u32 crc; u64 csize, usize, off; u32 dostime; int isdir; } ZWEnt;
typedef struct {
    Sink out; u64 pos; ZWEnt *e; int n, cap; BW bw; u32 crc; u64 usize, csize, start; int open; u32 dostime; int err;
    u8 *pend; size_t pendn;
} ZW;
#define ZW_BLOCK (256 * 1024)
static void zw_out(ZW *z, const void *p, size_t n) { if (n && !z->err) { if (z->out.write(z->out.ctx, p, n)) z->err = 1; z->pos += n; } }
static void zw_le(u8 *b, u32 v, int n) { for (int i = 0; i < n; i++) b[i] = (u8)(v >> (8 * i)); }
static void zw_init(ZW *z, Sink s) { memset(z, 0, sizeof *z); z->out = s; }
static int zw_begin(ZW *z, const char *name, int isdir, u32 dostime) {
    if (z->n >= z->cap) { z->cap = z->cap ? z->cap * 2 : 128; z->e = (ZWEnt *)realloc(z->e, z->cap * sizeof(ZWEnt)); }
    ZWEnt *e = &z->e[z->n++]; memset(e, 0, sizeof *e); size_t nl = strlen(name); e->name = (char *)malloc(nl + 2); memcpy(e->name, name, nl);
    if (isdir && (!nl || name[nl - 1] != '/')) e->name[nl++] = '/'; e->name[nl] = 0;
    e->off = z->pos; e->dostime = dostime; e->isdir = isdir; nl = strlen(e->name);
    u8 h[30]; memset(h, 0, 30); zw_le(h, 0x04034b50, 4); zw_le(h + 4, 20, 2); zw_le(h + 6, isdir ? 0x0800 : 0x0808, 2); zw_le(h + 8, isdir ? 0 : 8, 2);
    zw_le(h + 10, dostime, 4); zw_le(h + 26, (u32)nl, 2);
    zw_out(z, h, 30); zw_out(z, e->name, nl);
    memset(&z->bw, 0, sizeof z->bw); z->crc = 0; z->usize = z->csize = 0; z->open = !isdir; z->pendn = 0;
    if (!z->pend) z->pend = (u8 *)malloc(ZW_BLOCK);
    return 0;
}
static void zw_flush_block(ZW *z, int final) {
    size_t before = z->bw.n; deflate_block(&z->bw, z->pend, z->pendn, final); z->pendn = 0;
    if (final) bw_flush(&z->bw);
    zw_out(z, z->bw.buf, z->bw.n); z->csize += z->bw.n; z->bw.n = 0; (void)before;
}
static void zw_feed(ZW *z, const u8 *d, size_t n) {
    z->crc = crc32_upd(z->crc, d, n); z->usize += n;
    while (n) { size_t c = ZW_BLOCK - z->pendn; if (c > n) c = n; memcpy(z->pend + z->pendn, d, c); z->pendn += c; d += c; n -= c; if (z->pendn == ZW_BLOCK) zw_flush_block(z, 0); }
}
static void zw_end(ZW *z) {
    ZWEnt *e = &z->e[z->n - 1]; if (!z->open) return;
    zw_flush_block(z, 1);   /* always emits a final block (possibly empty) */
    e->crc = z->crc; e->csize = z->csize; e->usize = z->usize; z->open = 0;
    u8 dd[16]; zw_le(dd, 0x08074b50, 4); zw_le(dd + 4, e->crc, 4); zw_le(dd + 8, (u32)e->csize, 4); zw_le(dd + 12, (u32)e->usize, 4); zw_out(z, dd, 16);
}
static int zw_finish(ZW *z) {
    u64 cdstart = z->pos;
    for (int i = 0; i < z->n; i++) {
        ZWEnt *e = &z->e[i]; size_t nl = strlen(e->name); u8 h[46]; memset(h, 0, 46);
        zw_le(h, 0x02014b50, 4); zw_le(h + 4, 20, 2); zw_le(h + 6, 20, 2); zw_le(h + 8, e->isdir ? 0x0800 : 0x0808, 2); zw_le(h + 10, e->isdir ? 0 : 8, 2);
        zw_le(h + 12, e->dostime, 4); zw_le(h + 16, e->crc, 4); zw_le(h + 20, (u32)e->csize, 4); zw_le(h + 24, (u32)e->usize, 4); zw_le(h + 28, (u32)nl, 2);
        zw_le(h + 38, e->isdir ? 0x10 : 0, 4); zw_le(h + 42, (u32)e->off, 4);
        zw_out(z, h, 46); zw_out(z, e->name, nl);
    }
    u64 cdsize = z->pos - cdstart; u8 t[22]; memset(t, 0, 22); zw_le(t, 0x06054b50, 4); zw_le(t + 8, (u32)z->n, 2); zw_le(t + 10, (u32)z->n, 2); zw_le(t + 12, (u32)cdsize, 4); zw_le(t + 16, (u32)cdstart, 4);
    zw_out(z, t, 22);
    for (int i = 0; i < z->n; i++) free(z->e[i].name); free(z->e); free(z->pend); free(z->bw.buf);
    int er = z->err; memset(z, 0, sizeof *z); return er;
}

/* ==========================================================================
   d_xml.h - tiny XML pull parser + docx / xlsx / pptx text models
   ========================================================================== */

/* ---- document model shared by every format ---- */
enum { B_PARA, B_H1, B_H2, B_H3, B_LIST, B_ROW, B_CELL, B_SHAPE, B_ABS, B_PAGEBREAK, B_TITLE, B_IMG };
typedef struct { int kind, row, col, bold; double x, y, w, h, size; char *text; } DBlock;
typedef struct { DBlock *b; int n, cap; char title[160]; double pw, ph; int ncols; } DPage;
enum { DOC_NONE, DOC_FLOW, DOC_SHEETS, DOC_SLIDES, DOC_PDF };
typedef struct { int kind; DPage *pg; int npg, cap; char err[160]; } Doc;
static DPage *doc_newpage(Doc *d, const char *title) {
    if (d->npg >= d->cap) { d->cap = d->cap ? d->cap * 2 : 8; d->pg = (DPage *)realloc(d->pg, d->cap * sizeof(DPage)); }
    DPage *p = &d->pg[d->npg++]; memset(p, 0, sizeof *p); if (title) snprintf(p->title, sizeof p->title, "%s", title); return p;
}
static DBlock *page_add(DPage *p, int kind, const char *text, size_t len) {
    if (p->n >= p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->b = (DBlock *)realloc(p->b, p->cap * sizeof(DBlock)); }
    DBlock *b = &p->b[p->n++]; memset(b, 0, sizeof *b); b->kind = kind; b->text = (char *)malloc(len + 1); if (len) memcpy(b->text, text, len); b->text[len] = 0; return b;
}
static void doc_free(Doc *d) {
    for (int i = 0; i < d->npg; i++) { for (int k = 0; k < d->pg[i].n; k++) free(d->pg[i].b[k].text); free(d->pg[i].b); }
    free(d->pg); memset(d, 0, sizeof *d);
}

/* ---- XML pull parser ---- */
typedef struct { const char *p, *end; } XP;
typedef struct { int kind; /* 0 eof, 1 start, 2 end, 3 text */ char name[80]; const char *attrs; size_t alen; const char *text; size_t tlen; int selfclose; } XEv;
static int xml_next(XP *x, XEv *e) {
    memset(e, 0, sizeof *e);
    for (;;) {
        if (x->p >= x->end) return 0;
        if (*x->p != '<') { const char *s = x->p; while (x->p < x->end && *x->p != '<') x->p++; e->kind = 3; e->text = s; e->tlen = (size_t)(x->p - s); return 3; }
        if (x->end - x->p >= 4 && !memcmp(x->p, "<!--", 4)) { const char *q = x->p + 4; while (q + 3 <= x->end && memcmp(q, "-->", 3)) q++; x->p = q + 3 > x->end ? x->end : q + 3; continue; }
        if (x->end - x->p >= 9 && !memcmp(x->p, "<![CDATA[", 9)) { const char *s = x->p + 9, *q = s; while (q + 3 <= x->end && memcmp(q, "]]>", 3)) q++; e->kind = 3; e->text = s; e->tlen = (size_t)(q - s); e->selfclose = 2; x->p = q + 3 > x->end ? x->end : q + 3; return 3; }
        if (x->p + 1 < x->end && (x->p[1] == '?' || x->p[1] == '!')) { const char *q = x->p; while (q < x->end && *q != '>') q++; x->p = q < x->end ? q + 1 : x->end; continue; }
        const char *q = x->p + 1; int endtag = 0; if (q < x->end && *q == '/') { endtag = 1; q++; }
        size_t nl = 0; while (q + nl < x->end && q[nl] != ' ' && q[nl] != '>' && q[nl] != '/' && q[nl] != '\t' && q[nl] != '\r' && q[nl] != '\n') nl++;
        if (nl > 79) nl = 79; memcpy(e->name, q, nl); e->name[nl] = 0; q += nl;
        const char *as = q; int inq = 0; char qc = 0;
        while (q < x->end && (inq || *q != '>')) { if (!inq && (*q == '"' || *q == '\'')) { inq = 1; qc = *q; } else if (inq && *q == qc) inq = 0; q++; }
        e->attrs = as; e->alen = (size_t)(q - as); if (e->alen && as[e->alen - 1] == '/') { e->selfclose = 1; e->alen--; }
        x->p = q < x->end ? q + 1 : x->end; e->kind = endtag ? 2 : 1; return e->kind;
    }
}
static int xml_attr(const XEv *e, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name); const char *p = e->attrs, *end = e->attrs + e->alen; out[0] = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        const char *ns = p; while (p < end && *p != '=' && *p != ' ') p++; size_t kl = (size_t)(p - ns);
        while (p < end && (*p == ' ' || *p == '=')) p++;
        if (p >= end) break; char q = *p; if (q != '"' && q != '\'') break; p++; const char *vs = p; while (p < end && *p != q) p++;
        if (kl == nl && !memcmp(ns, name, nl)) { size_t vl = (size_t)(p - vs); if (vl >= cap) vl = cap - 1; memcpy(out, vs, vl); out[vl] = 0; return 1; }
        p++;
    }
    return 0;
}
static void xml_text(const char *s, size_t n, CBuf *o) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            const char *e = (const char *)memchr(s + i, ';', n - i > 10 ? 10 : n - i);
            if (e) {
                size_t l = (size_t)(e - (s + i)) - 1; const char *t = s + i + 1;
                if (l == 2 && !memcmp(t, "lt", 2)) { cb_addc(o, '<'); i = (size_t)(e - s); continue; }
                if (l == 2 && !memcmp(t, "gt", 2)) { cb_addc(o, '>'); i = (size_t)(e - s); continue; }
                if (l == 3 && !memcmp(t, "amp", 3)) { cb_addc(o, '&'); i = (size_t)(e - s); continue; }
                if (l == 4 && !memcmp(t, "quot", 4)) { cb_addc(o, '"'); i = (size_t)(e - s); continue; }
                if (l == 4 && !memcmp(t, "apos", 4)) { cb_addc(o, '\''); i = (size_t)(e - s); continue; }
                if (l >= 2 && t[0] == '#') { u32 cp = (t[1] == 'x' || t[1] == 'X') ? (u32)strtoul(t + 2, NULL, 16) : (u32)strtoul(t + 1, NULL, 10); if (cp) cb_utf8(o, cp); i = (size_t)(e - s); continue; }
            }
        }
        cb_addc(o, s[i]);
    }
}

/* ---- load a part of a zip ---- */
static int zip_part(FArc *a, const char *name, u8 **buf, size_t *n) {
    int i = zip_find(a, name); if (i < 0) return -1; return zip_read_entry(a, i, buf, n, 96u << 20);
}
static char *fm_strdup(const char *s) { size_t l = strlen(s); char *p = (char *)malloc(l + 1); memcpy(p, s, l + 1); return p; }

/* ==========================================================================
   DOCX
   ========================================================================== */
static void para_flush(DPage *pg, CBuf *para, int kind, int bold) {
    DBlock *b = page_add(pg, kind, para->s ? para->s : "", para->n); b->bold = bold; para->n = 0; if (para->s) para->s[0] = 0;
}
static int docx_load(FArc *a, Doc *d) {
    u8 *buf; size_t n; if (zip_part(a, "word/document.xml", &buf, &n)) return -1;
    d->kind = DOC_FLOW; DPage *pg = doc_newpage(d, "Page 1");
    XP x = { (const char *)buf, (const char *)buf + n }; XEv e; CBuf para = { 0 }, row = { 0 };
    int in_t = 0, level = 0, list = 0, intbl = 0, bold = 0, cellcount = 0, pb_before = 0, inp = 0;
    while (xml_next(&x, &e)) {
        if (e.kind == 1) {
            if (!strcmp(e.name, "w:p")) { inp = 1; para.n = 0; level = 0; list = 0; bold = 0; pb_before = 0; if (para.s) para.s[0] = 0; }
            else if (!strcmp(e.name, "w:t")) in_t = e.selfclose ? 0 : 1;
            else if (!strcmp(e.name, "w:tab") && inp) cb_addc(&para, '\t');
            else if ((!strcmp(e.name, "w:br") || !strcmp(e.name, "w:cr")) && inp) {
                char t[24]; if (xml_attr(&e, "w:type", t, sizeof t) && !strcmp(t, "page")) {
                    if (para.n) para_flush(pg, &para, list ? B_LIST : B_PARA, 0);
                    if (pg->n) { char tt[32]; snprintf(tt, sizeof tt, "Page %d", d->npg + 1); pg = doc_newpage(d, tt); }
                } else cb_addc(&para, '\n');
            }
            else if (!strcmp(e.name, "w:lastRenderedPageBreak") && !intbl) { if (para.n) para_flush(pg, &para, list ? B_LIST : B_PARA, 0); if (pg->n) { char tt[32]; snprintf(tt, sizeof tt, "Page %d", d->npg + 1); pg = doc_newpage(d, tt); } }
            else if (!strcmp(e.name, "w:pageBreakBefore")) pb_before = 1;
            else if (!strcmp(e.name, "w:pStyle")) { char v[64]; if (xml_attr(&e, "w:val", v, sizeof v)) { if (!strncmp(v, "Heading", 7) || !strncmp(v, "heading", 7)) { int l = atoi(v + 7); level = l < 1 ? 1 : l > 3 ? 3 : l; } else if (!strcmp(v, "Title")) level = 1; else if (!strncmp(v, "List", 4)) list = 1; } }
            else if (!strcmp(e.name, "w:numPr")) list = 1;
            else if (!strcmp(e.name, "w:b") && inp) bold = 1;
            else if (!strcmp(e.name, "w:tbl")) intbl++;
            else if (!strcmp(e.name, "w:tr")) { row.n = 0; if (row.s) row.s[0] = 0; cellcount = 0; }
            else if (!strcmp(e.name, "w:tc")) { if (cellcount++) cb_addc(&row, '\t'); }
            if (pb_before && pg->n && !intbl) { char tt[32]; snprintf(tt, sizeof tt, "Page %d", d->npg + 1); pg = doc_newpage(d, tt); pb_before = 0; }
        } else if (e.kind == 2) {
            if (!strcmp(e.name, "w:t")) in_t = 0;
            else if (!strcmp(e.name, "w:p")) {
                inp = 0;
                if (intbl) { if (para.n) { if (row.n && row.s[row.n - 1] != '\t') cb_addc(&row, ' '); cb_add(&row, para.s, para.n); } }
                else para_flush(pg, &para, level ? B_H1 + level - 1 : list ? B_LIST : B_PARA, bold);
                para.n = 0; if (para.s) para.s[0] = 0;
            }
            else if (!strcmp(e.name, "w:tr")) { if (row.n) page_add(pg, B_ROW, row.s, row.n); row.n = 0; if (row.s) row.s[0] = 0; }
            else if (!strcmp(e.name, "w:tbl")) { if (intbl) intbl--; page_add(pg, B_PARA, "", 0); }
        } else if (e.kind == 3 && in_t) xml_text(e.text, e.tlen, &para);
    }
    free(para.s); free(row.s); free(buf);
    while (d->npg > 1 && d->pg[d->npg - 1].n == 0) { free(d->pg[d->npg - 1].b); d->npg--; }
    return 0;
}

/* ==========================================================================
   XLSX
   ========================================================================== */
static int col_from_ref(const char *r, int *row) {
    int c = 0; while ((*r >= 'A' && *r <= 'Z') || (*r >= 'a' && *r <= 'z')) { c = c * 26 + ((*r & ~32) - 'A' + 1); r++; } *row = atoi(r); return c - 1;
}
static int xlsx_load(FArc *a, Doc *d) {
    d->kind = DOC_SHEETS; char **sst = NULL; int nsst = 0; u8 *buf; size_t n;
    if (!zip_part(a, "xl/sharedStrings.xml", &buf, &n)) {
        XP x = { (const char *)buf, (const char *)buf + n }; XEv e; CBuf cur = { 0 }; int in_si = 0, in_t = 0, cap = 0, in_ph = 0;
        while (xml_next(&x, &e)) {
            if (e.kind == 1 && !strcmp(e.name, "si")) { in_si = 1; cur.n = 0; if (cur.s) cur.s[0] = 0; }
            else if (e.kind == 1 && !strcmp(e.name, "rPh")) in_ph = 1;
            else if (e.kind == 2 && !strcmp(e.name, "rPh")) in_ph = 0;
            else if (e.kind == 1 && !strcmp(e.name, "t")) in_t = e.selfclose ? 0 : 1;
            else if (e.kind == 2 && !strcmp(e.name, "t")) in_t = 0;
            else if (e.kind == 2 && !strcmp(e.name, "si")) { if (nsst >= cap) { cap = cap ? cap * 2 : 256; sst = (char **)realloc(sst, cap * sizeof(char *)); } sst[nsst++] = fm_strdup(cur.s ? cur.s : ""); in_si = 0; }
            else if (e.kind == 3 && in_si && in_t && !in_ph) xml_text(e.text, e.tlen, &cur);
        }
        free(cur.s); free(buf);
    }
    static char names[64][100], rids[64][32], targets[64][160]; int nsheets = 0;
    for (int i = 0; i < 64; i++) targets[i][0] = rids[i][0] = 0;
    if (!zip_part(a, "xl/workbook.xml", &buf, &n)) {
        XP x = { (const char *)buf, (const char *)buf + n }; XEv e;
        while (xml_next(&x, &e)) if (e.kind == 1 && !strcmp(e.name, "sheet") && nsheets < 64) {
            CBuf nm = { 0 }; char v[256]; if (xml_attr(&e, "name", v, sizeof v)) xml_text(v, strlen(v), &nm); snprintf(names[nsheets], 100, "%s", nm.s ? nm.s : "Sheet"); free(nm.s);
            if (!xml_attr(&e, "r:id", rids[nsheets], 32)) rids[nsheets][0] = 0; nsheets++; }
        free(buf);
    }
    if (!zip_part(a, "xl/_rels/workbook.xml.rels", &buf, &n)) {
        XP x = { (const char *)buf, (const char *)buf + n }; XEv e;
        while (xml_next(&x, &e)) if (e.kind == 1 && !strcmp(e.name, "Relationship")) {
            char id[32], tg[200]; if (xml_attr(&e, "Id", id, 32) && xml_attr(&e, "Target", tg, sizeof tg))
                for (int i = 0; i < nsheets; i++) if (!strcmp(rids[i], id)) { if (tg[0] == '/') snprintf(targets[i], 160, "%s", tg + 1); else snprintf(targets[i], 160, "xl/%s", tg); }
        }
        free(buf);
    }
    if (!nsheets) { for (int i = 0; i < a->n && nsheets < 64; i++) if (!strncmp(a->e[i].name, "xl/worksheets/sheet", 19)) { snprintf(names[nsheets], 100, "Sheet%d", nsheets + 1); snprintf(targets[nsheets], 160, "%s", a->e[i].name); nsheets++; } }
    for (int s = 0; s < nsheets; s++) {
        DPage *pg = doc_newpage(d, names[s]); if (!targets[s][0] || zip_part(a, targets[s], &buf, &n)) continue;
        XP x = { (const char *)buf, (const char *)buf + n }; XEv e; CBuf val = { 0 }; int r = 0, c = 0, in_v = 0, in_is = 0, typ = 0, curc = 0, currow = 0, maxc = 0; char t[24];
        while (xml_next(&x, &e)) {
            if (e.kind == 1) {
                if (!strcmp(e.name, "row")) { if (xml_attr(&e, "r", t, sizeof t)) currow = atoi(t) - 1; else currow = r; r = currow + 1; c = 0; }
                else if (!strcmp(e.name, "c")) {
                    char rf[24]; if (xml_attr(&e, "r", rf, sizeof rf)) { int rr; curc = col_from_ref(rf, &rr); if (rr > 0) currow = rr - 1; } else curc = c; c = curc + 1;
                    typ = 0; if (xml_attr(&e, "t", t, sizeof t)) typ = !strcmp(t, "s") ? 1 : !strcmp(t, "inlineStr") ? 2 : !strcmp(t, "b") ? 3 : !strcmp(t, "str") ? 4 : 0;
                    val.n = 0; if (val.s) val.s[0] = 0;
                    if (e.selfclose) continue;
                } else if (!strcmp(e.name, "v") || (!strcmp(e.name, "t") && in_is)) in_v = e.selfclose ? 0 : 1;
                else if (!strcmp(e.name, "is")) in_is = 1;
            } else if (e.kind == 2) {
                if (!strcmp(e.name, "v") || !strcmp(e.name, "t")) in_v = 0;
                else if (!strcmp(e.name, "is")) in_is = 0;
                else if (!strcmp(e.name, "c")) {
                    const char *txt = val.s ? val.s : "";
                    if (typ == 1) { int k = atoi(txt); txt = (k >= 0 && k < nsst) ? sst[k] : ""; }
                    else if (typ == 3) txt = !strcmp(txt, "1") ? "TRUE" : "FALSE";
                    if (*txt) { DBlock *b = page_add(pg, B_CELL, txt, strlen(txt)); b->row = currow; b->col = curc; if (curc + 1 > maxc) maxc = curc + 1; if (pg->n > 120000) break; }
                }
            } else if (e.kind == 3 && in_v) xml_text(e.text, e.tlen, &val);
        }
        pg->ncols = maxc; free(val.s); free(buf);
    }
    for (int i = 0; i < nsst; i++) free(sst[i]); free(sst);
    return nsheets ? 0 : -1;
}

/* ==========================================================================
   PPTX
   ========================================================================== */
static int pptx_slide_num(const char *name) { return atoi(name + 16); }
static int pptx_load(FArc *a, Doc *d) {
    d->kind = DOC_SLIDES; int idx[500], ns = 0;
    for (int i = 0; i < a->n && ns < 500; i++) { const char *nm = a->e[i].name; size_t l = strlen(nm); if (!strncmp(nm, "ppt/slides/slide", 16) && l > 4 && !strcmp(nm + l - 4, ".xml")) idx[ns++] = i; }
    for (int i = 0; i < ns; i++) for (int j = i + 1; j < ns; j++) if (pptx_slide_num(a->e[idx[j]].name) < pptx_slide_num(a->e[idx[i]].name)) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    double SW = 12192000.0, SH = 6858000.0; u8 *buf; size_t n;
    if (!zip_part(a, "ppt/presentation.xml", &buf, &n)) { XP x = { (const char *)buf, (const char *)buf + n }; XEv e; while (xml_next(&x, &e)) if (e.kind == 1 && !strcmp(e.name, "p:sldSz")) { char v[32]; if (xml_attr(&e, "cx", v, 32)) SW = atof(v); if (xml_attr(&e, "cy", v, 32)) SH = atof(v); break; } free(buf); }
    if (SW < 1 || SH < 1) { SW = 12192000.0; SH = 6858000.0; }
    for (int s = 0; s < ns; s++) {
        char title[32]; snprintf(title, sizeof title, "Slide %d", s + 1); DPage *pg = doc_newpage(d, title); pg->pw = SW / 12700.0; pg->ph = SH / 12700.0;
        if (zip_read_entry(a, idx[s], &buf, &n, 32u << 20)) continue;
        XP x = { (const char *)buf, (const char *)buf + n }; XEv e; CBuf para = { 0 }, shape = { 0 };
        int in_sp = 0, in_t = 0, is_title = 0, have_xf = 0, bullet = 0; double ox = 0, oy = 0, cx = 0, cy = 0;
        while (xml_next(&x, &e)) {
            if (e.kind == 1) {
                if (!strcmp(e.name, "p:sp") || !strcmp(e.name, "p:graphicFrame")) { in_sp = 1; shape.n = 0; if (shape.s) shape.s[0] = 0; is_title = 0; have_xf = 0; ox = oy = cx = cy = 0; }
                else if (!strcmp(e.name, "p:ph")) { char t[40]; if (xml_attr(&e, "type", t, sizeof t) && (!strcmp(t, "title") || !strcmp(t, "ctrTitle"))) is_title = 1; }
                else if (!strcmp(e.name, "a:off")) { char v[32]; if (xml_attr(&e, "x", v, 32)) { ox = atof(v); have_xf = 1; } if (xml_attr(&e, "y", v, 32)) oy = atof(v); }
                else if (!strcmp(e.name, "a:ext")) { char v[32]; if (xml_attr(&e, "cx", v, 32)) cx = atof(v); if (xml_attr(&e, "cy", v, 32)) cy = atof(v); }
                else if (!strcmp(e.name, "a:p")) { para.n = 0; if (para.s) para.s[0] = 0; bullet = 0; }
                else if (!strcmp(e.name, "a:buChar") || !strcmp(e.name, "a:buAutoNum")) bullet = 1;
                else if (!strcmp(e.name, "a:t")) in_t = e.selfclose ? 0 : 1;
                else if (!strcmp(e.name, "a:br")) cb_addc(&para, '\n');
                else if (!strcmp(e.name, "a:tab")) cb_addc(&para, '\t');
                else if (!strcmp(e.name, "p:pic")) { DBlock *b = page_add(pg, B_IMG, "[picture]", 9); b->x = -1; }
            } else if (e.kind == 2) {
                if (!strcmp(e.name, "a:t")) in_t = 0;
                else if (!strcmp(e.name, "a:p") && in_sp) { if (para.n) { if (shape.n) cb_addc(&shape, '\n'); if (bullet) cb_adds(&shape, "\xE2\x80\xA2 "); cb_add(&shape, para.s, para.n); } para.n = 0; }
                else if ((!strcmp(e.name, "p:sp") || !strcmp(e.name, "p:graphicFrame")) && in_sp) {
                    if (shape.n) { DBlock *b = page_add(pg, B_SHAPE, shape.s, shape.n); b->bold = is_title; b->x = have_xf ? ox / 12700.0 : -1; b->y = have_xf ? oy / 12700.0 : -1; b->w = have_xf ? cx / 12700.0 : 0; b->h = have_xf ? cy / 12700.0 : 0; b->size = is_title; }
                    in_sp = 0;
                }
            } else if (e.kind == 3 && in_t) xml_text(e.text, e.tlen, &para);
        }
        free(para.s); free(shape.s); free(buf);
    }
    return ns ? 0 : -1;
}

/* ---- dispatcher for OOXML ---- fmt: 1 docx 2 xlsx 3 pptx */
static int office_load(Rd *rd, int fmt, Doc *d) {
    FArc a; memset(d, 0, sizeof *d); int r = zip_open(&a, rd);
    if (r) { snprintf(d->err, sizeof d->err, "Not a valid Office package (zip error %d)", r); return r; }
    r = fmt == 1 ? docx_load(&a, d) : fmt == 2 ? xlsx_load(&a, d) : pptx_load(&a, d);
    if (r) snprintf(d->err, sizeof d->err, "Could not read the document content"); arc_free(&a); return r;
}

/* ==========================================================================
   d_pdf.h - PDF text extraction (no rendering): xref-less object scan, object streams,
   Flate streams, ToUnicode maps, page tree walk, simple text-matrix tracking
   ========================================================================== */
#include <math.h>
#include <ctype.h>

typedef struct { const u8 *p, *e; } PRef;
typedef struct { u32 lo, hi, cp[4]; int ncp; } CmapEnt;
typedef struct { int num, two, simple_ok; CmapEnt *m; int nm; } PFont;
typedef struct { double x, y, size; char *t; int len; } Chunk;
typedef struct { const u8 *d; size_t n; PRef *tab; int ntab; u8 **keep; int nkeep, ckeep; PFont fonts[96]; int nfonts; } Pdf;

static const u8 *pd_ws(const u8 *p, const u8 *e) {
    while (p < e) { if (*p == '%') { while (p < e && *p != '\n' && *p != '\r') p++; } else if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '\f' || *p == 0) p++; else break; }
    return p;
}
static int pd_delim(u8 c) { return strchr("()<>[]{}/%", c) != NULL; }
static int pd_space(u8 c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
static const u8 *pd_skip(const u8 *p, const u8 *e);
static const u8 *pd_skip_str(const u8 *p, const u8 *e) { int dep = 0; do { if (*p == '\\') p++; else if (*p == '(') dep++; else if (*p == ')') dep--; p++; } while (p < e && dep > 0); return p; }
static const u8 *pd_skip(const u8 *p, const u8 *e) {
    p = pd_ws(p, e); if (p >= e) return e;
    if (*p == '<' && p + 1 < e && p[1] == '<') { p += 2; for (;;) { p = pd_ws(p, e); if (p >= e) return e; if (*p == '>' && p + 1 < e && p[1] == '>') return p + 2; p = pd_skip(p, e); } }
    if (*p == '[') { p++; for (;;) { p = pd_ws(p, e); if (p >= e) return e; if (*p == ']') return p + 1; p = pd_skip(p, e); } }
    if (*p == '(') return pd_skip_str(p, e);
    if (*p == '<') { while (p < e && *p != '>') p++; return p < e ? p + 1 : e; }
    if (*p == '/') { p++; while (p < e && !pd_space(*p) && !pd_delim(*p)) p++; return p; }
    const u8 *s = p; while (p < e && !pd_space(*p) && !pd_delim(*p)) p++;
    if (p == s) return p + 1;
    if (isdigit(*s)) {   /* maybe "N G R" */
        const u8 *q = pd_ws(p, e), *g = q; while (q < e && isdigit(*q)) q++;
        if (q > g) { const u8 *r = pd_ws(q, e); if (r < e && *r == 'R' && (r + 1 >= e || pd_space(r[1]) || pd_delim(r[1]))) return r + 1; }
    }
    return p;
}
static int pd_dget(const u8 *p, const u8 *e, const char *key, const u8 **vs, const u8 **ve) {
    p = pd_ws(p, e); if (p + 1 >= e || p[0] != '<' || p[1] != '<') return 0; p += 2; size_t kl = strlen(key);
    for (;;) {
        p = pd_ws(p, e); if (p >= e || *p == '>') return 0; if (*p != '/') { p = pd_skip(p, e); continue; }
        const u8 *ks = p + 1; const u8 *q = ks; while (q < e && !pd_space(*q) && !pd_delim(*q)) q++;
        const u8 *v = pd_ws(q, e), *vend = pd_skip(v, e);
        if ((size_t)(q - ks) == kl && !memcmp(ks, key, kl)) { *vs = v; *ve = vend; return 1; }
        p = vend;
    }
}
static int pd_isref(const u8 *vs, const u8 *ve, int *num) {
    const u8 *p = vs; if (p >= ve || !isdigit(*p)) return 0; int n = atoi((const char *)p); while (p < ve && isdigit(*p)) p++; p = pd_ws(p, ve);
    if (p >= ve || !isdigit(*p)) return 0; while (p < ve && isdigit(*p)) p++; p = pd_ws(p, ve); if (p < ve && *p == 'R') { *num = n; return 1; } return 0;
}
static double pd_num(const u8 *p, const u8 *e) { char b[40]; size_t l = 0; p = pd_ws(p, e); while (p < e && l < 39 && (isdigit(*p) || *p == '.' || *p == '-' || *p == '+')) b[l++] = (char)*p++; b[l] = 0; return atof(b); }
static int pd_resolve(Pdf *pf, const u8 *vs, const u8 *ve, const u8 **rs, const u8 **re) {
    int n; if (pd_isref(vs, ve, &n)) { if (n < 0 || n >= pf->ntab || !pf->tab[n].p) return 0; *rs = pf->tab[n].p; *re = pf->tab[n].e; return 1; }
    *rs = vs; *re = ve; return 1;
}
static int pd_get(Pdf *pf, const u8 *dp, const u8 *de, const char *key, const u8 **rs, const u8 **re) {
    const u8 *vs, *ve; if (!pd_dget(dp, de, key, &vs, &ve)) return 0; return pd_resolve(pf, vs, ve, rs, re);
}
static void pd_keep(Pdf *pf, u8 *b) { if (pf->nkeep >= pf->ckeep) { pf->ckeep = pf->ckeep ? pf->ckeep * 2 : 16; pf->keep = (u8 **)realloc(pf->keep, pf->ckeep * sizeof(u8 *)); } pf->keep[pf->nkeep++] = b; }
static const u8 *pd_find(const u8 *p, const u8 *e, const char *s) { size_t l = strlen(s); for (; p + l <= e; p++) if (*p == (u8)s[0] && !memcmp(p, s, l)) return p; return NULL; }
/* decode the stream of object [op, oe) ; returns 0 ok (caller frees *out) */
static int pd_stream(Pdf *pf, const u8 *op, const u8 *oe, u8 **out, size_t *outn) {
    *out = NULL; *outn = 0;
    const u8 *ds = pd_ws(op, oe); const u8 *de = pd_skip(ds, oe); const u8 *s = pd_find(de, oe, "stream"); if (!s) return -1;
    s += 6; if (s < oe && *s == '\r') s++; if (s < oe && *s == '\n') s++;
    size_t len = 0; int havelen = 0; const u8 *ls, *le;
    if (pd_dget(ds, de, "Length", &ls, &le)) { const u8 *rs, *re; if (pd_resolve(pf, ls, le, &rs, &re)) { len = (size_t)pd_num(rs, re); havelen = 1; } }
    if (!havelen || s + len > oe || (len && !pd_find(s + len > oe ? oe : s + len, s + len + 20 > oe ? oe : s + len + 20, "endstream"))) {
        const u8 *es = pd_find(s, oe, "endstream"); len = es ? (size_t)(es - s) : (size_t)(oe - s); while (len && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--; }
    const u8 *fs, *fe; int flate = 0;
    if (pd_dget(ds, de, "Filter", &fs, &fe)) { const u8 *q = pd_find(fs, fe, "Fl"); if (q) flate = 1; else if (pd_find(fs, fe, "DCT") || pd_find(fs, fe, "LZW") || pd_find(fs, fe, "JPX") || pd_find(fs, fe, "CCITT") || pd_find(fs, fe, "Crypt")) return -2; }
    u8 *a85 = NULL;
    if (pd_dget(ds, de, "Filter", &fs, &fe) && (pd_find(fs, fe, "A85") || pd_find(fs, fe, "ASCII85"))) {
        a85 = (u8 *)malloc(len + 8); size_t o = 0; u32 grp[5]; int gn = 0;
        for (size_t i = 0; i < len; i++) { u8 ch = s[i]; if (pd_space(ch)) continue; if (ch == '~') break;
            if (ch == 'z' && gn == 0) { memset(a85 + o, 0, 4); o += 4; continue; } if (ch < '!' || ch > 'u') continue;
            grp[gn++] = ch - '!'; if (gn == 5) { u32 v = 0; for (int k = 0; k < 5; k++) v = v * 85 + grp[k]; a85[o++] = v >> 24; a85[o++] = v >> 16; a85[o++] = v >> 8; a85[o++] = v; gn = 0; } }
        if (gn > 1) { for (int k = gn; k < 5; k++) grp[k] = 84; u32 v = 0; for (int k = 0; k < 5; k++) v = v * 85 + grp[k]; for (int k = 0; k < gn - 1; k++) a85[o++] = (u8)(v >> (24 - 8 * k)); }
        s = a85; len = o;
    }
    if (!flate) { *out = (u8 *)malloc(len + 1); memcpy(*out, s, len); *outn = len; free(a85); return 0; }
    int r = inflate_zlib(s, len, out, outn, 128u << 20); free(a85); if (r) { if (!*out) return -3; }
    /* PNG predictor (xref streams / object streams) */
    const u8 *dp, *dpe; if (pd_dget(ds, de, "DecodeParms", &dp, &dpe)) {
        const u8 *ps, *pe; if (pd_dget(dp, dpe, "Predictor", &ps, &pe) && pd_num(ps, pe) >= 10) {
            const u8 *cs, *ce; int cols = 1; if (pd_dget(dp, dpe, "Columns", &cs, &ce)) cols = (int)pd_num(cs, ce);
            size_t row = (size_t)cols + 1, rows = *outn / row; u8 *o2 = (u8 *)malloc(rows * cols + 1);
            for (size_t y = 0; y < rows; y++) { u8 *src = *out + y * row; u8 f = src[0]; u8 *dst = o2 + y * cols; u8 *up = y ? o2 + (y - 1) * cols : NULL;
                for (int x = 0; x < cols; x++) { int a = x ? dst[x - 1] : 0, b = up ? up[x] : 0, cc = (x && up) ? up[x - 1] : 0, v = src[1 + x];
                    if (f == 1) v += a; else if (f == 2) v += b; else if (f == 3) v += (a + b) / 2; else if (f == 4) { int p0 = a + b - cc, pa = abs(p0 - a), pb = abs(p0 - b), pc = abs(p0 - cc); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc); }
                    dst[x] = (u8)v; } }
            free(*out); *out = o2; *outn = rows * cols;
        }
    }
    return 0;
}
static void pd_settab(Pdf *pf, int num, const u8 *p, const u8 *e) {
    if (num < 0 || num > 4000000) return;
    if (num >= pf->ntab) { int nn = num + 1024; pf->tab = (PRef *)realloc(pf->tab, nn * sizeof(PRef)); memset(pf->tab + pf->ntab, 0, (nn - pf->ntab) * sizeof(PRef)); pf->ntab = nn; }
    pf->tab[num].p = p; pf->tab[num].e = e;
}
static void pd_scan(Pdf *pf) {
    const u8 *d = pf->d, *end = pf->d + pf->n;
    for (const u8 *p = d + 4; p + 3 < end; p++) {
        if (p[0] != 'o' || p[1] != 'b' || p[2] != 'j') continue;
        if (p + 3 < end && isalnum(p[3])) continue;
        if (!pd_space(p[-1])) continue;
        const u8 *q = p - 1; while (q > d && pd_space(*q)) q--; const u8 *ge = q + 1; while (q > d && isdigit(*q)) q--; if (!isdigit(q[1]) || q + 1 >= ge) continue;
        while (q > d && pd_space(*q)) q--; const u8 *ne = q + 1; while (q > d && isdigit(*q)) q--; const u8 *ns = isdigit(*q) ? q : q + 1; if (ns >= ne) continue;
        int num = atoi((const char *)ns);
        const u8 *eo = pd_find(p + 3, end, "endobj"); const u8 *oe = eo ? eo : end;
        pd_settab(pf, num, p + 3, oe); p = oe - 1;
    }
    /* object streams */
    int n0 = pf->ntab;
    for (int i = 0; i < n0; i++) {
        PRef r = pf->tab[i]; if (!r.p) continue; const u8 *ts, *te;
        if (!pd_dget(r.p, r.e, "Type", &ts, &te) || te - ts < 7 || memcmp(ts, "/ObjStm", 7)) continue;
        u8 *dec; size_t dn; if (pd_stream(pf, r.p, r.e, &dec, &dn) || !dec) continue; pd_keep(pf, dec);
        const u8 *ns, *ne, *fs, *fe; int N = 0, first = 0; if (pd_dget(r.p, r.e, "N", &ns, &ne)) N = (int)pd_num(ns, ne); if (pd_dget(r.p, r.e, "First", &fs, &fe)) first = (int)pd_num(fs, fe);
        const u8 *h = dec, *he = dec + dn; int *nums = (int *)malloc(sizeof(int) * (N + 1) * 2); int k = 0;
        for (int j = 0; j < N && h < he; j++) { h = pd_ws(h, he); nums[k++] = atoi((const char *)h); while (h < he && isdigit(*h)) h++; h = pd_ws(h, he); nums[k++] = atoi((const char *)h); while (h < he && isdigit(*h)) h++; }
        for (int j = 0; j < k / 2; j++) { size_t o = (size_t)first + nums[2 * j + 1], o2 = (j + 1 < k / 2) ? (size_t)first + nums[2 * j + 3] : dn; if (o < dn && o2 <= dn && o <= o2) pd_settab(pf, nums[2 * j], dec + o, dec + o2); }
        free(nums);
    }
}
/* ---- fonts / ToUnicode ---- */
static u32 pd_hex(const u8 **pp, const u8 *e, int *nbytes, u32 *cps, int *ncp) {
    const u8 *p = *pp; while (p < e && *p != '<') p++; p++; u32 v = 0; int nd = 0; u32 units[8]; int nu = 0, ud = 0; u32 cur = 0;
    while (p < e && *p != '>') { if (isxdigit(*p)) { int dv = isdigit(*p) ? *p - '0' : (*p | 32) - 'a' + 10; v = (v << 4) | dv; nd++; cur = (cur << 4) | dv; if (++ud == 4) { if (nu < 8) units[nu++] = cur; cur = 0; ud = 0; } } p++; }
    if (ud && nu < 8) units[nu++] = cur;
    *pp = p < e ? p + 1 : e; if (nbytes) *nbytes = (nd + 1) / 2;
    if (cps) { int c = 0; for (int i = 0; i < nu && c < 4; i++) { u32 u = units[i]; if (u >= 0xD800 && u < 0xDC00 && i + 1 < nu) { u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00); i++; } cps[c++] = u; } *ncp = c; }
    return v;
}
static void pd_load_cmap(PFont *f, const u8 *s, size_t n) {
    const u8 *e = s + n, *p; int cap = 0; f->two = 0;
    p = pd_find(s, e, "begincodespacerange"); if (p) { int nb = 1; const u8 *q = p + 19; pd_hex(&q, e, &nb, NULL, NULL); f->two = nb >= 2; }
    p = s;
    while ((p = pd_find(p, e, "beginbf")) != NULL) {
        int range = !memcmp(p + 7, "range", 5); p += range ? 12 : 11; const u8 *end = pd_find(p, e, "endbf"); if (!end) end = e;
        while (p < end) {
            p = pd_ws(p, end); if (p >= end || *p != '<') break;
            CmapEnt en; memset(&en, 0, sizeof en); en.lo = pd_hex(&p, end, NULL, NULL, NULL);
            if (range) {
                en.hi = pd_hex(&p, end, NULL, NULL, NULL); p = pd_ws(p, end);
                if (p < end && *p == '[') { p++; u32 c = en.lo; while (p < end) { p = pd_ws(p, end); if (*p == ']') { p++; break; } CmapEnt x; memset(&x, 0, sizeof x); x.lo = x.hi = c++; pd_hex(&p, end, NULL, x.cp, &x.ncp); if (f->nm >= cap) { cap = cap ? cap * 2 : 256; f->m = (CmapEnt *)realloc(f->m, cap * sizeof(CmapEnt)); } f->m[f->nm++] = x; } continue; }
                pd_hex(&p, end, NULL, en.cp, &en.ncp);
            } else { en.hi = en.lo; pd_hex(&p, end, NULL, en.cp, &en.ncp); }
            if (f->nm >= cap) { cap = cap ? cap * 2 : 256; f->m = (CmapEnt *)realloc(f->m, cap * sizeof(CmapEnt)); } f->m[f->nm++] = en;
        }
        p = end;
    }
}
static PFont *pd_font(Pdf *pf, int num, const u8 *os, const u8 *oe) {
    for (int i = 0; i < pf->nfonts; i++) if (pf->fonts[i].num == num && num > 0) return &pf->fonts[i];
    if (pf->nfonts >= 96) return &pf->fonts[0];
    PFont *f = &pf->fonts[pf->nfonts++]; memset(f, 0, sizeof *f); f->num = num;
    const u8 *ts, *te; if (pd_get(pf, os, oe, "ToUnicode", &ts, &te)) { u8 *dec; size_t dn; if (!pd_stream(pf, ts, te, &dec, &dn) && dec) { pd_load_cmap(f, dec, dn); free(dec); } }
    const u8 *st, *se; if (pd_dget(os, oe, "Subtype", &st, &se) && se - st >= 6 && !memcmp(st, "/Type0", 6)) { if (!f->nm) f->two = 1; else if (!f->two) f->two = 0; if (!f->nm) f->simple_ok = 0; }
    else f->two = 0;
    return f;
}
static const u16 CP1252[32] = { 0x20AC,0x20,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x20,0x17D,0x20,0x20,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x20,0x17E,0x178 };
static void pd_decode(PFont *f, const u8 *s, size_t n, CBuf *o) {
    size_t step = f && f->two ? 2 : 1;
    for (size_t i = 0; i + step <= n; i += step) {
        u32 code = step == 2 ? (u32)(s[i] << 8 | s[i + 1]) : s[i]; int found = 0;
        if (f && f->nm) for (int k = 0; k < f->nm; k++) if (code >= f->m[k].lo && code <= f->m[k].hi) {
            CmapEnt *m = &f->m[k]; for (int c = 0; c < m->ncp; c++) { u32 cp = m->cp[c]; if (c == 0 && m->hi > m->lo && m->ncp == 1) cp += code - m->lo; if (cp) cb_utf8(o, cp); } found = 1; break; }
        if (found) continue;
        if (f && f->two) { continue; }
        if (code >= 0x80 && code < 0xA0) cb_utf8(o, CP1252[code - 0x80]); else if (code >= 32 || code == '\t') cb_utf8(o, code);
    }
}
/* ---- content stream interpreter ---- */
typedef struct { double a, b, c, d, e, f; } Mx;
static Mx mx_mul(Mx m, Mx n) { Mx r; r.a = m.a * n.a + m.b * n.c; r.b = m.a * n.b + m.b * n.d; r.c = m.c * n.a + m.d * n.c; r.d = m.c * n.b + m.d * n.d; r.e = m.e * n.a + m.f * n.c + n.e; r.f = m.e * n.b + m.f * n.d + n.f; return r; }
typedef struct { Chunk *c; int n, cap; } ChunkList;
static void ch_add(ChunkList *l, double x, double y, double size, const char *t, size_t len) {
    if (!len) return; if (l->n >= l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->c = (Chunk *)realloc(l->c, l->cap * sizeof(Chunk)); }
    Chunk *c = &l->c[l->n++]; c->x = x; c->y = y; c->size = size; c->t = (char *)malloc(len + 1); memcpy(c->t, t, len); c->t[len] = 0; c->len = 0;
    for (size_t i = 0; i < len; i++) if (((u8)t[i] & 0xC0) != 0x80) c->len++;
}
typedef struct { double num[8]; int nn; u8 *str; size_t slen; int isstr; u8 name[64]; } Opnd;
static int pd_literal(const u8 **pp, const u8 *e, CBuf *o) {
    const u8 *p = *pp + 1; int dep = 1;
    while (p < e && dep > 0) {
        u8 c = *p++;
        if (c == '\\' && p < e) { u8 d = *p++; if (d == 'n') cb_addc(o, '\n'); else if (d == 'r') cb_addc(o, '\r'); else if (d == 't') cb_addc(o, '\t'); else if (d == 'b') cb_addc(o, '\b'); else if (d == 'f') cb_addc(o, '\f');
            else if (d >= '0' && d <= '7') { int v = d - '0'; for (int k = 0; k < 2 && p < e && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0'); cb_addc(o, (char)v); }
            else if (d == '\r') { if (p < e && *p == '\n') p++; } else if (d == '\n') {} else cb_addc(o, (char)d); }
        else if (c == '(') { dep++; cb_addc(o, '('); } else if (c == ')') { if (--dep > 0) cb_addc(o, ')'); } else cb_addc(o, (char)c);
    }
    *pp = p; return 0;
}
static void pd_hexstr(const u8 **pp, const u8 *e, CBuf *o) {
    const u8 *p = *pp + 1; int hi = -1;
    while (p < e && *p != '>') { if (isxdigit(*p)) { int v = isdigit(*p) ? *p - '0' : (*p | 32) - 'a' + 10; if (hi < 0) hi = v; else { cb_addc(o, (char)(hi * 16 + v)); hi = -1; } } p++; }
    if (hi >= 0) cb_addc(o, (char)(hi * 16)); *pp = p < e ? p + 1 : e;
}
typedef struct { const char *name; PFont *f; } FontSlot;
static void pd_run(Pdf *pf, const u8 *s, size_t n, FontSlot *fonts, int nf, ChunkList *out, double ph) {
    const u8 *p = s, *e = s + n; double nums[16]; int nn = 0; CBuf strs[4]; int nstr = 0; memset(strs, 0, sizeof strs);
    CBuf tj[64]; int tjn[64]; double tjk[64]; int ntj = 0, in_arr = 0; (void)tjn; char lastname[64] = "";
    Mx ctm = { 1, 0, 0, 1, 0, 0 }, stack[16]; int sp = 0; Mx tm = { 1, 0, 0, 1, 0, 0 }, tlm = tm; double fsz = 12, lead = 0; PFont *font = NULL;
    for (int i = 0; i < 4; i++) memset(&strs[i], 0, sizeof strs[i]); memset(tj, 0, sizeof tj);
    while (p < e) {
        p = pd_ws(p, e); if (p >= e) break; u8 c = *p;
        if (c == '(') { if (in_arr) { if (ntj < 64) { tj[ntj].n = 0; pd_literal(&p, e, &tj[ntj]); tjk[ntj] = 0; ntj++; } else { CBuf t = { 0 }; pd_literal(&p, e, &t); free(t.s); } } else { if (nstr < 4) { strs[nstr].n = 0; pd_literal(&p, e, &strs[nstr]); nstr++; } else { CBuf t = { 0 }; pd_literal(&p, e, &t); free(t.s); } } continue; }
        if (c == '<' && p + 1 < e && p[1] == '<') { p = pd_skip(p, e); continue; }
        if (c == '<') { if (in_arr) { if (ntj < 64) { tj[ntj].n = 0; pd_hexstr(&p, e, &tj[ntj]); tjk[ntj] = 0; ntj++; } else { CBuf t = { 0 }; pd_hexstr(&p, e, &t); free(t.s); } } else { if (nstr < 4) { strs[nstr].n = 0; pd_hexstr(&p, e, &strs[nstr]); nstr++; } else { CBuf t = { 0 }; pd_hexstr(&p, e, &t); free(t.s); } } continue; }
        if (c == '[') { in_arr = 1; ntj = 0; p++; continue; }
        if (c == ']') { in_arr = 0; p++; continue; }
        if (c == '/') { const u8 *q = p + 1; size_t l = 0; while (q < e && !pd_space(*q) && !pd_delim(*q)) { if (l < 63) lastname[l++] = (char)*q; q++; } lastname[l] = 0; p = q; continue; }
        if (c == '-' || c == '+' || c == '.' || isdigit(c)) {
            const u8 *q = p; char b[40]; size_t l = 0; while (q < e && (isdigit(*q) || *q == '.' || *q == '-' || *q == '+') && l < 39) b[l++] = (char)*q++; b[l] = 0; p = q;
            if (in_arr) { if (ntj < 64) { tj[ntj].n = 0; tjk[ntj] = atof(b); ntj++; } } else if (nn < 16) nums[nn++] = atof(b); continue; }
        if (c == '>' || c == ')' || c == '{' || c == '}') { p++; continue; }
        /* operator */
        char op[8]; size_t l = 0; while (p < e && !pd_space(*p) && !pd_delim(*p)) { if (l < 7) op[l++] = (char)*p; p++; } op[l] = 0;
        double fs_eff = fsz * fabs(tm.d != 0 ? tm.d : tm.a) * fabs(ctm.d != 0 ? ctm.d : ctm.a);
        if (!strcmp(op, "BT")) { tm = tlm = (Mx){ 1, 0, 0, 1, 0, 0 }; }
        else if (!strcmp(op, "q")) { if (sp < 16) stack[sp++] = ctm; }
        else if (!strcmp(op, "Q")) { if (sp > 0) ctm = stack[--sp]; }
        else if (!strcmp(op, "cm") && nn >= 6) { Mx m = { nums[0], nums[1], nums[2], nums[3], nums[4], nums[5] }; ctm = mx_mul(m, ctm); }
        else if (!strcmp(op, "Tf")) { font = NULL; for (int i = 0; i < nf; i++) if (!strcmp(fonts[i].name, lastname)) font = fonts[i].f; if (nn >= 1) fsz = nums[nn - 1]; }
        else if (!strcmp(op, "TL") && nn >= 1) lead = nums[0];
        else if (!strcmp(op, "Td") && nn >= 2) { tlm.e += nums[0] * tlm.a + nums[1] * tlm.c; tlm.f += nums[0] * tlm.b + nums[1] * tlm.d; tm = tlm; }
        else if (!strcmp(op, "TD") && nn >= 2) { lead = -nums[1]; tlm.e += nums[0] * tlm.a + nums[1] * tlm.c; tlm.f += nums[0] * tlm.b + nums[1] * tlm.d; tm = tlm; }
        else if (!strcmp(op, "Tm") && nn >= 6) { tm = tlm = (Mx){ nums[0], nums[1], nums[2], nums[3], nums[4], nums[5] }; }
        else if (!strcmp(op, "T*")) { tlm.e += -lead * tlm.c; tlm.f += -lead * tlm.d; tm = tlm; }
        else if (!strcmp(op, "Tj") || !strcmp(op, "'") || !strcmp(op, "\"") || !strcmp(op, "TJ")) {
            if (op[0] == '\'' || op[0] == '"') { tlm.e += -lead * tlm.c; tlm.f += -lead * tlm.d; tm = tlm; }
            CBuf t = { 0 };
            if (!strcmp(op, "TJ")) {
                for (int i = 0; i < ntj; i++) { if (tj[i].n) pd_decode(font, (const u8 *)tj[i].s, tj[i].n, &t); else if (tjk[i] < -220 && t.n && t.s[t.n - 1] != ' ') cb_addc(&t, ' '); tm.e -= tjk[i] / 1000.0 * fsz * tm.a; }
            } else if (nstr) pd_decode(font, (const u8 *)strs[nstr - 1].s, strs[nstr - 1].n, &t);
            if (t.n) {
                double X = tm.e * ctm.a + tm.f * ctm.c + ctm.e, Y = tm.e * ctm.b + tm.f * ctm.d + ctm.f;
                ch_add(out, X, ph - Y, fs_eff > 1 ? fs_eff : 10, t.s, t.n);
                int chars = 0; for (size_t i = 0; i < t.n; i++) if (((u8)t.s[i] & 0xC0) != 0x80) chars++;
                tm.e += chars * fsz * 0.5 * tm.a;
            }
            free(t.s);
        }
        nn = 0; nstr = 0; ntj = 0; in_arr = 0;
    }
    for (int i = 0; i < 4; i++) free(strs[i].s); for (int i = 0; i < 64; i++) free(tj[i].s);
}
static int ch_cmp(const void *a, const void *b) {
    const Chunk *x = (const Chunk *)a, *y = (const Chunk *)b; double th = (x->size < y->size ? x->size : y->size) * 0.5;
    if (fabs(x->y - y->y) > th) return x->y < y->y ? -1 : 1; return x->x < y->x ? -1 : x->x > y->x ? 1 : 0;
}
static void pd_emit_page(DPage *pg, ChunkList *l) {
    if (!l->n) return; qsort(l->c, l->n, sizeof(Chunk), ch_cmp);
    int i = 0;
    while (i < l->n) {
        CBuf line = { 0 }; Chunk *f = &l->c[i]; double x0 = f->x, y0 = f->y, sz = f->size, endx = f->x; int prevlen = 0; int j = i;
        for (; j < l->n; j++) {
            Chunk *c = &l->c[j]; if (fabs(c->y - y0) > sz * 0.5) break;
            if (j > i) { double gap = c->x - endx; int need = 0;
                if (line.n && line.s[line.n - 1] != ' ' && c->t[0] != ' ') { if (prevlen <= 1 && c->len <= 1) need = gap > c->size * 0.4; else need = gap > c->size * 0.12; }
                if (gap < -c->size * 3) { /* new column on the same baseline */ need = 1; }
                if (need) cb_addc(&line, ' '); }
            cb_adds(&line, c->t); endx = c->x + c->len * c->size * 0.5; prevlen = c->len;
        }
        while (line.n && line.s[line.n - 1] == ' ') line.s[--line.n] = 0;
        if (line.n) { DBlock *b = page_add(pg, B_ABS, line.s, line.n); b->x = x0; b->y = y0 - sz; b->size = sz; }
        free(line.s); i = j > i ? j : i + 1;
    }
}
static void pd_page(Pdf *pf, Doc *d, const u8 *ps, const u8 *pe, const u8 *res_s, const u8 *res_e, double *mb, int *pageno) {
    char title[32]; snprintf(title, sizeof title, "Page %d", ++*pageno); DPage *pg = doc_newpage(d, title);
    double box[4] = { mb[0], mb[1], mb[2], mb[3] }; const u8 *bs, *be;
    if (pd_get(pf, ps, pe, "MediaBox", &bs, &be)) { const u8 *q = bs + 1; for (int i = 0; i < 4; i++) { box[i] = pd_num(q, be); q = pd_ws(q, be); q = pd_skip(q, be); } }
    pg->pw = box[2] - box[0]; pg->ph = box[3] - box[1]; if (pg->pw < 10 || pg->ph < 10) { pg->pw = 612; pg->ph = 792; }
    const u8 *rs = res_s, *re = res_e, *x, *y; if (pd_get(pf, ps, pe, "Resources", &x, &y)) { rs = x; re = y; }
    FontSlot fs[64]; int nf = 0;
    if (rs) { const u8 *fds, *fde; if (pd_get(pf, rs, re, "Font", &fds, &fde)) {
        const u8 *q = pd_ws(fds, fde); if (q + 1 < fde && q[0] == '<' && q[1] == '<') { q += 2;
            while (nf < 64) { q = pd_ws(q, fde); if (q >= fde || *q == '>') break; if (*q != '/') { q = pd_skip(q, fde); continue; }
                const u8 *ks = q + 1, *k2 = ks; while (k2 < fde && !pd_space(*k2) && !pd_delim(*k2)) k2++;
                const u8 *vs = pd_ws(k2, fde), *ve = pd_skip(vs, fde); int num = 0; const u8 *os, *oe; static char names[64][64];
                if (pd_isref(vs, ve, &num) && pd_resolve(pf, vs, ve, &os, &oe)) { size_t l = (size_t)(k2 - ks); if (l > 63) l = 63; memcpy(names[nf], ks, l); names[nf][l] = 0; fs[nf].name = names[nf]; fs[nf].f = pd_font(pf, num, os, oe); nf++; }
                else if (vs + 1 < ve && vs[0] == '<') { size_t l = (size_t)(k2 - ks); if (l > 63) l = 63; memcpy(names[nf], ks, l); names[nf][l] = 0; fs[nf].name = names[nf]; fs[nf].f = pd_font(pf, -(nf + 1), vs, ve); nf++; }
                q = ve; } } } }
    /* contents */
    const u8 *cs, *ce; ChunkList cl = { 0 };
    if (pd_dget(ps, pe, "Contents", &cs, &ce)) {
        const u8 *items[64][2]; int ni = 0; const u8 *a = pd_ws(cs, ce);
        if (*a == '[') { a++; while (ni < 64) { a = pd_ws(a, ce); if (a >= ce || *a == ']') break; const u8 *ve = pd_skip(a, ce); const u8 *os, *oe; if (pd_resolve(pf, a, ve, &os, &oe)) { items[ni][0] = os; items[ni][1] = oe; ni++; } a = ve; } }
        else { const u8 *os, *oe; if (pd_resolve(pf, cs, ce, &os, &oe)) { items[0][0] = os; items[0][1] = oe; ni = 1; } }
        CBuf all = { 0 };
        for (int i = 0; i < ni; i++) { u8 *dec; size_t dn; if (!pd_stream(pf, items[i][0], items[i][1], &dec, &dn) && dec) { cb_add(&all, (const char *)dec, dn); cb_addc(&all, '\n'); free(dec); } }
        if (all.n) pd_run(pf, (const u8 *)all.s, all.n, fs, nf, &cl, pg->ph);
        free(all.s);
    }
    pd_emit_page(pg, &cl); for (int i = 0; i < cl.n; i++) free(cl.c[i].t); free(cl.c);
}
static void pd_walk(Pdf *pf, Doc *d, const u8 *ns, const u8 *ne, const u8 *rs, const u8 *re, double *mb, int *pageno, int depth) {
    if (depth > 24 || d->npg > 3000) return;
    const u8 *ks, *ke; const u8 *r2s = rs, *r2e = re, *x, *y; double box[4] = { mb[0], mb[1], mb[2], mb[3] };
    if (pd_get(pf, ns, ne, "Resources", &x, &y)) { r2s = x; r2e = y; }
    if (pd_get(pf, ns, ne, "MediaBox", &x, &y)) { const u8 *q = x + 1; for (int i = 0; i < 4; i++) { box[i] = pd_num(q, y); q = pd_ws(q, y); q = pd_skip(q, y); } }
    if (pd_dget(ns, ne, "Kids", &ks, &ke)) {
        const u8 *a = pd_ws(ks, ke); const u8 *os, *oe; if (*a != '[') { if (pd_resolve(pf, ks, ke, &os, &oe)) a = pd_ws(os, oe), ke = oe; }
        if (*a == '[') { a++; while (a < ke) { a = pd_ws(a, ke); if (a >= ke || *a == ']') break; const u8 *ve = pd_skip(a, ke); if (pd_resolve(pf, a, ve, &os, &oe)) pd_walk(pf, d, os, oe, r2s, r2e, box, pageno, depth + 1); a = ve; } }
    } else { PFont *dummy = NULL; (void)dummy; pd_page(pf, d, ns, ne, r2s, r2e, box, pageno); }
}
static int pdf_load(const u8 *data, size_t n, Doc *d) {
    memset(d, 0, sizeof *d); d->kind = DOC_PDF; Pdf pf; memset(&pf, 0, sizeof pf); pf.d = data; pf.n = n;
    if (n < 16 || memcmp(data, "%PDF", 4)) { snprintf(d->err, sizeof d->err, "Not a PDF file"); return -1; }
    pd_scan(&pf);
    if (pd_find(data, data + (n > 4000000 ? 4000000 : n), "/Encrypt") && pd_find(data, data + n, "/Encrypt ")) { /* encrypted? only flag when trailer references it */ }
    int root = -1, pagesobj = -1;
    for (int i = 0; i < pf.ntab; i++) { PRef r = pf.tab[i]; if (!r.p) continue; const u8 *ts, *te;
        if (pd_dget(r.p, r.e, "Type", &ts, &te) && te - ts >= 8 && !memcmp(ts, "/Catalog", 8)) { const u8 *vs, *ve; if (pd_dget(r.p, r.e, "Pages", &vs, &ve) && pd_isref(vs, ve, &pagesobj)) { root = i; } } }
    (void)root;
    if (pagesobj < 0 || pagesobj >= pf.ntab || !pf.tab[pagesobj].p) {
        /* fallback: any /Type /Pages without parent */
        for (int i = 0; i < pf.ntab; i++) { PRef r = pf.tab[i]; if (!r.p) continue; const u8 *ts, *te, *ps, *pe; if (pd_dget(r.p, r.e, "Type", &ts, &te) && te - ts >= 6 && !memcmp(ts, "/Pages", 6) && !pd_dget(r.p, r.e, "Parent", &ps, &pe)) { pagesobj = i; break; } }
    }
    if (pagesobj < 0) { snprintf(d->err, sizeof d->err, "No pages found (encrypted or damaged PDF)"); }
    else { double mb[4] = { 0, 0, 612, 792 }; int pn = 0; pd_walk(&pf, d, pf.tab[pagesobj].p, pf.tab[pagesobj].e, NULL, NULL, mb, &pn, 0); }
    for (int i = 0; i < pf.nfonts; i++) free(pf.fonts[i].m);
    for (int i = 0; i < pf.nkeep; i++) free(pf.keep[i]); free(pf.keep); free(pf.tab);
    if (!d->npg && !d->err[0]) snprintf(d->err, sizeof d->err, "No readable pages");
    return d->npg ? 0 : -1;
}


static WCHAR g_logpath[MAX_PATH];
static int ON_WINE = 0;
static void fm_log(const char *fmt, ...) {
    FILE *f = _wfopen(g_logpath, L"ab"); if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d INFO ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static void log_init(void) {
    WCHAR t[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"TEMP", t, MAX_PATH);
    if (!n || n >= MAX_PATH) lstrcpyW(t, L"C:\\");
    wfmt(g_logpath, MAX_PATH, L"%ls%lswinshell_fm.log", t, (t[lstrlenW(t) - 1] == '\\') ? L"" : L"\\");
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(g_logpath, GetFileExInfoStandard, &d) && d.nFileSizeLow > 1000000) {
        WCHAR o[MAX_PATH]; wfmt(o, MAX_PATH, L"%ls.1", g_logpath);
        MoveFileExW(g_logpath, o, MOVEFILE_REPLACE_EXISTING);
    }
}
static void detect_wine(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) { ON_WINE = 1; return; }
    WCHAR b[8];
    if (GetEnvironmentVariableW(L"WINEPREFIX", b, 8) || GetEnvironmentVariableW(L"WINELOADER", b, 8) ||
        GetEnvironmentVariableW(L"WINEARCH", b, 8)) ON_WINE = 1;
}

/* ========================================================================== */
/* colour helpers                                                             */
/* ========================================================================== */
#define RGBH(x) RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)
static COLORREF mixc(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
static COLORREF shadec(COLORREF c, double f) {
    int r = (int)(GetRValue(c) * f), g = (int)(GetGValue(c) * f), b = (int)(GetBValue(c) * f);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}
static double lumc(COLORREF c) { return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c); }
static COLORREF contrast(COLORREF bg) { return lumc(bg) < 140 ? RGB(255, 255, 255) : RGB(0, 0, 0); }
static COLORREF hex_color(const WCHAR *t, COLORREF def) {
    if (!t) return def; if (*t == '#') t++;
    if (lstrlenW(t) < 6) return def;
    WCHAR *e; WCHAR b[7]; lstrcpynW(b, t, 7); u32 v = (u32)wcstoul(b, &e, 16);
    if (*e) return def; return RGBH(v);
}

/* ========================================================================== */
/* CONFIG (JSON, shares file format with the Python version)                  */
/* ========================================================================== */
typedef struct { WCHAR k[64]; WCHAR *raw; } Extra;
typedef struct {
    WCHAR theme[16], accent[16], font[64], view[16], sort[16], last_dir[MAX_PATH];
    WCHAR os_style[16], style_accent[16], sort_dirs[8];
    int font_size, icon_size, preview_width, search_limit, window_w, window_h, window_x, window_y, session_cur;
    double scale;
    int show_hidden, show_preview, asc, restore_session, style_dark, style_fonts, search_subfolders;
    int autostart_enabled, run_at_login, thumbnails;
    WCHAR doc_font[64], search_ext[64];
    int show_tree, checkboxes, thumb_img, thumb_vid, thumb_gfx, drag_drop, rubber, admin_menu, video_mode, pv_zoom, ribbon_open, ribbon_tab, auto_class, about_day, show_counts;
    int pvsz0, pvsz1, pvsz2, pvsz3, pvsz4, pvsz5, pvsz6, pvsz7;
    SL favorites, pins, recent_folders, recent_files, session_tabs, pinned_tabs, autostart_apps;
    SL doc_fonts, search_hist, search_pins, folder_colors, icon_map, assoc;
    Extra *extra; int nextra;
} Cfg;
static Cfg C;
static WCHAR CONFIG_DIR[MAX_PATH], CONFIG_PATH[MAX_PATH];

enum { FT_STR, FT_INT, FT_BOOL, FT_DBL, FT_LIST };
typedef struct { const WCHAR *key; int type; size_t off; int cap; } Field;
#define FLD_S(k, m) { L##k, FT_STR, offsetof(Cfg, m), (int)(sizeof(((Cfg *)0)->m) / sizeof(WCHAR)) }
#define FLD_I(k, m) { L##k, FT_INT, offsetof(Cfg, m), 0 }
#define FLD_B(k, m) { L##k, FT_BOOL, offsetof(Cfg, m), 0 }
#define FLD_D(k, m) { L##k, FT_DBL, offsetof(Cfg, m), 0 }
#define FLD_L(k, m) { L##k, FT_LIST, offsetof(Cfg, m), 0 }
static const Field CFG_FIELDS[] = {
    FLD_S("theme", theme), FLD_S("accent", accent), FLD_S("font", font), FLD_I("font_size", font_size), FLD_D("scale", scale),
    FLD_S("view", view), FLD_B("show_hidden", show_hidden), FLD_I("icon_size", icon_size), FLD_B("show_preview", show_preview),
    FLD_I("preview_width", preview_width), FLD_L("favorites", favorites), FLD_L("pins", pins), FLD_S("sort", sort),
    FLD_B("asc", asc), FLD_S("last_dir", last_dir), FLD_I("window_w", window_w), FLD_I("window_h", window_h),
    FLD_I("window_x", window_x), FLD_I("window_y", window_y), FLD_L("recent_folders", recent_folders),
    FLD_L("recent_files", recent_files), FLD_B("restore_session", restore_session), FLD_L("session_tabs", session_tabs),
    FLD_I("session_cur", session_cur), FLD_L("pinned_tabs", pinned_tabs), FLD_S("os_style", os_style),
    FLD_B("style_dark", style_dark), FLD_S("style_accent", style_accent), FLD_B("style_fonts", style_fonts),
    FLD_B("search_subfolders", search_subfolders), FLD_I("search_limit", search_limit),
    FLD_B("autostart_enabled", autostart_enabled), FLD_L("autostart_apps", autostart_apps),
    FLD_B("run_at_login", run_at_login), FLD_B("thumbnails", thumbnails), FLD_S("sort_dirs", sort_dirs),
    FLD_S("doc_font", doc_font), FLD_S("search_ext", search_ext), FLD_B("show_tree", show_tree), FLD_B("checkboxes", checkboxes),
    FLD_B("thumb_img", thumb_img), FLD_B("thumb_vid", thumb_vid), FLD_B("thumb_gfx", thumb_gfx), FLD_B("drag_drop", drag_drop),
    FLD_B("rubber", rubber), FLD_B("admin_menu", admin_menu), FLD_I("video_mode", video_mode), FLD_I("pv_zoom", pv_zoom),
    FLD_B("ribbon_open", ribbon_open), FLD_I("ribbon_tab", ribbon_tab), FLD_B("auto_class", auto_class), FLD_I("about_day", about_day),
    FLD_B("show_counts", show_counts), FLD_I("pvsz_text", pvsz0), FLD_I("pvsz_image", pvsz1), FLD_I("pvsz_video", pvsz2), FLD_I("pvsz_pdf", pvsz3),
    FLD_I("pvsz_word", pvsz4), FLD_I("pvsz_excel", pvsz5), FLD_I("pvsz_ppt", pvsz6), FLD_I("pvsz_archive", pvsz7),
    FLD_L("doc_fonts", doc_fonts), FLD_L("search_hist", search_hist), FLD_L("search_pins", search_pins),
    FLD_L("folder_colors", folder_colors), FLD_L("icon_map", icon_map), FLD_L("assoc", assoc),
};
#define NFIELDS ARRLEN(CFG_FIELDS)

static void cfg_defaults(void) {
    for (int i = 0; i < C.nextra; i++) free(C.extra[i].raw);
    free(C.extra);
    SL *lists[] = { &C.favorites, &C.pins, &C.recent_folders, &C.recent_files, &C.session_tabs, &C.pinned_tabs, &C.autostart_apps,
                    &C.doc_fonts, &C.search_hist, &C.search_pins, &C.folder_colors, &C.icon_map, &C.assoc };
    enum { NLISTS = 13 };
    SL keep[NLISTS];
    for (int i = 0; i < NLISTS; i++) { sl_clear(lists[i]); keep[i] = *lists[i]; }
    memset(&C, 0, sizeof C);
    for (int i = 0; i < NLISTS; i++) *lists[i] = keep[i];
    scpy(C.theme, 16, L"dark"); scpy(C.accent, 16, L"#0078D4"); scpy(C.font, 64, L"Segoe UI");
    C.font_size = 14; C.scale = 1.0; scpy(C.view, 16, L"details"); C.icon_size = 64;
    C.show_preview = 1; C.preview_width = 300; scpy(C.sort, 16, L"name"); C.asc = 1;
    C.window_w = 1180; C.window_h = 720; C.window_x = CFG_UNSET; C.window_y = CFG_UNSET;
    C.restore_session = 1; scpy(C.os_style, 16, L"win10"); C.style_fonts = 1; C.search_subfolders = 1;
    C.search_limit = 20000; C.thumbnails = 1; scpy(C.sort_dirs, 8, L"first");
    C.checkboxes = 1; C.thumb_img = 1; C.thumb_vid = 1; C.thumb_gfx = 1; C.drag_drop = 1; C.rubber = 1; C.admin_menu = 1;
    C.video_mode = 0; C.pv_zoom = 100; C.ribbon_open = 1; C.ribbon_tab = 1; C.auto_class = 1; C.show_counts = 1;
    C.pvsz0 = 1; C.pvsz1 = 1; C.pvsz2 = 1; C.pvsz3 = 2; C.pvsz4 = 2; C.pvsz5 = 2; C.pvsz6 = 2; C.pvsz7 = 1;
}

/* --- tiny JSON reader (operates on UTF-16) --- */
static const WCHAR *js_ws(const WCHAR *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const WCHAR *js_str(const WCHAR *p, WCHAR *out, int cap) {
    int n = 0; p++;
    while (*p && *p != '"') {
        WCHAR c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': { unsigned v = 0; for (int i = 0; i < 4 && *p; i++) { WCHAR h = *p++;
                        v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10); } c = (WCHAR)v; } break;
            default: break;
            }
        }
        if (out && n < cap - 1) out[n++] = c;
    }
    if (*p == '"') p++;
    if (out) out[n] = 0;
    return p;
}
static const WCHAR *js_skip(const WCHAR *p) {
    p = js_ws(p);
    if (*p == '"') return js_str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int d = 0;
        do {
            if (*p == '"') { p = js_str(p, NULL, 0); continue; }
            if (*p == '{' || *p == '[') d++; else if (*p == '}' || *p == ']') d--;
            p++;
        } while (*p && d > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}
static const WCHAR *js_num(const WCHAR *p, double *v, int *isnull) {
    p = js_ws(p); *isnull = 0; *v = 0;
    if (!wcsncmp(p, L"true", 4)) { *v = 1; return p + 4; }
    if (!wcsncmp(p, L"false", 5)) { return p + 5; }
    if (!wcsncmp(p, L"null", 4)) { *isnull = 1; return p + 4; }
    WCHAR *e; double d = wcstod(p, &e);
    if (e == p) { *isnull = 1; return js_skip(p); }
    *v = d; return e;
}
static void cfg_parse(const WCHAR *p) {
    p = js_ws(p); if (*p != '{') return; p++;
    for (;;) {
        p = js_ws(p);
        if (*p == '}' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        WCHAR key[64]; p = js_str(p, key, 64); p = js_ws(p); if (*p == ':') p++; p = js_ws(p);
        const Field *f = NULL;
        for (int i = 0; i < (int)NFIELDS; i++) if (!wcscmp(CFG_FIELDS[i].key, key)) { f = &CFG_FIELDS[i]; break; }
        if (!f) {
            const WCHAR *e = js_skip(p);
            C.extra = (Extra *)realloc(C.extra, (C.nextra + 1) * sizeof(Extra));
            Extra *x = &C.extra[C.nextra++]; scpy(x->k, 64, key);
            int n = (int)(e - p); x->raw = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)); memcpy(x->raw, p, n * sizeof(WCHAR)); x->raw[n] = 0;
            p = e; continue;
        }
        u8 *base = (u8 *)&C + f->off;
        if (f->type == FT_STR) {
            if (*p == '"') p = js_str(p, (WCHAR *)base, f->cap); else p = js_skip(p);
        } else if (f->type == FT_LIST) {
            SL *l = (SL *)base;
            if (*p == '[') {
                sl_clear(l); p++;
                for (;;) {
                    p = js_ws(p);
                    if (*p == ']' || !*p) { if (*p) p++; break; }
                    if (*p == ',') { p++; continue; }
                    if (*p == '"') { WCHAR t[MAX_PATH]; p = js_str(p, t, MAX_PATH); sl_add(l, t); }
                    else p = js_skip(p);
                }
            } else p = js_skip(p);
        } else {
            double v; int nl; p = js_num(p, &v, &nl);
            if (!nl) {
                if (f->type == FT_DBL) *(double *)base = v; else *(int *)base = (int)v;
            }
        }
    }
}
static WCHAR *read_text_file(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    if (sz == INVALID_FILE_SIZE || sz > 64u << 20) { CloseHandle(h); return NULL; }
    char *buf = (char *)malloc(sz + 1); ReadFile(h, buf, sz, &rd, NULL); CloseHandle(h);
    char *s = buf; int n = (int)rd;
    if (n >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) { s += 3; n -= 3; }
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    WCHAR *out = (WCHAR *)malloc((w + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, n, out, w); out[w] = 0; free(buf); return out;
}
static int write_utf8_file(const WCHAR *path, const WCHAR *w, int n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    char *b = (char *)malloc(len + 1); WideCharToMultiByte(CP_UTF8, 0, w, n, b, len, NULL, NULL);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    if (h != INVALID_HANDLE_VALUE) { DWORD wr; ok = WriteFile(h, b, len, &wr, NULL) && wr == (DWORD)len; CloseHandle(h); }
    free(b); return ok;
}
static void cfg_validate(void);
static void cfg_save(void) {
    SB b = { 0 }; sb_add(&b, L"{\n");
    for (int i = 0; i < (int)NFIELDS; i++) {
        const Field *f = &CFG_FIELDS[i]; u8 *base = (u8 *)&C + f->off;
        sb_add(&b, L"  "); sb_jstr(&b, f->key); sb_add(&b, L": ");
        if (f->type == FT_STR) sb_jstr(&b, (WCHAR *)base);
        else if (f->type == FT_BOOL) sb_add(&b, *(int *)base ? L"true" : L"false");
        else if (f->type == FT_INT) { int v = *(int *)base; if (v == CFG_UNSET) sb_add(&b, L"null"); else sb_addf(&b, L"%d", v); }
        else if (f->type == FT_DBL) sb_addf(&b, L"%g", *(double *)base);
        else {
            SL *l = (SL *)base;
            if (!l->n) sb_add(&b, L"[]");
            else { sb_add(&b, L"[\n"); for (int k = 0; k < l->n; k++) { sb_add(&b, L"    "); sb_jstr(&b, l->v[k]); sb_add(&b, k + 1 < l->n ? L",\n" : L"\n"); } sb_add(&b, L"  ]"); }
        }
        sb_add(&b, (i + 1 < (int)NFIELDS || C.nextra) ? L",\n" : L"\n");
    }
    for (int i = 0; i < C.nextra; i++) {
        sb_add(&b, L"  "); sb_jstr(&b, C.extra[i].k); sb_add(&b, L": "); sb_add(&b, C.extra[i].raw);
        sb_add(&b, i + 1 < C.nextra ? L",\n" : L"\n");
    }
    sb_add(&b, L"}\n");
    if (!write_utf8_file(CONFIG_PATH, b.s, b.n)) fm_log("cfg save failed");
    free(b.s);
}
static void cfg_init_paths(void) {
    WCHAR pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PROGRAMDATA", pd, MAX_PATH)) lstrcpyW(pd, L"C:\\ProgramData");
    wfmt(CONFIG_DIR, MAX_PATH, L"%ls\\WinShell", pd);
    wfmt(CONFIG_PATH, MAX_PATH, L"%ls\\fm_config.json", CONFIG_DIR);
}
static void cfg_load(void) {
    cfg_defaults();
    SHCreateDirectoryExW(NULL, CONFIG_DIR, NULL);
    WCHAR *t = read_text_file(CONFIG_PATH);
    if (t) { cfg_parse(t); free(t); }
    cfg_validate();
    cfg_save();
}

/* ========================================================================== */
/* THEMES / PALETTE / OS STYLES                                               */
/* ========================================================================== */
typedef struct { const WCHAR *name; u32 bg, alt; int dark; } Theme;
static const Theme FM_THEMES[] = {
    { L"light", 0xFFFFFF, 0xF3F3F3, 0 }, { L"dark", 0x202020, 0x2B2B2B, 1 },
    { L"amoled", 0x000000, 0x101010, 1 }, { L"midnight", 0x121826, 0x1B2438, 1 },
    { L"ocean", 0x0F2837, 0x17384B, 1 }, { L"nord", 0x2E3440, 0x3B4252, 1 },
};
#define NTHEMES ARRLEN(FM_THEMES)
static const Theme *theme_by_name(const WCHAR *n) {
    for (int i = 0; i < (int)NTHEMES; i++) if (wieq(FM_THEMES[i].name, n)) return &FM_THEMES[i];
    return &FM_THEMES[1];
}
static const WCHAR *FM_ACCENTS[] = { L"#0078D4", L"#15A6F0", L"#E95420", L"#8FA876", L"#3498DB", L"#0A64DC", L"#E81123", L"#8E5BD8" };

typedef struct {
    COLORREF dark, bg, alt, text, dim, line, accent, border, hover, sel, cap, cap_text, input, input_border;
    COLORREF btn, btn_press, btn_border, sb_track, sb_thumb, side, tool, status, cap2, nav_sel, nav_sel_text;
    COLORREF sel_text, zebra, hdr_bg, hdr;
} Pal;

static void palette_core(Pal *p, COLORREF bg, COLORREF alt, COLORREF text, int dark, COLORREF acc) {
    COLORREF black = RGB(0, 0, 0);
    memset(p, 0, sizeof *p);
    p->dark = dark; p->bg = bg; p->alt = alt; p->text = text; p->accent = acc;
    p->dim = mixc(text, bg, 0.42); p->line = mixc(bg, text, 0.16); p->border = mixc(bg, text, 0.28);
    if (dark) {
        p->hover = mixc(bg, text, 0.14); p->sel = mixc(bg, acc, 0.42); p->cap = mixc(bg, black, 0.10);
        p->input = mixc(bg, text, 0.08); p->input_border = mixc(bg, text, 0.30);
        p->btn = mixc(bg, text, 0.12); p->btn_border = mixc(bg, text, 0.34);
        p->sb_track = mixc(bg, black, 0.35); p->sb_thumb = mixc(bg, text, 0.28);
    } else {
        p->hover = mixc(bg, acc, 0.10); p->sel = mixc(bg, acc, 0.20); p->cap = bg; p->input = bg;
        p->input_border = mixc(bg, text, 0.24); p->btn = mixc(bg, black, 0.04); p->btn_border = mixc(bg, text, 0.24);
        p->sb_track = mixc(bg, black, 0.06); p->sb_thumb = mixc(bg, black, 0.25);
    }
    p->cap_text = contrast(p->cap); p->btn_press = mixc(bg, acc, 0.35); p->hdr = p->dim;
    p->side = alt; p->tool = bg; p->status = alt; p->cap2 = p->cap; p->nav_sel = p->sel;
    p->nav_sel_text = text; p->sel_text = text; p->zebra = bg; p->hdr_bg = bg;
}

/* colour table index: 0 bg,1 side,2 text,3 dim,4 line,5 cap,6 cap2,7 cap_text,8 tool,9 status,
   10 input,11 input_b,12 hover,13 sel,14 nav_sel,15 accent,16 zebra,17 hdr_bg */
enum { LAY_RIBBON, LAY_WIN7, LAY_HEADER, LAY_TOOLBAR, LAY_MENUBAR };
typedef struct {
    const WCHAR *key, *label, *info;
    int layout, cap_h_fm, menubar_h, tool_h, field_h, tb_btn, tb_gap, search_w, nav_w, nav_row, row_h, hdr_h, status_h, sb_w;
    int sel_solid, zebra, round, hdr_raised, nav_tree, base_dark;
    const WCHAR *font[4];
    const WCHAR *sec[4];                 /* home, recent, fav, pc */
    u32 folder;                          /* folder icon colour */
    u32 light[18]; u32 dark[18]; int has_dark;
} Style;

#define PAL_(...) { __VA_ARGS__ }
static const Style OS_STYLES[] = {
 { L"win10", L"Windows 10", L"Command bar - flat - Segoe UI", LAY_RIBBON, 32, 0, 40, 28, 34, 4, 240, 220, 24, 22, 26, 24, 17,
   0, 0, 0, 0, 1, 0, { L"Segoe UI" }, { L"Quick Access", L"Recent", L"Favorites", L"This PC" }, 0xF8D775,
   PAL_(0), PAL_(0), 1 },
 { L"win11", L"Windows 11", L"Fluent - rounded - Mica", LAY_TOOLBAR, 36, 0, 48, 32, 34, 4, 250, 230, 32, 30, 32, 28, 12,
   0, 0, 8, 0, 1, 0, { L"Segoe UI Variable Text", L"Segoe UI" }, { L"Home", L"Recent", L"Favorites", L"This PC" }, 0xFFCE4A,
   PAL_(0xFFFFFF,0xF3F3F3,0x1A1A1A,0x616161,0xE5E5E5,0xF3F3F3,0xF3F3F3,0x1A1A1A,0xFFFFFF,0xF9F9F9,0xFFFFFF,0xD0D0D0,0xEAEAEA,0xCCE4F7,0xE4E4E4,0x005FB8,0xFFFFFF,0xFFFFFF),
   PAL_(0x202020,0x2B2B2B,0xFFFFFF,0xB3B3B3,0x3A3A3A,0x2B2B2B,0x2B2B2B,0xFFFFFF,0x202020,0x272727,0x2D2D2D,0x5A5A5A,0x383838,0x2E4A63,0x3A3A3A,0x4CC2FF,0x202020,0x202020), 1 },
 { L"win7", L"Windows 7", L"Aero glass - command bar - details pane", LAY_WIN7, 30, 0, 40, 26, 28, 4, 210, 190, 22, 21, 24, 52, 17,
   0, 0, 3, 1, 1, 0, { L"Segoe UI" }, { L"Favorites", L"Recent Places", L"Libraries", L"Computer" }, 0xF0CF6A,
   PAL_(0xFFFFFF,0xFFFFFF,0x000000,0x6D6D6D,0xD9E1EC,0xBBD1EC,0xE1ECF9,0x000000,0xF1F5FB,0xF1F5FA,0xFFFFFF,0x8E9BB0,0xE6F1FD,0xCCE4FA,0xD3E6FA,0x3399FF,0xFFFFFF,0xF6F9FD),
   PAL_(0), 0 },
 { L"macos", L"macOS", L"Finder - unified toolbar", LAY_HEADER, 52, 0, 0, 28, 30, 2, 190, 200, 26, 24, 24, 28, 12,
   1, 1, 6, 0, 0, 0, { L"SF Pro Text", L"Helvetica Neue", L"Segoe UI" }, { L"Favorites", L"Recents", L"Pinned", L"Locations" }, 0x5AB8F5,
   PAL_(0xFFFFFF,0xE8E6E8,0x1D1D1F,0x86868B,0xD8D6D8,0xECEAEC,0xDEDCDE,0x3C3C43,0xECEAEC,0xF7F6F7,0xF7F6F7,0xCFCDCF,0xEDEDEF,0x0A64DC,0xCFCDD1,0x0A64DC,0xF5F5F7,0xFFFFFF),
   PAL_(0x1E1E1E,0x2A2829,0xF5F5F7,0x98989D,0x3A383A,0x3A383B,0x2F2D30,0xEDEDED,0x3A383B,0x262426,0x2C2A2D,0x4A484B,0x2E2E30,0x0A64DC,0x4A484B,0x0A64DC,0x252525,0x1E1E1E), 1 },
 { L"ubuntu", L"Ubuntu", L"Nautilus - Yaru header bar", LAY_HEADER, 48, 0, 0, 32, 32, 2, 190, 220, 30, 28, 28, 24, 12,
   1, 0, 6, 0, 0, 0, { L"Ubuntu", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0xE99A5C,
   PAL_(0xFFFFFF,0xF6F5F4,0x333333,0x7E7E7E,0xD5D3CF,0xEBEBEA,0xE1E0DF,0x333333,0xEBEBEA,0xF6F5F4,0xFFFFFF,0xCDC7C2,0xEDEBEA,0xE95420,0xE95420,0xE95420,0xFFFFFF,0xF6F5F4),
   PAL_(0x2B2B2B,0x323232,0xEEEEEC,0xA0A0A0,0x1E1E1E,0x262626,0x202020,0xEEEEEC,0x262626,0x303030,0x1F1F1F,0x4A4A4A,0x3A3A3A,0xE95420,0xE95420,0xE95420,0x2B2B2B,0x303030), 1 },
 { L"zorin", L"Zorin OS", L"Nautilus - rounded - Zorin blue", LAY_HEADER, 50, 0, 0, 32, 32, 2, 190, 220, 32, 30, 28, 26, 12,
   1, 0, 9, 0, 0, 0, { L"Inter", L"Cantarell", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x4DB2F2,
   PAL_(0xFAFAFA,0xF1F1F1,0x2B2B2B,0x7A7A7A,0xDEDEDE,0xF4F4F4,0xECECEC,0x2B2B2B,0xF4F4F4,0xF1F1F1,0xFFFFFF,0xD4D4D4,0xE9E9E9,0x15A6F0,0x15A6F0,0x15A6F0,0xFAFAFA,0xF4F4F4),
   PAL_(0x222222,0x1B1B1B,0xEDEDED,0x9A9A9A,0x333333,0x2B2B2B,0x262626,0xEDEDED,0x2B2B2B,0x1E1E1E,0x1A1A1A,0x444444,0x2E2E2E,0x15A6F0,0x15A6F0,0x15A6F0,0x222222,0x262626), 1 },
 { L"mint", L"Linux Mint", L"Nemo - Mint-Y - green toolbar", LAY_TOOLBAR, 30, 0, 42, 28, 32, 2, 180, 200, 26, 24, 26, 26, 14,
   1, 0, 3, 1, 0, 0, { L"Noto Sans", L"DejaVu Sans", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x9DB87F,
   PAL_(0xFFFFFF,0xF5F5F5,0x2F2F2F,0x7A7A7A,0xD8D8D8,0xE6E6E6,0xD9D9D9,0x2F2F2F,0xEFEFEF,0xEFEFEF,0xFFFFFF,0xC5C5C5,0xE4E4E4,0x8FA876,0x8FA876,0x8FA876,0xFFFFFF,0xF1F1F1),
   PAL_(0x2F2F2F,0x272727,0xEBEBEB,0x9B9B9B,0x1F1F1F,0x202020,0x1A1A1A,0xEBEBEB,0x343434,0x2A2A2A,0x262626,0x4D4D4D,0x3B3B3B,0x8FA876,0x8FA876,0x8FA876,0x2F2F2F,0x333333), 1 },
 { L"kde", L"KDE Dolphin", L"Breeze - Plasma - places panel", LAY_MENUBAR, 28, 24, 40, 28, 30, 2, 190, 200, 26, 24, 24, 26, 14,
   1, 1, 4, 0, 0, 0, { L"Noto Sans", L"Segoe UI" }, { L"Places", L"Recent", L"Favorites", L"Devices" }, 0x3DAEE9,
   PAL_(0xFCFCFC,0xEFF0F1,0x232627,0x7F8C8D,0xBDC3C7,0xE3E5E7,0xD5D8DB,0x232627,0xEFF0F1,0xEFF0F1,0xFCFCFC,0xBDC3C7,0xDDEFFA,0x3DAEE9,0x3DAEE9,0x3DAEE9,0xF3F4F5,0xEFF0F1),
   PAL_(0x31363B,0x232629,0xEFF0F1,0x7F8C8D,0x4D4D4D,0x2A2E32,0x232629,0xEFF0F1,0x31363B,0x232629,0x232629,0x4D4D4D,0x3B4045,0x3DAEE9,0x3DAEE9,0x3DAEE9,0x2C3035,0x2A2E32), 1 },
 { L"chromeos", L"ChromeOS Files", L"Material - pill search", LAY_HEADER, 52, 0, 0, 36, 34, 2, 240, 230, 36, 34, 32, 28, 12,
   0, 0, 18, 0, 0, 0, { L"Roboto", L"Segoe UI" }, { L"My files", L"Recent", L"Starred", L"Drives" }, 0x5F6368,
   PAL_(0xFFFFFF,0xF8F9FA,0x202124,0x5F6368,0xDADCE0,0xF1F3F4,0xE8EAED,0x202124,0xF8F9FA,0xF8F9FA,0xF1F3F4,0xF1F3F4,0xF1F3F4,0xD2E3FC,0xD2E3FC,0x1A73E8,0xFFFFFF,0xFFFFFF),
   PAL_(0x202124,0x292A2D,0xE8EAED,0x9AA0A6,0x3C4043,0x292A2D,0x202124,0xE8EAED,0x292A2D,0x292A2D,0x3C4043,0x3C4043,0x35363A,0x394457,0x394457,0x8AB4F8,0x202124,0x292A2D), 1 },
 { L"xfce", L"XFCE", L"Thunar - Greybird - menu bar", LAY_MENUBAR, 26, 24, 38, 26, 30, 2, 170, 180, 24, 22, 24, 24, 14,
   1, 0, 2, 1, 0, 0, { L"DejaVu Sans", L"Cantarell", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x6FA8DC,
   PAL_(0xFFFFFF,0xFAFAFA,0x3C3C3C,0x7C7C7B,0xCECECD,0xE3E2E1,0xCFCECD,0x3C3C3C,0xEDECEB,0xEDECEB,0xFFFFFF,0xABABAA,0xDDDCDB,0x3498DB,0x3498DB,0x3498DB,0xFFFFFF,0xEDECEB),
   PAL_(0x333333,0x2A2A2A,0xE6E6E6,0x9A9A9A,0x1E1E1E,0x3C3C3C,0x2E2E2E,0xE6E6E6,0x3A3A3A,0x343434,0x2A2A2A,0x555555,0x454545,0x3498DB,0x3498DB,0x3498DB,0x333333,0x3A3A3A), 1 },
 { L"total", L"Total Commander", L"Classic Win95 - navy - raised", LAY_MENUBAR, 22, 20, 34, 22, 26, 2, 170, 170, 18, 18, 20, 20, 16,
   1, 0, 0, 1, 1, 0, { L"MS Sans Serif", L"Tahoma", L"Segoe UI" }, { L"Favorites", L"Recent", L"Pinned", L"Drives" }, 0xFFFF55,
   PAL_(0xFFFFFF,0xC0C0C0,0x000000,0x404040,0x808080,0x000080,0x1084D0,0xFFFFFF,0xC0C0C0,0xC0C0C0,0xFFFFFF,0x808080,0xD4D0C8,0x000080,0x000080,0x000080,0xFFFFFF,0xC0C0C0),
   PAL_(0), 0 },
 { L"mc", L"Midnight Commander", L"Text-mode - blue - monospace", LAY_MENUBAR, 22, 22, 34, 22, 26, 2, 170, 180, 18, 18, 20, 20, 14,
   1, 0, 0, 0, 0, 1, { L"Consolas", L"Courier New", L"DejaVu Sans Mono", L"Segoe UI" }, { L"Panel", L"History", L"Hotlist", L"Drives" }, 0x55FFFF,
   PAL_(0x0000AA,0x0000AA,0xE8E8E8,0xFFFF55,0xAAAAAA,0x00AAAA,0x00AAAA,0x000000,0x00AAAA,0x00AAAA,0x000088,0x55FFFF,0x0000CC,0x00AAAA,0x00AAAA,0x00AAAA,0x0000AA,0x0000AA),
   PAL_(0), 0 },
 { L"winxp", L"Windows XP", L"Luna blue - task pane - classic Explorer", LAY_WIN7, 30, 0, 38, 24, 26, 4, 190, 200, 22, 20, 22, 22, 17,
   1, 0, 3, 1, 1, 0, { L"Tahoma", L"Segoe UI" }, { L"Favorites", L"Recent", L"Folders", L"My Computer" }, 0xF5D96B,
   PAL_(0xFFFFFF,0xD6DFF7,0x000000,0x6B6B6B,0xACA899,0x0A5BD8,0x3D95FF,0xFFFFFF,0xECE9D8,0xECE9D8,0xFFFFFF,0x7F9DB9,0xEFF3FB,0x316AC5,0x316AC5,0x316AC5,0xFFFFFF,0xECE9D8),
   PAL_(0), 0 },
 { L"haiku", L"Haiku Tracker", L"BeOS style - yellow tabs - grey panels", LAY_TOOLBAR, 30, 0, 36, 24, 28, 4, 170, 170, 22, 20, 22, 22, 16,
   0, 0, 0, 1, 0, 0, { L"Noto Sans", L"DejaVu Sans", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Volumes" }, 0xF2C43D,
   PAL_(0xE8E8E8,0xD8D8D8,0x000000,0x666666,0x999999,0xFFCB00,0xFFE066,0x000000,0xD8D8D8,0xD8D8D8,0xFFFFFF,0x888888,0xDADADA,0xB6CCE4,0xB6CCE4,0x3F6FB5,0xE8E8E8,0xD8D8D8),
   PAL_(0), 0 },
 { L"neon", L"Cyber Neon", L"Dark synthwave - neon accents - mono", LAY_HEADER, 48, 0, 0, 32, 32, 2, 220, 210, 30, 28, 28, 24, 12,
   1, 0, 6, 0, 0, 1, { L"Consolas", L"Segoe UI" }, { L"Places", L"Recent", L"Pinned", L"Drives" }, 0xFF2E97,
   PAL_(0x0D0221,0x150634,0xE6E6FF,0x8A7FB8,0x2A1458,0x1A0840,0x2A1260,0x00F5D4,0x150634,0x0A0118,0x1D0A48,0x00F5D4,0x2A1458,0xFF2E97,0x7B2FF7,0x00F5D4,0x120432,0x1A0840),
   PAL_(0), 0 },
 { L"opus", L"Directory Opus", L"Power-user dark - orange - dual toolbar", LAY_TOOLBAR, 28, 0, 40, 26, 28, 3, 200, 200, 24, 22, 24, 24, 12,
   1, 1, 3, 0, 1, 1, { L"Segoe UI" }, { L"Listers", L"Recent", L"Favourites", L"Drives" }, 0xE0A030,
   PAL_(0x2B2F36,0x23262C,0xDADFE6,0x8A93A0,0x3A4049,0x1D2025,0x262A31,0xDADFE6,0x2F343C,0x1D2025,0x23262C,0x4A515C,0x3A404A,0xE07B00,0xE07B00,0xE07B00,0x2F343C,0x262A31),
   PAL_(0), 0 },
};
#define NSTYLES ARRLEN(OS_STYLES)
/* order shown in the Styles menu (indices into OS_STYLES) */
static const int STYLE_ORDER[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static int style_index(const WCHAR *k) {
    for (int i = 0; i < (int)NSTYLES; i++) if (!wcscmp(OS_STYLES[i].key, k)) return i;
    return 0;
}

static void cfg_validate(void) {
    if (wcscmp(C.view, L"details") && wcscmp(C.view, L"icons") && wcscmp(C.view, L"tiles")) scpy(C.view, 16, L"details");
    if (wcscmp(C.sort, L"name") && wcscmp(C.sort, L"date") && wcscmp(C.sort, L"type") && wcscmp(C.sort, L"size")) scpy(C.sort, 16, L"name");
    if (wcscmp(C.sort_dirs, L"first") && wcscmp(C.sort_dirs, L"last") && wcscmp(C.sort_dirs, L"mixed")) scpy(C.sort_dirs, 8, L"first");
    scpy(C.os_style, 16, OS_STYLES[style_index(C.os_style)].key);
    scpy(C.theme, 16, theme_by_name(C.theme)->name);
    if (C.preview_width < 220) C.preview_width = 220;
    if (C.preview_width > 900) C.preview_width = 900;
    if (C.search_limit < 100) C.search_limit = 100;
    if (C.font_size < 8) C.font_size = 8;
    if (C.font_size > 40) C.font_size = 40;
    if (C.icon_size < 32) C.icon_size = 32;
    if (C.icon_size > 160) C.icon_size = 160;
    if (C.scale < 0.5 || C.scale > 3.0) C.scale = 1.0;
    if (C.window_w < 640) C.window_w = 1180;
    if (C.window_h < 420) C.window_h = 720;
    if (C.pv_zoom < 25 || C.pv_zoom > 400) C.pv_zoom = 100;
    if (C.video_mode < 0 || C.video_mode > 2) C.video_mode = 0;
    if (C.ribbon_tab < 0 || C.ribbon_tab > 5) C.ribbon_tab = 1;
    int *pz[8] = { &C.pvsz0, &C.pvsz1, &C.pvsz2, &C.pvsz3, &C.pvsz4, &C.pvsz5, &C.pvsz6, &C.pvsz7 };
    for (int i = 0; i < 8; i++) if (*pz[i] < 0 || *pz[i] > 3) *pz[i] = 1;
    if (ON_WINE && !wcscmp(C.theme, L"light")) { fm_log("Migrating light->dark on Wine"); scpy(C.theme, 16, L"dark"); }
}

static const Style *cur_style(void) { return &OS_STYLES[style_index(C.os_style)]; }
static int is_win10(void) { return !wcscmp(C.os_style, L"win10"); }
static int is_dark(void) {
    if (is_win10()) return theme_by_name(C.theme)->dark;
    return C.style_dark && cur_style()->has_dark;
}
static void build_pal(Pal *p) {
    const Style *st = cur_style();
    if (is_win10()) {
        const Theme *t = theme_by_name(C.theme);
        COLORREF acc = hex_color(C.accent, RGB(0, 120, 212));
        palette_core(p, RGBH(t->bg), RGBH(t->alt), t->dark ? RGB(255, 255, 255) : RGB(0, 0, 0), t->dark, acc);
        return;
    }
    int dk = C.style_dark && st->has_dark;
    const u32 *v = dk ? st->dark : st->light;
    int isdark = dk || st->base_dark;
    COLORREF acc = RGBH(v[15]);
    if (C.style_accent[0]) acc = hex_color(C.style_accent, acc);
    palette_core(p, RGBH(v[0]), RGBH(v[1]), RGBH(v[2]), isdark, acc);
    p->dim = RGBH(v[3]); p->line = RGBH(v[4]); p->border = mixc(RGBH(v[4]), RGBH(v[2]), 0.12);
    p->hover = RGBH(v[12]); p->cap = RGBH(v[5]); p->cap2 = RGBH(v[6]); p->cap_text = RGBH(v[7]);
    p->tool = RGBH(v[8]); p->status = RGBH(v[9]); p->input = RGBH(v[10]); p->input_border = RGBH(v[11]);
    p->zebra = RGBH(v[16]); p->hdr_bg = RGBH(v[17]); p->hdr = p->dim; p->side = RGBH(v[1]);
    p->btn = mixc(p->tool, RGBH(v[2]), 0.05); p->btn_border = mixc(p->tool, RGBH(v[2]), 0.25);
    p->btn_press = mixc(RGBH(v[0]), acc, 0.35);
    if (st->sel_solid) { p->sel = acc; p->nav_sel = !wcscmp(st->key, L"macos") ? RGBH(v[14]) : acc; }
    else if (!C.style_accent[0]) { p->sel = RGBH(v[13]); p->nav_sel = RGBH(v[14]); }
    else { p->sel = mixc(RGBH(v[0]), acc, 0.22); p->nav_sel = mixc(RGBH(v[0]), acc, 0.18); }
    p->sel_text = st->sel_solid ? contrast(p->sel) : p->text;
    p->nav_sel_text = st->sel_solid ? contrast(p->nav_sel) : p->text;
}

/* ========================================================================== */
/* GDI helpers                                                                */
/* ========================================================================== */
static int px(double v) { int r = (int)(v * C.scale + 0.5); return r < 1 ? 1 : r; }
static void fillr(HDC dc, const RECT *r, COLORREF c) {
    SetBkColor(dc, c); ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}
static void fill4(HDC dc, int l, int t, int r, int b, COLORREF c) { RECT q = { l, t, r, b }; fillr(dc, &q, c); }
static void framer(HDC dc, const RECT *r, COLORREF c) {
    fill4(dc, r->left, r->top, r->right, r->top + 1, c); fill4(dc, r->left, r->bottom - 1, r->right, r->bottom, c);
    fill4(dc, r->left, r->top, r->left + 1, r->bottom, c); fill4(dc, r->right - 1, r->top, r->right, r->bottom, c);
}
static void rrect(HDC dc, const RECT *r, COLORREF fillc, COLORREF outline, int rad, int use_outline) {
    HBRUSH b = CreateSolidBrush(fillc); HPEN p = use_outline ? CreatePen(PS_SOLID, 1, outline) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    if (rad > 1) RoundRect(dc, r->left, r->top, r->right + 1, r->bottom + 1, rad * 2, rad * 2);
    else Rectangle(dc, r->left, r->top, r->right + 1, r->bottom + 1);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); if (use_outline) DeleteObject(p);
}
static void linec(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w) {
    HPEN p = CreatePen(PS_SOLID, w, c); HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL); LineTo(dc, x2, y2); SelectObject(dc, o); DeleteObject(p);
}
static void poly(HDC dc, const POINT *pt, int n, COLORREF f) {
    HBRUSH b = CreateSolidBrush(f); HPEN p = CreatePen(PS_SOLID, 1, f);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p); Polygon(dc, pt, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void ellipsec(HDC dc, const RECT *r, COLORREF f, COLORREF o, int hasf) {
    HBRUSH b = hasf ? CreateSolidBrush(f) : (HBRUSH)GetStockObject(NULL_BRUSH); HPEN p = CreatePen(PS_SOLID, 1, o);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, r->left, r->top, r->right, r->bottom);
    SelectObject(dc, ob); SelectObject(dc, op); if (hasf) DeleteObject(b); DeleteObject(p);
}
static void textr(HDC dc, const WCHAR *s, const RECT *r, UINT fl, COLORREF c) {
    RECT q = *r; SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &q, fl | DT_NOPREFIX);
}
#define TL (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT)
#define TC (DT_SINGLELINE | DT_VCENTER | DT_CENTER)
static int text_w(HDC dc, const WCHAR *s) { SIZE z; GetTextExtentPoint32W(dc, s, lstrlenW(s), &z); return z.cx; }
static int in_rect(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static void vgrad(HDC dc, const RECT *r, COLORREF a, COLORREF b) {
    int h = r->bottom - r->top; if (h <= 0) return;
    for (int y = 0; y < h; y += 2) fill4(dc, r->left, r->top + y, r->right, r->top + y + 2 > r->bottom ? r->bottom : r->top + y + 2, mixc(a, b, (double)y / h));
}

static void fmt_bytes(u64 n, WCHAR *o, int cap) {
    static const WCHAR *u[] = { L"B", L"KB", L"MB", L"GB", L"TB" }; double v = (double)n; int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    if (i == 0) wfmt(o, cap, L"%d B", (int)n); else wfmt(o, cap, L"%.1f %ls", v, u[i]);
}
static void fmt_date(const FILETIME *ft, WCHAR *o, int cap) {
    SYSTEMTIME u, l; if (!ft->dwHighDateTime && !ft->dwLowDateTime) { o[0] = 0; return; }
    FileTimeToSystemTime(ft, &u); SystemTimeToTzSpecificLocalTime(NULL, &u, &l);
    wfmt(o, cap, L"%04d-%02d-%02d %02d:%02d", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
}
static int CALLBACK font_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD ty, LPARAM lp) { (void)lf; (void)tm; (void)ty; *(int *)lp = 1; return 0; }
static int font_available(const WCHAR *n) {
    HDC dc = GetDC(NULL); LOGFONTW lf; memset(&lf, 0, sizeof lf); lf.lfCharSet = DEFAULT_CHARSET; lstrcpynW(lf.lfFaceName, n, 32);
    int found = 0; EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)font_cb, (LPARAM)&found, 0); ReleaseDC(NULL, dc); return found;
}
static const WCHAR *pick_font(const WCHAR *const *c) {
    for (int i = 0; i < 4 && c[i]; i++) if (font_available(c[i])) return c[i];
    return L"Segoe UI";
}
static HFONT make_font(const WCHAR *n, int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, n);
}

static u64 free_space(const WCHAR *path, u64 *total) {
    ULARGE_INTEGER a, t, f; WCHAR root[8]; wfmt(root, 8, L"%c:\\", path[0]);
    if (GetDiskFreeSpaceExW(root, &a, &t, &f)) { if (total) *total = t.QuadPart; return f.QuadPart; }
    if (total) *total = 0; return 0;
}

/* ========================================================================== */
/* GDI+ (dynamic flat API)  - used for thumbnails + preview                   */
/* ========================================================================== */
typedef struct { UINT v; void *cb; BOOL a, b; } GdipInit;
static struct {
    int ok; ULONG_PTR tok;
    int (WINAPI *Startup)(ULONG_PTR *, const GdipInit *, void *);
    int (WINAPI *FromFile)(const WCHAR *, void **);
    int (WINAPI *W)(void *, UINT *); int (WINAPI *H)(void *, UINT *);
    int (WINAPI *FromHDC)(HDC, void **); int (WINAPI *DelG)(void *); int (WINAPI *DelI)(void *);
    int (WINAPI *Interp)(void *, int); int (WINAPI *Draw)(void *, void *, int, int, int, int);
} G;
static void gdip_start(void) {
    HMODULE m = LoadLibraryW(L"gdiplus.dll"); if (!m) return;
#define GP(f, n) *(FARPROC *)&G.f = GetProcAddress(m, n)
    GP(Startup, "GdiplusStartup"); GP(FromFile, "GdipCreateBitmapFromFile"); GP(W, "GdipGetImageWidth"); GP(H, "GdipGetImageHeight");
    GP(FromHDC, "GdipCreateFromHDC"); GP(DelG, "GdipDeleteGraphics"); GP(DelI, "GdipDisposeImage");
    GP(Interp, "GdipSetInterpolationMode"); GP(Draw, "GdipDrawImageRectI");
    if (!G.Startup || !G.FromFile || !G.Draw || !G.FromHDC) return;
    GdipInit in = { 1, NULL, FALSE, FALSE };
    G.ok = (G.Startup(&G.tok, &in, NULL) == 0);
}
/* draw image fitted (aspect preserved) into box; returns 1 on success */
static int gdip_draw_fit(HDC dc, const WCHAR *path, int x, int y, int w, int h, int *iw, int *ih) {
    if (!G.ok) return 0;
    void *img = NULL; if (G.FromFile(path, &img) || !img) return 0;
    UINT W_ = 0, H_ = 0; G.W(img, &W_); G.H(img, &H_);
    if (!W_ || !H_) { G.DelI(img); return 0; }
    if (iw) *iw = (int)W_; if (ih) *ih = (int)H_;
    double s = (double)w / W_; if ((double)h / H_ < s) s = (double)h / H_; if (s > 1.0 && w > 200) s = 1.0;
    int dw = (int)(W_ * s), dh = (int)(H_ * s); if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    void *g = NULL; G.FromHDC(dc, &g); if (g) { G.Interp(g, 7); G.Draw(g, img, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh); G.DelG(g); }
    G.DelI(img); return 1;
}

/* thumbnail cache: HBITMAP per (path,size) */
typedef struct { WCHAR path[MAX_PATH]; int size; HBITMAP bmp; int w, h; } Thumb;
static Thumb g_thumbs[256]; static int g_thumb_next;
static int ext_in(const WCHAR *path, const WCHAR *list) {   /* list like L".png.jpg." */
    const WCHAR *e = PathFindExtensionW(path); if (!*e) return 0;
    WCHAR k[24]; wfmt(k, 24, L"%ls.", e); CharLowerW(k);
    return wcsstr(list, k) != NULL;
}
static int is_image(const WCHAR *p) { return ext_in(p, L".png.jpg.jpeg.bmp.gif.tif.tiff.ico.webp.jfif."); }
static int g_thumb_more;
static HBITMAP thumb_get(HDC dc, const WCHAR *path, int size, int *budget) {
    for (int i = 0; i < 256; i++) if (g_thumbs[i].size == size && !lstrcmpiW(g_thumbs[i].path, path)) return g_thumbs[i].bmp;
    if (!G.ok) return NULL;
    if (*budget <= 0) { g_thumb_more = 1; return NULL; }
    (*budget)--;
    Thumb *t = &g_thumbs[g_thumb_next]; g_thumb_next = (g_thumb_next + 1) % 256;
    if (t->bmp) DeleteObject(t->bmp);
    scpy(t->path, MAX_PATH, path); t->size = size; t->bmp = NULL;
    HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, size, size); HGDIOBJ ob = SelectObject(m, b);
    RECT r = { 0, 0, size, size }; fillr(m, &r, RGB(128, 128, 128));
    int ok = gdip_draw_fit(m, path, 0, 0, size, size, NULL, NULL);
    SelectObject(m, ob); DeleteDC(m);
    if (!ok) { DeleteObject(b); b = NULL; }
    t->bmp = b; return b;
}

/* ========================================================================== */
/* icons                                                                      */
/* ========================================================================== */
typedef struct { WCHAR key[MAX_PATH + 4]; HICON ic; } IconEnt;
static IconEnt g_icons[700]; static int g_icon_next;
static HICON icon_for(const WCHAR *path, int large, int is_dir) {
    const WCHAR *e = PathFindExtensionW(path); WCHAR key[MAX_PATH + 4];
    int per_file = ext_in(path, L".exe.ico.lnk.scr.cur.ani.");
    if (per_file) wfmt(key, MAX_PATH + 4, L"%c|%ls", large ? 'L' : 'S', path);
    else wfmt(key, MAX_PATH + 4, L"%c|%ls", large ? 'L' : 'S', e);
    CharLowerW(key);
    for (int i = 0; i < 700; i++) if (g_icons[i].ic && !wcscmp(g_icons[i].key, key)) return g_icons[i].ic;
    SHFILEINFOW sfi; memset(&sfi, 0, sizeof sfi);
    UINT fl = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (!per_file) fl |= SHGFI_USEFILEATTRIBUTES;
    SHGetFileInfoW(per_file ? path : (e[0] ? e : L"file"), is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, fl);
    IconEnt *g = &g_icons[g_icon_next]; g_icon_next = (g_icon_next + 1) % 700;
    if (g->ic) DestroyIcon(g->ic);
    scpy(g->key, MAX_PATH + 4, key); g->ic = sfi.hIcon;
    return g->ic;
}
static void paint_folder(HDC dc, int x, int y, int s, u32 col) {
    COLORREF c = RGBH(col), back = shadec(c, 0.82), hi = shadec(c, 1.12);
    int tw = s * 4 / 10, th = s / 8 + 1, top = y + s / 8, bh = s * 3 / 4;
    RECT a = { x + s / 16, top, x + s / 16 + tw, top + th + 2 }; rrect(dc, &a, back, back, 2, 0);
    RECT b = { x + s / 16, top + th, x + s - s / 16, top + th + bh - th }; fillr(dc, &b, back);
    RECT f = { x + s / 16, top + th + s / 8, x + s - s / 16, top + bh }; rrect(dc, &f, c, c, 2, 0);
    fill4(dc, f.left + 1, f.top, f.right - 1, f.top + 1, hi);
}
/* generic painted glyphs for nav roots / drives */
static void paint_glyph(HDC dc, int kind, int x, int y, int s, COLORREF c) {
    HBRUSH b; RECT r;
    switch (kind) {
    case 1: { POINT p[5] = { { x + s / 2, y + 1 }, { x + s - 1, y + s / 2 }, { x + s - 3, y + s / 2 }, { x + s - 3, y + s - 2 }, { x + 3, y + s - 2 } };
              POINT q[3] = { { x + 1, y + s / 2 }, { x + s / 2, y + 1 }, { x + 3, y + s / 2 } }; (void)q; poly(dc, p, 5, c);
              fill4(dc, x + 3, y + s / 2, x + s - 3, y + s / 2 + 1, c); break; }                           /* home */
    case 2: { RECT e = { x + 1, y + 1, x + s - 1, y + s - 1 }; ellipsec(dc, &e, 0, c, 0);
              linec(dc, x + s / 2, y + s / 2, x + s / 2, y + 3, c, 1); linec(dc, x + s / 2, y + s / 2, x + s - 4, y + s / 2, c, 1); break; } /* recent */
    case 3: { POINT p[10]; for (int i = 0; i < 10; i++) { double a = -1.5708 + i * 0.62832, rr = (i & 1) ? s * 0.22 : s * 0.5;
              p[i].x = x + s / 2 + (int)(cos(a) * rr); p[i].y = y + s / 2 + 1 + (int)(sin(a) * rr); } poly(dc, p, 10, c); break; } /* star */
    case 4: { r.left = x + 1; r.top = y + 2; r.right = x + s - 1; r.bottom = y + s - 5; framer(dc, &r, c); framer(dc, &(RECT){ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, c);
              fill4(dc, x + s / 2 - 2, y + s - 5, x + s / 2 + 2, y + s - 2, c); fill4(dc, x + 3, y + s - 2, x + s - 3, y + s - 1, c); break; } /* pc */
    case 5: { r.left = x + 1; r.top = y + s / 3; r.right = x + s - 1; r.bottom = y + s * 2 / 3 + 2; b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
              fill4(dc, x + s - 5, y + s / 2, x + s - 3, y + s / 2 + 2, RGB(90, 220, 90)); break; }                  /* drive */
    }
}

/* ========================================================================== */
/* MODEL: items, tabs, global state                                           */
/* ========================================================================== */
#define FM_MAXTABS 16
#define WM_APP_REFRESH (WM_APP + 1)
#define WM_APP_EDITKEY (WM_APP + 2)
#define WM_APP_TASKDONE (WM_APP + 3)
#define WM_APP_ASK (WM_APP + 4)
#define WM_APP_PVDONE (WM_APP + 5)
#define WM_APP_ARCDONE (WM_APP + 6)
#define TIMER_VID 4
#define TIMER_PREVIEW 5
#define TIMER_UI 1
#define TIMER_NAV 2
#define TIMER_SEARCH 3
#define FM_CLASS L"WinShellFM.Window"
#define FM_MUTEX L"WinShellFM.Mutex"

typedef struct {
    WCHAR name[256], path[MAX_PATH], type[64];
    int dir, kind, sel; u64 size; FILETIME mt;      /* kind: 0 normal, 1 drive/special */
} Item;
typedef struct {
    WCHAR path[MAX_PATH], filter[256];
    SL hist; int hidx;
    Item *it; int n, cap; int *view; int nv;
    int scroll, focus, anchor, searching, pinned;
    int nfold, nfile; u64 tot;
} Tab;
typedef struct { WCHAR label[MAX_PATH], path[MAX_PATH]; int root, glyph, sect, depth, exp; } NavEnt;
typedef struct { RECT r; WCHAR path[MAX_PATH]; } Crumb;
typedef struct { WCHAR path[MAX_PATH]; int kind; WCHAR *text; int tscroll; u64 size; FILETIME mt; WCHAR type[64]; int dir; } Prev;

typedef struct {
    volatile LONG cancel, done, refs; CRITICAL_SECTION cs;
    WCHAR root[MAX_PATH], q[256]; int wild, sub, limit, hidden;
    Item *res; int n, cap, taken;
} SearchCtx;

static struct {
    HWND hwnd, e_addr, e_search, e_rename;
    HFONT font, font_big, font_small; Pal pal; HBRUSH br_input;
    Tab tabs[FM_MAXTABS]; int ntabs, cur;
    SL clip; int clip_cut;
    NavEnt *nav; int nnav, ncap_nav; DWORD nav_t; int nav_scroll; int nav_open[4];
    WCHAR toast[256]; DWORD toast_t; WCHAR title[MAX_PATH + 64];
    SearchCtx *search; WCHAR status_msg[160];
    int hover_item, hover_nav, hover_tab, hover_btn, noevent, addr_edit, rename_item;
    int dragsb, sb_off; Prev prev;
    Crumb crumbs[40]; int ncrumbs;
    WCHAR typebuf[32]; DWORD type_t;
    volatile LONG busy; int autostart_done;
    WNDPROC edit_old;
} S;

static Tab *curtab(void) { return &S.tabs[S.cur]; }
static int g_dirty = 1;
static void redraw(void) { g_dirty = 1; if (S.hwnd) InvalidateRect(S.hwnd, NULL, FALSE); }
static void toast(const WCHAR *s) { scpy(S.toast, 256, s); S.toast_t = GetTickCount(); redraw(); }

/* ---- path helpers ---- */
static int is_special(const WCHAR *p) { return p[0] == ':' && p[1] == ':'; }
static int dir_exists(const WCHAR *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int path_exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void path_join(WCHAR *o, int cap, const WCHAR *dir, const WCHAR *name) {
    scpy(o, cap, dir); int l = lstrlenW(o); if (l && o[l - 1] != '\\') scat(o, cap, L"\\"); scat(o, cap, name);
}
static void norm_path(WCHAR *p) {
    WCHAR *s = p; while (*s == ' ' || *s == '"') s++;
    if (s != p) memmove(p, s, (lstrlenW(s) + 1) * sizeof(WCHAR));
    int l = lstrlenW(p); while (l && (p[l - 1] == ' ' || p[l - 1] == '"')) p[--l] = 0;
    for (WCHAR *c = p; *c; c++) if (*c == '/') *c = '\\';
    while (l > 3 && p[l - 1] == '\\') p[--l] = 0;
    if (l == 2 && p[1] == ':') { p[2] = '\\'; p[3] = 0; }
}
static const WCHAR *base_name(const WCHAR *p) {
    const WCHAR *b = wcsrchr(p, '\\'); return b ? b + 1 : p;
}
static void parent_of(const WCHAR *p, WCHAR *o) {
    scpy(o, MAX_PATH, p); int l = lstrlenW(o);
    while (l > 3 && o[l - 1] == '\\') o[--l] = 0;
    if (l <= 3) { o[0] = 0; return; }
    WCHAR *s = wcsrchr(o, '\\'); if (!s) { o[0] = 0; return; }
    if (s - o == 2 && o[1] == ':') s[1] = 0; else *s = 0;
}
static int under_path(const WCHAR *child, const WCHAR *parent) {
    int l = lstrlenW(parent); if (_wcsnicmp(child, parent, l)) return 0;
    return child[l] == '\\' || child[l] == 0 || (l && parent[l - 1] == '\\');
}
static void unique_name(const WCHAR *dir, const WCHAR *name, int isdir, WCHAR *out, int cap) {
    WCHAR stem[MAX_PATH], ext[MAX_PATH] = L""; scpy(stem, MAX_PATH, name);
    if (!isdir) { WCHAR *e = PathFindExtensionW(stem); if (*e) { scpy(ext, MAX_PATH, e); *e = 0; } }
    WCHAR cand[MAX_PATH]; path_join(out, cap, dir, name);
    for (int i = 1; path_exists(out) && i < 10000; i++) {
        if (i == 1) wfmt(cand, MAX_PATH, L"%ls - Copy%ls", stem, ext); else wfmt(cand, MAX_PATH, L"%ls - Copy (%d)%ls", stem, i, ext);
        path_join(out, cap, dir, cand);
    }
}
static void type_name(const WCHAR *name, int dir, WCHAR *o, int cap) {
    if (dir) { scpy(o, cap, L"File folder"); return; }
    const WCHAR *e = PathFindExtensionW(name);
    if (!*e) { scpy(o, cap, L"File"); return; }
    WCHAR t[40]; scpy(t, 40, e + 1); CharUpperW(t); wfmt(o, cap, L"%ls File", t);
}
static int hidden_name(const WCHAR *n, DWORD attr) { return n[0] == '.' || (attr & FILE_ATTRIBUTE_HIDDEN); }

/* ---- item/tab storage ---- */
static Item *tab_add(Tab *t) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->it = (Item *)realloc(t->it, t->cap * sizeof(Item)); }
    Item *i = &t->it[t->n++]; memset(i, 0, sizeof *i); return i;
}
static void tab_clear_items(Tab *t) { t->n = 0; t->nv = 0; t->focus = t->anchor = -1; }
static void item_from_fd(Item *i, const WCHAR *dir, const WIN32_FIND_DATAW *fd) {
    scpy(i->name, 256, fd->cFileName); path_join(i->path, MAX_PATH, dir, fd->cFileName);
    i->dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    i->size = i->dir ? 0 : (((u64)fd->nFileSizeHigh << 32) | fd->nFileSizeLow);
    i->mt = fd->ftLastWriteTime; type_name(fd->cFileName, i->dir, i->type, 64);
}

/* ---- sorting ---- */
static Tab *g_st;
static int is_pinned(const Item *it) { return sl_find(&C.pins, it->path) >= 0; }
static int cmp_view(const void *a, const void *b) {
    const Item *x = &g_st->it[*(const int *)a], *y = &g_st->it[*(const int *)b];
    int px_ = is_pinned(x), py_ = is_pinned(y); if (px_ != py_) return py_ - px_;
    if (wcscmp(C.sort_dirs, L"mixed") && x->dir != y->dir) { int d = y->dir - x->dir; return wcscmp(C.sort_dirs, L"first") ? -d : d; }
    int r = 0;
    if (!wcscmp(C.sort, L"date")) r = CompareFileTime(&x->mt, &y->mt);
    else if (!wcscmp(C.sort, L"size")) r = x->size < y->size ? -1 : x->size > y->size;
    else if (!wcscmp(C.sort, L"type")) { r = lstrcmpiW(x->type, y->type); if (!r) r = StrCmpLogicalW(x->name, y->name); }
    else r = StrCmpLogicalW(x->name, y->name);
    if (!r) r = StrCmpLogicalW(x->name, y->name);
    return C.asc ? r : -r;
}
static int ext_in(const WCHAR *path, const WCHAR *list);
static int ext_filter_ok(const Item *it) {
    if (!C.search_ext[0] || it->dir) return 1;
    return ext_in(it->name, C.search_ext);
}
static void rebuild_view(Tab *t) {
    free(t->view); t->view = (int *)malloc((t->n + 1) * sizeof(int)); t->nv = 0; t->nfold = t->nfile = 0; t->tot = 0;
    for (int i = 0; i < t->n; i++) {
        if (!t->searching && t->filter[0] && !StrStrIW(t->it[i].name, t->filter)) continue;
        if (!ext_filter_ok(&t->it[i])) continue;
        t->view[t->nv++] = i;
        if (t->it[i].dir) t->nfold++; else { t->nfile++; t->tot += t->it[i].size; }
    }
    g_st = t; qsort(t->view, t->nv, sizeof(int), cmp_view);
}

/* ---- special folders ---- */
static void home_dir(WCHAR *o) {
    if (!GetEnvironmentVariableW(L"USERPROFILE", o, MAX_PATH)) lstrcpyW(o, L"C:\\users\\Public");
}
static void add_folder_item(Tab *t, const WCHAR *path, int kind) {
    if (!dir_exists(path)) return; Item *i = tab_add(t);
    scpy(i->path, MAX_PATH, path); i->dir = 1; i->kind = kind;
    const WCHAR *b = base_name(path); scpy(i->name, 256, *b ? b : path); scpy(i->type, 64, L"File folder");
    WIN32_FILE_ATTRIBUTE_DATA d; if (GetFileAttributesExW(path, GetFileExInfoStandard, &d)) i->mt = d.ftLastWriteTime;
}
static void read_special(Tab *t) {
    WCHAR h[MAX_PATH], p[MAX_PATH]; home_dir(h);
    if (!t->path[0]) {                              /* This PC: drives */
        DWORD mask = GetLogicalDrives();
        for (int k = 0; k < 26; k++) {
            if (!(mask & (1u << k))) continue;
            WCHAR root[4] = { (WCHAR)('A' + k), ':', '\\', 0 }; UINT dt = GetDriveTypeW(root);
            const WCHAR *tn = dt == DRIVE_REMOVABLE ? L"Removable Disk" : dt == DRIVE_REMOTE ? L"Network Drive" : dt == DRIVE_CDROM ? L"CD Drive" : L"Local Disk";
            Item *i = tab_add(t); scpy(i->path, MAX_PATH, root); i->dir = 1; i->kind = 1; scpy(i->type, 64, tn);
            wfmt(i->name, 256, L"%lc: (%ls)", root[0], tn);
            u64 tot = 0; free_space(root, &tot); i->size = tot;
        }
    } else if (!wcscmp(t->path, L"::quick")) {
        static const WCHAR *n[] = { L"Desktop", L"Downloads", L"Documents", L"Pictures", L"Music", L"Videos" };
        for (int k = 0; k < 6; k++) { path_join(p, MAX_PATH, h, n[k]); add_folder_item(t, p, 0); }
        for (int k = 0; k < C.favorites.n; k++) add_folder_item(t, C.favorites.v[k], 0);
    } else if (!wcscmp(t->path, L"::fav")) {
        for (int k = 0; k < C.favorites.n; k++) add_folder_item(t, C.favorites.v[k], 0);
    } else if (!wcscmp(t->path, L"::recent")) {
        for (int k = 0; k < C.recent_files.n; k++) {
            WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(C.recent_files.v[k], &fd);
            if (f == INVALID_HANDLE_VALUE) continue; FindClose(f);
            WCHAR dir[MAX_PATH]; parent_of(C.recent_files.v[k], dir); Item *i = tab_add(t); item_from_fd(i, dir, &fd);
            scpy(i->path, MAX_PATH, C.recent_files.v[k]);
        }
        for (int k = 0; k < C.recent_folders.n; k++) add_folder_item(t, C.recent_folders.v[k], 0);
    }
}
static void read_dir(Tab *t, WCHAR *err) {
    err[0] = 0; WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, t->path, L"*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { wfmt(err, 160, L"Cannot open folder (error %lu)", GetLastError()); return; }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!C.show_hidden && hidden_name(fd.cFileName, fd.dwFileAttributes)) continue;
        item_from_fd(tab_add(t), t->path, &fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---- recursive search (worker thread) ---- */
static void sctx_release(SearchCtx *c) {
    if (InterlockedDecrement(&c->refs) == 0) { DeleteCriticalSection(&c->cs); free(c->res); free(c); }
}
static void search_walk(SearchCtx *c, const WCHAR *dir, int depth) {
    WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, dir, L"*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (c->cancel) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!c->hidden && hidden_name(fd.cFileName, fd.dwFileAttributes)) continue;
        int isd = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int match = c->wild ? PathMatchSpecW(fd.cFileName, c->q) : (StrStrIW(fd.cFileName, c->q) != NULL);
        if (match) {
            EnterCriticalSection(&c->cs);
            if (c->n < c->limit) {
                if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 512; c->res = (Item *)realloc(c->res, c->cap * sizeof(Item)); }
                Item *i = &c->res[c->n++]; memset(i, 0, sizeof *i); item_from_fd(i, dir, &fd); scpy(i->type, 64, dir);
            } else c->cancel = 1;
            LeaveCriticalSection(&c->cs);
        }
        if (isd && c->sub && depth < 24 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WCHAR sub[MAX_PATH]; path_join(sub, MAX_PATH, dir, fd.cFileName); search_walk(c, sub, depth + 1);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static DWORD WINAPI search_thread(LPVOID p) {
    SearchCtx *c = (SearchCtx *)p; search_walk(c, c->root, 0); c->done = 1; sctx_release(c); return 0;
}
static void cancel_search(void) {
    if (S.search) { S.search->cancel = 1; sctx_release(S.search); S.search = NULL; }
    for (int i = 0; i < S.ntabs; i++) S.tabs[i].searching = 0;
}
static void start_search(const WCHAR *q) {
    Tab *t = curtab(); cancel_search();
    SearchCtx *c = (SearchCtx *)calloc(1, sizeof *c); InitializeCriticalSection(&c->cs); c->refs = 2;
    scpy(c->root, MAX_PATH, t->path); scpy(c->q, 256, q); c->wild = wcspbrk(q, L"*?") != NULL;
    c->sub = C.search_subfolders; c->limit = C.search_limit; c->hidden = C.show_hidden;
    S.search = c; t->searching = 1; tab_clear_items(t); rebuild_view(t); t->scroll = 0;
    HANDLE h = CreateThread(NULL, 0, search_thread, c, 0, NULL);
    if (h) CloseHandle(h); else { c->done = 1; c->refs = 1; t->searching = 0; }
    redraw();
}
static void poll_search(void) {
    SearchCtx *c = S.search; Tab *t = curtab(); if (!c || !t->searching) return;
    int added = 0;
    EnterCriticalSection(&c->cs);
    for (; c->taken < c->n; c->taken++) { Item *d = tab_add(t); *d = c->res[c->taken]; added++; }
    int done = c->done, total = c->n, hit = c->cancel;
    LeaveCriticalSection(&c->cs);
    if (added) rebuild_view(t);
    if (done) {
        wfmt(S.status_msg, 160, L"Search finished: %d found%ls", total, hit ? L" (limit reached)" : L"");
        sctx_release(c); S.search = NULL; t->searching = 1;   /* keep results shown as search mode */
    } else wfmt(S.status_msg, 160, L"Searching... %d found", total);
    if (added || done) redraw();
}

/* ---- reload / navigate ---- */
static void nav_rebuild(void);
static void sel_clear(Tab *t) { for (int i = 0; i < t->n; i++) t->it[i].sel = 0; t->focus = t->anchor = -1; }
static void preview_update(void);
static void reload(int keep) {
    Tab *t = curtab(); SL keepsel = { 0 };
    if (keep) for (int i = 0; i < t->n; i++) if (t->it[i].sel) sl_add(&keepsel, t->it[i].path);
    int sc = t->scroll;
    if (t->filter[0] && t->path[0] && !is_special(t->path) && C.search_subfolders) {
        S.nav_t = 0; start_search(t->filter); sl_clear(&keepsel); free(keepsel.v); return;
    }
    cancel_search(); t->searching = 0; tab_clear_items(t); WCHAR err[160] = L"";
    if (!t->path[0] || is_special(t->path)) read_special(t); else read_dir(t, err);
    rebuild_view(t);
    for (int i = 0; i < t->n; i++) if (sl_find(&keepsel, t->it[i].path) >= 0) t->it[i].sel = 1;
    sl_clear(&keepsel); free(keepsel.v);
    t->scroll = keep ? sc : 0; scpy(S.status_msg, 160, err); S.nav_t = 0;
    preview_update(); redraw();
}
static void set_title(void) {
    Tab *t = curtab(); WCHAR ti[MAX_PATH + 32];
    const WCHAR *n = !t->path[0] ? L"This PC" : !wcscmp(t->path, L"::quick") ? cur_style()->sec[0] : !wcscmp(t->path, L"::recent") ? cur_style()->sec[1]
                   : !wcscmp(t->path, L"::fav") ? cur_style()->sec[2] : (base_name(t->path)[0] ? base_name(t->path) : t->path);
    wfmt(ti, MAX_PATH + 32, L"%ls - %ls", n, cur_style()->label); SetWindowTextW(S.hwnd, ti); scpy(S.title, MAX_PATH + 64, ti); redraw();
}
static void set_search_text(const WCHAR *s) { S.noevent++; SetWindowTextW(S.e_search, s); S.noevent--; }
static void navigate(const WCHAR *path, int push) {
    WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, path); Tab *t = curtab();
    if (p[0] && !is_special(p)) { norm_path(p); if (!dir_exists(p)) { toast(L"Folder not found"); return; } }
    cancel_search(); t->filter[0] = 0; set_search_text(L""); scpy(t->path, MAX_PATH, p);
    if (push) {
        while (t->hist.n > t->hidx + 1) sl_del(&t->hist, t->hist.n - 1);
        if (!t->hist.n || !wieq(t->hist.v[t->hist.n - 1], p)) sl_add(&t->hist, p);
        t->hidx = t->hist.n - 1;
    }
    sel_clear(t); t->scroll = 0;
    if (p[0] && !is_special(p)) { sl_front(&C.recent_folders, p, 12); scpy(C.last_dir, MAX_PATH, p); }
    reload(0); set_title();
}
static void go_back(void) { Tab *t = curtab(); if (t->hidx > 0) { t->hidx--; navigate(t->hist.v[t->hidx], 0); } }
static void go_fwd(void) { Tab *t = curtab(); if (t->hidx < t->hist.n - 1) { t->hidx++; navigate(t->hist.v[t->hidx], 0); } }
static void go_up(void) {
    Tab *t = curtab(); if (!t->path[0]) return;
    if (is_special(t->path)) { navigate(L"", 1); return; }
    WCHAR p[MAX_PATH]; parent_of(t->path, p); navigate(p, 1);
}

/* ---- tabs ---- */
static void tab_init(Tab *t, const WCHAR *path) {
    memset(t, 0, sizeof *t); scpy(t->path, MAX_PATH, path); sl_add(&t->hist, path); t->focus = t->anchor = -1;
}
static void switch_tab(int i) {
    if (i < 0 || i >= S.ntabs) return; cancel_search(); S.cur = i;
    set_search_text(curtab()->filter); set_title(); reload(1);
}
static void new_tab(const WCHAR *path) {
    if (S.ntabs >= FM_MAXTABS) { toast(L"Too many tabs"); return; }
    WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, path ? path : curtab()->path); if (p[0] && !is_special(p)) norm_path(p);
    if (p[0] && !is_special(p) && !dir_exists(p)) p[0] = 0;
    tab_init(&S.tabs[S.ntabs], p); S.cur = S.ntabs++; set_search_text(L""); set_title(); reload(0);
}
static void tab_free(Tab *t) { sl_clear(&t->hist); free(t->hist.v); free(t->it); free(t->view); }
static void close_tab(int i) {
    if (i < 0 || i >= S.ntabs || S.tabs[i].pinned) return;
    if (S.ntabs == 1) { navigate(L"::quick", 1); return; }
    cancel_search(); tab_free(&S.tabs[i]);
    memmove(&S.tabs[i], &S.tabs[i + 1], (S.ntabs - i - 1) * sizeof(Tab)); S.ntabs--;
    if (S.cur >= S.ntabs) S.cur = S.ntabs - 1; else if (S.cur > i) S.cur--;
    set_search_text(curtab()->filter); set_title(); reload(1);
}

/* ---- selection ---- */
static int sel_count(Tab *t, u64 *bytes) {
    int n = 0; u64 b = 0;
    for (int i = 0; i < t->n; i++) if (t->it[i].sel) { n++; b += t->it[i].size; }
    if (bytes) *bytes = b; return n;
}
static Item *focus_item(Tab *t) { return (t->focus >= 0 && t->focus < t->nv) ? &t->it[t->view[t->focus]] : NULL; }
static void sel_view(int vi, int ctrl, int shift) {
    Tab *t = curtab(); if (vi < 0 || vi >= t->nv) return;
    if (shift && t->anchor >= 0 && t->anchor < t->nv) {
        if (!ctrl) for (int i = 0; i < t->n; i++) t->it[i].sel = 0;
        int a = t->anchor < vi ? t->anchor : vi, b = t->anchor < vi ? vi : t->anchor;
        for (int i = a; i <= b; i++) t->it[t->view[i]].sel = 1;
    } else if (ctrl) { Item *x = &t->it[t->view[vi]]; x->sel = !x->sel; t->anchor = vi; }
    else { for (int i = 0; i < t->n; i++) t->it[i].sel = 0; t->it[t->view[vi]].sel = 1; t->anchor = vi; }
    t->focus = vi; preview_update(); redraw();
}
static void select_all(void) { Tab *t = curtab(); for (int i = 0; i < t->nv; i++) t->it[t->view[i]].sel = 1; preview_update(); redraw(); }
static void invert_sel(void) { Tab *t = curtab(); for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; x->sel = !x->sel; } preview_update(); redraw(); }
static void collect_sel(Tab *t, SL *out) {
    for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; if (x->sel && x->kind != 1) sl_add(out, x->path); }
}

/* ---- file operations ---- */
static int fm_msgbox(const WCHAR *text, const WCHAR *title, UINT flags);
static void clear_ro(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
}
static int delete_tree(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p); if (a == INVALID_FILE_ATTRIBUTES) return 1;
    clear_ro(p);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, p, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                     WCHAR c[MAX_PATH]; path_join(c, MAX_PATH, p, fd.cFileName); delete_tree(c);
                } while (FindNextFileW(h, &fd)); FindClose(h);
            }
        }
        return RemoveDirectoryW(p) != 0;
    }
    return DeleteFileW(p) != 0;
}
static int copy_tree(const WCHAR *src, const WCHAR *dst) {
    DWORD a = GetFileAttributesW(src); if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (under_path(dst, src)) return 0;                  /* folder into itself */
        if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, src, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        int ok = 1; if (h == INVALID_HANDLE_VALUE) return 1;
        do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
             WCHAR s2[MAX_PATH], d2[MAX_PATH]; path_join(s2, MAX_PATH, src, fd.cFileName); path_join(d2, MAX_PATH, dst, fd.cFileName);
             if (!copy_tree(s2, d2)) ok = 0;
        } while (FindNextFileW(h, &fd)); FindClose(h); return ok;
    }
    return CopyFileW(src, dst, FALSE) != 0;
}
typedef struct { WCHAR dest[MAX_PATH]; SL srcs; int move; HWND hwnd; } PasteJob;
typedef struct { const WCHAR *text, *title; UINT flags; int result; } AskReq;
static int ask_ui(HWND h, const WCHAR *text, const WCHAR *title, UINT flags) { AskReq a; a.text = text; a.title = title; a.flags = flags; a.result = 0; SendMessageW(h, WM_APP_ASK, 0, (LPARAM)&a); return a.result; }
static DWORD WINAPI paste_thread(LPVOID p) {
    PasteJob *j = (PasteJob *)p; int done = 0, fail = 0, apply = 0, mode = 0;   /* mode 1 replace 2 both 3 skip */
    for (int i = 0; i < j->srcs.n; i++) {
        const WCHAR *src = j->srcs.v[i]; WCHAR dst[MAX_PATH], par[MAX_PATH]; const WCHAR *nm = base_name(src);
        parent_of(src, par); int same = wieq(par, j->dest) || (j->dest[0] && wieq(par, j->dest));
        wfmt(S.status_msg, 160, L"%ls %d of %d: %ls", j->move ? L"Moving" : L"Copying", i + 1, j->srcs.n, nm); PostMessageW(j->hwnd, WM_APP_REFRESH, 0, 0);
        path_join(dst, MAX_PATH, j->dest, nm);
        if (same && j->move) continue;
        if (same) unique_name(j->dest, nm, dir_exists(src), dst, MAX_PATH);
        else if (path_exists(dst)) {
            int m = mode;
            if (!apply) {
                WCHAR msg[MAX_PATH + 200]; wfmt(msg, MAX_PATH + 200, L"\"%ls\" already exists in the destination.\n\nYes = Replace\nNo = Keep both\nCancel = Skip", nm);
                int r = ask_ui(j->hwnd, msg, L"Name conflict", MB_YESNOCANCEL | MB_ICONQUESTION);
                m = r == IDYES ? 1 : r == IDNO ? 2 : 3;
                if (j->srcs.n - i > 1 && ask_ui(j->hwnd, L"Do this for the remaining conflicts?", L"Name conflict", MB_YESNO) == IDYES) { apply = 1; mode = m; }
            }
            if (m == 3) continue;
            if (m == 2) unique_name(j->dest, nm, dir_exists(src), dst, MAX_PATH);
            else delete_tree(dst);
        }
        int ok;
        if (j->move) {
            ok = MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED) != 0;
            if (!ok) { ok = copy_tree(src, dst); if (ok) delete_tree(src); }
        } else ok = copy_tree(src, dst);
        if (ok) done++; else fail++;
    }
    wfmt(S.status_msg, 160, L"%d item(s) %ls%ls", done, j->move ? L"moved" : L"copied", fail ? L" - some failed" : L"");
    PostMessageW(j->hwnd, WM_APP_TASKDONE, fail, 0);
    sl_clear(&j->srcs); free(j->srcs.v); free(j); return 0;
}
static void start_transfer(const WCHAR *dest, SL *srcs, int move) {
    if (InterlockedCompareExchange(&S.busy, 1, 0)) { toast(L"Another operation is running"); return; }
    PasteJob *j = (PasteJob *)calloc(1, sizeof *j); scpy(j->dest, MAX_PATH, dest); j->move = move; j->hwnd = S.hwnd;
    for (int i = 0; i < srcs->n; i++) sl_add(&j->srcs, srcs->v[i]);
    HANDLE h = CreateThread(NULL, 0, paste_thread, j, 0, NULL);
    if (h) CloseHandle(h); else { S.busy = 0; sl_clear(&j->srcs); free(j->srcs.v); free(j); }
}
static void do_paste(void) {
    Tab *t = curtab(); if (!S.clip.n) { toast(L"Clipboard is empty"); return; }
    if (!t->path[0] || is_special(t->path)) { toast(L"Open a folder to paste into"); return; }
    if (InterlockedCompareExchange(&S.busy, 1, 0)) { toast(L"Another operation is running"); return; }
    PasteJob *j = (PasteJob *)calloc(1, sizeof *j); scpy(j->dest, MAX_PATH, t->path); j->move = S.clip_cut; j->hwnd = S.hwnd;
    for (int i = 0; i < S.clip.n; i++) sl_add(&j->srcs, S.clip.v[i]);
    if (S.clip_cut) { sl_clear(&S.clip); S.clip_cut = 0; }
    HANDLE h = CreateThread(NULL, 0, paste_thread, j, 0, NULL);
    if (h) CloseHandle(h); else { S.busy = 0; sl_clear(&j->srcs); free(j->srcs.v); free(j); }
}
static void do_copy(int cut) {
    Tab *t = curtab(); sl_clear(&S.clip); collect_sel(t, &S.clip); S.clip_cut = cut;
    WCHAR m[64]; wfmt(m, 64, L"%d item(s) %ls", S.clip.n, cut ? L"cut" : L"copied"); if (S.clip.n) toast(m);
}
static void do_delete(void) {
    Tab *t = curtab(); SL s = { 0 }; collect_sel(t, &s); if (!s.n) return;
    WCHAR msg[MAX_PATH + 120];
    if (s.n == 1) wfmt(msg, MAX_PATH + 120, L"Permanently delete \"%ls\"?\nThis cannot be undone.", base_name(s.v[0]));
    else wfmt(msg, 200, L"Permanently delete %d items?\nThis cannot be undone.", s.n);
    if (fm_msgbox( msg, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
        int fail = 0; for (int i = 0; i < s.n; i++) if (!delete_tree(s.v[i])) fail++;
        for (int i = 0; i < s.n; i++) { int k = sl_find(&C.pins, s.v[i]); if (k >= 0) sl_del(&C.pins, k); }
        if (fail) toast(L"Some items could not be deleted");
        reload(0);
    }
    sl_clear(&s); free(s.v);
}
static void begin_rename(void);
static void unique_new(const WCHAR *dir, const WCHAR *stem, const WCHAR *ext, WCHAR *out, int cap) {
    WCHAR n[MAX_PATH]; wfmt(n, MAX_PATH, L"%ls%ls", stem, ext); path_join(out, cap, dir, n);
    for (int i = 2; path_exists(out) && i < 10000; i++) { wfmt(n, MAX_PATH, L"%ls (%d)%ls", stem, i, ext); path_join(out, cap, dir, n); }
}
static void new_item(int folder) {
    Tab *t = curtab(); if (!t->path[0] || is_special(t->path)) { toast(L"Open a folder first"); return; }
    WCHAR p[MAX_PATH]; if (folder) unique_new(t->path, L"New folder", L"", p, MAX_PATH); else unique_new(t->path, L"New Text Document", L".txt", p, MAX_PATH);
    int ok = folder ? CreateDirectoryW(p, NULL) : (CloseHandle(CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)) != 0);
    if (!ok) { toast(L"Could not create item"); return; }
    reload(0);
    for (int i = 0; i < t->nv; i++) if (wieq(t->it[t->view[i]].path, p)) { sel_view(i, 0, 0); break; }
    begin_rename();
}
static void open_item(Item *it) {
    if (!it) return;
    if (it->dir) { navigate(it->path, 1); return; }
    sl_front(&C.recent_files, it->path, 20);
    WCHAR dir[MAX_PATH]; parent_of(it->path, dir);
    { const WCHAR *ex = PathFindExtensionW(it->path);
      if (*ex) for (int k = 0; k < C.assoc.n; k++) {
          const WCHAR *bar = wcschr(C.assoc.v[k], '|'); if (!bar) continue;
          int el = (int)(bar - C.assoc.v[k]); if ((int)lstrlenW(ex) == el && !_wcsnicmp(ex, C.assoc.v[k], el) && path_exists(bar + 1)) {
              WCHAR args[MAX_PATH + 4]; wfmt(args, MAX_PATH + 4, L"\"%ls\"", it->path); ShellExecuteW(S.hwnd, L"open", bar + 1, args, dir[0] ? dir : NULL, SW_SHOWNORMAL); return; } } }
    HINSTANCE r = ShellExecuteW(S.hwnd, L"open", it->path, NULL, dir[0] ? dir : NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) toast(L"No program is associated with this file - use Open with...");
}
static void open_with(Item *it);
static void show_properties(Item *it);
static void copy_path_text(Item *it) {
    if (!it || !OpenClipboard(S.hwnd)) return; EmptyClipboard();
    size_t n = (lstrlenW(it->path) + 1) * sizeof(WCHAR); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) { memcpy(GlobalLock(g), it->path, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard(); toast(L"Path copied");
}
static void open_cmd(void) {
    Tab *t = curtab(); WCHAR a[MAX_PATH + 16];
    if (t->path[0] && !is_special(t->path)) wfmt(a, MAX_PATH + 16, L"/k cd /d \"%ls\"", t->path); else lstrcpyW(a, L"/k");
    ShellExecuteW(S.hwnd, L"open", L"cmd.exe", a, NULL, SW_SHOWNORMAL);
}
/* ========================================================================== */
/* THEMED POPUP MENUS (own windows, scrollable, cascading)                    */
/* ========================================================================== */
#define PMF_CHECK 1
#define PMF_DIS 2
#define PMF_SEP 4
#define PMF_HEAD 8
#define PMF_RADIO 16
typedef struct PMenu PMenu;
typedef struct { WCHAR text[200]; WCHAR acc[40]; int id, flags, has_sw; COLORREF sw; PMenu *sub; } PMItem;
struct PMenu { PMItem *it; int n, cap; };
static PMenu *pm_new(void) { return (PMenu *)calloc(1, sizeof(PMenu)); }
static PMItem *pm_push(PMenu *m) {
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 16; m->it = (PMItem *)realloc(m->it, m->cap * sizeof(PMItem)); }
    PMItem *i = &m->it[m->n++]; memset(i, 0, sizeof *i); return i;
}
static void pm_settext(PMItem *i, const WCHAR *s) {
    const WCHAR *t = wcschr(s, '\t');
    if (t) { lstrcpynW(i->text, s, (int)(t - s) + 1 < 200 ? (int)(t - s) + 1 : 200); scpy(i->acc, 40, t + 1); } else scpy(i->text, 200, s);
}
static void pm_add(PMenu *m, int id, const WCHAR *text, int checked, int enabled) {
    PMItem *i = pm_push(m); pm_settext(i, text); i->id = id; i->flags = (checked ? PMF_CHECK : 0) | (enabled ? 0 : PMF_DIS);
}
static void pm_radio(PMenu *m, int id, const WCHAR *text, int checked, int enabled) { pm_add(m, id, text, checked, enabled); m->it[m->n - 1].flags |= PMF_RADIO; }
static void pm_swatch(PMenu *m, int id, const WCHAR *text, COLORREF c, int checked) { pm_add(m, id, text, checked, 1); m->it[m->n - 1].has_sw = 1; m->it[m->n - 1].sw = c; }
static void pm_sep(PMenu *m) { if (!m->n || (m->it[m->n - 1].flags & PMF_SEP)) return; pm_push(m)->flags = PMF_SEP; }
static void pm_head(PMenu *m, const WCHAR *text) { PMItem *i = pm_push(m); pm_settext(i, text); i->flags = PMF_HEAD | PMF_DIS; }
static PMenu *pm_sub(PMenu *m, const WCHAR *text) { PMItem *i = pm_push(m); pm_settext(i, text); i->sub = pm_new(); return i->sub; }
static void pm_free(PMenu *m) { if (!m) return; for (int i = 0; i < m->n; i++) pm_free(m->it[i].sub); free(m->it); free(m); }

typedef struct { PMenu *m; HWND hw; RECT rc; int scroll, hot, content_h, view_h, itemh, parent_item; int w, h; } PMLevel;
static PMLevel g_pml[8]; static int g_pmn, g_pm_res, g_pm_done; static int g_pm_arrow;   /* -1 up, 1 down hover scroll */
#define PM_CLASS L"WinShellFM.Popup"
static int pm_rowh(const PMLevel *L, const PMItem *i) { return (i->flags & PMF_SEP) ? px(9) : L->itemh; }
static int pm_item_y(const PMLevel *L, int idx) { int y = 0; for (int k = 0; k < idx; k++) y += pm_rowh(L, &L->m->it[k]); return y; }
static int pm_arrows(const PMLevel *L) { return L->content_h > L->view_h; }
static int pm_hit(const PMLevel *L, int sx, int sy) {  /* index, or -2 up arrow, -3 down arrow, -1 none */
    int x = sx - L->rc.left, y = sy - L->rc.top; if (x < 0 || y < 0 || x >= L->w || y >= L->h) return -1;
    int bd = 1, ah = pm_arrows(L) ? px(16) : 0;
    if (ah && y < bd + ah) return -2; if (ah && y >= L->h - bd - ah) return -3;
    int yy = y - bd - ah + L->scroll, acc = 0;
    for (int k = 0; k < L->m->n; k++) { int h = pm_rowh(L, &L->m->it[k]); if (yy >= acc && yy < acc + h) return k; acc += h; }
    return -1;
}
static void pm_clamp_scroll(PMLevel *L) {
    int mx = L->content_h - L->view_h; if (mx < 0) mx = 0; if (L->scroll > mx) L->scroll = mx; if (L->scroll < 0) L->scroll = 0;
}
static void pm_ensure_hot(PMLevel *L) {
    if (L->hot < 0) return; int y = pm_item_y(L, L->hot), h = pm_rowh(L, &L->m->it[L->hot]);
    if (y < L->scroll) L->scroll = y; else if (y + h > L->scroll + L->view_h) L->scroll = y + h - L->view_h; pm_clamp_scroll(L);
}
static void pm_paint(HWND hw) {
    PMLevel *L = (PMLevel *)GetWindowLongPtrW(hw, GWLP_USERDATA); PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps);
    if (!L) { EndPaint(hw, &ps); return; }
    const Pal *p = &S.pal; const Style *st = cur_style();
    HDC m = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, L->w, L->h); HGDIOBJ ob = SelectObject(m, bm);
    RECT all = { 0, 0, L->w, L->h }; COLORREF bg = mixc(p->tool, p->bg, 0.5); fillr(m, &all, bg); framer(m, &all, p->border); SetBkMode(m, TRANSPARENT);
    SelectObject(m, S.font);
    int ah = pm_arrows(L) ? px(16) : 0, top = 1 + ah, viewb = L->h - 1 - ah;
    HRGN rg = CreateRectRgn(1, top, L->w - 1, viewb); SelectClipRgn(m, rg); DeleteObject(rg);
    int y = top - L->scroll, pad = px(8), gut = px(26);
    for (int k = 0; k < L->m->n; k++) {
        PMItem *it = &L->m->it[k]; int h = pm_rowh(L, it); RECT r = { 2, y, L->w - 2, y + h }; y += h;
        if (r.bottom < top || r.top > viewb) continue;
        if (it->flags & PMF_SEP) { fill4(m, pad, r.top + h / 2, L->w - pad, r.top + h / 2 + 1, p->line); continue; }
        int dis = (it->flags & PMF_DIS) != 0, hot = k == L->hot && !dis && !(it->flags & PMF_HEAD);
        COLORREF fg = dis ? p->dim : p->text;
        if (hot) { rrect(m, &r, st->sel_solid ? p->sel : p->hover, st->sel_solid ? p->sel : p->hover, st->round > 6 ? 5 : (st->round > 2 ? 2 : 0), 0); if (st->sel_solid) fg = p->sel_text; }
        if (it->flags & PMF_HEAD) { SelectObject(m, S.font_big); fg = p->accent; }
        int tx = r.left + gut;
        if (it->flags & PMF_CHECK) {
            int cx = r.left + gut / 2, cy = (r.top + r.bottom) / 2;
            if (it->flags & PMF_RADIO) { RECT e = { cx - 3, cy - 3, cx + 4, cy + 4 }; ellipsec(m, &e, fg, fg, 1); }
            else { linec(m, cx - 4, cy, cx - 1, cy + 3, fg, 2); linec(m, cx - 1, cy + 3, cx + 5, cy - 4, fg, 2); }
        }
        if (it->has_sw) { RECT sw = { r.left + px(6), r.top + h / 2 - px(7), r.left + px(6) + px(14), r.top + h / 2 + px(7) }; rrect(m, &sw, it->sw, p->border, 3, 1); tx = r.left + px(26); }
        RECT tr = { tx, r.top, r.right - pad - (it->acc[0] ? text_w(m, it->acc) + px(24) : 0) - (it->sub ? px(14) : 0), r.bottom }; textr(m, it->text, &tr, TL, fg);
        if (it->acc[0]) { RECT ar = { r.right - pad - text_w(m, it->acc) - px(4), r.top, r.right - pad, r.bottom }; textr(m, it->acc, &ar, TL | DT_RIGHT, hot && st->sel_solid ? fg : p->dim); }
        if (it->sub) { int ax = r.right - px(12), ay = (r.top + r.bottom) / 2; POINT tp[3] = { { ax, ay - 4 }, { ax, ay + 4 }, { ax + 4, ay } }; poly(m, tp, 3, fg); }
        if (it->flags & PMF_HEAD) SelectObject(m, S.font);
    }
    SelectClipRgn(m, NULL);
    if (ah) {
        RECT u = { 1, 1, L->w - 1, 1 + ah }, d = { 1, L->h - 1 - ah, L->w - 1, L->h - 1 };
        fillr(m, &u, mixc(bg, p->text, 0.08)); fillr(m, &d, mixc(bg, p->text, 0.08));
        int cx = L->w / 2, cu = (u.top + u.bottom) / 2, cd = (d.top + d.bottom) / 2;
        POINT a1[3] = { { cx - 5, cu + 3 }, { cx + 5, cu + 3 }, { cx, cu - 3 } }, a2[3] = { { cx - 5, cd - 3 }, { cx + 5, cd - 3 }, { cx, cd + 3 } };
        poly(m, a1, 3, L->scroll > 0 ? p->text : p->dim); poly(m, a2, 3, L->scroll < L->content_h - L->view_h ? p->text : p->dim);
        int vh = viewb - top, tl = vh * L->view_h / L->content_h; if (tl < px(18)) tl = px(18); int rng = vh - tl, mx = L->content_h - L->view_h;
        int ty = top + (mx ? L->scroll * rng / mx : 0); RECT th = { L->w - px(7), ty, L->w - 2, ty + tl }; rrect(m, &th, p->sb_thumb, p->sb_thumb, 2, 0);
    }
    BitBlt(dc, 0, 0, L->w, L->h, m, 0, 0, SRCCOPY); SelectObject(m, ob); DeleteObject(bm); DeleteDC(m); EndPaint(hw, &ps);
}
static LRESULT CALLBACK pm_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) { pm_paint(h); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}
static void pm_register(void) {
    static int done; if (done) return; done = 1; WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = pm_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = PM_CLASS; wc.style = CS_SAVEBITS; RegisterClassExW(&wc);
}
static void pm_close_to(int n) {
    while (g_pmn > n) { PMLevel *L = &g_pml[--g_pmn]; if (L->hw) { SetWindowLongPtrW(L->hw, GWLP_USERDATA, 0); DestroyWindow(L->hw); L->hw = NULL; } }
}
static void pm_open_level(PMenu *m, int sx, int sy, int parent_item, const RECT *prc) {
    if (g_pmn >= 8 || !m->n) return; pm_register();
    PMLevel *L = &g_pml[g_pmn]; memset(L, 0, sizeof *L); L->m = m; L->hot = -1; L->parent_item = parent_item;
    HDC sdc = GetDC(NULL); HGDIOBJ of = SelectObject(sdc, S.font); int mw = 0, hasacc = 0, anysub = 0;
    TEXTMETRICW tm; GetTextMetricsW(sdc, &tm); L->itemh = tm.tmHeight + px(12); if (L->itemh < px(26)) L->itemh = px(26);
    for (int k = 0; k < m->n; k++) { PMItem *it = &m->it[k]; int w = text_w(sdc, it->text) + (it->acc[0] ? text_w(sdc, it->acc) + px(30) : 0); if (it->sub) anysub = 1; if (w > mw) mw = w; if (it->acc[0]) hasacc = 1; }
    (void)hasacc; SelectObject(sdc, of); ReleaseDC(NULL, sdc);
    L->w = mw + px(26) + px(24) + (anysub ? px(10) : 0); if (L->w < px(190)) L->w = px(190);
    for (int k = 0; k < m->n; k++) L->content_h += pm_rowh(L, &m->it[k]);
    POINT pt = { sx, sy }; HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; GetMonitorInfoW(mon, &mi);
    RECT wk = mi.rcWork; int maxh = wk.bottom - wk.top - px(8);
    L->h = L->content_h + 2; if (L->h > maxh) L->h = maxh; L->view_h = L->h - 2; if (L->content_h > L->view_h) L->view_h = L->h - 2 - 2 * px(16);
    int x = sx, y = sy;
    if (x + L->w > wk.right) { x = prc ? prc->left - L->w + px(4) : sx - L->w; if (x < wk.left) x = wk.right - L->w; }
    if (x < wk.left) x = wk.left;
    if (y + L->h > wk.bottom) { if (!prc && y - L->h >= wk.top) y = sy - L->h; else y = wk.bottom - L->h; }
    if (y < wk.top) y = wk.top;
    L->rc = (RECT){ x, y, x + L->w, y + L->h };
    L->hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, PM_CLASS, L"", WS_POPUP, x, y, L->w, L->h, S.hwnd, NULL, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(L->hw, GWLP_USERDATA, (LONG_PTR)L);
    int rr = cur_style()->round > 8 ? 8 : cur_style()->round; if (rr > 2) SetWindowRgn(L->hw, CreateRoundRectRgn(0, 0, L->w + 1, L->h + 1, rr * 2, rr * 2), TRUE);
    ShowWindow(L->hw, SW_SHOWNOACTIVATE); UpdateWindow(L->hw); g_pmn++;
}
static void pm_open_sub(int lvl, int idx) {
    PMLevel *L = &g_pml[lvl]; PMItem *it = &L->m->it[idx]; if (!it->sub) return;
    pm_close_to(lvl + 1); int y = L->rc.top + 1 + (pm_arrows(L) ? px(16) : 0) + pm_item_y(L, idx) - L->scroll; RECT prc = L->rc;
    pm_open_level(it->sub, L->rc.right - 2, y - 1, idx, &prc);
}
static int pm_level_at(int sx, int sy) { for (int i = g_pmn - 1; i >= 0; i--) { PMLevel *L = &g_pml[i]; if (sx >= L->rc.left && sx < L->rc.right && sy >= L->rc.top && sy < L->rc.bottom) return i; } return -1; }
static void pm_set_hot(int lvl, int idx) {
    PMLevel *L = &g_pml[lvl]; if (L->hot == idx) return; L->hot = idx; InvalidateRect(L->hw, NULL, FALSE);
    if (g_pmn > lvl + 1) pm_close_to(lvl + 1);
    if (idx >= 0 && L->m->it[idx].sub && !(L->m->it[idx].flags & PMF_DIS)) pm_open_sub(lvl, idx);
}
static void pm_activate(int lvl, int idx) {
    PMItem *it = &g_pml[lvl].m->it[idx]; if (it->flags & (PMF_DIS | PMF_SEP | PMF_HEAD)) return;
    if (it->sub) { pm_open_sub(lvl, idx); if (g_pmn > lvl + 1) g_pml[lvl + 1].hot = -1; return; }
    g_pm_res = it->id; g_pm_done = 1;
}
static void pm_scroll(int lvl, int d) { PMLevel *L = &g_pml[lvl]; if (!pm_arrows(L)) return; int o = L->scroll; L->scroll += d; pm_clamp_scroll(L); if (o != L->scroll) InvalidateRect(L->hw, NULL, FALSE); }
static void pm_move_hot(int lvl, int dir) {
    PMLevel *L = &g_pml[lvl]; int n = L->m->n, k = L->hot;
    for (int tries = 0; tries < n; tries++) { k = k < 0 ? (dir > 0 ? 0 : n - 1) : (k + dir + n) % n; PMItem *it = &L->m->it[k]; if (!(it->flags & (PMF_SEP | PMF_DIS | PMF_HEAD))) { L->hot = k; pm_ensure_hot(L); if (g_pmn > lvl + 1) pm_close_to(lvl + 1); InvalidateRect(L->hw, NULL, FALSE); return; } }
}
/* modal: returns the chosen command id (0 = none). Frees the menu. */
static int pm_track(PMenu *m, int sx, int sy) {
    g_pmn = 0; g_pm_res = 0; g_pm_done = 0; g_pm_arrow = 0;
    pm_open_level(m, sx, sy, -1, NULL);
    if (!g_pmn) { pm_free(m); return 0; }
    SetCapture(g_pml[0].hw);
    MSG msg; DWORD t0 = GetTickCount(); int pressed_ok = 0; (void)t0; (void)pressed_ok;
    while (!g_pm_done && g_pmn > 0) {
        if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (g_pm_arrow && g_pmn) pm_scroll(g_pmn - 1, g_pm_arrow * px(18));
            MsgWaitForMultipleObjects(0, NULL, FALSE, 40, QS_ALLINPUT); continue;
        }
        if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
        switch (msg.message) {
        case WM_MOUSEMOVE: {
            int sx2 = msg.pt.x, sy2 = msg.pt.y, lv = pm_level_at(sx2, sy2); g_pm_arrow = 0;
            if (lv >= 0) { int h = pm_hit(&g_pml[lv], sx2, sy2); if (h == -2) g_pm_arrow = -1; else if (h == -3) g_pm_arrow = 1; else if (h >= 0) { if (!(g_pml[lv].m->it[h].flags & (PMF_SEP | PMF_HEAD))) pm_set_hot(lv, h); else if (lv == g_pmn - 1 && g_pml[lv].hot >= 0) pm_set_hot(lv, -1); } }
            continue; }
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
            int lv = pm_level_at(msg.pt.x, msg.pt.y);
            if (lv < 0) { g_pm_res = 0; g_pm_done = 1; }
            else { int h = pm_hit(&g_pml[lv], msg.pt.x, msg.pt.y); if (h >= 0) pm_activate(lv, h); else if (h == -2) pm_scroll(lv, -px(40)); else if (h == -3) pm_scroll(lv, px(40)); }
            continue; }
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: continue;
        case WM_MOUSEWHEEL: { int lv = pm_level_at(msg.pt.x, msg.pt.y); if (lv < 0) lv = g_pmn - 1; short d = (short)HIWORD(msg.wParam); pm_scroll(lv, -d * px(30) / 120); continue; }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            int lv = g_pmn - 1; PMLevel *L = &g_pml[lv];
            if (msg.wParam == VK_ESCAPE) { if (g_pmn > 1) pm_close_to(g_pmn - 1); else g_pm_done = 1; }
            else if (msg.wParam == VK_DOWN) pm_move_hot(lv, 1); else if (msg.wParam == VK_UP) pm_move_hot(lv, -1);
            else if (msg.wParam == VK_RIGHT) { if (L->hot >= 0 && L->m->it[L->hot].sub) { pm_open_sub(lv, L->hot); if (g_pmn > lv + 1) pm_move_hot(lv + 1, 1); } }
            else if (msg.wParam == VK_LEFT) { if (g_pmn > 1) pm_close_to(g_pmn - 1); }
            else if (msg.wParam == VK_RETURN || msg.wParam == VK_SPACE) { if (L->hot >= 0) pm_activate(lv, L->hot); }
            else if (msg.wParam == VK_HOME) { L->hot = -1; pm_move_hot(lv, 1); } else if (msg.wParam == VK_END) { L->hot = -1; pm_move_hot(lv, -1); }
            else if (msg.wParam == VK_NEXT) pm_scroll(lv, L->view_h); else if (msg.wParam == VK_PRIOR) pm_scroll(lv, -L->view_h);
            continue; }
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: continue;
        case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: g_pm_done = 1; continue;
        case WM_ACTIVATEAPP: if (!msg.wParam) g_pm_done = 1; break;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    ReleaseCapture(); pm_close_to(0); pm_free(m); int r = g_pm_res; g_pm_res = 0; g_pm_done = 0; return r;
}
static void popup_at(PMenu *m, int x, int y) {        /* client coordinates of the main window */
    POINT pt = { x, y }; ClientToScreen(S.hwnd, &pt); int cmd = pm_track(m, pt.x, pt.y);
    if (cmd) PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}
/* menu from a button: opens below the rect */
static int pm_pick_below(PMenu *m, const RECT *client_rc) {
    POINT pt = { client_rc->left, client_rc->bottom }; ClientToScreen(S.hwnd, &pt); return pm_track(m, pt.x, pt.y);
}

/* ========================================================================== */
/* THEMED DIALOG FRAMEWORK (immediate rows: headings, checks, choices, lists)  */
/* ========================================================================== */
enum { R_HEAD, R_NOTE, R_CHECK, R_CHOICE, R_SPIN, R_BTN, R_ITEM, R_SWATCH, R_LABEL2, R_SPACE };
typedef struct { int type, id; WCHAR text[300], sub[300]; int *iv; int mn, mx, st, nopts, sel, enabled; const WCHAR *const *opts; u32 chips[6]; COLORREF col; } Row;
typedef struct Dlg {
    WCHAR title[96]; int w, h; Row *rows; int nr, cap; int scroll, total;
    int ntab; WCHAR tabs[10][40]; int tab;
    int nbtn; struct { WCHAR t[40]; int id, primary; } btns[4];
    void (*build)(struct Dlg *); int (*act)(struct Dlg *, Row *, int); void *user;
    HWND hw, owner; int done, result, hover, dragsb, sb_off; RECT rc_body, rc_cap, rc_close, rc_tabs, rc_foot, rc_btn[4], rc_sb; RECT *rrc; int rrc_cap;
    int tabhover, closehover, btnhover, ctl_h;
} Dlg;
static Row *dlg_row(Dlg *d, int type, int id, const WCHAR *text) {
    if (d->nr == d->cap) { d->cap = d->cap ? d->cap * 2 : 32; d->rows = (Row *)realloc(d->rows, d->cap * sizeof(Row)); }
    Row *r = &d->rows[d->nr++]; memset(r, 0, sizeof *r); r->type = type; r->id = id; r->enabled = 1; scpy(r->text, 300, text); return r;
}
static void dlg_head(Dlg *d, const WCHAR *t) { dlg_row(d, R_HEAD, 0, t); }
static void dlg_note(Dlg *d, const WCHAR *t) { dlg_row(d, R_NOTE, 0, t); }
static void dlg_check(Dlg *d, int id, const WCHAR *t, int *v) { dlg_row(d, R_CHECK, id, t)->iv = v; }
static void dlg_choice(Dlg *d, int id, const WCHAR *t, int *v, const WCHAR *const *opts, int n) { Row *r = dlg_row(d, R_CHOICE, id, t); r->iv = v; r->opts = opts; r->nopts = n; }
static void dlg_spin(Dlg *d, int id, const WCHAR *t, int *v, int mn, int mx, int st, const WCHAR *unit) { Row *r = dlg_row(d, R_SPIN, id, t); r->iv = v; r->mn = mn; r->mx = mx; r->st = st; if (unit) scpy(r->sub, 300, unit); }
static Row *dlg_btn(Dlg *d, int id, const WCHAR *t) { return dlg_row(d, R_BTN, id, t); }
static Row *dlg_item(Dlg *d, int id, const WCHAR *t, const WCHAR *sub, int selected) { Row *r = dlg_row(d, R_ITEM, id, t); scpy(r->sub, 300, sub); r->sel = selected; return r; }
static void dlg_kv(Dlg *d, const WCHAR *k, const WCHAR *v) { Row *r = dlg_row(d, R_LABEL2, 0, k); scpy(r->sub, 300, v); }
static void dlg_clear(Dlg *d) { d->nr = 0; }
static void dlg_foot(Dlg *d, int id, const WCHAR *t, int primary) { if (d->nbtn < 4) { scpy(d->btns[d->nbtn].t, 40, t); d->btns[d->nbtn].id = id; d->btns[d->nbtn].primary = primary; d->nbtn++; } }

static int dlg_rowh(Dlg *d, Row *r, HDC dc, int width) {
    switch (r->type) {
    case R_HEAD: return px(40);
    case R_SPACE: return px(10);
    case R_NOTE: { RECT q = { 0, 0, width, 0 }; DrawTextW(dc, r->text, -1, &q, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX); return q.bottom + px(10); }
    case R_ITEM: return r->sub[0] ? px(48) : px(36);
    case R_SWATCH: return px(58);
    case R_LABEL2: { RECT q = { 0, 0, width * 6 / 10, 0 }; DrawTextW(dc, r->sub, -1, &q, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX); int h = q.bottom + px(10); return h < px(30) ? px(30) : h; }
    default: return px(38);
    }
}
static void dlg_layout(Dlg *d, HDC dc) {
    RECT cr; GetClientRect(d->hw, &cr); int W = cr.right, H = cr.bottom, cap = px(34), foot = d->nbtn ? px(54) : 0, bd = 1;
    d->rc_cap = (RECT){ 0, 0, W, cap }; d->rc_close = (RECT){ W - px(44), 0, W, cap };
    int tabw = d->ntab ? px(160) : 0;
    d->rc_tabs = (RECT){ bd, cap, tabw, H - foot - bd }; d->rc_foot = (RECT){ bd, H - foot - bd, W - bd, H - bd };
    d->rc_body = (RECT){ tabw + px(4), cap + px(4), W - px(14) - bd, H - foot - bd - px(4) };
    d->rc_sb = (RECT){ W - px(14) - bd, cap, W - bd, H - foot - bd };
    int bw = d->rc_body.right - d->rc_body.left - px(20); if (d->nr > d->rrc_cap) { d->rrc_cap = d->nr + 16; d->rrc = (RECT *)realloc(d->rrc, d->rrc_cap * sizeof(RECT)); }
    HGDIOBJ of = SelectObject(dc, S.font); int y = 0;
    for (int i = 0; i < d->nr; i++) { int h = dlg_rowh(d, &d->rows[i], dc, bw); d->rrc[i] = (RECT){ d->rc_body.left + px(8), d->rc_body.top + y - d->scroll, d->rc_body.right - px(8), d->rc_body.top + y + h - d->scroll }; y += h; }
    d->total = y + px(8); int vh = d->rc_body.bottom - d->rc_body.top, mx = d->total - vh; if (mx < 0) mx = 0; if (d->scroll > mx) { int ds = d->scroll - mx; d->scroll = mx; for (int i = 0; i < d->nr; i++) { d->rrc[i].top += ds; d->rrc[i].bottom += ds; } }
    int x = W - bd - px(14); for (int i = d->nbtn - 1; i >= 0; i--) { int w = text_w(dc, d->btns[i].t) + px(34); if (w < px(90)) w = px(90); d->rc_btn[i] = (RECT){ x - w, d->rc_foot.top + px(10), x, d->rc_foot.bottom - px(10) }; x -= w + px(10); }
    SelectObject(dc, of);
}
static void dlg_button(HDC dc, const RECT *r, const WCHAR *t, int primary, int hot) {
    const Pal *p = &S.pal; const Style *st = cur_style(); int rad = st->round > 8 ? 8 : st->round;
    COLORREF bg = primary ? p->accent : (hot ? p->hover : p->btn), fg = primary ? contrast(p->accent) : p->text;
    if (primary && hot) bg = mixc(p->accent, p->text, 0.15);
    rrect(dc, r, bg, primary ? bg : p->btn_border, rad, 1); textr(dc, t, r, TC, fg);
}
static void dlg_paint(Dlg *d, HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); RECT cr; GetClientRect(d->hw, &cr); RECT all = cr; SelectObject(dc, S.font); SetBkMode(dc, TRANSPARENT);
    dlg_layout(d, dc);
    fillr(dc, &all, p->bg);
    if (st->layout == LAY_WIN7 || !wcscmp(st->key, L"total")) vgrad(dc, &d->rc_cap, p->cap, p->cap2); else fillr(dc, &d->rc_cap, p->cap);
    RECT tr = { px(12), 0, d->rc_close.left - 4, d->rc_cap.bottom }; SelectObject(dc, S.font_big); textr(dc, d->title, &tr, TL, p->cap_text); SelectObject(dc, S.font);
    if (d->closehover) fillr(dc, &d->rc_close, !wcscmp(st->key, L"macos") ? p->hover : RGB(232, 17, 35));
    { int cx = (d->rc_close.left + d->rc_close.right) / 2, cy = (d->rc_close.top + d->rc_close.bottom) / 2; COLORREF cc = d->closehover ? RGB(255, 255, 255) : p->cap_text;
      linec(dc, cx - 5, cy - 5, cx + 6, cy + 6, cc, 1); linec(dc, cx - 5, cy + 5, cx + 6, cy - 6, cc, 1); }
    fill4(dc, 0, d->rc_cap.bottom - 1, cr.right, d->rc_cap.bottom, p->line);
    if (d->ntab) {
        fillr(dc, &d->rc_tabs, p->side); fill4(dc, d->rc_tabs.right - 1, d->rc_tabs.top, d->rc_tabs.right, d->rc_tabs.bottom, p->line);
        for (int i = 0; i < d->ntab; i++) { RECT r = { d->rc_tabs.left + px(6), d->rc_tabs.top + px(8) + i * px(38), d->rc_tabs.right - px(8), d->rc_tabs.top + px(8) + i * px(38) + px(34) };
            int sel = i == d->tab; if (sel) rrect(dc, &r, p->nav_sel, p->nav_sel, st->round > 6 ? 6 : st->round, 0); else if (i == d->tabhover) rrect(dc, &r, p->hover, p->hover, st->round > 6 ? 6 : st->round, 0);
            RECT t2 = { r.left + px(12), r.top, r.right - 4, r.bottom }; textr(dc, d->tabs[i], &t2, TL, sel ? p->nav_sel_text : p->text); }
    }
    HRGN rg = CreateRectRgn(d->rc_body.left, d->rc_body.top, d->rc_body.right, d->rc_body.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
    int rad = st->round > 8 ? 8 : st->round;
    for (int i = 0; i < d->nr; i++) {
        Row *r = &d->rows[i]; RECT q = d->rrc[i]; if (q.bottom < d->rc_body.top || q.top > d->rc_body.bottom) continue; int hot = i == d->hover && r->enabled;
        COLORREF fg = r->enabled ? p->text : p->dim; int mid = (q.top + q.bottom) / 2;
        switch (r->type) {
        case R_HEAD: SelectObject(dc, S.font_big); { RECT t = q; t.top += px(8); textr(dc, r->text, &t, TL, p->accent); } SelectObject(dc, S.font); fill4(dc, q.left, q.bottom - 2, q.right, q.bottom - 1, p->line); break;
        case R_NOTE: textr(dc, r->text, &q, DT_WORDBREAK, p->dim); break;
        case R_CHECK: { int bs = px(18); RECT b = { q.left + px(4), mid - bs / 2, q.left + px(4) + bs, mid + bs / 2 }; if (hot) rrect(dc, &q, p->hover, p->hover, rad, 0);
            rrect(dc, &b, *r->iv ? p->accent : p->input, *r->iv ? p->accent : p->input_border, 3, 1);
            if (*r->iv) { COLORREF c2 = contrast(p->accent); linec(dc, b.left + 4, mid, b.left + 7, mid + 3, c2, 2); linec(dc, b.left + 7, mid + 3, b.right - 3, mid - 4, c2, 2); }
            RECT t = { b.right + px(10), q.top, q.right, q.bottom }; textr(dc, r->text, &t, TL, fg); break; }
        case R_CHOICE: { if (hot) rrect(dc, &q, p->hover, p->hover, rad, 0); RECT t = { q.left + px(8), q.top, q.left + (q.right - q.left) * 5 / 10, q.bottom }; textr(dc, r->text, &t, TL, fg);
            RECT b = { q.left + (q.right - q.left) * 5 / 10, q.top + px(4), q.right - px(4), q.bottom - px(4) }; rrect(dc, &b, p->input, p->input_border, rad, 1);
            int v = r->iv ? *r->iv : 0; RECT t2 = { b.left + px(10), b.top, b.right - px(24), b.bottom }; textr(dc, v >= 0 && v < r->nopts ? r->opts[v] : L"", &t2, TL, p->text);
            int ax = b.right - px(14); POINT a[3] = { { ax - 4, mid - 2 }, { ax + 4, mid - 2 }, { ax, mid + 3 } }; poly(dc, a, 3, p->dim); break; }
        case R_SPIN: { if (hot) rrect(dc, &q, p->hover, p->hover, rad, 0); RECT t = { q.left + px(8), q.top, q.left + (q.right - q.left) / 2, q.bottom }; textr(dc, r->text, &t, TL, fg);
            int bw = px(34); RECT m1 = { q.right - px(4) - bw * 2 - px(70), q.top + px(4), q.right - px(4) - bw - px(70), q.bottom - px(4) }, m2 = { q.right - px(4) - bw, q.top + px(4), q.right - px(4), q.bottom - px(4) }, vr = { m1.right, q.top, m2.left, q.bottom };
            rrect(dc, &m1, p->btn, p->btn_border, rad, 1); rrect(dc, &m2, p->btn, p->btn_border, rad, 1); textr(dc, L"-", &m1, TC, p->text); textr(dc, L"+", &m2, TC, p->text);
            WCHAR v[60]; wfmt(v, 60, L"%d%ls", r->iv ? *r->iv : 0, r->sub); textr(dc, v, &vr, TC, p->text); break; }
        case R_BTN: { int w = text_w(dc, r->text) + px(34); RECT b = { q.left + px(4), q.top + px(3), q.left + px(4) + w, q.bottom - px(3) }; dlg_button(dc, &b, r->text, 0, hot); break; }
        case R_ITEM: { if (r->sel) rrect(dc, &q, p->sel, p->sel, rad, 0); else if (hot) rrect(dc, &q, p->hover, p->hover, rad, 0);
            COLORREF f2 = r->sel ? p->sel_text : fg; paint_glyph(dc, 5, q.left + px(8), mid - px(8), px(16), f2);
            RECT t = { q.left + px(34), q.top + (r->sub[0] ? px(3) : 0), q.right - px(8), r->sub[0] ? q.top + px(24) : q.bottom }; textr(dc, r->text, &t, TL, f2);
            if (r->sub[0]) { SelectObject(dc, S.font_small); RECT t2 = { q.left + px(34), q.top + px(24), q.right - px(8), q.bottom - px(2) }; textr(dc, r->sub, &t2, TL, r->sel ? f2 : p->dim); SelectObject(dc, S.font); } break; }
        case R_SWATCH: { if (r->sel) rrect(dc, &q, mixc(p->bg, p->accent, 0.2), p->accent, rad, 1); else if (hot) rrect(dc, &q, p->hover, p->border, rad, 1); else rrect(dc, &q, p->bg, p->line, rad, 1);
            RECT t = { q.left + px(12), q.top + px(4), q.left + (q.right - q.left) * 6 / 10, q.top + px(28) }; SelectObject(dc, S.font_big); textr(dc, r->text, &t, TL, p->text); SelectObject(dc, S.font_small);
            RECT t2 = { q.left + px(12), q.top + px(28), q.left + (q.right - q.left) * 6 / 10, q.bottom - px(4) }; textr(dc, r->sub, &t2, TL, p->dim); SelectObject(dc, S.font);
            int cx = q.right - px(14) - 6 * px(22); for (int k = 0; k < 6; k++) { RECT c = { cx + k * px(22), mid - px(10), cx + k * px(22) + px(18), mid + px(10) }; rrect(dc, &c, RGBH(r->chips[k]), p->border, 3, 1); } break; }
        case R_LABEL2: { RECT t = { q.left + px(8), q.top, q.left + (q.right - q.left) * 3 / 10, q.bottom }; textr(dc, r->text, &t, TL, p->dim); RECT t2 = { q.left + (q.right - q.left) * 33 / 100, q.top + px(2), q.right - px(8), q.bottom }; textr(dc, r->sub, &t2, DT_WORDBREAK | DT_VCENTER, p->text); break; }
        }
    }
    SelectClipRgn(dc, NULL);
    int vh = d->rc_body.bottom - d->rc_body.top;
    if (d->total > vh) { int tl = vh * vh / d->total; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = d->total - vh; int ty = d->rc_body.top + (mx ? d->scroll * rng / mx : 0);
        RECT th = { d->rc_sb.left + px(3), ty, d->rc_sb.right - px(3), ty + tl }; rrect(dc, &th, d->dragsb ? p->text : p->sb_thumb, p->sb_thumb, 4, 0); }
    if (d->nbtn) { fillr(dc, &d->rc_foot, p->status); fill4(dc, d->rc_foot.left, d->rc_foot.top, d->rc_foot.right, d->rc_foot.top + 1, p->line);
        for (int i = 0; i < d->nbtn; i++) dlg_button(dc, &d->rc_btn[i], d->btns[i].t, d->btns[i].primary, d->btnhover == i + 1); }
    framer(dc, &all, p->border);
}
static void dlg_rebuild(Dlg *d) { if (d->build) { d->nr = 0; d->build(d); } }
static int dlg_hit_row(Dlg *d, int x, int y) {
    if (!in_rect(&d->rc_body, x, y)) return -1;
    for (int i = 0; i < d->nr; i++) if (in_rect(&d->rrc[i], x, y)) return i; return -1;
}
static void dlg_finish(Dlg *d, int result) { d->result = result; d->done = 1; }
static void dlg_do_act(Dlg *d, Row *r, int id) {
    int rc = d->act ? d->act(d, r, id) : (r ? 0 : 1);
    if (rc) { dlg_finish(d, rc == 1 && !r ? id : (rc == 1 ? id : rc)); return; }
    if (!d->done) { dlg_rebuild(d); InvalidateRect(d->hw, NULL, FALSE); }
}
static void dlg_click(Dlg *d, int x, int y) {
    if (in_rect(&d->rc_close, x, y)) { dlg_finish(d, 0); return; }
    for (int i = 0; i < d->nbtn; i++) if (in_rect(&d->rc_btn[i], x, y)) { dlg_do_act(d, NULL, d->btns[i].id); return; }
    if (d->ntab && in_rect(&d->rc_tabs, x, y)) { int i = (y - d->rc_tabs.top - px(8)) / px(38); if (i >= 0 && i < d->ntab && i != d->tab) { d->tab = i; d->scroll = 0; dlg_rebuild(d); InvalidateRect(d->hw, NULL, FALSE); } return; }
    if (in_rect(&d->rc_sb, x, y)) { int vh = d->rc_body.bottom - d->rc_body.top; if (d->total > vh) { int tl = vh * vh / d->total; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = d->total - vh, ty = d->rc_body.top + d->scroll * rng / mx;
        if (y >= ty && y < ty + tl) { d->dragsb = 1; d->sb_off = y - ty; SetCapture(d->hw); } else { d->scroll += y < ty ? -vh : vh; if (d->scroll < 0) d->scroll = 0; InvalidateRect(d->hw, NULL, FALSE); } } return; }
    int i = dlg_hit_row(d, x, y); if (i < 0) return; Row *r = &d->rows[i]; if (!r->enabled) return; RECT q = d->rrc[i];
    switch (r->type) {
    case R_CHECK: *r->iv = !*r->iv; dlg_do_act(d, r, r->id); break;
    case R_CHOICE: { PMenu *m = pm_new(); for (int k = 0; k < r->nopts; k++) pm_radio(m, 1000 + k, r->opts[k], k == *r->iv, 1);
        POINT pt = { q.left + (q.right - q.left) * 5 / 10, q.bottom }; ClientToScreen(d->hw, &pt); int c = pm_track(m, pt.x, pt.y); if (c >= 1000) { *r->iv = c - 1000; dlg_do_act(d, r, r->id); } break; }
    case R_SPIN: { int bw = px(34); int m2l = q.right - px(4) - bw, m1r = q.right - px(4) - bw - px(70);
        if (x >= m2l) *r->iv += r->st; else if (x < m1r && x >= m1r - bw) *r->iv -= r->st; else break; if (*r->iv < r->mn) *r->iv = r->mn; if (*r->iv > r->mx) *r->iv = r->mx; dlg_do_act(d, r, r->id); break; }
    case R_BTN: { HDC bdc = GetDC(d->hw); HGDIOBJ bf = SelectObject(bdc, S.font); int w = text_w(bdc, r->text) + px(34); SelectObject(bdc, bf); ReleaseDC(d->hw, bdc); if (x <= q.left + px(4) + w) dlg_do_act(d, r, r->id); break; }
    case R_ITEM: case R_SWATCH: dlg_do_act(d, r, r->id); break;
    default: break;
    }
}
static LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dlg *d = (Dlg *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) { d = (Dlg *)((CREATESTRUCTW *)lp)->lpCreateParams; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d); }
    if (!d) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc); HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(m, b);
        dlg_paint(d, m); BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY); SelectObject(m, ob); DeleteObject(b); DeleteDC(m); EndPaint(h, &ps); return 0; }
    case WM_NCHITTEST: { POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &pt); if (d->rc_cap.bottom && in_rect(&d->rc_cap, pt.x, pt.y) && !in_rect(&d->rc_close, pt.x, pt.y)) return HTCAPTION; return HTCLIENT; }
    case WM_LBUTTONDOWN: dlg_click(d, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: if (d->dragsb) { d->dragsb = 0; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_MOUSEMOVE: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (d->dragsb) { int vh = d->rc_body.bottom - d->rc_body.top; int tl = vh * vh / d->total; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = d->total - vh; int ty = y - d->sb_off - d->rc_body.top; if (ty < 0) ty = 0; if (ty > rng) ty = rng; d->scroll = rng ? ty * mx / rng : 0; InvalidateRect(h, NULL, FALSE); return 0; }
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te);
        int hr = dlg_hit_row(d, x, y), ch = in_rect(&d->rc_close, x, y), th = -1, bh = 0;
        if (d->ntab && in_rect(&d->rc_tabs, x, y)) { th = (y - d->rc_tabs.top - px(8)) / px(38); if (th >= d->ntab || th < 0) th = -1; }
        for (int i = 0; i < d->nbtn; i++) if (in_rect(&d->rc_btn[i], x, y)) bh = i + 1;
        if (hr != d->hover || ch != d->closehover || th != d->tabhover || bh != d->btnhover) { d->hover = hr; d->closehover = ch; d->tabhover = th; d->btnhover = bh; InvalidateRect(h, NULL, FALSE); } return 0; }
    case WM_MOUSELEAVE: d->hover = -1; d->closehover = 0; d->tabhover = -1; d->btnhover = 0; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL: { int vh = d->rc_body.bottom - d->rc_body.top; if (d->total > vh) { d->scroll -= (short)HIWORD(wp) * px(60) / 120; if (d->scroll < 0) d->scroll = 0; if (d->scroll > d->total - vh) d->scroll = d->total - vh; InvalidateRect(h, NULL, FALSE); } return 0; }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { dlg_finish(d, 0); return 0; }
        if (wp == VK_RETURN) { for (int i = 0; i < d->nbtn; i++) if (d->btns[i].primary) { dlg_do_act(d, NULL, d->btns[i].id); return 0; } }
        if (wp == VK_NEXT || wp == VK_PRIOR || wp == VK_DOWN || wp == VK_UP) { int vh = d->rc_body.bottom - d->rc_body.top; int dd = (wp == VK_NEXT ? vh : wp == VK_PRIOR ? -vh : wp == VK_DOWN ? px(40) : -px(40)); d->scroll += dd; if (d->scroll < 0) d->scroll = 0; if (d->total > vh && d->scroll > d->total - vh) d->scroll = d->total - vh; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_CLOSE: dlg_finish(d, 0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static HWND g_dlg_top;
/* runs the dialog modally; returns d->result */
static int dlg_run(Dlg *d) {
    static int reg; if (!reg) { reg = 1; WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = dlg_proc; wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"WinShellFM.Dlg"; RegisterClassExW(&wc); }
    d->owner = g_dlg_top ? g_dlg_top : S.hwnd; d->hover = -1; d->tabhover = -1; d->done = 0; d->result = 0;
    RECT or_; GetWindowRect(S.hwnd, &or_); MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromWindow(S.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int sw = mi.rcWork.right - mi.rcWork.left, sh = mi.rcWork.bottom - mi.rcWork.top; int w = d->w, h = d->h; if (w > sw - 10) w = sw - 10; if (h > sh - 10) h = sh - 10;
    int x = (or_.left + or_.right) / 2 - w / 2, y = (or_.top + or_.bottom) / 2 - h / 2; if (x < mi.rcWork.left) x = mi.rcWork.left; if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w; if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    dlg_rebuild(d);
    d->hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"WinShellFM.Dlg", d->title, WS_POPUP, x, y, w, h, d->owner, NULL, GetModuleHandleW(NULL), d);
    HWND prevtop = g_dlg_top; g_dlg_top = d->hw; EnableWindow(d->owner, FALSE);
    ShowWindow(d->hw, SW_SHOW); SetForegroundWindow(d->hw); SetFocus(d->hw);
    MSG msg; while (!d->done) { if (GetMessageW(&msg, NULL, 0, 0) <= 0) { PostQuitMessage((int)msg.wParam); break; } TranslateMessage(&msg); DispatchMessageW(&msg); }
    EnableWindow(d->owner, TRUE); g_dlg_top = prevtop; DestroyWindow(d->hw); d->hw = NULL; SetForegroundWindow(d->owner); SetFocus(d->owner);
    free(d->rows); d->rows = NULL; free(d->rrc); d->rrc = NULL; d->nr = d->cap = d->rrc_cap = 0; redraw();
    return d->result;
}
/* themed MessageBox replacement */
static void msg_build(Dlg *d) { dlg_note(d, (const WCHAR *)d->user); }
static int msg_act(Dlg *d, Row *r, int id) { (void)d; (void)r; return id ? id : 0; }
static int fm_msgbox(const WCHAR *text, const WCHAR *title, UINT flags) {
    Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, title ? title : L"WinShell"); d.user = (void *)text;
    HDC dc = GetDC(NULL); HGDIOBJ of = SelectObject(dc, S.font ? S.font : GetStockObject(DEFAULT_GUI_FONT)); RECT q = { 0, 0, px(430), 0 }; DrawTextW(dc, text, -1, &q, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX); SelectObject(dc, of); ReleaseDC(NULL, dc);
    d.w = px(500); d.h = q.bottom + px(34 + 54 + 40); if (d.h < px(190)) d.h = px(190); d.build = msg_build; d.act = msg_act;
    UINT k = flags & 0xF;
    if (k == MB_YESNO || k == MB_YESNOCANCEL) { dlg_foot(&d, IDYES, L"Yes", 1); dlg_foot(&d, IDNO, L"No", 0); if (k == MB_YESNOCANCEL) dlg_foot(&d, IDCANCEL, L"Cancel", 0); }
    else if (k == MB_OKCANCEL) { dlg_foot(&d, IDOK, L"OK", 1); dlg_foot(&d, IDCANCEL, L"Cancel", 0); }
    else dlg_foot(&d, IDOK, L"OK", 1);
    int r = dlg_run(&d); if (!r) r = (k == MB_YESNO) ? IDNO : (k == MB_OK ? IDOK : IDCANCEL); return r;
}

/* ---- shared globals and the command enum (included before the preview engine) ---- */
static int g_mx = -1, g_my = -1, g_fmx = -1, g_fmy = -1;
enum {
    CMD_NEWTAB = 100, CMD_NEWFOLDER, CMD_NEWFILE, CMD_CMD, CMD_EXIT, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELALL, CMD_INVERT, CMD_RENAME, CMD_DELETE,
    CMD_COPYPATH, CMD_PROPS, CMD_OPEN, CMD_OPENWITH, CMD_REFRESH, CMD_VDET, CMD_VICON, CMD_VTILE, CMD_PREVIEW, CMD_HIDDEN, CMD_THUMBS,
    CMD_SORT_NAME, CMD_SORT_DATE, CMD_SORT_TYPE, CMD_SORT_SIZE, CMD_ASC, CMD_DIRS_FIRST, CMD_DIRS_LAST, CMD_DIRS_MIXED,
    CMD_ICON_PLUS, CMD_ICON_MINUS, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_BACK, CMD_FWD, CMD_UP, CMD_HOME, CMD_PC, CMD_FAV, CMD_RECENT,
    CMD_DARK, CMD_STYLEFONTS, CMD_SUBFOLDERS, CMD_RESTORE, CMD_FONT_PLUS, CMD_FONT_MINUS, CMD_BACKUP, CMD_RESTORE_CFG,
    CMD_AUTO_EN, CMD_AUTO_ADD, CMD_AUTO_RUN, CMD_AUTO_LOGIN, CMD_PIN, CMD_FAVTOGGLE, CMD_TABCLOSE, CMD_TABOTHERS, CMD_TABPIN, CMD_TABDUP,
    CMD_ABOUT, CMD_RESET_ACCENT, CMD_PREVW, CMD_SETTINGS, CMD_STYLES, CMD_OSNEXT, CMD_SHORTCUT, CMD_ADMIN, CMD_EXTRACT_HERE, CMD_EXTRACT_NAME,
    CMD_EXTRACT_TO, CMD_COMPRESS_ZIP, CMD_COMPRESS_TAR, CMD_COMPRESS_TGZ, CMD_COMPRESS_7Z, CMD_COMPRESS_RAR, CMD_FOLDERCOLOR, CMD_FOLDERCOLOR_CLEAR,
    CMD_SETICON, CMD_CLEARICON, CMD_PVFULL, CMD_TREE, CMD_CHECKS, CMD_RIBBON, CMD_SELNONE, CMD_ADDFONT, CMD_HAMBURGER, CMD_SHORTCUT_HERE,
    CMD_THUMB_IMG, CMD_THUMB_VID, CMD_THUMB_GFX, CMD_DRAGDROP, CMD_RUBBER, CMD_ADMINMENU, CMD_COUNTS, CMD_CLEARHIST, CMD_PVSIDE, CMD_ADDSEARCH, CMD_PVPLAY,
    CMD_FOLDERFIRST, CMD_NEWWIN, CMD_SETICON_EXT, CMD_SETICON_DIR, CMD_EXTRACT_MENU, CMD_PVCLOSE, CMD_SRCH_HIST0 = 700, CMD_SRCH_PIN0 = 760, CMD_SRCH_EXT0 = 820, CMD_SRCH_PINCUR = 880, CMD_SRCH_CLEAR, CMD_SRCH_UNPIN0 = 900, CMD_OW0 = 2000, CMD_OW_MORE = 2400,
    CMD_TAB0 = 200, CMD_STYLE0 = 300, CMD_THEME0 = 340, CMD_ACCENT0 = 360, CMD_AUTOREM0 = 400, CMD_COLOR0 = 460, CMD_PVBTN0 = 600
};
static int g_tab_ctx = 0;

/* ========================================================================== */
/* PREVIEW ENGINE: text, image, PDF, Word/Excel/PowerPoint, archives, media   */
/* ========================================================================== */
enum { PK_NONE, PK_INFO, PK_TEXT, PK_IMAGE, PK_DOC, PK_MEDIA, PK_ARCHIVE, PK_LOADING, PK_ERROR };
typedef struct { int x, y, w, h, fh; u8 bold, mono, kind, sep, right; u32 col; WCHAR *t; int len; } PRun;   /* kind 0 text,1 fill,2 frame */
typedef struct { PRun *r; int n, cap; int w, h; u32 bg; WCHAR title[80]; HBITMAP img; int iw, ih; } PPage;
typedef struct {
    PPage *pg; int npg, cap, cur; int kind, fmt; WCHAR err[220], path[MAX_PATH], info[200];
    int a_run, a_ch, b_run, b_ch, selecting, havesel; int gen; int canext; int arc_n; u64 arc_total;
} PV;
static PV g_pv;
typedef struct { int id; RECT r; int en, on; } PBtn;
typedef struct {
    RECT area, hdr, tb, body, view, vsb, hsb, mc; PBtn b[20]; int nb; int full; HWND hw; int zoom, rot, sx, sy; int pgnav_x; RECT pgtxt;
    int dragv, dragh, dragoff, selecting; int fit_pending; int last_pg; int hot; RECT seek; RECT closeb;
} PVC;
static PVC g_pc_panel, g_pc_full;
enum { PB_PREV = 1, PB_NEXT, PB_ZOUT, PB_ZIN, PB_FIT, PB_ROTL, PB_ROTR, PB_FONT, PB_FULL, PB_COPY, PB_SIZE, PB_EXTRACT, PB_OPENEXT, PB_PLAY, PB_STOP, PB_BACK10, PB_FWD10, PB_CLOSE, PB_ALL, PB_ADDFONT };

/* ---- fonts ---- */
static const WCHAR *pv_face(void) { return C.doc_font[0] ? C.doc_font : L"Segoe UI"; }
static struct { int fh, bold, mono; HFONT f; WCHAR face[64]; } g_pvf[40]; static int g_pvf_next;
static void pv_font_reset(void) { for (int i = 0; i < 40; i++) { if (g_pvf[i].f) DeleteObject(g_pvf[i].f); g_pvf[i].f = NULL; } }
static HFONT pv_font(int fh, int bold, int mono) {
    if (fh < 4) fh = 4; const WCHAR *face = mono ? (font_available(L"Consolas") ? L"Consolas" : L"Courier New") : pv_face();
    for (int i = 0; i < 40; i++) if (g_pvf[i].f && g_pvf[i].fh == fh && g_pvf[i].bold == bold && g_pvf[i].mono == mono && !wcscmp(g_pvf[i].face, face)) return g_pvf[i].f;
    int k = g_pvf_next; g_pvf_next = (g_pvf_next + 1) % 40; if (g_pvf[k].f) DeleteObject(g_pvf[k].f);
    g_pvf[k].fh = fh; g_pvf[k].bold = bold; g_pvf[k].mono = mono; scpy(g_pvf[k].face, 64, face); g_pvf[k].f = make_font(face, fh, bold ? 700 : 400); return g_pvf[k].f;
}
static HDC g_pvdc;
static HDC pv_mdc(void) { if (!g_pvdc) g_pvdc = CreateCompatibleDC(NULL); return g_pvdc; }
static int pv_tw(const WCHAR *s, int n, int fh, int bold, int mono) { HDC dc = pv_mdc(); HGDIOBJ o = SelectObject(dc, pv_font(fh, bold, mono)); SIZE z; GetTextExtentPoint32W(dc, s, n, &z); SelectObject(dc, o); return z.cx; }

/* ---- page building ---- */
static PPage *pv_newpage(int w, int h, u32 bg, const WCHAR *title) {
    PV *v = &g_pv; if (v->npg == v->cap) { v->cap = v->cap ? v->cap * 2 : 8; v->pg = (PPage *)realloc(v->pg, v->cap * sizeof(PPage)); }
    PPage *p = &v->pg[v->npg++]; memset(p, 0, sizeof *p); p->w = w; p->h = h; p->bg = bg; scpy(p->title, 80, title ? title : L""); return p;
}
static PRun *pv_run(PPage *p, int kind, int x, int y, int w, int h, int fh, int bold, int mono, u32 col, const WCHAR *t, int len, int sep) {
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 128; p->r = (PRun *)realloc(p->r, p->cap * sizeof(PRun)); }
    PRun *r = &p->r[p->n++]; memset(r, 0, sizeof *r); r->kind = (u8)kind; r->x = x; r->y = y; r->w = w; r->h = h; r->fh = fh; r->bold = (u8)bold; r->mono = (u8)mono; r->col = col; r->sep = (u8)sep;
    if (kind == 0 && t) { r->t = (WCHAR *)malloc((len + 1) * sizeof(WCHAR)); memcpy(r->t, t, len * sizeof(WCHAR)); r->t[len] = 0; r->len = len; } return r;
}
static void pv_free(void) {
    PV *v = &g_pv; for (int i = 0; i < v->npg; i++) { for (int k = 0; k < v->pg[i].n; k++) free(v->pg[i].r[k].t); free(v->pg[i].r); if (v->pg[i].img) DeleteObject(v->pg[i].img); }
    free(v->pg); v->pg = NULL; v->npg = v->cap = v->cur = 0; v->havesel = 0; v->err[0] = 0; v->info[0] = 0; v->canext = 0;
}
/* wrap text into lines inside [x, x+maxw]; returns y after last line. sep_first = separator before first run */
static int pv_wrap(PPage *p, const WCHAR *s, int n, int x, int y, int maxw, int fh, int bold, int mono, u32 col, int lineh, int sep_first, int *maxx) {
    HDC dc = pv_mdc(); HGDIOBJ o = SelectObject(dc, pv_font(fh, bold, mono)); int pos = 0, first = 1;
    if (maxw < 40) maxw = 40;
    while (pos <= n) {
        int e = pos; while (e < n && s[e] != '\n') e++;
        int ps = pos, pl = e - pos;
        if (pl == 0) { y += lineh; first = 0; }
        while (pl > 0) {
            int fit = 0; SIZE z; GetTextExtentExPointW(dc, s + ps, pl, maxw, &fit, NULL, &z);
            int take = pl; if (fit < pl) { take = fit < 1 ? 1 : fit; int b = take; while (b > 1 && s[ps + b - 1] != ' ') b--; if (b > 1) take = b; }
            int tl = take; while (tl > 0 && s[ps + tl - 1] == ' ' && tl < pl) tl--;
            GetTextExtentPoint32W(dc, s + ps, tl, &z); if (maxx && x + z.cx > *maxx) *maxx = x + z.cx;
            pv_run(p, 0, x, y, z.cx + 2, lineh, fh, bold, mono, col, s + ps, tl, first ? sep_first : 0); first = 0; y += lineh; ps += take; pl -= take;
            while (pl > 0 && s[ps] == ' ') { ps++; pl--; }
        }
        pos = e + 1; if (e >= n) break;
    }
    SelectObject(dc, o); return y;
}
static WCHAR *pv_u8(const char *s, int *outn) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0); WCHAR *w = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)); MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); w[n] = 0; if (outn) *outn = n > 0 ? n - 1 : 0; return w;
}
/* plain text -> one tall page */
static void pv_build_text(const WCHAR *text) {
    int W = 760, fh = 15, lh = 20, m = 14, y = m; PPage *p = pv_newpage(W + 2 * m, 100, 0, L"Text"); int len = lstrlenW(text), pos = 0, lines = 0;
    u32 col = 0xFFFFFFFF;   /* theme text colour marker */
    while (pos < len && lines < 12000) {
        int e = pos; while (e < len && text[e] != '\n') e++; int l = e - pos; if (l && text[pos + l - 1] == '\r') l--;
        if (l > 4000) l = 4000; WCHAR *tmp = (WCHAR *)malloc((l + 1) * sizeof(WCHAR)); for (int i = 0; i < l; i++) tmp[i] = text[pos + i] == '\t' ? ' ' : text[pos + i]; tmp[l] = 0;
        int y0 = y; y = pv_wrap(p, tmp, l, m, y, W, fh, 0, 1, col, lh, 0, NULL); lines += (y - y0) / lh; free(tmp); pos = e + 1;
    }
    p->h = y + m; g_pv.kind = PK_TEXT;
}
/* Office / PDF document -> pages */
static int pv_is_num(const char *s) { int dig = 0; for (; *s; s++) { if (*s >= '0' && *s <= '9') dig++; else if (*s != '.' && *s != '-' && *s != ',' && *s != '%' && *s != ' ' && *s != '$') return 0; } return dig > 0; }
static void pv_build_flow(Doc *d) {
    enum { PW = 816, PH = 1056, M = 64 }; int tw = PW - 2 * M; u32 ink = 0x202020; PPage *p = NULL; int y = 0, pn = 0; WCHAR t[40];
    for (int di = 0; di < d->npg; di++) {
        DPage *dp = &d->pg[di]; wfmt(t, 40, L"Page %d", pn + 1); p = pv_newpage(PW, PH, 0xFFFFFF, t); pn++; y = M;
        for (int bi = 0; bi < dp->n; bi++) {
            DBlock *b = &dp->b[bi]; int n; WCHAR *w = pv_u8(b->text, &n); int fh = 16, bold = b->bold, lh = 23, x = M, sep = 0;
            if (b->kind == B_H1) { fh = 30; lh = 40; bold = 1; } else if (b->kind == B_H2) { fh = 24; lh = 32; bold = 1; } else if (b->kind == B_H3) { fh = 20; lh = 28; bold = 1; }
            if (b->kind == B_ROW) {
                int nc = 1; for (int i = 0; i < n; i++) if (w[i] == '\t') nc++; int cw = tw / nc, st = 0, rowh = lh + 8, maxy = 0;
                if (y + rowh > PH - M && p->n) { wfmt(t, 40, L"Page %d", pn + 1); p = pv_newpage(PW, PH, 0xFFFFFF, t); pn++; y = M; }
                for (int c = 0; c < nc; c++) { int e = st; while (e < n && w[e] != '\t') e++; int cy = pv_wrap(p, w + st, e - st, M + c * cw + 6, y + 4, cw - 12, 15, 0, 0, ink, 21, c ? 1 : 0, NULL); if (cy > maxy) maxy = cy; st = e + 1; }
                rowh = maxy - y + 4; for (int c = 0; c < nc; c++) pv_run(p, 2, M + c * cw, y, cw, rowh, 0, 0, 0, 0xB0B0B0, NULL, 0, 0);
                y += rowh; free(w); continue;
            }
            if (b->kind == B_LIST) { x = M + 24; WCHAR *w2 = (WCHAR *)malloc((n + 4) * sizeof(WCHAR)); w2[0] = 0x2022; w2[1] = ' '; memcpy(w2 + 2, w, (n + 1) * sizeof(WCHAR)); free(w); w = w2; n += 2; x = M + 8; }
            int need = lh * (n / 90 + 1); if (y + (n ? lh : lh / 2) > PH - M && p->n) { wfmt(t, 40, L"Page %d", pn + 1); p = pv_newpage(PW, PH, 0xFFFFFF, t); pn++; y = M; }
            (void)need; if (!n) { y += lh / 2; free(w); continue; }
            int maxp = PH - M;
            /* wrap line by line so long paragraphs can flow onto the next page */
            int pos = 0; while (pos < n) {
                int fit = 0; SIZE z; HDC dc = pv_mdc(); HGDIOBJ o = SelectObject(dc, pv_font(fh, bold, 0)); int e = pos; while (e < n && w[e] != '\n') e++;
                int pl = e - pos; if (pl == 0) { y += lh; pos = e + 1; SelectObject(dc, o); continue; }
                GetTextExtentExPointW(dc, w + pos, pl, tw - (x - M), &fit, NULL, &z); SelectObject(dc, o);
                int take = pl; if (fit < pl) { take = fit < 1 ? 1 : fit; int bb = take; while (bb > 1 && w[pos + bb - 1] != ' ') bb--; if (bb > 1) take = bb; }
                if (y + lh > maxp && p->n) { wfmt(t, 40, L"Page %d", pn + 1); p = pv_newpage(PW, PH, 0xFFFFFF, t); pn++; y = M; }
                int tl = take; while (tl > 0 && w[pos + tl - 1] == ' ') tl--; int wd = pv_tw(w + pos, tl, fh, bold, 0);
                pv_run(p, 0, x, y, wd + 2, lh, fh, bold, 0, ink, w + pos, tl, sep); sep = 0; y += lh; pos += take; if (pos < n && w[pos] == '\n') pos++; while (pos < n && w[pos] == ' ') pos++;
                if (take == pl && e < n) { pos = e + 1; }
            }
            sep = 0; free(w);
        }
    }
    g_pv.kind = PK_DOC;
}
static void pv_build_sheets(Doc *d) {
    u32 ink = 0x202020, grid = 0xD0D0D0, hdrc = 0xEEEEEE; const int RH = 24, RHW = 46;
    for (int di = 0; di < d->npg; di++) {
        DPage *dp = &d->pg[di]; int maxr = 0, nc = dp->ncols < 1 ? 1 : dp->ncols; if (nc > 200) nc = 200;
        for (int i = 0; i < dp->n; i++) if (dp->b[i].row > maxr) maxr = dp->b[i].row; if (maxr > 20000) maxr = 20000;
        int *cw = (int *)calloc(nc, sizeof(int)); for (int c = 0; c < nc; c++) cw[c] = 64;
        WCHAR **txt = (WCHAR **)calloc(dp->n, sizeof(WCHAR *));
        for (int i = 0; i < dp->n && i < 40000; i++) { DBlock *b = &dp->b[i]; if (b->col >= nc || b->row > 20000) continue; int n; txt[i] = pv_u8(b->text, &n); if (n > 200) { txt[i][200] = 0; n = 200; }
            if (b->row < 400) { int w = pv_tw(txt[i], n, 14, 0, 0) + 16; if (w > cw[b->col]) cw[b->col] = w > 300 ? 300 : w; } }
        int W = RHW; for (int c = 0; c < nc; c++) W += cw[c]; int H = RH * (maxr + 2);
        PPage *p = pv_newpage(W + 1, H + 1, 0xFFFFFF, L""); scpy(p->title, 80, L""); { int n; WCHAR *t = pv_u8(dp->title, &n); scpy(p->title, 80, t); free(t); }
        pv_run(p, 1, 0, 0, W, RH, 0, 0, 0, hdrc, NULL, 0, 0); pv_run(p, 1, 0, 0, RHW, H, 0, 0, 0, hdrc, NULL, 0, 0);
        int x = RHW; for (int c = 0; c < nc; c++) { WCHAR l[8]; int ci = c; int k = 0; WCHAR tmp[8]; do { tmp[k++] = (WCHAR)('A' + ci % 26); ci = ci / 26 - 1; } while (ci >= 0 && k < 4); for (int j = 0; j < k; j++) l[j] = tmp[k - 1 - j]; l[k] = 0;
            pv_run(p, 0, x + cw[c] / 2 - 5 * k, 3, cw[c], RH - 4, 14, 1, 0, 0x606060, l, k, 1); pv_run(p, 1, x, 0, 1, H, 0, 0, 0, grid, NULL, 0, 0); x += cw[c]; }
        pv_run(p, 1, W, 0, 1, H, 0, 0, 0, grid, NULL, 0, 0);
        for (int r = 0; r <= maxr + 1; r++) { pv_run(p, 1, 0, (r + 1) * RH, W, 1, 0, 0, 0, grid, NULL, 0, 0); if (r <= maxr) { WCHAR l[12]; wfmt(l, 12, L"%d", r + 1); pv_run(p, 0, 6, (r + 1) * RH + 3, RHW - 8, RH - 4, 14, 0, 0, 0x606060, l, lstrlenW(l), 0); } }
        int *cx = (int *)malloc(nc * sizeof(int)); x = RHW; for (int c = 0; c < nc; c++) { cx[c] = x; x += cw[c]; }
        for (int i = 0; i < dp->n && i < 40000; i++) { DBlock *b = &dp->b[i]; if (!txt[i]) continue; int n = lstrlenW(txt[i]); int tx = cx[b->col] + 6, w = cw[b->col] - 8;
            if (pv_is_num(b->text)) { int tw2 = pv_tw(txt[i], n, 14, 0, 0); if (tw2 < w) tx = cx[b->col] + cw[b->col] - tw2 - 6; }
            pv_run(p, 0, tx, (b->row + 1) * RH + 3, w, RH - 4, 14, 0, 0, ink, txt[i], n, b->col == 0 ? 0 : 1); free(txt[i]); }
        free(txt); free(cx); free(cw);
    }
    g_pv.kind = PK_DOC;
}
static void pv_build_slides(Doc *d) {
    const double SWd = 960.0; u32 ink = 0x202020;
    for (int di = 0; di < d->npg; di++) {
        DPage *dp = &d->pg[di]; double k = dp->pw > 1 ? SWd / dp->pw : 1.0; int H = (int)(dp->ph * k); if (H < 200) H = 540;
        int n; WCHAR *t = pv_u8(dp->title, &n); PPage *p = pv_newpage((int)SWd, H, 0xFFFFFF, t); free(t); int cy = 56, nimg = 0;
        for (int bi = 0; bi < dp->n; bi++) {
            DBlock *b = &dp->b[bi]; if (b->kind == B_IMG) { int bx = 560 + nimg * 30, by = H - 160 - nimg * 20; pv_run(p, 2, bx, by, 200, 120, 0, 0, 0, 0xA0A0A0, NULL, 0, 0); pv_run(p, 0, bx + 60, by + 48, 100, 24, 16, 0, 0, 0x808080, L"[picture]", 9, 0); nimg++; continue; }
            WCHAR *w = pv_u8(b->text, &n); int title = b->bold || b->size > 0; int fh = title ? 40 : 24, lh = fh + (title ? 10 : 8);
            int x = 48, y = cy, mw = 864;
            if (b->x >= 0) { x = (int)(b->x * k); y = (int)(b->y * k); mw = b->w > 20 ? (int)(b->w * k) : 864; if (mw < 100) mw = 100; if (mw > (int)SWd - x - 10) mw = (int)SWd - x - 10; if (mw < 100) mw = 100; if (b->w > 0 && b->h > 0 && b->h * k < 60) fh = 20, lh = 26; }
            int ny = pv_wrap(p, w, n, x, y, mw, fh, title, 0, ink, lh, 0, NULL); if (b->x < 0) cy = ny + 16; free(w);
            if (ny > H) { /* grow slide if text overflows */ p->h = ny + 20; }
        }
    }
    g_pv.kind = PK_DOC;
}
static void pv_build_pdf(Doc *d) {
    u32 ink = 0x202020;
    for (int di = 0; di < d->npg; di++) {
        DPage *dp = &d->pg[di]; double k = dp->pw > 1 ? 816.0 / dp->pw : 1.0; int n; WCHAR *t = pv_u8(dp->title, &n); PPage *p = pv_newpage(816, (int)(dp->ph * k), 0xFFFFFF, t); free(t);
        for (int bi = 0; bi < dp->n; bi++) { DBlock *b = &dp->b[bi]; int fh = (int)(b->size * k); if (fh < 8) fh = 8; if (fh > 60) fh = 60; WCHAR *w = pv_u8(b->text, &n); int wd = pv_tw(w, n, fh, 0, 0);
            pv_run(p, 0, (int)(b->x * k), (int)(b->y * k), wd + 2, fh + 4, fh, 0, 0, ink, w, n, 0); free(w); }
    }
    g_pv.kind = PK_DOC;
}
static void pv_build_arc(FArc *a, const WCHAR *name) {
    int W = 760, y = 14; PPage *p = pv_newpage(W + 28, 100, 0, L"Archive"); u64 tot = 0; int nf = 0, nd = 0; WCHAR b[300];
    for (int i = 0; i < a->n; i++) { if (a->e[i].isdir) nd++; else { nf++; tot += a->e[i].size; } }
    WCHAR sz[40]; fmt_bytes(tot, sz, 40); wfmt(b, 300, L"%ls  -  %d files, %d folders, %ls unpacked", name, nf, nd, sz);
    y = pv_wrap(p, b, lstrlenW(b), 14, y, W, 15, 1, 0, 0xFFFFFFFF, 22, 0, NULL) + 8; pv_run(p, 1, 14, y - 4, W, 1, 0, 0, 0, 0x808080, NULL, 0, 0);
    for (int i = 0; i < a->n && i < 6000; i++) {
        int n; WCHAR *w = pv_u8(a->e[i].name, &n); int depth = 0; for (int k = 0; k < n; k++) if (w[k] == '/' && k < n - 1) depth++;
        if (n && w[n - 1] == '/') w[--n] = 0; const WCHAR *bn = wcsrchr(w, '/'); bn = bn ? bn + 1 : w; int bl = lstrlenW(bn); if (depth > 8) depth = 8;
        WCHAR line[330]; wfmt(line, 330, L"%ls%ls", a->e[i].isdir ? L"\x25B8 " : L"   ", bn); (void)bl;
        pv_run(p, 0, 14 + depth * 16, y, W - 160 - depth * 16, 20, 14, a->e[i].isdir, 0, 0xFFFFFFFF, line, lstrlenW(line), 0);
        if (!a->e[i].isdir) { fmt_bytes(a->e[i].size, sz, 40); pv_run(p, 0, W - 110, y, 120, 20, 14, 0, 0, 0xFFFFFFFE, sz, lstrlenW(sz), 1); }
        free(w); y += 21;
    }
    if (a->n > 6000) { pv_run(p, 0, 14, y, W, 20, 14, 0, 0, 0xFFFFFFFE, L"... (list truncated)", 20, 0); y += 21; }
    p->h = y + 14; g_pv.kind = PK_ARCHIVE;
}
/* ---- image page ---- */
static HBITMAP gdip_load_dib(const WCHAR *path, int maxdim, int *ow, int *oh) {
    if (!G.ok) return NULL; void *img = NULL; if (G.FromFile(path, &img) || !img) return NULL; UINT W = 0, H = 0; G.W(img, &W); G.H(img, &H); if (!W || !H) { G.DelI(img); return NULL; }
    double s = 1.0; if ((int)W > maxdim || (int)H > maxdim) s = (double)maxdim / (W > H ? W : H); int w = (int)(W * s), h = (int)(H * s); if (w < 1) w = 1; if (h < 1) h = 1;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL; HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0); if (!bm) { G.DelI(img); return NULL; }
    HDC m = CreateCompatibleDC(NULL); HGDIOBJ o = SelectObject(m, bm); RECT r = { 0, 0, w, h }; fillr(m, &r, RGB(255, 255, 255));
    void *g = NULL; G.FromHDC(m, &g); if (g) { G.Interp(g, 7); G.Draw(g, img, 0, 0, w, h); G.DelG(g); } SelectObject(m, o); DeleteDC(m); G.DelI(img); *ow = w; *oh = h; return bm;
}
static void pv_build_image(const WCHAR *path) {
    int w, h; HBITMAP b = gdip_load_dib(path, 3200, &w, &h);
    if (!b) { g_pv.kind = PK_ERROR; scpy(g_pv.err, 220, L"This image format cannot be decoded here"); return; }
    PPage *p = pv_newpage(w, h, 0, L"Image"); p->img = b; p->iw = w; p->ih = h; g_pv.kind = PK_IMAGE;
}
/* rotate a 32bpp DIB: rot 1=90cw 2=180 3=270cw. returns new DIB section */
static HBITMAP pv_rotate_bmp(HBITMAP src, int sw, int sh, int rot, int *ow, int *oh) {
    int dw = (rot & 1) ? sh : sw, dh = (rot & 1) ? sw : sh; BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = sw; bi.bmiHeader.biHeight = -sh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    u32 *in = (u32 *)malloc((size_t)sw * sh * 4); HDC sdc = GetDC(NULL); GetDIBits(sdc, src, 0, sh, in, &bi, DIB_RGB_COLORS);
    BITMAPINFO bo = bi; bo.bmiHeader.biWidth = dw; bo.bmiHeader.biHeight = -dh; void *bits = NULL; HBITMAP out = CreateDIBSection(sdc, &bo, DIB_RGB_COLORS, &bits, NULL, 0); ReleaseDC(NULL, sdc);
    if (out && bits) { u32 *o = (u32 *)bits;
        for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) { u32 v = in[(size_t)y * sw + x]; int nx, ny;
            if (rot == 1) { nx = sh - 1 - y; ny = x; } else if (rot == 2) { nx = sw - 1 - x; ny = sh - 1 - y; } else { nx = y; ny = sw - 1 - x; } o[(size_t)ny * dw + nx] = v; } }
    free(in); *ow = dw; *oh = dh; return out;
}

/* ---- geometry ---- */
static PPage *pv_page(void) { PV *v = &g_pv; if (v->cur < 0) v->cur = 0; if (v->cur >= v->npg) v->cur = v->npg - 1; return v->npg ? &v->pg[v->cur] : NULL; }
static void pv_dims(PVC *c, int *w, int *h) {
    PPage *p = pv_page(); if (!p) { *w = *h = 0; return; } int pw = p->w * c->zoom / 100, ph = p->h * c->zoom / 100; if (c->rot & 1) { *w = ph; *h = pw; } else { *w = pw; *h = ph; }
}
static void pv_clamp(PVC *c) {
    int w, h; pv_dims(c, &w, &h); int m = px(10), vw = c->view.right - c->view.left, vh = c->view.bottom - c->view.top;
    int mx = w + 2 * m - vw, my = h + 2 * m - vh; if (mx < 0) mx = 0; if (my < 0) my = 0; if (c->sx > mx) c->sx = mx; if (c->sy > my) c->sy = my; if (c->sx < 0) c->sx = 0; if (c->sy < 0) c->sy = 0;
}
static void pv_fit(PVC *c) {
    PPage *p = pv_page(); if (!p || !p->w) return; int vw = c->view.right - c->view.left - 2 * px(10) - px(12), vh = c->view.bottom - c->view.top - 2 * px(10);
    int pw = (c->rot & 1) ? p->h : p->w; int z = vw * 100 / (pw ? pw : 1); if (g_pv.kind == PK_IMAGE) { int ph = (c->rot & 1) ? p->w : p->h; int z2 = vh * 100 / (ph ? ph : 1); if (z2 < z) z = z2; }
    if (z < 25) z = 25; if (z > 400) z = 400; c->zoom = z; c->sx = c->sy = 0; pv_clamp(c);
}
static void pv_set_zoom(PVC *c, int z, int cx, int cy) {
    if (z < 25) z = 25; if (z > 400) z = 400; if (z == c->zoom) return; int oz = c->zoom;
    int ax = c->sx + cx - c->view.left, ay = c->sy + cy - c->view.top; c->zoom = z; c->sx = ax * z / oz - (cx - c->view.left); c->sy = ay * z / oz - (cy - c->view.top); pv_clamp(c);
}
static void pv_goto(PVC *c, int pg) { PV *v = &g_pv; if (pg < 0) pg = 0; if (pg >= v->npg) pg = v->npg - 1; if (pg == v->cur) return; v->cur = pg; v->havesel = 0; c->sx = c->sy = 0; if (g_pc_panel.last_pg != pg) g_pc_panel.last_pg = pg; pv_clamp(c); }

/* ---- layout of the preview widgets ---- */
static void pv_glyph(HDC dc, int id, const RECT *r, COLORREF c) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = (r->bottom - r->top) / 4; if (s < 3) s = 3; POINT t[3];
    switch (id) {
    case PB_PREV: t[0] = (POINT){ cx + s / 2, cy - s }; t[1] = (POINT){ cx + s / 2, cy + s }; t[2] = (POINT){ cx - s, cy }; poly(dc, t, 3, c); break;
    case PB_NEXT: t[0] = (POINT){ cx - s / 2, cy - s }; t[1] = (POINT){ cx - s / 2, cy + s }; t[2] = (POINT){ cx + s, cy }; poly(dc, t, 3, c); break;
    case PB_ZOUT: linec(dc, cx - s, cy, cx + s + 1, cy, c, 2); break;
    case PB_ZIN: linec(dc, cx - s, cy, cx + s + 1, cy, c, 2); linec(dc, cx, cy - s, cx, cy + s + 1, c, 2); break;
    case PB_FIT: { int a = s + 1; linec(dc, cx - a, cy - a, cx - a + 4, cy - a, c, 2); linec(dc, cx - a, cy - a, cx - a, cy - a + 4, c, 2); linec(dc, cx + a, cy - a, cx + a - 4, cy - a, c, 2); linec(dc, cx + a, cy - a, cx + a, cy - a + 4, c, 2);
        linec(dc, cx - a, cy + a, cx - a + 4, cy + a, c, 2); linec(dc, cx - a, cy + a, cx - a, cy + a - 4, c, 2); linec(dc, cx + a, cy + a, cx + a - 4, cy + a, c, 2); linec(dc, cx + a, cy + a, cx + a, cy + a - 4, c, 2); break; }
    case PB_ROTR: case PB_ROTL: { HPEN pn = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, pn); Arc(dc, cx - s, cy - s, cx + s, cy + s, cx + s, cy, cx, cy - s); SelectObject(dc, o); DeleteObject(pn);
        if (id == PB_ROTR) { t[0] = (POINT){ cx + s - 3, cy - s + 1 }; t[1] = (POINT){ cx + s + 3, cy - s + 1 }; t[2] = (POINT){ cx + s, cy - s + 5 }; } else { t[0] = (POINT){ cx - s - 3, cy - s + 1 }; t[1] = (POINT){ cx - s + 3, cy - s + 1 }; t[2] = (POINT){ cx - s, cy - s + 5 }; } poly(dc, t, 3, c); break; }
    case PB_FONT: { RECT q = *r; SelectObject(dc, S.font_small); textr(dc, L"Aa", &q, TC, c); SelectObject(dc, S.font); break; }
    case PB_FULL: { int a = s + 1; linec(dc, cx - a, cy - a, cx - a + 5, cy - a, c, 2); linec(dc, cx - a, cy - a, cx - a, cy - a + 5, c, 2); linec(dc, cx + a, cy + a, cx + a - 5, cy + a, c, 2); linec(dc, cx + a, cy + a, cx + a, cy + a - 5, c, 2); linec(dc, cx - a, cy - a, cx + a, cy + a, c, 1); break; }
    case PB_COPY: { RECT a = { cx - s, cy - s + 1, cx + s - 2, cy + s + 1 }, b = { cx - s + 3, cy - s - 2, cx + s + 1, cy + s - 2 }; framer(dc, &a, c); framer(dc, &b, c); break; }
    case PB_SIZE: { RECT a = { cx - s - 1, cy - s, cx + s + 2, cy + s + 1 }; framer(dc, &a, c); fill4(dc, cx - s - 1, cy - s, cx + s + 2, cy - s + 2, c); break; }
    case PB_EXTRACT: { linec(dc, cx, cy - s, cx, cy + s - 2, c, 2); t[0] = (POINT){ cx - s + 1, cy }; t[1] = (POINT){ cx + s - 1, cy }; t[2] = (POINT){ cx, cy + s }; poly(dc, t, 3, c); fill4(dc, cx - s - 1, cy + s + 1, cx + s + 2, cy + s + 3, c); break; }
    case PB_OPENEXT: { RECT a = { cx - s, cy - s + 2, cx + s - 1, cy + s + 1 }; framer(dc, &a, c); linec(dc, cx, cy, cx + s + 2, cy - s - 2, c, 2); break; }
    case PB_PLAY: t[0] = (POINT){ cx - s / 2, cy - s }; t[1] = (POINT){ cx - s / 2, cy + s }; t[2] = (POINT){ cx + s, cy }; poly(dc, t, 3, c); break;
    case PB_STOP: { RECT a = { cx - s + 1, cy - s + 1, cx + s, cy + s }; fillr(dc, &a, c); break; }
    case PB_BACK10: t[0] = (POINT){ cx - 1, cy - s }; t[1] = (POINT){ cx - 1, cy + s }; t[2] = (POINT){ cx - s - 1, cy }; poly(dc, t, 3, c); t[0].x += s; t[1].x += s; t[2].x += s; poly(dc, t, 3, c); break;
    case PB_FWD10: t[0] = (POINT){ cx + 1, cy - s }; t[1] = (POINT){ cx + 1, cy + s }; t[2] = (POINT){ cx + s + 1, cy }; poly(dc, t, 3, c); t[0].x -= s; t[1].x -= s; t[2].x -= s; poly(dc, t, 3, c); break;
    case PB_CLOSE: linec(dc, cx - s, cy - s, cx + s + 1, cy + s + 1, c, 2); linec(dc, cx - s, cy + s, cx + s + 1, cy - s - 1, c, 2); break;
    case PB_ALL: { RECT a = { cx - s, cy - s, cx + s, cy + s }; framer(dc, &a, c); linec(dc, cx - s + 2, cy, cx + s - 2, cy, c, 1); break; }
    case PB_ADDFONT: linec(dc, cx - s, cy, cx + s + 1, cy, c, 2); linec(dc, cx, cy - s, cx, cy + s + 1, c, 2); break;
    }
}
static const WCHAR *pv_btn_tip(int id) {
    switch (id) { case PB_PREV: return L"Previous page"; case PB_NEXT: return L"Next page"; case PB_ZOUT: return L"Zoom out"; case PB_ZIN: return L"Zoom in"; case PB_FIT: return L"Fit to width"; case PB_ROTL: return L"Rotate left"; case PB_ROTR: return L"Rotate right";
    case PB_FONT: return L"Document font"; case PB_FULL: return L"Full screen preview"; case PB_COPY: return L"Copy selected text (or whole page)"; case PB_SIZE: return L"Preview size: min / mid / large"; case PB_EXTRACT: return L"Extract archive"; case PB_OPENEXT: return L"Open with default app";
    case PB_PLAY: return L"Play / pause"; case PB_STOP: return L"Stop"; case PB_BACK10: return L"Back 10 s"; case PB_FWD10: return L"Forward 10 s"; case PB_CLOSE: return L"Close"; case PB_ALL: return L"Select all text"; } return L"";
}
static int vid_is_video(void);
static void pv_layout(PVC *c, RECT area, int full) {
    c->area = area; c->full = full; c->nb = 0; int pad = full ? px(14) : px(10), bs = px(full ? 34 : 28), gap = px(4);
    int hh = full ? px(44) : px(54); c->hdr = (RECT){ area.left + pad, area.top + px(8), area.right - pad - (full ? px(50) : 0), area.top + px(8) + hh };
    int tx = area.left + pad, ty = c->hdr.bottom + px(2); int maxx = area.right - pad - (full ? px(56) : 0);
    PV *v = &g_pv; int ids[20], n = 0;
    if (v->kind == PK_DOC || v->kind == PK_IMAGE) { if (v->npg > 1) { ids[n++] = PB_PREV; ids[n++] = PB_NEXT; } ids[n++] = PB_ZOUT; ids[n++] = PB_ZIN; ids[n++] = PB_FIT; ids[n++] = PB_ROTL; ids[n++] = PB_ROTR; if (v->kind == PK_DOC) { ids[n++] = PB_FONT; ids[n++] = PB_COPY; } }
    else if (v->kind == PK_TEXT || v->kind == PK_ARCHIVE) { ids[n++] = PB_ZOUT; ids[n++] = PB_ZIN; ids[n++] = PB_FIT; ids[n++] = PB_FONT; ids[n++] = PB_COPY; if (v->kind == PK_ARCHIVE) ids[n++] = PB_EXTRACT; }
    else if (v->kind == PK_MEDIA) { ids[n++] = PB_OPENEXT; }
    if (v->kind != PK_NONE && v->kind != PK_INFO && !full) ids[n++] = PB_SIZE; if (v->kind != PK_NONE && !full && (v->kind == PK_INFO)) { n = 0; }
    if (!full && v->kind >= PK_TEXT && v->kind != PK_LOADING && v->kind != PK_ERROR) ids[n++] = PB_FULL;
    int x = tx, y = ty, rows = 1;
    int pgw = (v->npg > 1 && (v->kind == PK_DOC || v->kind == PK_IMAGE)) ? px(64) : 0;
    for (int i = 0; i < n; i++) {
        int w = bs; if (ids[i] == PB_NEXT) w = bs; if (x + w > maxx && x > tx) { x = tx; y += bs + gap; rows++; }
        PBtn *b = &c->b[c->nb++]; b->id = ids[i]; b->r = (RECT){ x, y, x + w, y + bs }; b->en = 1; b->on = 0; x += w + gap;
        if (ids[i] == PB_PREV && pgw) { c->pgtxt = (RECT){ x, y, x + pgw, y + bs }; x += pgw + gap; }
        if (c->nb >= 18) break;
    }
    c->tb = (RECT){ tx, ty, maxx, n ? y + bs : ty };
    int top = c->tb.bottom + (n ? px(6) : px(4)); int bot = area.bottom - pad; c->mc = (RECT){ 0, 0, 0, 0 };
    if (v->kind == PK_MEDIA) { int mh = px(full ? 84 : 76); c->mc = (RECT){ area.left + pad, bot - mh, area.right - pad, bot }; bot -= mh + px(6); }
    c->body = (RECT){ area.left + pad, top, area.right - pad, bot }; if (c->body.bottom < c->body.top + 20) c->body.bottom = c->body.top + 20;
    int sbw = px(12); int w, h; pv_dims(c, &w, &h); int m = px(10); int vw = c->body.right - c->body.left, vh = c->body.bottom - c->body.top;
    int needv = h + 2 * m > vh, needh = w + 2 * m > vw - (needv ? sbw : 0); if (needh) needv = h + 2 * m > vh - sbw;
    c->view = c->body; if (needv) c->view.right -= sbw; if (needh) c->view.bottom -= sbw;
    c->vsb = needv ? (RECT){ c->view.right, c->view.top, c->body.right, c->view.bottom } : (RECT){ 0, 0, 0, 0 }; c->hsb = needh ? (RECT){ c->view.left, c->view.bottom, c->view.right, c->body.bottom } : (RECT){ 0, 0, 0, 0 };
    c->closeb = full ? (RECT){ area.right - px(54), area.top + px(8), area.right - px(10), area.top + px(8) + px(40) } : (RECT){ 0, 0, 0, 0 };
    if (c->fit_pending && v->npg && (v->kind == PK_DOC || v->kind == PK_IMAGE)) { c->fit_pending = 0; pv_fit(c); }
    pv_clamp(c);
}
/* ---- media (MCI via winmm, loaded dynamically) ---- */
static struct { int open, playing, video; HWND host; WCHAR path[MAX_PATH]; int len; DWORD pos; int err; WCHAR errmsg[160]; } g_vid;
static DWORD (WINAPI *p_mci)(LPCWSTR, LPWSTR, UINT, HWND); static BOOL (WINAPI *p_mcierr)(DWORD, LPWSTR, UINT);
static int mci_init(void) {
    if (p_mci) return 1; HMODULE m = LoadLibraryW(L"winmm.dll"); if (!m) return 0; *(FARPROC *)&p_mci = GetProcAddress(m, "mciSendStringW"); *(FARPROC *)&p_mcierr = GetProcAddress(m, "mciGetErrorStringW"); return p_mci != NULL;
}
static int mci_cmd(const WCHAR *cmd, WCHAR *ret, int rc) {
    if (!mci_init()) return -1; DWORD e = p_mci(cmd, ret, ret ? rc : 0, NULL); if (e && p_mcierr) { p_mcierr(e, g_vid.errmsg, 160); } return (int)e;
}
static int vid_is_video(void) { return g_vid.video; }
static LRESULT CALLBACK vidhost_proc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; } if (m == WM_LBUTTONDOWN) { PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(PB_PLAY + 5000, 0), 0); return 0; } return DefWindowProcW(h, m, w, l); }
static void vid_close(void) {
    if (g_vid.open) { mci_cmd(L"stop fmvid", NULL, 0); mci_cmd(L"close fmvid", NULL, 0); g_vid.open = 0; } g_vid.playing = 0;
    if (g_vid.host) { DestroyWindow(g_vid.host); g_vid.host = NULL; } KillTimer(S.hwnd, TIMER_VID);
}
static void vid_place(const RECT *r) { if (g_vid.host && g_vid.open) { MoveWindow(g_vid.host, r->left, r->top, r->right - r->left, r->bottom - r->top, TRUE); WCHAR c[120]; wfmt(c, 120, L"put fmvid destination at 0 0 %d %d", r->right - r->left, r->bottom - r->top); if (g_vid.video) mci_cmd(c, NULL, 0); } }
static int vid_open(const WCHAR *path, HWND parent, const RECT *area, int is_video) {
    static int reg; if (!reg) { reg = 1; WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = vidhost_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"WinShellFM.VidHost"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW); RegisterClassExW(&wc); }
    vid_close(); g_vid.err = 0; g_vid.errmsg[0] = 0; g_vid.video = is_video; scpy(g_vid.path, MAX_PATH, path);
    if (is_video) g_vid.host = CreateWindowExW(0, L"WinShellFM.VidHost", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, area->left, area->top, area->right - area->left, area->bottom - area->top, parent, NULL, GetModuleHandleW(NULL), NULL);
    WCHAR cmd[MAX_PATH + 120];
    if (is_video) wfmt(cmd, MAX_PATH + 120, L"open \"%ls\" type mpegvideo alias fmvid style child parent %lu", path, (unsigned long)(ULONG_PTR)g_vid.host);
    else wfmt(cmd, MAX_PATH + 120, L"open \"%ls\" alias fmvid", path);
    int e = mci_cmd(cmd, NULL, 0);
    if (e && is_video) { wfmt(cmd, MAX_PATH + 120, L"open \"%ls\" alias fmvid style child parent %lu", path, (unsigned long)(ULONG_PTR)g_vid.host); e = mci_cmd(cmd, NULL, 0); }
    if (e) { g_vid.err = e; if (g_vid.host) { DestroyWindow(g_vid.host); g_vid.host = NULL; } return 0; }
    g_vid.open = 1; mci_cmd(L"set fmvid time format milliseconds", NULL, 0); WCHAR r[40] = L""; if (!mci_cmd(L"status fmvid length", r, 40)) g_vid.len = _wtoi(r);
    if (is_video) { vid_place(area); mci_cmd(L"seek fmvid to start", NULL, 0); mci_cmd(L"window fmvid state show", NULL, 0); }
    SetTimer(S.hwnd, TIMER_VID, 400, NULL); return 1;
}
static void vid_poll(void) {
    if (!g_vid.open) return; WCHAR r[40] = L""; if (!mci_cmd(L"status fmvid position", r, 40)) g_vid.pos = (DWORD)_wtoi(r);
    r[0] = 0; if (!mci_cmd(L"status fmvid mode", r, 40)) g_vid.playing = !wcscmp(r, L"playing");
}
static void vid_toggle(void) {
    if (!g_vid.open) return; vid_poll();
    if (g_vid.playing) mci_cmd(L"pause fmvid", NULL, 0); else { if (g_vid.len && g_vid.pos + 200 >= (DWORD)g_vid.len) mci_cmd(L"seek fmvid to start", NULL, 0); mci_cmd(L"play fmvid", NULL, 0); }
    vid_poll();
}
static void vid_seek(int ms) { if (!g_vid.open) return; if (ms < 0) ms = 0; if (g_vid.len && ms > g_vid.len) ms = g_vid.len; WCHAR c[80]; int was = g_vid.playing; wfmt(c, 80, L"seek fmvid to %d", ms); mci_cmd(c, NULL, 0); if (was) mci_cmd(L"play fmvid", NULL, 0); vid_poll(); }
static void vid_stop(void) { if (!g_vid.open) return; mci_cmd(L"stop fmvid", NULL, 0); mci_cmd(L"seek fmvid to start", NULL, 0); vid_poll(); }

/* ---- drawing ---- */
static int pv_sel_order(int *r0, int *c0, int *r1, int *c1) {
    PV *v = &g_pv; if (!v->havesel) return 0; *r0 = v->a_run; *c0 = v->a_ch; *r1 = v->b_run; *c1 = v->b_ch;
    if (*r0 > *r1 || (*r0 == *r1 && *c0 > *c1)) { int t = *r0; *r0 = *r1; *r1 = t; t = *c0; *c0 = *c1; *c1 = t; } return 1;
}
static HBITMAP g_rotcache; static int g_rc_page = -1, g_rc_rot = -1, g_rc_w, g_rc_h; static HBITMAP g_rc_src;
static void pv_draw_page(HDC dc, PVC *c, PPage *p, int ox, int oy, const RECT *clip) {
    const Pal *pl = &S.pal; int z = c->zoom; int pw = p->w * z / 100, ph = p->h * z / 100;
    COLORREF paper = p->bg ? RGBH(p->bg) : pl->bg; RECT pr = { ox, oy, ox + pw, oy + ph };
    if (p->bg) { RECT sh = pr; sh.left += 3; sh.top += 3; sh.right += 3; sh.bottom += 3; fillr(dc, &sh, mixc(pl->bg, RGB(0, 0, 0), 0.25)); }
    fillr(dc, &pr, paper); if (p->bg) framer(dc, &pr, mixc(paper, RGB(0, 0, 0), 0.3));
    int r0, c0, r1, c1, hs = pv_sel_order(&r0, &c0, &r1, &c1); (void)hs;
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < p->n; i++) {
        PRun *r = &p->r[i]; int x = ox + r->x * z / 100, y = oy + r->y * z / 100, w = r->w * z / 100, h = r->h * z / 100; if (r->kind != 0 && w < 1) w = 1; if (h < 1) h = 1;
        if (x > clip->right || x + w < clip->left - 4 || y > clip->bottom || y + h < clip->top) continue;
        COLORREF col = r->col == 0xFFFFFFFF ? pl->text : r->col == 0xFFFFFFFE ? pl->dim : RGBH(r->col);
        if (r->kind == 1) { fill4(dc, x, y, x + w, y + h, col); continue; }
        if (r->kind == 2) { RECT q = { x, y, x + w, y + h }; framer(dc, &q, col); continue; }
        HFONT f = pv_font(r->fh * z / 100, r->bold, r->mono); HGDIOBJ of = SelectObject(dc, f);
        if (g_pv.havesel && g_pc_panel.rot == 0 && c->rot == 0 && i >= r0 && i <= r1) {
            int a = i == r0 ? c0 : 0, b = i == r1 ? c1 : r->len; if (a > r->len) a = r->len; if (b > r->len) b = r->len; if (b > a || (i > r0 && i < r1)) {
                SIZE za, zb; GetTextExtentPoint32W(dc, r->t, a, &za); GetTextExtentPoint32W(dc, r->t, b, &zb); RECT hl = { x + za.cx, y, x + zb.cx + (b == r->len && i < r1 ? px(4) : 0), y + h }; fillr(dc, &hl, pl->sel); } }
        SetTextColor(dc, col); RECT cr = { x, y, x + w + px(6), y + h + 2 }; ExtTextOutW(dc, x, y + (h - r->fh * z / 100) / 2 > y ? y : y, ETO_CLIPPED, &cr, r->t, r->len, NULL); SelectObject(dc, of);
    }
}
static void pv_scrollbar(HDC dc, const RECT *sb, int vert, int pos, int content, int view) {
    const Pal *p = &S.pal; if (sb->right <= sb->left || sb->bottom <= sb->top) return; fillr(dc, sb, p->sb_track);
    int len = vert ? sb->bottom - sb->top : sb->right - sb->left, tl = (int)((double)len * view / content); if (tl < px(24)) tl = px(24); int rng = len - tl, mx = content - view; int tp = mx > 0 ? (int)((double)pos / mx * rng) : 0;
    RECT t = vert ? (RECT){ sb->left + 2, sb->top + tp, sb->right - 2, sb->top + tp + tl } : (RECT){ sb->left + tp, sb->top + 2, sb->left + tp + tl, sb->bottom - 2 }; rrect(dc, &t, p->sb_thumb, p->sb_thumb, 3, 0);
}
static void vid_draw_controls(HDC dc, PVC *c) {
    const Pal *p = &S.pal; RECT m = c->mc; if (m.right <= m.left) return; int bs = px(c->full ? 34 : 30), gap = px(4);
    int ids[5] = { PB_PLAY, PB_STOP, PB_BACK10, PB_FWD10, PB_OPENEXT }, x = m.left, y = m.bottom - bs;
    for (int i = 0; i < 5; i++) { RECT b = { x, y, x + bs, y + bs }; int hot = in_rect(&b, g_mx, g_my) && !c->full; rrect(dc, &b, hot ? p->hover : p->btn, p->btn_border, 4, 1);
        if (ids[i] == PB_PLAY && g_vid.playing) { RECT a = { b.left + bs / 3 - 1, b.top + bs / 4, b.left + bs / 3 + 3, b.bottom - bs / 4 }, a2 = { b.right - bs / 3 - 3, b.top + bs / 4, b.right - bs / 3 + 1, b.bottom - bs / 4 }; fillr(dc, &a, p->text); fillr(dc, &a2, p->text); } else pv_glyph(dc, ids[i], &b, p->text);
        x += bs + gap; }
    WCHAR t[60]; int cur = (int)g_vid.pos / 1000, tot = g_vid.len / 1000; wfmt(t, 60, L"%d:%02d / %d:%02d", cur / 60, cur % 60, tot / 60, tot % 60); RECT tr = { x + px(6), y, m.right, m.bottom }; SelectObject(dc, S.font_small); textr(dc, t, &tr, TL, p->dim);
    c->seek = (RECT){ m.left, m.top + px(6), m.right, m.top + px(22) }; RECT tk = { c->seek.left, c->seek.top + px(5), c->seek.right, c->seek.top + px(11) }; rrect(dc, &tk, p->sb_track, p->border, 3, 1);
    if (g_vid.len > 0) { int w = (int)((double)(tk.right - tk.left) * g_vid.pos / g_vid.len); RECT f = { tk.left, tk.top, tk.left + w, tk.bottom }; rrect(dc, &f, p->accent, p->accent, 3, 0); RECT kn = { tk.left + w - px(6), c->seek.top + px(1), tk.left + w + px(6), c->seek.bottom - px(1) }; ellipsec(dc, &kn, p->accent, p->accent, 1); }
    SelectObject(dc, S.font);
}
static void pv_pick_icon(HDC dc, const RECT *body);
static void pv_paint(HDC dc, PVC *c, const WCHAR *name, const WCHAR *meta) {
    const Pal *p = &S.pal; PV *v = &g_pv; SelectObject(dc, S.font); SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, S.font_big); RECT nr = c->hdr; nr.bottom = nr.top + px(c->full ? 24 : 22); textr(dc, name, &nr, TL, p->text);
    SelectObject(dc, S.font_small); RECT mr = c->hdr; mr.top = nr.bottom; textr(dc, meta, &mr, DT_WORDBREAK | DT_END_ELLIPSIS, p->dim); SelectObject(dc, S.font);
    for (int i = 0; i < c->nb; i++) { PBtn *b = &c->b[i]; int hot = in_rect(&b->r, g_mx, g_my) && !c->full; rrect(dc, &b->r, hot ? p->hover : p->btn, p->btn_border, 4, 1); pv_glyph(dc, b->id, &b->r, b->en ? p->text : p->dim); }
    if (c->pgtxt.right > c->pgtxt.left) { WCHAR t[40]; wfmt(t, 40, L"%d / %d", v->cur + 1, v->npg); textr(dc, t, &c->pgtxt, TC, p->text); }
    if (c->full) { int hot = in_rect(&c->closeb, g_fmx, g_fmy); rrect(dc, &c->closeb, hot ? RGB(200, 40, 40) : p->btn, p->btn_border, 6, 1); pv_glyph(dc, PB_CLOSE, &c->closeb, hot ? RGB(255, 255, 255) : p->text); }
    RECT body = c->body; fillr(dc, &body, mixc(p->bg, p->dark ? RGB(0, 0, 0) : RGB(128, 128, 128), p->dark ? 0.25 : 0.12));
    if (v->kind == PK_LOADING) { textr(dc, L"Loading preview...", &body, TC, p->dim); return; }
    if (v->kind == PK_ERROR) { RECT q = body; q.left += px(10); q.right -= px(10); q.top += px(14); textr(dc, v->err, &q, DT_WORDBREAK | DT_CENTER, p->dim); }
    else if (v->kind == PK_INFO || v->kind == PK_NONE) { pv_pick_icon(dc, &body); }
    else if (v->kind == PK_MEDIA) {
        if (!g_vid.open) { RECT q = body; q.left += px(10); q.right -= px(10); q.top += px(14); WCHAR t[260]; wfmt(t, 260, L"%ls", g_vid.err ? (g_vid.errmsg[0] ? g_vid.errmsg : L"This media cannot be played here (codec missing?)") : L"Preview not started"); textr(dc, t, &q, DT_WORDBREAK | DT_CENTER, p->dim); }
        else if (!g_vid.video) { RECT q = body; textr(dc, L"Audio", &q, TC, p->dim); }
        vid_draw_controls(dc, c);
    } else if (v->npg) {
        PPage *pg = pv_page(); int w, h; pv_dims(c, &w, &h); int m = px(10); int vw = c->view.right - c->view.left, vh = c->view.bottom - c->view.top;
        int ox = c->view.left + (w + 2 * m < vw ? (vw - w) / 2 : m - c->sx), oy = c->view.top + (h + 2 * m < vh ? m : m - c->sy);
        HRGN rg = CreateRectRgn(c->view.left, c->view.top, c->view.right, c->view.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
        if (pg->img || c->rot) {
            HBITMAP src = pg->img; int sw = pg->iw, sh = pg->ih; HBITMAP tmp = NULL;
            if (!src) { /* render run page offscreen at zoom for rotation */
                int zw = pg->w * c->zoom / 100, zh = pg->h * c->zoom / 100; if ((double)zw * zh > 12e6) { zw = zw / 2; zh = zh / 2; }
                BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = zw; bi.bmiHeader.biHeight = -zh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; void *bits;
                tmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0); HDC m2 = CreateCompatibleDC(dc); HGDIOBJ o2 = SelectObject(m2, tmp); PVC cc = *c; cc.zoom = zw * 100 / (pg->w ? pg->w : 1); RECT big = { 0, 0, zw, zh };
                SelectObject(m2, S.font); pv_draw_page(m2, &cc, pg, 0, 0, &big); SelectObject(m2, o2); DeleteDC(m2); src = tmp; sw = zw; sh = zh; }
            int rw = sw, rh = sh; HBITMAP rot = NULL; if (c->rot) { rot = pv_rotate_bmp(src, sw, sh, c->rot, &rw, &rh); } HBITMAP use = rot ? rot : src;
            HDC m3 = CreateCompatibleDC(dc); HGDIOBJ o3 = SelectObject(m3, use); SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL); StretchBlt(dc, ox, oy, w, h, m3, 0, 0, rw, rh, SRCCOPY); SelectObject(m3, o3); DeleteDC(m3);
            if (rot) DeleteObject(rot); if (tmp) DeleteObject(tmp);
        } else pv_draw_page(dc, c, pg, ox, oy, &c->view);
        SelectClipRgn(dc, NULL);
        pv_scrollbar(dc, &c->vsb, 1, c->sy, h + 2 * m, c->view.bottom - c->view.top); pv_scrollbar(dc, &c->hsb, 0, c->sx, w + 2 * m, c->view.right - c->view.left);
        WCHAR zt[20]; wfmt(zt, 20, L"%d%%", c->zoom); SelectObject(dc, S.font_small); RECT zr = { c->view.right - px(60) - (c->vsb.right ? 0 : 0), c->view.bottom - px(20), c->view.right - px(4), c->view.bottom - px(2) }; textr(dc, zt, &zr, TL | DT_RIGHT, p->dim); SelectObject(dc, S.font);
    }
}

/* ========================================================================== */
/* ARCHIVE OPERATIONS: file reader, extract, compress, external 7-Zip/WinRAR   */
/* ========================================================================== */
typedef struct { HANDLE h; } FRd;
static int frd_read(Rd *r, u64 off, void *buf, size_t n) {
    FRd *f = (FRd *)r->ctx; LARGE_INTEGER li; li.QuadPart = (LONGLONG)off; if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) return -1;
    u8 *p = (u8 *)buf; while (n) { DWORD c = n > (1u << 24) ? (1u << 24) : (DWORD)n, got = 0; if (!ReadFile(f->h, p, c, &got, NULL) || !got) return -1; p += got; n -= got; } return 0;
}
static int frd_open(Rd *r, FRd *f, const WCHAR *path) {
    f->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL); if (f->h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz; GetFileSizeEx(f->h, &sz); r->read = frd_read; r->size = (u64)sz.QuadPart; r->ctx = f; return 0;
}
static u8 *read_whole(const WCHAR *path, size_t *n, size_t cap) {
    Rd r; FRd f; if (frd_open(&r, &f, path)) return NULL; if (r.size > cap) { CloseHandle(f.h); return NULL; }
    u8 *b = (u8 *)malloc((size_t)r.size + 1); if (!b) { CloseHandle(f.h); return NULL; } if (r.size && frd_read(&r, 0, b, (size_t)r.size)) { free(b); CloseHandle(f.h); return NULL; }
    CloseHandle(f.h); *n = (size_t)r.size; return b;
}
static int arc_kind_of(const WCHAR *p) {
    if (ext_in(p, L".zip.jar.apk.docx.xlsx.pptx.odt.epub.")) return ARC_ZIP; if (ext_in(p, L".tar.")) return ARC_TAR; if (ext_in(p, L".tgz.")) return ARC_TGZ;
    int l = lstrlenW(p); if (l > 7 && !_wcsicmp(p + l - 7, L".tar.gz")) return ARC_TGZ; if (ext_in(p, L".gz.")) return ARC_GZ; if (ext_in(p, L".iso.")) return ARC_ISO;
    if (ext_in(p, L".7z.rar.bz2.xz.cab.lzh.arj.z.wim.")) return ARC_EXT; return ARC_NONE;
}
/* ---- external tools ---- */
static int find_tool(const WCHAR *const *names, WCHAR *out) {
    WCHAR exe[MAX_PATH], dir[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH); parent_of(exe, dir);
    static const WCHAR *pf[] = { L"%ProgramFiles%\\7-Zip", L"%ProgramFiles(x86)%\\7-Zip", L"%ProgramFiles%\\WinRAR", L"%ProgramFiles(x86)%\\WinRAR", L"C:\\Program Files\\7-Zip", L"C:\\Program Files\\WinRAR" };
    for (int i = 0; names[i]; i++) {
        path_join(out, MAX_PATH, dir, names[i]); if (path_exists(out)) return 1;
        for (int k = 0; k < 6; k++) { WCHAR b[MAX_PATH]; ExpandEnvironmentStringsW(pf[k], b, MAX_PATH); path_join(out, MAX_PATH, b, names[i]); if (path_exists(out)) return 1; }
        WCHAR f[MAX_PATH]; lstrcpyW(f, names[i]); if (PathFindOnPathW(f, NULL)) { lstrcpyW(out, f); return 1; }
    } return 0;
}
static int find_7z(WCHAR *o) { static const WCHAR *n[] = { L"7z.exe", L"7za.exe", L"7zr.exe", NULL }; return find_tool(n, o); }
static int find_rar(WCHAR *o) { static const WCHAR *n[] = { L"rar.exe", L"WinRAR.exe", NULL }; return find_tool(n, o); }
/* run a command line, capture stdout (up to cap bytes), returns exit code or -1 */
static int run_capture(WCHAR *cmdline, char **out, size_t *outn, DWORD timeout) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE }; HANDLE rd = NULL, wr = NULL; if (!CreatePipe(&rd, &wr, &sa, 0)) return -1; SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return -1; }
    CloseHandle(wr); size_t cap = 65536, n = 0; char *b = (char *)malloc(cap); DWORD got; DWORD t0 = GetTickCount();
    while (ReadFile(rd, b + n, (DWORD)(cap - n - 1), &got, NULL) && got) { n += got; if (n + 4096 > cap) { if (cap > (64u << 20)) break; cap *= 2; b = (char *)realloc(b, cap); } if (GetTickCount() - t0 > timeout) break; }
    b[n] = 0; DWORD ec = 1; WaitForSingleObject(pi.hProcess, 2000); GetExitCodeProcess(pi.hProcess, &ec); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    if (out) *out = b; else free(b); if (outn) *outn = n; return (int)ec;
}
/* ---- listing through 7-Zip ---- */
static int ext_list(const WCHAR *path, FArc *a) {
    WCHAR z[MAX_PATH]; if (!find_7z(z)) return -1; WCHAR cmd[MAX_PATH * 2 + 40]; wfmt(cmd, MAX_PATH * 2 + 40, L"\"%ls\" l -slt -ba -sccUTF-8 \"%ls\"", z, path);
    char *o; size_t n; int ec = run_capture(cmd, &o, &n, 20000); if (ec < 0) return -1; memset(a, 0, sizeof *a); a->kind = ARC_EXT; int cap = 0; char *p = o; char name[1024] = ""; u64 size = 0; int isdir = 0, have = 0;
    while (*p) { char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p); char line[1100]; if (l > 1090) l = 1090; memcpy(line, p, l); line[l] = 0; if (l && line[l - 1] == '\r') line[--l] = 0;
        if (!strncmp(line, "Path = ", 7)) { if (have) { AEnt *x = arc_push(a, &cap, name, strlen(name)); x->size = size; x->isdir = isdir; } snprintf(name, sizeof name, "%s", line + 7); size = 0; isdir = 0; have = 1; }
        else if (!strncmp(line, "Size = ", 7)) size = strtoull(line + 7, NULL, 10); else if (!strncmp(line, "Folder = +", 10)) isdir = 1;
        if (!e) break; p = e + 1; }
    if (have) { AEnt *x = arc_push(a, &cap, name, strlen(name)); x->size = size; x->isdir = isdir; } free(o); return 0;
}
/* ---- extraction ---- */
typedef struct { WCHAR src[MAX_PATH], dest[MAX_PATH]; HWND hwnd; } ExJob;
typedef struct { WCHAR out[MAX_PATH], fmt[8]; SL srcs; HWND hwnd; } CoJob;
static int ensure_dirs(WCHAR *path) {   /* creates all parents of a file path (or the dir itself if trailing '\\') */
    for (WCHAR *p = path + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryW(path, NULL); *p = '\\'; } return 1;
}
static int safe_rel(const char *n) { if (!n[0] || n[0] == '/' || (n[1] == ':')) return 0; for (const char *p = n; *p; p++) if (p[0] == '.' && p[1] == '.' && (p == n || p[-1] == '/') && (p[2] == '/' || !p[2])) return 0; return 1; }
static int write_file_w(const WCHAR *p, const u8 *d, size_t n) {
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h == INVALID_HANDLE_VALUE) return -1;
    while (n) { DWORD c = n > (1u << 24) ? (1u << 24) : (DWORD)n, w = 0; if (!WriteFile(h, d, c, &w, NULL) || !w) { CloseHandle(h); return -1; } d += w; n -= w; } CloseHandle(h); return 0;
}
static int extract_entry_to(const WCHAR *dest, const AEnt *e, const u8 *data, size_t n) {
    if (!safe_rel(e->name)) return 0; int w = MultiByteToWideChar(CP_UTF8, 0, e->name, -1, NULL, 0); WCHAR *rel = (WCHAR *)malloc((w + 2) * sizeof(WCHAR)); MultiByteToWideChar(CP_UTF8, 0, e->name, -1, rel, w);
    for (WCHAR *c = rel; *c; c++) if (*c == '/') *c = '\\'; WCHAR full[MAX_PATH + 8]; path_join(full, MAX_PATH, dest, rel); free(rel); int ok = 0;
    if (e->isdir) { WCHAR t[MAX_PATH + 8]; wfmt(t, MAX_PATH + 8, L"%ls\\", full); ensure_dirs(t); CreateDirectoryW(full, NULL); ok = 1; }
    else { ensure_dirs(full); ok = write_file_w(full, data, n) == 0; } return ok;
}
static int do_extract(const WCHAR *src, const WCHAR *dest, WCHAR *msg) {
    int k = arc_kind_of(src); int ok = 0, bad = 0; CreateDirectoryW(dest, NULL); WCHAR d2[MAX_PATH + 4]; wfmt(d2, MAX_PATH + 4, L"%ls\\x", dest); ensure_dirs(d2);
    if (k == ARC_ZIP || k == ARC_ISO) {
        Rd r; FRd f; if (frd_open(&r, &f, src)) { scpy(msg, 160, L"Cannot open archive"); return 0; } FArc a; int rc = k == ARC_ZIP ? zip_open(&a, &r) : iso_open(&a, &r); if (rc) { CloseHandle(f.h); scpy(msg, 160, L"Archive is damaged or unsupported"); return 0; }
        for (int i = 0; i < a.n; i++) { u8 *d = NULL; size_t n = 0; int rr;
            if (k == ARC_ZIP) rr = zip_read_entry(&a, i, &d, &n, 700u << 20); else { n = (size_t)a.e[i].size; rr = 0; if (!a.e[i].isdir) { if (n > (700u << 20)) rr = -10; else { d = (u8 *)malloc(n + 1); if (r.read(&r, a.e[i].off, d, n)) rr = -2; } } }
            if (rr) { bad++; free(d); continue; } if (extract_entry_to(dest, &a.e[i], d, n)) ok++; else bad++; free(d); }
        arc_free(&a); CloseHandle(f.h);
    } else if (k == ARC_TAR || k == ARC_TGZ || k == ARC_GZ) {
        size_t n; u8 *b = read_whole(src, &n, 1u << 30); if (!b) { scpy(msg, 160, L"Cannot read archive (too large?)"); return 0; } u8 *t = b; size_t tn = n;
        if (k != ARC_TAR) { if (gzip_unpack(b, n, &t, &tn, 1u << 30)) { free(b); scpy(msg, 160, L"Not a valid gzip file"); return 0; } free(b); b = t; }
        FArc a; memset(&a, 0, sizeof a); int istar = tn > 512 && !memcmp(t + 257, "ustar", 5);
        if (k == ARC_GZ && !istar) { WCHAR nm[MAX_PATH]; scpy(nm, MAX_PATH, base_name(src)); int l = lstrlenW(nm); if (l > 3) nm[l - 3] = 0; WCHAR out[MAX_PATH]; path_join(out, MAX_PATH, dest, nm); ok = write_file_w(out, t, tn) == 0; }
        else { tar_scan(&a, t, tn); for (int i = 0; i < a.n; i++) { if (extract_entry_to(dest, &a.e[i], t + a.e[i].off, (size_t)a.e[i].size)) ok++; else bad++; } arc_free(&a); }
        free(t);
    } else {
        WCHAR z[MAX_PATH]; if (!find_7z(z)) { scpy(msg, 160, L"This format needs 7-Zip: put 7z.exe (+7z.dll) next to this program or install 7-Zip"); return 0; }
        WCHAR cmd[MAX_PATH * 3]; wfmt(cmd, MAX_PATH * 3, L"\"%ls\" x -y -aoa \"-o%ls\" \"%ls\"", z, dest, src); int ec = run_capture(cmd, NULL, NULL, 3600000);
        if (ec == 0) { scpy(msg, 160, L"Extracted"); return 1; } wfmt(msg, 160, L"7-Zip reported an error (code %d)", ec); return 0;
    }
    if (bad) wfmt(msg, 160, L"Extracted %d item(s), %d failed", ok, bad); else wfmt(msg, 160, L"Extracted %d item(s)", ok); return ok > 0;
}
static DWORD WINAPI extract_thread(LPVOID p) {
    ExJob *j = (ExJob *)p; WCHAR msg[160] = L""; do_extract(j->src, j->dest, msg); scpy(S.status_msg, 160, msg); PostMessageW(j->hwnd, WM_APP_TASKDONE, 0, 0); free(j); return 0;
}
static void start_extract(const WCHAR *src, const WCHAR *dest) {
    if (InterlockedCompareExchange(&S.busy, 1, 0)) { toast(L"Another operation is running"); return; } ExJob *j = (ExJob *)calloc(1, sizeof *j); scpy(j->src, MAX_PATH, src); scpy(j->dest, MAX_PATH, dest); j->hwnd = S.hwnd;
    scpy(S.status_msg, 160, L"Extracting..."); redraw(); HANDLE h = CreateThread(NULL, 0, extract_thread, j, 0, NULL); if (h) CloseHandle(h);
}
/* ---- compression ---- */
static int fsink_write(void *ctx, const void *p, size_t n) { HANDLE h = (HANDLE)ctx; const u8 *b = (const u8 *)p; while (n) { DWORD w = 0, c = n > (1u << 24) ? (1u << 24) : (DWORD)n; if (!WriteFile(h, b, c, &w, NULL) || !w) return -1; b += w; n -= w; } return 0; }
static u32 dos_time_now(void) { FILETIME ft; WORD d, t; GetSystemTimeAsFileTime(&ft); FileTimeToLocalFileTime(&ft, &ft); FileTimeToDosDateTime(&ft, &d, &t); return ((u32)d << 16) | t; }
typedef struct { ZW *z; CBuf tar; int mode; /* 0 zip, 1 tar */ int count; } CompCtx;
static void tar_hdr(CBuf *t, const char *name, u64 size, int isdir) {
    u8 h[512]; memset(h, 0, 512); size_t nl = strlen(name); if (nl > 99) nl = 99; memcpy(h, name, nl); snprintf((char *)h + 100, 8, "%07o", isdir ? 0755 : 0644); snprintf((char *)h + 108, 8, "%07o", 0); snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)size); snprintf((char *)h + 136, 12, "%011llo", (unsigned long long)time(NULL)); memset(h + 148, ' ', 8); h[156] = isdir ? '5' : '0'; memcpy(h + 257, "ustar", 5); h[262] = 0; memcpy(h + 263, "00", 2);
    unsigned sum = 0; for (int i = 0; i < 512; i++) sum += h[i]; snprintf((char *)h + 148, 7, "%06o", sum); h[154] = 0; h[155] = ' '; cb_add(t, (char *)h, 512);
}
static void comp_add(CompCtx *c, const WCHAR *full, const char *rel, int isdir, int depth) {
    if (depth > 40) return;
    if (isdir) {
        char r2[1100]; snprintf(r2, sizeof r2, "%s/", rel); if (c->mode == 0) { zw_begin(c->z, r2, 1, dos_time_now()); } else tar_hdr(&c->tar, r2, 0, 1);
        WCHAR pat[MAX_PATH + 4]; path_join(pat, MAX_PATH, full, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
        do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue; WCHAR cf[MAX_PATH]; path_join(cf, MAX_PATH, full, fd.cFileName); char nm[600]; WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, 600, NULL, NULL);
            char cr[1100]; snprintf(cr, sizeof cr, "%s/%s", rel, nm); comp_add(c, cf, cr, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, depth + 1); } while (FindNextFileW(h, &fd)); FindClose(h);
    } else {
        HANDLE f = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL); if (f == INVALID_HANDLE_VALUE) return; LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
        if (c->mode == 0) { zw_begin(c->z, rel, 0, dos_time_now()); u8 *buf = (u8 *)malloc(1 << 18); DWORD got; while (ReadFile(f, buf, 1 << 18, &got, NULL) && got) zw_feed(c->z, buf, got); free(buf); zw_end(c->z); }
        else { tar_hdr(&c->tar, rel, (u64)sz.QuadPart, 0); u8 *buf = (u8 *)malloc(1 << 18); DWORD got; u64 tot = 0; while (ReadFile(f, buf, 1 << 18, &got, NULL) && got) { cb_add(&c->tar, (char *)buf, got); tot += got; } free(buf); size_t pad = (size_t)((512 - (tot & 511)) & 511); if (pad) { char z[512] = { 0 }; cb_add(&c->tar, z, pad); } }
        CloseHandle(f); c->count++;
    }
}
static DWORD WINAPI compress_thread(LPVOID p) {
    CoJob *j = (CoJob *)p; WCHAR msg[200] = L""; int ok = 0;
    if (!wcscmp(j->fmt, L"zip") || !wcscmp(j->fmt, L"tar") || !wcscmp(j->fmt, L"tgz")) {
        CompCtx c; memset(&c, 0, sizeof c); HANDLE h = CreateFileW(j->out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) scpy(msg, 200, L"Cannot create the archive file");
        else {
            ZW z; if (!wcscmp(j->fmt, L"zip")) { Sink s = { fsink_write, h }; zw_init(&z, s); c.z = &z; c.mode = 0; } else c.mode = 1;
            for (int i = 0; i < j->srcs.n; i++) { char nm[600]; WideCharToMultiByte(CP_UTF8, 0, base_name(j->srcs.v[i]), -1, nm, 600, NULL, NULL); comp_add(&c, j->srcs.v[i], nm, dir_exists(j->srcs.v[i]), 0); }
            if (c.mode == 0) { ok = zw_finish(&z) == 0; }
            else { char z2[1024] = { 0 }; cb_add(&c.tar, z2, 1024);
                if (!wcscmp(j->fmt, L"tar")) ok = fsink_write(h, c.tar.s, c.tar.n) == 0;
                else { BW w; memset(&w, 0, sizeof w); u8 hd[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255 }; fsink_write(h, hd, 10); u32 crc = crc32_upd(0, (u8 *)c.tar.s, c.tar.n); size_t off = 0;
                    while (off < c.tar.n) { size_t k = c.tar.n - off; if (k > (1u << 18)) k = 1u << 18; int fin = off + k >= c.tar.n; deflate_block(&w, (u8 *)c.tar.s + off, k, fin); off += k; if (fin) bw_flush(&w); fsink_write(h, w.buf, w.n); w.n = 0; }
                    u8 tr[8]; zw_le(tr, crc, 4); zw_le(tr + 4, (u32)c.tar.n, 4); fsink_write(h, tr, 8); free(w.buf); ok = 1; }
                free(c.tar.s); }
            CloseHandle(h); wfmt(msg, 200, ok ? L"Created %ls" : L"Failed to write %ls", base_name(j->out)); if (!ok) DeleteFileW(j->out);
        }
    } else {
        WCHAR tool[MAX_PATH]; int is7 = !wcscmp(j->fmt, L"7z"); int found = is7 ? find_7z(tool) : (find_rar(tool) || find_7z(tool)); int israr = !is7;
        if (!found) scpy(msg, 200, is7 ? L"7z needs 7-Zip: put 7z.exe next to this program or install it" : L"RAR needs WinRAR's rar.exe next to this program or installed");
        else { WCHAR *cmd = (WCHAR *)malloc(32768 * sizeof(WCHAR)); int rarexe = israr && wcsstr(tool, L"rar") && !wcsstr(tool, L"7z");
            if (israr && !rarexe) { scpy(msg, 200, L"Creating .rar needs WinRAR (rar.exe); 7-Zip cannot create RAR"); }
            else { wfmt(cmd, 32768, rarexe ? L"\"%ls\" a -ep1 -y \"%ls\"" : L"\"%ls\" a -y \"%ls\"", tool, j->out);
                for (int i = 0; i < j->srcs.n; i++) { scat(cmd, 32768, L" \""); scat(cmd, 32768, j->srcs.v[i]); scat(cmd, 32768, L"\""); }
                int ec = run_capture(cmd, NULL, NULL, 3600000); ok = ec == 0 || ec == 1; if (ok) wfmt(msg, 200, L"Created %ls", base_name(j->out)); else wfmt(msg, 200, L"Archiver failed (code %d)", ec); }
            free(cmd); }
    }
    scpy(S.status_msg, 160, msg); PostMessageW(j->hwnd, WM_APP_TASKDONE, 0, 0); sl_clear(&j->srcs); free(j->srcs.v); free(j); return 0;
}
static void start_compress(const WCHAR *fmt) {
    Tab *t = curtab(); SL s = { 0 }; collect_sel(t, &s); if (!s.n) { toast(L"Select files or folders first"); free(s.v); return; }
    if (InterlockedCompareExchange(&S.busy, 1, 0)) { toast(L"Another operation is running"); sl_clear(&s); free(s.v); return; }
    CoJob *j = (CoJob *)calloc(1, sizeof *j); j->srcs = s; j->hwnd = S.hwnd; scpy(j->fmt, 8, fmt);
    WCHAR dir[MAX_PATH], stem[MAX_PATH]; parent_of(s.v[0], dir); if (!dir[0]) scpy(dir, MAX_PATH, t->path);
    if (s.n == 1) { scpy(stem, MAX_PATH, base_name(s.v[0])); if (!dir_exists(s.v[0])) { WCHAR *e = PathFindExtensionW(stem); if (*e) *e = 0; } } else { WCHAR dn[MAX_PATH]; scpy(dn, MAX_PATH, base_name(dir)); scpy(stem, MAX_PATH, dn[0] ? dn : L"Archive"); }
    WCHAR ext[16]; wfmt(ext, 16, L".%ls", !wcscmp(fmt, L"tgz") ? L"tar.gz" : fmt); unique_new(dir, stem, ext, j->out, MAX_PATH);
    scpy(S.status_msg, 160, L"Compressing..."); redraw(); HANDLE h = CreateThread(NULL, 0, compress_thread, j, 0, NULL); if (h) CloseHandle(h);
}

/* ========================================================================== */
/* PREVIEW I/O: loading, selection/copy, input handling, fonts, fullscreen     */
/* ========================================================================== */
static void pv_ctx_redraw(PVC *c);
static int g_pv_cls = -1, g_pv_override;
static const WCHAR *PV_TEXT = L".txt.md.log.ini.cfg.json.xml.csv.tsv.c.h.cpp.hpp.cc.py.js.ts.tsx.jsx.html.htm.css.bat.cmd.sh.yaml.yml.lua.java.rs.go.conf.inf.reg.srt.toml.properties.php.sql.rb.cs.vb.ps1.tex.rtf.diff.patch.gitignore.";
static const WCHAR *PV_VIDEO = L".mp4.avi.wmv.mkv.mov.mpg.mpeg.m4v.webm.flv.3gp.mp3.wav.wma.m4a.ogg.flac.aac.mid.midi.";
static int pv_is_audio(const WCHAR *p) { return ext_in(p, L".mp3.wav.wma.m4a.ogg.flac.aac.mid.midi."); }
/* class: 0 text 1 image 2 video 3 pdf 4 word 5 excel 6 ppt 7 archive, -1 none */
static int pv_class(const WCHAR *p) {
    if (ext_in(p, PV_TEXT)) return 0; if (is_image(p)) return 1; if (ext_in(p, PV_VIDEO)) return 2; if (ext_in(p, L".pdf.")) return 3; if (ext_in(p, L".docx.")) return 4;
    if (ext_in(p, L".xlsx.")) return 5; if (ext_in(p, L".pptx.")) return 6; if (arc_kind_of(p) != ARC_NONE && !ext_in(p, L".docx.xlsx.pptx.")) return 7; return -1;
}
static int pv_width_for(int cls) {
    int *pz[8] = { &C.pvsz0, &C.pvsz1, &C.pvsz2, &C.pvsz3, &C.pvsz4, &C.pvsz5, &C.pvsz6, &C.pvsz7 }; if (cls < 0) return C.preview_width; int v = *pz[cls];
    return v <= 0 ? 240 : v == 1 ? 360 : v == 2 ? 560 : 360;
}
static WCHAR *load_text_file(const WCHAR *path) {
    size_t n; u8 *b = read_whole(path, &n, 1u << 30); if (!b) { HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); if (h == INVALID_HANDLE_VALUE) return NULL;
        b = (u8 *)malloc(524289); DWORD rd = 0; ReadFile(h, b, 524288, &rd, NULL); CloseHandle(h); n = rd; }
    if (n > 524288) n = 524288; if (!n) { free(b); return NULL; } WCHAR *out;
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { int k = (int)(n - 2) / 2; out = (WCHAR *)malloc((k + 1) * sizeof(WCHAR)); memcpy(out, b + 2, k * 2); out[k] = 0; }
    else { u8 *s = b; size_t m = n; if (m >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; m -= 3; } UINT cp = CP_UTF8; int w = 0;
        for (int cut = 0; cut < 4 && m > (size_t)cut; cut++) { w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)s, (int)(m - cut), NULL, 0); if (w > 0) { m -= cut; break; } }
        if (w <= 0) { cp = CP_ACP; w = MultiByteToWideChar(cp, 0, (char *)s, (int)m, NULL, 0); } out = (WCHAR *)malloc((w + 1) * sizeof(WCHAR)); MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, (char *)s, (int)m, out, w); out[w] = 0; }
    free(b); return out;
}
static void pv_set_err(const WCHAR *m) { g_pv.kind = PK_ERROR; scpy(g_pv.err, 220, m); }
static void pv_load_office(const WCHAR *path, int fmt) {
    Rd r; FRd f; if (frd_open(&r, &f, path)) { pv_set_err(L"Cannot open the file"); return; } Doc d; int rc = office_load(&r, fmt, &d); CloseHandle(f.h);
    if (rc || !d.npg) { WCHAR e[220]; MultiByteToWideChar(CP_UTF8, 0, d.err[0] ? d.err : "Nothing to preview", -1, e, 220); pv_set_err(e); doc_free(&d); return; }
    if (fmt == 1) pv_build_flow(&d); else if (fmt == 2) pv_build_sheets(&d); else pv_build_slides(&d); doc_free(&d);
}
static void pv_load_pdf(const WCHAR *path) {
    size_t n; u8 *b = read_whole(path, &n, 96u << 20); if (!b) { pv_set_err(L"PDF is too large or unreadable"); return; } Doc d; int rc = pdf_load(b, n, &d);
    if (rc || !d.npg) { WCHAR e[220]; MultiByteToWideChar(CP_UTF8, 0, d.err[0] ? d.err : "No text found", -1, e, 220); WCHAR m[260]; wfmt(m, 260, L"%ls\n(Scanned/image-only or encrypted PDFs cannot be shown as text.)", e); pv_set_err(m); free(b); doc_free(&d); return; }
    pv_build_pdf(&d); doc_free(&d); free(b); g_pv.info[0] = 0;
}
static void pv_load_archive(const WCHAR *path) {
    int k = arc_kind_of(path); FArc a; memset(&a, 0, sizeof a); int rc = -1; Rd r; FRd f; int opened = 0;
    if (k == ARC_ZIP || k == ARC_ISO) { if (!frd_open(&r, &f, path)) { opened = 1; rc = k == ARC_ZIP ? zip_open(&a, &r) : iso_open(&a, &r); } }
    else if (k == ARC_TAR || k == ARC_TGZ || k == ARC_GZ) { size_t n; u8 *b = read_whole(path, &n, 400u << 20); if (b) { u8 *t = b; size_t tn = n; rc = 0; if (k != ARC_TAR) { if (gzip_unpack(b, n, &t, &tn, 600u << 20)) rc = -1; else { free(b); b = t; } }
        if (!rc) { if (k == ARC_GZ && !(tn > 512 && !memcmp(t + 257, "ustar", 5))) { int cap = 0; WCHAR nm[MAX_PATH]; scpy(nm, MAX_PATH, base_name(path)); int l = lstrlenW(nm); if (l > 3) nm[l - 3] = 0; char u[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, nm, -1, u, sizeof u, NULL, NULL); AEnt *e = arc_push(&a, &cap, u, strlen(u)); e->size = tn; }
            else tar_scan(&a, t, tn); } free(b); } }
    else { rc = ext_list(path, &a); if (rc) { pv_set_err(L"This archive type needs 7-Zip to preview/extract.\nPut 7z.exe (and 7z.dll) next to this program or install 7-Zip."); g_pv.canext = 0; return; } }
    if (rc) { pv_set_err(L"Archive is damaged or unsupported"); if (opened) CloseHandle(f.h); arc_free(&a); return; }
    WCHAR nm[MAX_PATH]; scpy(nm, MAX_PATH, base_name(path)); pv_build_arc(&a, nm); g_pv.canext = 1; arc_free(&a); if (opened) CloseHandle(f.h);
}
static void preview_free(void) { vid_close(); pv_free(); pv_font_reset(); g_pv.kind = PK_NONE; }
static void pv_reset_view(PVC *c) { c->zoom = 100; c->rot = 0; c->sx = c->sy = 0; c->fit_pending = 1; c->dragv = c->dragh = 0; }
static void preview_update(void) {
    preview_free(); Tab *t = curtab(); Item *it = NULL; int ns = sel_count(t, NULL);
    if (ns == 1) for (int i = 0; i < t->n; i++) if (t->it[i].sel) { it = &t->it[i]; break; }
    S.prev.path[0] = 0; g_pv.path[0] = 0; if (!it) { g_pv_cls = -1; return; }
    scpy(S.prev.path, MAX_PATH, it->path); scpy(S.prev.type, 64, it->type); S.prev.size = it->size; S.prev.mt = it->mt; S.prev.dir = it->dir; scpy(g_pv.path, MAX_PATH, it->path);
    pv_reset_view(&g_pc_panel); g_pv.kind = PK_INFO; if (it->dir || it->kind == 1) { g_pv_cls = -1; return; }
    int cls = pv_class(it->path); if (cls != g_pv_cls) g_pv_override = 0; g_pv_cls = cls; if (cls < 0 || !C.show_preview) { if (cls < 0) { if (it->size > 0 && it->size < (4u << 20)) { /* sniff unknown files as text */ WCHAR *w = load_text_file(it->path);
        if (w) { int bin = 0; for (int i = 0; w[i] && i < 2000; i++) if (w[i] < 9 || (w[i] > 13 && w[i] < 32)) { bin = 1; break; } if (!bin) { pv_build_text(w); g_pc_panel.zoom = 100; g_pc_panel.fit_pending = 0; g_pv_cls = 0; } free(w); } } } return; }
    switch (cls) {
    case 0: { WCHAR *w = load_text_file(it->path); if (w) { pv_build_text(w); free(w); g_pc_panel.fit_pending = 0; } else pv_set_err(L"Empty or unreadable file"); break; }
    case 1: pv_build_image(it->path); break;
    case 2: g_pv.kind = PK_MEDIA; if (C.video_mode == 0 || pv_is_audio(it->path)) { RECT r = { 0, 0, 16, 16 }; vid_open(it->path, S.hwnd, &r, !pv_is_audio(it->path)); } break;
    case 3: pv_load_pdf(it->path); break;
    case 4: pv_load_office(it->path, 1); break; case 5: pv_load_office(it->path, 2); break; case 6: pv_load_office(it->path, 3); break;
    case 7: pv_load_archive(it->path); break;
    }
    int *pz[8] = { &C.pvsz0, &C.pvsz1, &C.pvsz2, &C.pvsz3, &C.pvsz4, &C.pvsz5, &C.pvsz6, &C.pvsz7 };
    if (*pz[cls] == 3 || (cls == 2 && C.video_mode == 1)) PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(CMD_PVFULL, 0), 0);
}
/* ---- text selection / copy ---- */
static int pv_hit(PVC *c, int x, int y, int *run, int *ch) {
    PPage *p = pv_page(); if (!p || c->rot || !p->n) return 0; int w, h; pv_dims(c, &w, &h); int m = px(10), vw = c->view.right - c->view.left, vh = c->view.bottom - c->view.top;
    int ox = c->view.left + (w + 2 * m < vw ? (vw - w) / 2 : m - c->sx), oy = c->view.top + (h + 2 * m < vh ? m : m - c->sy); int zx = (x - ox) * 100 / c->zoom, zy = (y - oy) * 100 / c->zoom;
    int best = -1, bd = 1 << 30; for (int i = 0; i < p->n; i++) { PRun *r = &p->r[i]; if (r->kind != 0) continue; int dy = zy < r->y ? r->y - zy : zy > r->y + r->h ? zy - r->y - r->h : 0; int dx = zx < r->x ? r->x - zx : zx > r->x + r->w ? zx - r->x - r->w : 0; int d = dy * 1000 + dx; if (d < bd) { bd = d; best = i; } }
    if (best < 0) return 0; PRun *r = &p->r[best]; int off = zx - r->x; int cc = 0; if (off > 0) { HDC dc = pv_mdc(); HGDIOBJ o = SelectObject(dc, pv_font(r->fh, r->bold, r->mono)); int fit = 0; SIZE z; GetTextExtentExPointW(dc, r->t, r->len, off, &fit, NULL, &z);
        if (fit < r->len) { SIZE a, b; GetTextExtentPoint32W(dc, r->t, fit, &a); GetTextExtentPoint32W(dc, r->t, fit + 1, &b); if (off > (a.cx + b.cx) / 2) fit++; } cc = fit; SelectObject(dc, o); }
    *run = best; *ch = cc; return 1;
}
static WCHAR *pv_selected_text(int whole) {
    PPage *p = pv_page(); if (!p) return NULL; int r0, c0, r1, c1; int hs = pv_sel_order(&r0, &c0, &r1, &c1); if (!hs || whole) { r0 = 0; c0 = 0; r1 = p->n - 1; c1 = 1 << 20; }
    size_t cap = 4096, n = 0; WCHAR *o = (WCHAR *)malloc(cap * sizeof(WCHAR)); int lasty = -1, lastr = -1;
    for (int i = r0; i <= r1 && i < p->n; i++) { PRun *r = &p->r[i]; if (r->kind != 0) continue; int a = i == r0 ? c0 : 0, b = i == r1 ? c1 : r->len; if (a > r->len) a = r->len; if (b > r->len) b = r->len; if (b <= a && !(i > r0 && i < r1)) continue;
        if (n + (b - a) + 4 > cap) { cap = (cap + b - a + 4) * 2; o = (WCHAR *)realloc(o, cap * sizeof(WCHAR)); }
        if (lasty >= 0) { if (r->y > lasty + 2 || r->y + r->h < lastr - 2) o[n++] = '\n'; else if (r->sep) o[n++] = '\t'; } memcpy(o + n, r->t + a, (b - a) * sizeof(WCHAR)); n += b - a; lasty = r->y; lastr = r->y + r->h; }
    o[n] = 0; return o;
}
static void pv_copy(int whole) {
    WCHAR *t = pv_selected_text(whole); if (!t) return; if (!t[0]) { free(t); return; } if (OpenClipboard(S.hwnd)) { EmptyClipboard(); size_t n = (lstrlenW(t) + 1) * sizeof(WCHAR); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n); if (g) { memcpy(GlobalLock(g), t, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); } CloseClipboard(); toast(g_pv.havesel && !whole ? L"Selection copied" : L"Page text copied"); } free(t);
}
/* ---- fonts: TTF family parse + menu ---- */
static int ttf_family(const WCHAR *path, WCHAR *out, int cap) {
    size_t n; u8 *b = read_whole(path, &n, 64u << 20); if (!b || n < 12) { free(b); return 0; } u32 off = 0; if (!memcmp(b, "ttcf", 4)) off = (b[12] << 24) | (b[13] << 16) | (b[14] << 8) | b[15]; if (off + 12 > n) { free(b); return 0; }
    int nt = (b[off + 4] << 8) | b[off + 5]; int ok = 0;
    for (int i = 0; i < nt && off + 12 + 16 * (i + 1) <= n; i++) { u8 *t = b + off + 12 + 16 * i; if (memcmp(t, "name", 4)) continue; u32 to = (t[8] << 24) | (t[9] << 16) | (t[10] << 8) | t[11]; if (to + 6 > n) break; u8 *nm = b + to; int cnt = (nm[2] << 8) | nm[3]; u32 so = to + ((nm[4] << 8) | nm[5]);
        for (int k = 0; k < cnt && to + 6 + 12 * (k + 1) <= n; k++) { u8 *r = nm + 6 + 12 * k; int pid = (r[0] << 8) | r[1], nid = (r[6] << 8) | r[7], len = (r[8] << 8) | r[9]; u32 o2 = so + ((r[10] << 8) | r[11]); if (nid != 1 || o2 + len > n) continue;
            if (pid == 3) { int m = len / 2; if (m > cap - 1) m = cap - 1; for (int j = 0; j < m; j++) out[j] = (b[o2 + 2 * j] << 8) | b[o2 + 2 * j + 1]; out[m] = 0; ok = 1; break; } else if (pid == 1 && !ok) { int m = len; if (m > cap - 1) m = cap - 1; for (int j = 0; j < m; j++) out[j] = b[o2 + j]; out[m] = 0; ok = 1; } } break; }
    free(b); return ok;
}
static void doc_fonts_load(void) {
    for (int i = 0; i < C.doc_fonts.n; i++) if (path_exists(C.doc_fonts.v[i])) AddFontResourceExW(C.doc_fonts.v[i], FR_PRIVATE, 0);
}
static void add_font_file(void) {
    WCHAR f[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = S.hwnd; o.lpstrFilter = L"Fonts (*.ttf;*.otf;*.ttc)\0*.ttf;*.otf;*.ttc\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return; if (!AddFontResourceExW(f, FR_PRIVATE, 0)) { toast(L"Could not load that font file"); return; } if (sl_find(&C.doc_fonts, f) < 0) sl_add(&C.doc_fonts, f);
    WCHAR fam[64]; if (ttf_family(f, fam, 64)) { scpy(C.doc_font, 64, fam); } pv_font_reset(); cfg_save(); preview_update(); toast(L"Font added and selected");
}
static WCHAR g_fontnames[260][40]; static int g_nfonts;
static int CALLBACK fontenum_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD ty, LPARAM lp) { (void)tm; (void)ty; (void)lp; if (lf->lfFaceName[0] == '@' || g_nfonts >= 260) return 1;
    for (int i = 0; i < g_nfonts; i++) if (!lstrcmpW(g_fontnames[i], lf->lfFaceName)) return 1; lstrcpynW(g_fontnames[g_nfonts++], lf->lfFaceName, 40); return 1; }
static int font_menu(int sx, int sy) {
    g_nfonts = 0; HDC dc = GetDC(NULL); LOGFONTW lf; memset(&lf, 0, sizeof lf); lf.lfCharSet = DEFAULT_CHARSET; EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)fontenum_cb, 0, 0); ReleaseDC(NULL, dc);
    PMenu *m = pm_new(); pm_head(m, L"Document font"); pm_radio(m, 8999, L"Default (Segoe UI)", !C.doc_font[0], 1);
    static const WCHAR *common[] = { L"Segoe UI", L"Arial", L"Times New Roman", L"Calibri", L"Cambria", L"Georgia", L"Verdana", L"Tahoma", L"Nirmala UI", L"Mangal", L"Kokila", L"Noto Sans", L"DejaVu Sans", L"Courier New" };
    int idx = 0; for (int i = 0; i < 14; i++) if (font_available(common[i])) { pm_radio(m, 9000 + i, common[i], wieq(C.doc_font, common[i]), 1); idx++; }
    if (C.doc_fonts.n) { pm_sep(m); pm_head(m, L"Your font files"); for (int i = 0; i < C.doc_fonts.n && i < 30; i++) { WCHAR fam[64]; if (!ttf_family(C.doc_fonts.v[i], fam, 64)) lstrcpyW(fam, base_name(C.doc_fonts.v[i])); pm_radio(m, 9100 + i, fam, wieq(C.doc_font, fam), 1); } }
    pm_sep(m); PMenu *all = pm_sub(m, L"All installed fonts"); for (int i = 0; i < g_nfonts; i++) pm_radio(all, 9300 + i, g_fontnames[i], wieq(C.doc_font, g_fontnames[i]), 1);
    pm_sep(m); pm_add(m, 9999, L"Add font file...", 0, 1);
    int r = pm_track(m, sx, sy); pm_free(m); if (!r) return 0;
    if (r == 8999) C.doc_font[0] = 0; else if (r == 9999) { add_font_file(); return 1; }
    else if (r >= 9000 && r < 9100) scpy(C.doc_font, 64, common[r - 9000]); else if (r >= 9100 && r < 9300) { WCHAR fam[64]; int i = r - 9100; if (ttf_family(C.doc_fonts.v[i], fam, 64)) scpy(C.doc_font, 64, fam); }
    else if (r >= 9300 && r - 9300 < g_nfonts) scpy(C.doc_font, 64, g_fontnames[r - 9300]);
    pv_font_reset(); cfg_save(); preview_update(); return 1;
}
/* ---- fullscreen window ---- */
static HWND g_fullwnd;
static void pv_open_full(void);
static void pv_close_full(void) { if (g_fullwnd) { HWND w = g_fullwnd; g_fullwnd = NULL; g_pc_full.hw = NULL; vid_close(); DestroyWindow(w); SetForegroundWindow(S.hwnd); preview_update(); redraw(); } }
static void pv_size_menu(int sx, int sy) {
    if (g_pv_cls < 0) { toast(L"Select a previewable file"); return; } int *pz[8] = { &C.pvsz0, &C.pvsz1, &C.pvsz2, &C.pvsz3, &C.pvsz4, &C.pvsz5, &C.pvsz6, &C.pvsz7 }; static const WCHAR *cn[8] = { L"text", L"image", L"video/audio", L"PDF", L"Word", L"Excel", L"PowerPoint", L"archive" };
    PMenu *m = pm_new(); WCHAR h[80]; wfmt(h, 80, L"Preview size for %ls files", cn[g_pv_cls]); pm_head(m, h); static const WCHAR *nm[4] = { L"Min (narrow panel)", L"Mid", L"Large", L"Full screen" };
    for (int i = 0; i < 4; i++) pm_radio(m, 1 + i, nm[i], *pz[g_pv_cls] == i, 1); pm_sep(m); pm_add(m, 10, L"Drag the divider to resize the panel", 0, 0);
    int r = pm_track(m, sx, sy); pm_free(m); if (r >= 1 && r <= 4) { *pz[g_pv_cls] = r - 1; g_pv_override = 0; cfg_save(); if (r == 4) pv_open_full(); else { redraw(); } }
}
static void pv_do_btn(PVC *c, int id, int sx, int sy) {
    PV *v = &g_pv; switch (id) {
    case PB_PREV: pv_goto(c, v->cur - 1); break; case PB_NEXT: pv_goto(c, v->cur + 1); break;
    case PB_ZOUT: pv_set_zoom(c, c->zoom * 4 / 5, (c->view.left + c->view.right) / 2, (c->view.top + c->view.bottom) / 2); break; case PB_ZIN: pv_set_zoom(c, c->zoom * 5 / 4 + 1, (c->view.left + c->view.right) / 2, (c->view.top + c->view.bottom) / 2); break;
    case PB_FIT: pv_fit(c); break; case PB_ROTL: c->rot = (c->rot + 3) & 3; pv_fit(c); break; case PB_ROTR: c->rot = (c->rot + 1) & 3; pv_fit(c); break;
    case PB_FONT: font_menu(sx, sy); break; case PB_COPY: pv_copy(0); break; case PB_ALL: { PPage *p = pv_page(); if (p && p->n) { v->havesel = 1; v->a_run = 0; v->a_ch = 0; v->b_run = p->n - 1; v->b_ch = p->r[p->n - 1].len; } break; }
    case PB_FULL: pv_open_full(); break; case PB_SIZE: pv_size_menu(sx, sy); break;
    case PB_EXTRACT: PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(CMD_EXTRACT_MENU, 0), 0); break;
    case PB_OPENEXT: { Item tmp; memset(&tmp, 0, sizeof tmp); scpy(tmp.path, MAX_PATH, v->path); open_item(&tmp); break; }
    case PB_PLAY: vid_toggle(); break; case PB_STOP: vid_stop(); break; case PB_BACK10: vid_poll(); vid_seek((int)g_vid.pos - 10000); break; case PB_FWD10: vid_poll(); vid_seek((int)g_vid.pos + 10000); break;
    }
    pv_clamp(c); pv_ctx_redraw(c);
}
static void pv_ctx_redraw(PVC *c) { if (c->full && c->hw) InvalidateRect(c->hw, NULL, FALSE); else redraw(); }
static int pv_sbdrag_calc(PVC *c, int vert, int pos) {
    int w, h; pv_dims(c, &w, &h); int m = px(10); const RECT *sb = vert ? &c->vsb : &c->hsb; int content = (vert ? h : w) + 2 * m, view = vert ? c->view.bottom - c->view.top : c->view.right - c->view.left; int len = vert ? sb->bottom - sb->top : sb->right - sb->left;
    int tl = (int)((double)len * view / content); if (tl < px(24)) tl = px(24); int rng = len - tl; if (rng <= 0) return 0; int p = pos - c->dragoff - (vert ? sb->top : sb->left); if (p < 0) p = 0; if (p > rng) p = rng; return (int)((double)p / rng * (content - view));
}
/* returns 1 when the click was handled */
static int pv_click(PVC *c, int x, int y, int sx, int sy) {
    PV *v = &g_pv; if (c->full && in_rect(&c->closeb, x, y)) { pv_close_full(); return 1; }
    for (int i = 0; i < c->nb; i++) if (in_rect(&c->b[i].r, x, y)) { pv_do_btn(c, c->b[i].id, sx, sy); return 1; }
    if (v->kind == PK_MEDIA) {
        if (c->mc.right > c->mc.left) { int bs = px(c->full ? 34 : 30), gap = px(4), by = c->mc.bottom - bs; int ids[5] = { PB_PLAY, PB_STOP, PB_BACK10, PB_FWD10, PB_OPENEXT };
            for (int i = 0; i < 5; i++) { RECT b = { c->mc.left + i * (bs + gap), by, c->mc.left + i * (bs + gap) + bs, by + bs }; if (in_rect(&b, x, y)) { pv_do_btn(c, ids[i], sx, sy); return 1; } }
            if (in_rect(&c->seek, x, y) && g_vid.len > 0) { double f = (double)(x - c->seek.left) / (c->seek.right - c->seek.left); vid_seek((int)(f * g_vid.len)); pv_ctx_redraw(c); return 1; } }
        return in_rect(&c->body, x, y);
    }
    if (in_rect(&c->vsb, x, y)) { int w, h; pv_dims(c, &w, &h); int m = px(10), content = h + 2 * m, view = c->view.bottom - c->view.top, len = c->vsb.bottom - c->vsb.top; int tl = (int)((double)len * view / content); if (tl < px(24)) tl = px(24);
        int mx = content - view, ty = c->vsb.top + (mx > 0 ? (int)((double)c->sy / mx * (len - tl)) : 0); if (y >= ty && y < ty + tl) { c->dragv = 1; c->dragoff = y - ty; } else { c->sy += y < ty ? -view : view; pv_clamp(c); } pv_ctx_redraw(c); return 1; }
    if (in_rect(&c->hsb, x, y)) { int w, h; pv_dims(c, &w, &h); int m = px(10), content = w + 2 * m, view = c->view.right - c->view.left, len = c->hsb.right - c->hsb.left; int tl = (int)((double)len * view / content); if (tl < px(24)) tl = px(24);
        int mx = content - view, tx = c->hsb.left + (mx > 0 ? (int)((double)c->sx / mx * (len - tl)) : 0); if (x >= tx && x < tx + tl) { c->dragh = 1; c->dragoff = x - tx; } else { c->sx += x < tx ? -view : view; pv_clamp(c); } pv_ctx_redraw(c); return 1; }
    if (in_rect(&c->view, x, y) && v->npg && (v->kind == PK_DOC || v->kind == PK_TEXT || v->kind == PK_ARCHIVE)) { int r, ch; if (pv_hit(c, x, y, &r, &ch)) { v->havesel = 1; v->a_run = v->b_run = r; v->a_ch = v->b_ch = ch; v->selecting = 1; c->selecting = 1; pv_ctx_redraw(c); } return 1; }
    return in_rect(&c->body, x, y) || in_rect(&c->hdr, x, y);
}
static int pv_move(PVC *c, int x, int y) {
    if (c->dragv) { c->sy = pv_sbdrag_calc(c, 1, y); pv_clamp(c); pv_ctx_redraw(c); return 1; } if (c->dragh) { c->sx = pv_sbdrag_calc(c, 0, x); pv_clamp(c); pv_ctx_redraw(c); return 1; }
    if (c->selecting) { int r, ch; if (y < c->view.top) { c->sy -= px(20); pv_clamp(c); } else if (y > c->view.bottom) { c->sy += px(20); pv_clamp(c); } if (pv_hit(c, x < c->view.left ? c->view.left : x, y < c->view.top ? c->view.top : y > c->view.bottom ? c->view.bottom - 2 : y, &r, &ch)) { g_pv.b_run = r; g_pv.b_ch = ch; } pv_ctx_redraw(c); return 1; }
    return 0;
}
static int pv_up(PVC *c) { int was = c->dragv || c->dragh || c->selecting; c->dragv = c->dragh = 0; if (c->selecting) { c->selecting = 0; g_pv.selecting = 0; if (g_pv.a_run == g_pv.b_run && g_pv.a_ch == g_pv.b_ch) g_pv.havesel = 0; } return was; }
static int pv_wheel(PVC *c, int d, int ctrl, int x, int y) {
    if (g_pv.kind == PK_MEDIA || !g_pv.npg) return 0; if (ctrl) { pv_set_zoom(c, d > 0 ? c->zoom * 11 / 10 + 1 : c->zoom * 10 / 11, x, y); }
    else if (GetKeyState(VK_SHIFT) < 0) c->sx -= d * px(60) / 120; else c->sy -= d * px(60) / 120; pv_clamp(c); pv_ctx_redraw(c); return 1;
}
static void pv_key(PVC *c, WPARAM vk) {
    int ctrl = GetKeyState(VK_CONTROL) < 0, vh = c->view.bottom - c->view.top;
    switch (vk) { case VK_UP: c->sy -= px(40); break; case VK_DOWN: c->sy += px(40); break; case VK_LEFT: c->sx -= px(40); break; case VK_RIGHT: c->sx += px(40); break;
    case VK_PRIOR: if (c->sy <= 0) pv_goto(c, g_pv.cur - 1); else c->sy -= vh - px(20); break; case VK_NEXT: { int w, h; pv_dims(c, &w, &h); if (c->sy + vh >= h) pv_goto(c, g_pv.cur + 1); else c->sy += vh - px(20); break; }
    case VK_HOME: c->sy = 0; break; case VK_END: c->sy = 1 << 28; break; case VK_OEM_PLUS: case VK_ADD: pv_do_btn(c, PB_ZIN, 0, 0); return; case VK_OEM_MINUS: case VK_SUBTRACT: pv_do_btn(c, PB_ZOUT, 0, 0); return;
    case 'C': if (ctrl) pv_copy(0); return; case 'A': if (ctrl) pv_do_btn(c, PB_ALL, 0, 0); return; case '0': if (ctrl) pv_fit(c); break; case VK_SPACE: if (g_pv.kind == PK_MEDIA) vid_toggle(); break; default: return; }
    pv_clamp(c); pv_ctx_redraw(c);
}
static LRESULT CALLBACK full_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PVC *c = &g_pc_full;
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc); HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(m, b);
        RECT all = rc; fillr(m, &all, S.pal.bg); pv_layout(c, rc, 1); WCHAR meta[200], sz[32], dt[40]; fmt_bytes(S.prev.size, sz, 32); fmt_date(&S.prev.mt, dt, 40); wfmt(meta, 200, L"%ls - %ls - %ls   (Esc or X to close)", S.prev.type, sz, dt);
        pv_paint(m, c, base_name(S.prev.path), meta); if (g_vid.open && g_vid.video) { RECT vr = c->view; vid_place(&vr); }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY); SelectObject(m, ob); DeleteObject(b); DeleteDC(m); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: { POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ClientToScreen(h, &pt); SetFocus(h); pv_click(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), pt.x, pt.y + px(36)); if (c->selecting || c->dragv || c->dragh) SetCapture(h); return 0; }
    case WM_LBUTTONUP: if (pv_up(c)) { ReleaseCapture(); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_MOUSEMOVE: g_fmx = GET_X_LPARAM(lp); g_fmy = GET_Y_LPARAM(lp); if (!pv_move(c, g_fmx, g_fmy)) { static int k; int nk = (in_rect(&c->closeb, g_fmx, g_fmy) ? 1 : 0); if (nk != k) { k = nk; InvalidateRect(h, NULL, FALSE); } } return 0;
    case WM_MOUSEWHEEL: { POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &pt); pv_wheel(c, GET_WHEEL_DELTA_WPARAM(wp), (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0, pt.x, pt.y); return 0; }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(CMD_PVCLOSE, 0), 0); return 0; } pv_key(c, wp); return 0;
    case WM_COMMAND: if (LOWORD(wp) == PB_PLAY + 5000) vid_toggle(); return 0;
    case WM_TIMER: if (wp == TIMER_VID) { vid_poll(); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_CLOSE: PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(CMD_PVCLOSE, 0), 0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void pv_open_full(void) {
    if (g_fullwnd || g_pv.kind < PK_TEXT || g_pv.kind == PK_ERROR || g_pv.kind == PK_LOADING) return;
    static int reg; if (!reg) { reg = 1; WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = full_proc; wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"WinShellFM.Full"; RegisterClassExW(&wc); }
    MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromWindow(S.hwnd, MONITOR_DEFAULTTONEAREST), &mi); RECT r = mi.rcMonitor; int media = g_pv.kind == PK_MEDIA; vid_close();
    g_fullwnd = CreateWindowExW(WS_EX_TOPMOST, L"WinShellFM.Full", L"Preview", WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left, r.bottom - r.top, S.hwnd, NULL, GetModuleHandleW(NULL), NULL); if (!g_fullwnd) return;
    g_pc_full.hw = g_fullwnd; g_pc_full.zoom = 100; g_pc_full.rot = 0; g_pc_full.sx = g_pc_full.sy = 0; g_pc_full.fit_pending = 1; g_pc_full.full = 1;
    if (media) { RECT a = { 0, 0, 16, 16 }; vid_open(S.prev.path, g_fullwnd, &a, !pv_is_audio(S.prev.path)); }
    ShowWindow(g_fullwnd, SW_SHOW); SetForegroundWindow(g_fullwnd); SetFocus(g_fullwnd); InvalidateRect(g_fullwnd, NULL, FALSE);
}

/* ========================================================================== */
/* LAYOUT                                                                     */
/* ========================================================================== */
typedef struct {
    RECT caption, capbtn[3], ribtabs, ribtab[6], ribcol, strip, navbar, tabs, nav, content, preview, split, status, hdr, body, sb;
    RECT back, fwd, up, refresh, menu, addr, search, sub, sdrop, osb, cmdb, setb, views[3]; int capkind, has_strip;
} Lay;
static Lay LY;
static int g_nav_dirty = 1, g_maxed;
static RECT g_tabr[FM_MAXTABS], g_tabplus, g_rb[24]; static int g_rb_id[24], g_nrb;
static const WCHAR *RIB_TABS[6] = { L"File", L"Home", L"Share", L"View", L"Style & Look", L"Tools" };
static int view_mode(void) { Tab *t = curtab(); if (!t->path[0]) return 0; return !wcscmp(C.view, L"icons") ? 1 : !wcscmp(C.view, L"tiles") ? 2 : 0; }
static int tw_(const WCHAR *s, HFONT f) { HDC dc = pv_mdc(); HGDIOBJ o = SelectObject(dc, f ? f : S.font); SIZE z; GetTextExtentPoint32W(dc, s, lstrlenW(s), &z); SelectObject(dc, o); return z.cx; }
static int cap_kind(const Style *st) {
    const WCHAR *k = st->key; if (!wcscmp(k, L"macos")) return 1; if (!wcscmp(k, L"ubuntu") || !wcscmp(k, L"zorin") || !wcscmp(k, L"mint") || !wcscmp(k, L"chromeos") || !wcscmp(k, L"neon")) return 2;
    if (!wcscmp(k, L"total") || !wcscmp(k, L"mc") || !wcscmp(k, L"haiku") || !wcscmp(k, L"opus")) return 3; return 0;
}
static int eff_pv_width(void) { return g_pv_override ? C.preview_width : (g_pv_cls >= 0 ? pv_width_for(g_pv_cls) : C.preview_width); }
static int rb_count(int tab, int *ids, const WCHAR **lab);
static void do_layout(int W, int H) {
    const Style *st = cur_style(); Lay *L = &LY; memset(L, 0, sizeof *L); int y = 0; L->capkind = cap_kind(st);
    int ch = px(L->capkind == 2 ? 38 : L->capkind == 1 ? 30 : 32); L->caption = (RECT){ 0, 0, W, ch }; y = ch;
    int bw = px(L->capkind == 0 ? 46 : L->capkind == 3 ? 26 : 24);
    if (L->capkind == 1) { int d = px(13); for (int i = 0; i < 3; i++) L->capbtn[i] = (RECT){ px(12) + i * px(21), ch / 2 - d / 2, px(12) + i * px(21) + d, ch / 2 + d / 2 }; }
    else if (L->capkind == 0) { for (int i = 0; i < 3; i++) L->capbtn[i] = (RECT){ W - bw * (3 - i), 0, W - bw * (2 - i), ch - (g_maxed ? 0 : 1) }; }
    else { int g = px(6), d = L->capkind == 2 ? px(24) : px(22); for (int i = 0; i < 3; i++) L->capbtn[i] = (RECT){ W - px(10) - (d + g) * (3 - i) + g, ch / 2 - d / 2, W - px(10) - (d + g) * (3 - i) + g + d, ch / 2 + d / 2 }; }
    int rh = px(28); L->ribtabs = (RECT){ 0, y, W, y + rh }; int x = px(8);
    for (int i = 0; i < 6; i++) { int w = tw_(RIB_TABS[i], S.font) + px(22); L->ribtab[i] = (RECT){ x, y + 2, x + w, y + rh }; x += w + px(2); }
    L->ribcol = (RECT){ W - px(30), y + 2, W - px(4), y + rh - 2}; y += rh;
    if (C.ribbon_open) { L->has_strip = 1; L->strip = (RECT){ 0, y, W, y + px(68) }; y += px(68); }
    int th = st->layout == LAY_HEADER ? px(st->cap_h_fm) : px(st->tool_h); if (th < px(38)) th = px(38); L->navbar = (RECT){ 0, y, W, y + th }; y += th;
    if (S.ntabs > 1) { L->tabs = (RECT){ 0, y, W, y + px(28) }; y += px(28); }
    int sh = px(st->status_h); if (sh < px(24)) sh = px(24); int bottom = H - sh; if (bottom < y) bottom = y; L->status = (RECT){ 0, bottom, W, H };
    int navw = px(st->nav_w); if (navw > W / 3) navw = W / 3; int pw = 0;
    if (C.show_preview) { pw = px(eff_pv_width()); if (pw > W - navw - px(280)) pw = W - navw - px(280); if (pw < px(140)) pw = 0; }
    L->nav = (RECT){ 0, y, navw, bottom }; L->content = (RECT){ navw, y, W - pw, bottom }; L->preview = (RECT){ W - pw, y, W, bottom }; if (pw) L->split = (RECT){ W - pw - px(3), y, W - pw + px(3), bottom };
    int bh = px(st->tb_btn), fh = px(st->field_h), gap = px(st->tb_gap), cy = (L->navbar.top + L->navbar.bottom) / 2; x = px(8); RECT *b[4] = { &L->back, &L->fwd, &L->up, &L->refresh };
    for (int i = 0; i < 4; i++) { *b[i] = (RECT){ x, cy - bh / 2, x + bh, cy - bh / 2 + bh }; x += bh + gap; }
    int right = W - px(8); RECT *rb[4] = { &L->menu, &L->setb, &L->cmdb, &L->osb };
    for (int i = 0; i < 4; i++) { *rb[i] = (RECT){ right - bh, cy - bh / 2, right, cy - bh / 2 + bh }; right -= bh + gap; }
    right -= gap; int sw = px(st->search_w) + px(60); if (sw > (right - x) * 6 / 10) sw = (right - x) * 6 / 10; L->search = (RECT){ right - sw, cy - fh / 2, right, cy - fh / 2 + fh };
    L->sdrop = (RECT){ L->search.right - fh * 7 / 10 - px(2), L->search.top, L->search.right - px(2), L->search.bottom }; int subw = tw_(L"Sub", S.font_small) + px(26); L->sub = (RECT){ L->sdrop.left - subw, L->search.top, L->sdrop.left, L->search.bottom };
    right = L->search.left - gap * 2 - px(4); L->addr = (RECT){ x + px(4), cy - fh / 2, right, cy - fh / 2 + fh };
    int det = view_mode() == 0, sbw = px(st->sb_w); if (sbw < px(10)) sbw = px(10); int hh = det ? px(st->hdr_h) : 0;
    L->hdr = (RECT){ L->content.left, L->content.top, L->content.right, L->content.top + hh }; L->body = (RECT){ L->content.left, L->content.top + hh, L->content.right - sbw, bottom }; L->sb = (RECT){ L->content.right - sbw, L->body.top, L->content.right, bottom };
    int vb = sh - px(4); if (vb < px(18)) vb = px(18); for (int i = 0; i < 3; i++) L->views[i] = (RECT){ W - px(10) - vb * (3 - i), bottom + (sh - vb) / 2, W - px(10) - vb * (2 - i), bottom + (sh - vb) / 2 + vb };
}
static int gutter(void) { return (view_mode() == 0 && C.checkboxes) ? px(26) : 0; }
static void col_x(int bw, int c[5]) {
    int date = px(150), typ = px(curtab()->searching ? 230 : 130), size = px(90); int name = bw - date - typ - size; if (name < px(160)) name = px(160);
    c[0] = 0; c[1] = name; c[2] = name + date; c[3] = name + date + typ; c[4] = name + date + typ + size;
}
static int cell_geom(int *cw, int *chh, int *per) {
    int bw = LY.body.right - LY.body.left; int m = view_mode(); if (m == 1) { *cw = px(C.icon_size * 1.5 + 30); *chh = px(C.icon_size + 52); } else { *cw = px(230); *chh = px(58); }
    *per = (bw - px(8)) / *cw; if (*per < 1) *per = 1; return m;
}
static int total_h(void) { Tab *t = curtab(); int m = view_mode(); if (m == 0) return t->nv * px(cur_style()->row_h) + px(4); int cw, ch, per; cell_geom(&cw, &ch, &per); return ((t->nv + per - 1) / per) * ch + px(8); }
static void clamp_scroll(void) { Tab *t = curtab(); int vh = LY.body.bottom - LY.body.top, mx = total_h() - vh; if (mx < 0) mx = 0; if (t->scroll > mx) t->scroll = mx; if (t->scroll < 0) t->scroll = 0; }
static RECT item_rect(int i) {
    Tab *t = curtab(); int m = view_mode(); RECT r; int sc = t->scroll;
    if (m == 0) { int rh = px(cur_style()->row_h); r = (RECT){ LY.body.left, LY.body.top + i * rh - sc, LY.body.right, LY.body.top + (i + 1) * rh - sc }; return r; }
    int cw, ch, per; cell_geom(&cw, &ch, &per); int col = i % per, row = i / per; r.left = LY.body.left + px(4) + col * cw; r.top = LY.body.top + px(4) + row * ch - sc; r.right = r.left + cw - px(4); r.bottom = r.top + ch - px(4); return r;
}
static int hit_item(int x, int y) {
    Tab *t = curtab(); if (!in_rect(&LY.body, x, y)) return -1; int m = view_mode(), vi;
    if (m == 0) vi = (y - LY.body.top + t->scroll) / px(cur_style()->row_h);
    else { int cw, ch, per; cell_geom(&cw, &ch, &per); int col = (x - LY.body.left - px(4)) / cw, row = (y - LY.body.top - px(4) + t->scroll) / ch; if (col >= per || col < 0 || row < 0) return -1; vi = row * per + col; }
    if (vi < 0 || vi >= t->nv) return -1; if (m != 0) { RECT r = item_rect(vi); if (!in_rect(&r, x, y)) return -1; } return vi;
}
static RECT check_rect(int vi) {
    RECT r = item_rect(vi); int s = px(16); if (view_mode() == 0) return (RECT){ r.left + px(5), (r.top + r.bottom) / 2 - s / 2, r.left + px(5) + s, (r.top + r.bottom) / 2 + s / 2 };
    int m2 = px(4); return (RECT){ r.right - s - m2, r.top + m2, r.right - m2, r.top + m2 + s };
}
static void ensure_visible(int vi) {
    Tab *t = curtab(); if (vi < 0 || vi >= t->nv) return; RECT r = item_rect(vi);
    if (r.top < LY.body.top) t->scroll -= LY.body.top - r.top; else if (r.bottom > LY.body.bottom) t->scroll += r.bottom - LY.body.bottom; clamp_scroll();
}
/* ---- nav model with tree ---- */
static SL g_tree_open;
static void nav_add(const WCHAR *label, const WCHAR *path, int root, int glyph, int sect, int depth, int exp) {
    if (S.nnav == S.ncap_nav) { S.ncap_nav = S.ncap_nav ? S.ncap_nav * 2 : 64; S.nav = (NavEnt *)realloc(S.nav, S.ncap_nav * sizeof(NavEnt)); }
    NavEnt *e = &S.nav[S.nnav++]; scpy(e->label, MAX_PATH, label); scpy(e->path, MAX_PATH, path); e->root = root; e->glyph = glyph; e->sect = sect; e->depth = depth; e->exp = exp;
}
static int cmp_wstr(const void *a, const void *b) { return lstrcmpiW(*(WCHAR *const *)a, *(WCHAR *const *)b); }
static void nav_tree(const WCHAR *dir, int depth, int sect) {
    if (depth > 6) return; WCHAR pat[MAX_PATH + 4]; path_join(pat, MAX_PATH, dir, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    WCHAR **names = NULL; int n = 0, cap = 0; do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue; if (!C.show_hidden && hidden_name(fd.cFileName, fd.dwFileAttributes)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; names = (WCHAR **)realloc(names, cap * sizeof(WCHAR *)); } names[n++] = wdup(fd.cFileName); } while (FindNextFileW(h, &fd) && n < 300); FindClose(h);
    qsort(names, n, sizeof(WCHAR *), cmp_wstr);
    for (int i = 0; i < n; i++) { WCHAR p[MAX_PATH]; path_join(p, MAX_PATH, dir, names[i]); int open = sl_find(&g_tree_open, p) >= 0; nav_add(names[i], p, 0, 0, sect, depth, open ? 1 : 0); if (open) nav_tree(p, depth + 1, sect); free(names[i]); } free(names);
}
static void nav_rebuild(void) {
    const Style *st = cur_style(); WCHAR h[MAX_PATH], p[MAX_PATH]; home_dir(h); S.nnav = 0; g_nav_dirty = 0;
    nav_add(st->sec[0], L"::quick", 1, 1, 0, 0, -1);
    if (!S.nav_open[0]) { static const WCHAR *n[] = { L"Desktop", L"Downloads", L"Documents", L"Pictures", L"Music", L"Videos" }; for (int k = 0; k < 6; k++) { path_join(p, MAX_PATH, h, n[k]); if (dir_exists(p)) nav_add(n[k], p, 0, 0, 0, 0, -1); } }
    nav_add(st->sec[1], L"::recent", 1, 2, 1, 0, -1);
    if (!S.nav_open[1]) for (int k = 0; k < C.recent_folders.n && k < 8; k++) if (dir_exists(C.recent_folders.v[k])) { const WCHAR *b = base_name(C.recent_folders.v[k]); nav_add(*b ? b : C.recent_folders.v[k], C.recent_folders.v[k], 0, 0, 1, 0, -1); }
    nav_add(st->sec[2], L"::fav", 1, 3, 2, 0, -1);
    if (!S.nav_open[2]) for (int k = 0; k < C.favorites.n; k++) if (dir_exists(C.favorites.v[k])) { const WCHAR *b = base_name(C.favorites.v[k]); nav_add(*b ? b : C.favorites.v[k], C.favorites.v[k], 0, 0, 2, 0, -1); }
    nav_add(st->sec[3], L"", 1, 4, 3, 0, -1);
    if (!S.nav_open[3]) { DWORD mask = GetLogicalDrives();
        for (int k = 0; k < 26; k++) if (mask & (1u << k)) { WCHAR root[4] = { (WCHAR)('A' + k), ':', '\\', 0 }, lab[40]; UINT ty = GetDriveTypeW(root); wfmt(lab, 40, L"%lc: (%ls)", root[0], ty == DRIVE_CDROM ? L"CD" : ty == DRIVE_REMOVABLE ? L"Removable" : L"Local Disk");
            int open = C.show_tree && sl_find(&g_tree_open, root) >= 0; nav_add(lab, root, 0, 5, 3, 0, C.show_tree ? (open ? 1 : 0) : -1); if (open) nav_tree(root, 1, 3); } }
    S.nav_t = GetTickCount();
}
static int nav_hit(int x, int y) { if (y < LY.nav.top || x >= LY.nav.right) return -1; int rh = px(cur_style()->nav_row); int i = (y - LY.nav.top - px(4) + S.nav_scroll) / rh; return (i >= 0 && i < S.nnav) ? i : -1; }
static void nav_toggle(int i) {
    NavEnt *e = &S.nav[i]; int k = sl_find(&g_tree_open, e->path); if (k >= 0) sl_del(&g_tree_open, k); else sl_add(&g_tree_open, e->path); nav_rebuild(); redraw();
}
/* ---- palette / fonts ---- */
static void update_pal(void) { build_pal(&S.pal); if (S.br_input) DeleteObject(S.br_input); S.br_input = CreateSolidBrush(S.pal.input); }
static void apply_fonts(void) {
    if (S.font) DeleteObject(S.font); if (S.font_big) DeleteObject(S.font_big); if (S.font_small) DeleteObject(S.font_small);
    int size = px(C.font_size); const WCHAR *fn = C.font[0] ? C.font : L"Segoe UI"; if (C.style_fonts) fn = pick_font(cur_style()->font);
    S.font = make_font(fn, size, 400); S.font_big = make_font(fn, size + 2, 600); S.font_small = make_font(fn, size > 10 ? size - 1 : size, 400);
    if (S.e_addr) { SendMessageW(S.e_addr, WM_SETFONT, (WPARAM)S.font, TRUE); SendMessageW(S.e_search, WM_SETFONT, (WPARAM)S.font, TRUE); SendMessageW(S.e_rename, WM_SETFONT, (WPARAM)S.font, TRUE); }
    pv_font_reset();
}
/* ---- icons: custom + folder colours ---- */
typedef struct { WCHAR key[MAX_PATH]; HICON ic; HBITMAP bm; int w, h, tried; } CIcon;
static CIcon g_cicons[48]; static int g_cic_next;
static const WCHAR *icon_map_find(const WCHAR *key) { int l = lstrlenW(key); for (int i = 0; i < C.icon_map.n; i++) { const WCHAR *s = C.icon_map.v[i]; const WCHAR *bar = wcsrchr(s, '|'); if (!bar) continue; if ((int)(bar - s) == l && !_wcsnicmp(s, key, l)) return bar + 1; } return NULL; }
static CIcon *cicon_load(const WCHAR *file) {
    for (int i = 0; i < 48; i++) if (g_cicons[i].tried && !wcscmp(g_cicons[i].key, file)) return &g_cicons[i];
    CIcon *c = &g_cicons[g_cic_next]; g_cic_next = (g_cic_next + 1) % 48; if (c->ic) DestroyIcon(c->ic); if (c->bm) DeleteObject(c->bm); memset(c, 0, sizeof *c); scpy(c->key, MAX_PATH, file); c->tried = 1;
    if (ext_in(file, L".ico.")) c->ic = (HICON)LoadImageW(NULL, file, IMAGE_ICON, 64, 64, LR_LOADFROMFILE); else if (is_image(file)) c->bm = gdip_load_dib(file, 128, &c->w, &c->h);
    return c;
}
static int draw_custom_icon(HDC dc, const Item *it, int x, int y, int s) {
    if (!C.icon_map.n) return 0; const WCHAR *f = icon_map_find(it->path); if (!f) { if (it->dir) f = icon_map_find(L"::folder"); else { const WCHAR *e = PathFindExtensionW(it->name); if (*e) f = icon_map_find(e); } }
    if (!f || !path_exists(f)) return 0; CIcon *c = cicon_load(f); if (c->ic) { DrawIconEx(dc, x, y, c->ic, s, s, 0, NULL, DI_NORMAL); return 1; }
    if (c->bm) { HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, c->bm); SetStretchBltMode(dc, HALFTONE); StretchBlt(dc, x, y, s, s, m, 0, 0, c->w, c->h, SRCCOPY); SelectObject(m, o); DeleteDC(m); return 1; } return 0;
}
static u32 folder_col(const WCHAR *path, u32 def) { int l = lstrlenW(path); for (int i = 0; i < C.folder_colors.n; i++) { const WCHAR *s = C.folder_colors.v[i]; const WCHAR *bar = wcsrchr(s, '|'); if (bar && (int)(bar - s) == l && !_wcsnicmp(s, path, l)) { COLORREF c = hex_color(bar + 1, 0xFFFFFFFF); if (c != 0xFFFFFFFF) return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); } } return def; }
static int is_gfx_thumb(const WCHAR *p) { return ext_in(p, L".ico.bmp.gif.tif.tiff.webp."); }
static void draw_item_icon(HDC dc, const Item *it, int x, int y, int s, int thumbs, int *budget, int badge) {
    const Style *st = cur_style(); if (it->kind == 1) { paint_glyph(dc, 5, x, y, s, S.pal.text); return; }
    if (draw_custom_icon(dc, it, x, y, s)) return;
    if (it->dir) { paint_folder(dc, x, y, s, folder_col(it->path, st->folder)); return; }
    if (thumbs && C.thumbnails && is_image(it->path) && ((is_gfx_thumb(it->path) && C.thumb_gfx) || (!is_gfx_thumb(it->path) && C.thumb_img))) {
        HBITMAP b = thumb_get(dc, it->path, s, budget); if (b) { HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, b); BitBlt(dc, x, y, s, s, m, 0, 0, SRCCOPY); SelectObject(m, o); DeleteDC(m);
            if (badge) { HICON ic = icon_for(it->path, 0, 0); int bs = px(16); RECT bg = { x - 1, y + s - bs - 2, x + bs + 3, y + s + 1 }; rrect(dc, &bg, S.pal.bg, S.pal.border, 3, 1); if (ic) DrawIconEx(dc, x + 1, y + s - bs, ic, bs, bs, 0, NULL, DI_NORMAL); } return; } }
    HICON ic = icon_for(it->path, s > 20, 0); if (ic) DrawIconEx(dc, x, y, ic, s, s, 0, NULL, DI_NORMAL);
    if (thumbs && C.thumb_vid && ext_in(it->path, PV_VIDEO) && !pv_is_audio(it->path)) { int d = s / 3 + 2; POINT t[3] = { { x + s - d - 1, y + s - d - 1 }, { x + s - d - 1, y + s - 1 }, { x + s - 1, y + s - d / 2 - 1 } }; poly(dc, t, 3, S.pal.accent); }
}
static void pv_pick_icon(HDC dc, const RECT *body) {
    Item tmp; memset(&tmp, 0, sizeof tmp); scpy(tmp.path, MAX_PATH, g_pv.path); scpy(tmp.name, 256, base_name(g_pv.path)); tmp.dir = S.prev.dir; int isz = px(64); int bud = 0;
    if (!g_pv.path[0]) { SelectObject(dc, S.font); RECT r = *body; textr(dc, L"Select a file to preview", &r, TC, S.pal.dim); return; }
    draw_item_icon(dc, &tmp, body->left + (body->right - body->left - isz) / 2, body->top + px(24), isz, 0, &bud, 0);
}
/* ---- draw helpers ---- */
static void tb_glyph(HDC dc, int kind, const RECT *r, COLORREF c) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = (r->bottom - r->top) / 5; if (s < 3) s = 3;
    switch (kind) {
    case 0: linec(dc, cx + s, cy, cx - s, cy, c, 2); linec(dc, cx - s, cy, cx, cy - s, c, 2); linec(dc, cx - s, cy, cx, cy + s, c, 2); break;
    case 1: linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx + s, cy, cx, cy - s, c, 2); linec(dc, cx + s, cy, cx, cy + s, c, 2); break;
    case 2: linec(dc, cx, cy + s, cx, cy - s, c, 2); linec(dc, cx, cy - s, cx - s, cy, c, 2); linec(dc, cx, cy - s, cx + s, cy, c, 2); break;
    case 3: { HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, p); Arc(dc, cx - s, cy - s, cx + s, cy + s, cx + s * 9 / 10, cy + s / 2, cx + s / 2, cy - s * 9 / 10); SelectObject(dc, o); DeleteObject(p); POINT t[3] = { { cx + s / 2 - 2, cy - s - 3 }, { cx + s / 2 + 5, cy - s + 1 }, { cx + s / 2 - 1, cy - s + 5 } }; poly(dc, t, 3, c); break; }
    case 4: linec(dc, cx - s, cy - s, cx + s, cy - s, c, 2); linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx - s, cy + s, cx + s, cy + s, c, 2); break;
    case 5: { HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Ellipse(dc, cx - s, cy - s, cx + s - 1, cy + s - 1); SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(p); linec(dc, cx + s / 2, cy + s / 2, cx + s + 2, cy + s + 2, c, 2); break; }
    case 6: linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx, cy - s, cx, cy + s, c, 2); break;
    case 7: for (int i = 0; i < 3; i++) { fill4(dc, cx - s, cy - s + i * s - 1, cx - s + 3, cy - s + i * s + 2, c); fill4(dc, cx - s + 5, cy - s + i * s, cx + s, cy - s + i * s + 1, c); } break;
    case 8: for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) { RECT q = { cx - s + a * (s + 1), cy - s + b * (s + 1), cx - s + a * (s + 1) + s - 1, cy - s + b * (s + 1) + s - 1 }; fillr(dc, &q, c); } break;
    case 9: for (int a = 0; a < 2; a++) { RECT q = { cx - s, cy - s + a * (s + 1), cx - s + 4, cy - s + a * (s + 1) + s - 1 }; fillr(dc, &q, c); fill4(dc, cx - s + 6, cy - s + a * (s + 1) + 2, cx + s, cy - s + a * (s + 1) + 3, c); } break;
    case 10: { RECT a = { cx - s - 1, cy - s, cx + s + 1, cy + s }; framer(dc, &a, c); linec(dc, cx - s + 1, cy - s + 3, cx - 1, cy, c, 1); linec(dc, cx - s + 1, cy + s - 3, cx - 1, cy, c, 1); linec(dc, cx + 1, cy + s - 3, cx + s, cy + s - 3, c, 1); break; }   /* terminal */
    case 11: { HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Ellipse(dc, cx - s, cy - s, cx + s, cy + s); SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(p);
            for (int i = 0; i < 4; i++) { static const int dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, 1, 0, -1 }; linec(dc, cx + dx[i] * s, cy + dy[i] * s, cx + dx[i] * (s + 3), cy + dy[i] * (s + 3), c, 2); } break; }   /* settings */
    case 12: { RECT a = { cx - s, cy - s, cx - 1, cy - 1 }, b = { cx + 1, cy - s, cx + s, cy - 1 }, d = { cx - s, cy + 1, cx - 1, cy + s }, e = { cx + 1, cy + 1, cx + s, cy + s }; fillr(dc, &a, c); framer(dc, &b, c); framer(dc, &d, c); fillr(dc, &e, c); break; }   /* os switch */
    }
}
static void tb_btn(HDC dc, const RECT *r, int kind, int enabled, int hover) {
    const Pal *p = &S.pal; const Style *st = cur_style(); if (hover && enabled) rrect(dc, r, p->hover, p->hover, st->round > 8 ? 8 : st->round, 0);
    tb_glyph(dc, kind, r, enabled ? p->text : mixc(p->text, p->tool, 0.65));
}
static const WCHAR *special_label(const WCHAR *p) {
    const Style *st = cur_style(); if (!p[0]) return st->sec[3]; if (!wcscmp(p, L"::quick")) return st->sec[0]; if (!wcscmp(p, L"::recent")) return st->sec[1]; if (!wcscmp(p, L"::fav")) return st->sec[2]; return p;
}
static void draw_check(HDC dc, const RECT *r, int on, int hot, int round) {
    const Pal *p = &S.pal; if (on) { if (round) { ellipsec(dc, r, p->accent, p->accent, 1); } else rrect(dc, r, p->accent, p->accent, 3, 0); int cx = r->left, cy = r->top, w = r->right - r->left; COLORREF wc = contrast(p->accent);
        linec(dc, cx + w * 24 / 100, cy + w * 52 / 100, cx + w * 43 / 100, cy + w * 70 / 100, wc, 2); linec(dc, cx + w * 43 / 100, cy + w * 70 / 100, cx + w * 76 / 100, cy + w * 30 / 100, wc, 2); }
    else { if (round) ellipsec(dc, r, hot ? p->hover : p->bg, p->dim, 1); else rrect(dc, r, hot ? p->hover : p->bg, p->dim, 3, 1); }
}
static void draw_crumbs(HDC dc) {
    const Pal *p = &S.pal; Tab *t = curtab(); RECT a = LY.addr; S.ncrumbs = 0; static WCHAR segs[40][MAX_PATH], tg[40][MAX_PATH]; int n = 0;
    if (!t->path[0] || is_special(t->path)) { scpy(segs[0], MAX_PATH, special_label(t->path)); scpy(tg[0], MAX_PATH, t->path); n = 1; }
    else { const WCHAR *sp = t->path; WCHAR cum[MAX_PATH] = L""; while (*sp && n < 39) { while (*sp == '\\') sp++; if (!*sp) break; const WCHAR *e = sp; while (*e && *e != '\\') e++; int l = (int)(e - sp); if (l >= MAX_PATH) l = MAX_PATH - 1; memcpy(segs[n], sp, l * sizeof(WCHAR)); segs[n][l] = 0;
        if (n == 0) wfmt(cum, MAX_PATH, L"%ls\\", segs[0]); else path_join(cum, MAX_PATH, cum, segs[n]); scpy(tg[n], MAX_PATH, cum); n++; sp = e; } }
    int pad = px(8), sep = px(14), avail = a.right - a.left - px(16), wid[40], tot = 0; for (int i = 0; i < n; i++) { wid[i] = text_w(dc, segs[i]) + pad * 2; tot += wid[i] + (i ? sep : 0); }
    int first = 0; while (first < n - 1 && tot > avail) { tot -= wid[first] + (first ? sep : 0); first++; } int x = a.left + px(8), cy = (a.top + a.bottom) / 2;
    for (int i = first; i < n; i++) { if (i > first) { int sx = x + sep / 2; linec(dc, sx - 2, cy - 4, sx + 2, cy, p->dim, 1); linec(dc, sx + 2, cy, sx - 2, cy + 4, p->dim, 1); x += sep; }
        RECT r = { x, a.top + 2, x + wid[i], a.bottom - 2 }; if (r.right > a.right - 2) r.right = a.right - 2; if (in_rect(&r, g_mx, g_my)) rrect(dc, &r, p->hover, p->hover, 3, 0); textr(dc, segs[i], &r, TC, p->text);
        if (S.ncrumbs < 40) { S.crumbs[S.ncrumbs].r = r; scpy(S.crumbs[S.ncrumbs].path, MAX_PATH, tg[i]); S.ncrumbs++; } x += wid[i]; }
}
/* ---- caption (custom title bar for every style) ---- */
static void cap_glyph(HDC dc, int which, const RECT *r, COLORREF c) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = px(4);
    if (which == 0) linec(dc, cx - s, cy + 2, cx + s + 1, cy + 2, c, 1);
    else if (which == 1) { if (g_maxed) { RECT a = { cx - s, cy - s + 2, cx + s - 1, cy + s + 1 }, b = { cx - s + 2, cy - s, cx + s + 1, cy + s - 1 }; framer(dc, &a, c); linec(dc, b.left, b.top, b.right, b.top, c, 1); linec(dc, b.right - 1, b.top, b.right - 1, b.bottom - 2, c, 1); } else { RECT a = { cx - s, cy - s, cx + s + 1, cy + s + 1 }; framer(dc, &a, c); } }
    else { linec(dc, cx - s, cy - s, cx + s + 1, cy + s + 1, c, 1); linec(dc, cx - s, cy + s, cx + s + 1, cy - s - 1, c, 1); }
}
static void draw_caption(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); RECT c = LY.caption; int kind = LY.capkind; int act = GetForegroundWindow() == S.hwnd || (g_dlg_top != NULL);
    COLORREF bg = p->cap; if (!wcscmp(st->key, L"win7")) { vgrad(dc, &c, mixc(p->cap, RGB(255, 255, 255), 0.35), p->cap); } else fillr(dc, &c, bg);
    if (kind == 3) { fill4(dc, 0, c.bottom - 1, c.right, c.bottom, p->border); fill4(dc, 0, c.top, c.right, c.top + 1, mixc(bg, RGB(255, 255, 255), 0.4)); } else fill4(dc, 0, c.bottom - 1, c.right, c.bottom, p->line);
    SelectObject(dc, S.font); WCHAR title[MAX_PATH + 80]; Tab *t = curtab(); const WCHAR *fn = !t->path[0] ? L"This PC" : is_special(t->path) ? special_label(t->path) : (base_name(t->path)[0] ? base_name(t->path) : t->path);
    wfmt(title, MAX_PATH + 80, L"%ls  -  Files(Winlator/Wine\xD83C\xDF77)", fn); COLORREF tc = act ? p->cap_text : mixc(p->cap_text, bg, 0.45);
    if (kind == 1) {
        static const COLORREF mc[3] = { RGB(255, 95, 87), RGB(254, 188, 46), RGB(40, 200, 64) }; int hov = 0; for (int i = 0; i < 3; i++) if (in_rect(&LY.capbtn[i], g_mx, g_my)) hov = 1;
        for (int i = 0; i < 3; i++) { RECT b = LY.capbtn[i]; ellipsec(dc, &b, act || hov ? mc[i] : mixc(bg, p->dim, 0.5), act || hov ? shadec(mc[i], 0.8) : p->border, 1); if (hov) cap_glyph(dc, i == 0 ? 2 : i == 1 ? 0 : 1, &b, RGB(70, 30, 20)); }
        RECT tr = { px(80), c.top, c.right - px(80), c.bottom }; textr(dc, title, &tr, TC, tc);
    } else {
        int ix = px(10); RECT ir = { ix, c.top + (c.bottom - c.top) / 2 - px(9), ix + px(18), c.top + (c.bottom - c.top) / 2 + px(9) }; paint_folder(dc, ir.left, ir.top, px(18), st->folder);
        RECT tr = { ir.right + px(8), c.top, LY.capbtn[0].left - px(8), c.bottom }; textr(dc, title, &tr, TL, tc);
        for (int i = 0; i < 3; i++) { RECT b = LY.capbtn[i]; int hot = in_rect(&b, g_mx, g_my); int isclose = i == 2;
            if (kind == 0) { if (hot) fillr(dc, &b, isclose ? RGB(232, 17, 35) : p->hover); cap_glyph(dc, i, &b, hot && isclose ? RGB(255, 255, 255) : p->cap_text); }
            else if (kind == 2) { ellipsec(dc, &b, hot ? (isclose ? RGB(220, 60, 50) : p->hover) : mixc(bg, p->text, 0.10), hot && isclose ? RGB(220, 60, 50) : mixc(bg, p->text, 0.10), 1); cap_glyph(dc, i, &b, hot && isclose ? RGB(255, 255, 255) : p->cap_text); }
            else { fillr(dc, &b, mixc(bg, RGB(128, 128, 128), 0.25)); framer(dc, &b, p->border); if (hot) fillr(dc, &(RECT){ b.left + 1, b.top + 1, b.right - 1, b.bottom - 1 }, p->hover); cap_glyph(dc, i, &b, p->cap_text); } }
    }
}
/* ---- ribbon tabs + command strip ---- */
typedef struct { int id; const WCHAR *label; } RBI;
static int cmd_checked(int id) {
    switch (id) { case CMD_PREVIEW: return C.show_preview; case CMD_TREE: return C.show_tree; case CMD_CHECKS: return C.checkboxes; case CMD_HIDDEN: return C.show_hidden; case CMD_THUMBS: return C.thumbnails;
    case CMD_VDET: return view_mode() == 0; case CMD_VICON: return view_mode() == 1; case CMD_VTILE: return view_mode() == 2; case CMD_DARK: return C.style_dark; case CMD_DRAGDROP: return C.drag_drop; case CMD_COUNTS: return C.show_counts; } return 0;
}
static int rb_items(int tab, RBI *o) {
    static const RBI file[] = { { CMD_NEWTAB, L"New tab" }, { CMD_NEWFOLDER, L"New folder" }, { CMD_NEWFILE, L"New file" }, { CMD_CMD, L"Open CMD" }, { CMD_NEWWIN, L"Duplicate tab" }, { CMD_SETTINGS, L"Settings" }, { CMD_ABOUT, L"About" }, { CMD_EXIT, L"Exit" } };
    static const RBI home[] = { { CMD_CUT, L"Cut" }, { CMD_COPY, L"Copy" }, { CMD_PASTE, L"Paste" }, { CMD_DELETE, L"Delete" }, { CMD_RENAME, L"Rename" }, { CMD_OPEN, L"Open" }, { CMD_OPENWITH, L"Open with" }, { CMD_SELALL, L"Select all" }, { CMD_SELNONE, L"Select none" }, { CMD_INVERT, L"Invert" }, { CMD_PROPS, L"Properties" } };
    static const RBI share[] = { { CMD_COMPRESS_ZIP, L"Zip" }, { CMD_COMPRESS_7Z, L"7z" }, { CMD_COMPRESS_TGZ, L"tar.gz" }, { CMD_COMPRESS_RAR, L"RAR" }, { CMD_EXTRACT_MENU, L"Extract" }, { CMD_SHORTCUT, L"Desktop shortcut" }, { CMD_SHORTCUT_HERE, L"Shortcut here" }, { CMD_COPYPATH, L"Copy path" } };
    static const RBI view[] = { { CMD_VDET, L"Details" }, { CMD_VICON, L"Icons" }, { CMD_VTILE, L"Tiles" }, { CMD_PREVIEW, L"Preview" }, { CMD_TREE, L"Tree" }, { CMD_CHECKS, L"Checkboxes" }, { CMD_HIDDEN, L"Hidden" }, { CMD_THUMBS, L"Thumbnails" }, { CMD_SORT_NAME + 0x4000, L"Sort by" }, { CMD_ZOOM_IN, L"Zoom +" }, { CMD_ZOOM_OUT, L"Zoom -" } };
    static const RBI style[] = { { CMD_STYLES, L"Styles" }, { CMD_OSNEXT, L"Next theme" }, { CMD_DARK, L"Dark mode" }, { CMD_ACCENT0 + 0x4000, L"Accent" }, { CMD_FOLDERCOLOR, L"Folder colour" }, { CMD_SETICON, L"Custom icon" }, { CMD_FONT_PLUS, L"Font +" }, { CMD_FONT_MINUS, L"Font -" } };
    static const RBI tools[] = { { CMD_CMD, L"Open CMD" }, { CMD_ADMIN, L"Run as admin" }, { CMD_AUTO_RUN, L"Run autostart" }, { CMD_SETTINGS, L"Settings" }, { CMD_BACKUP, L"Backup" }, { CMD_RESTORE_CFG, L"Restore" }, { CMD_REFRESH, L"Refresh" } };
    const RBI *s; int n; switch (tab) { case 0: s = file; n = 8; break; case 1: s = home; n = 11; break; case 2: s = share; n = 8; break; case 3: s = view; n = 11; break; case 4: s = style; n = 8; break; default: s = tools; n = 7; }
    for (int i = 0; i < n; i++) o[i] = s[i]; return n;
}
static void rb_glyph(HDC dc, int id, const RECT *r, COLORREF c) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = px(8); id &= 0x3FFF; RECT q;
    switch (id) {
    case CMD_NEWFOLDER: paint_folder(dc, cx - s - 2, cy - s - 2, s * 2 + 4, cur_style()->folder); linec(dc, cx + s, cy + 1, cx + s + 5, cy + 1, c, 2); linec(dc, cx + s + 2, cy - 2, cx + s + 2, cy + 4, c, 2); return;
    case CMD_NEWFILE: case CMD_NEWTAB: q = (RECT){ cx - s + 2, cy - s, cx + s - 2, cy + s }; framer(dc, &q, c); linec(dc, cx + s - 1, cy - s + 4, cx + s + 4, cy - s + 4, c, 2); linec(dc, cx + s + 1, cy - s + 1, cx + s + 1, cy - s + 7, c, 2); return;
    case CMD_CMD: tb_glyph(dc, 10, r, c); return; case CMD_SETTINGS: tb_glyph(dc, 11, r, c); return; case CMD_OSNEXT: case CMD_STYLES: tb_glyph(dc, 12, r, c); return;
    case CMD_COPY: case CMD_COPYPATH: q = (RECT){ cx - s, cy - s + 2, cx + 2, cy + s + 2 }; framer(dc, &q, c); q = (RECT){ cx - 2, cy - s - 2, cx + s + 2, cy + s - 2 }; framer(dc, &q, c); return;
    case CMD_PASTE: q = (RECT){ cx - s, cy - s + 2, cx + s, cy + s + 1 }; framer(dc, &q, c); fill4(dc, cx - 4, cy - s, cx + 4, cy - s + 4, c); return;
    case CMD_CUT: linec(dc, cx - s, cy + s, cx + s, cy - s, c, 2); linec(dc, cx + s, cy + s, cx - s, cy - s, c, 2); return;
    case CMD_DELETE: q = (RECT){ cx - s + 2, cy - s + 5, cx + s - 2, cy + s + 1 }; framer(dc, &q, c); linec(dc, cx - s, cy - s + 4, cx + s + 1, cy - s + 4, c, 2); fill4(dc, cx - 3, cy - s + 1, cx + 3, cy - s + 3, c); return;
    case CMD_VDET: tb_glyph(dc, 7, r, c); return; case CMD_VICON: tb_glyph(dc, 8, r, c); return; case CMD_VTILE: tb_glyph(dc, 9, r, c); return; case CMD_REFRESH: tb_glyph(dc, 3, r, c); return;
    case CMD_PREVIEW: q = (RECT){ cx - s, cy - s, cx + s + 1, cy + s }; framer(dc, &q, c); fill4(dc, cx + 1, cy - s + 1, cx + s, cy + s - 1, c); return;
    case CMD_COMPRESS_ZIP: case CMD_COMPRESS_7Z: case CMD_COMPRESS_TGZ: case CMD_COMPRESS_RAR: q = (RECT){ cx - s + 2, cy - s, cx + s - 2, cy + s }; framer(dc, &q, c); for (int i = 0; i < 4; i++) fill4(dc, cx - 2, cy - s + 1 + i * 4, cx + 2, cy - s + 3 + i * 4, i & 1 ? c : S.pal.accent); return;
    case CMD_EXTRACT_MENU: linec(dc, cx, cy - s, cx, cy + 3, c, 2); linec(dc, cx - 4, cy - 1, cx, cy + 4, c, 2); linec(dc, cx + 4, cy - 1, cx, cy + 4, c, 2); fill4(dc, cx - s, cy + s - 2, cx + s + 1, cy + s, c); return;
    case CMD_FOLDERCOLOR: paint_folder(dc, cx - s - 2, cy - s - 2, s * 2 + 4, 0x4D8DF2); return;
    case CMD_OPEN: case CMD_OPENWITH: q = (RECT){ cx - s, cy - s + 3, cx + s - 2, cy + s }; framer(dc, &q, c); linec(dc, cx, cy, cx + s, cy - s, c, 2); return;
    }
    WCHAR l[3] = { 0, 0, 0 }; RBI tmp[16]; (void)tmp; l[0] = id == CMD_SELALL ? 'A' : id == CMD_SELNONE ? 'N' : id == CMD_INVERT ? 'I' : id == CMD_RENAME ? 'R' : id == CMD_PROPS ? 'i' : id == CMD_ADMIN ? '!' : id == CMD_EXIT ? 'x' : id == CMD_ABOUT ? '?' : id == CMD_TREE ? 'T' :
        id == CMD_HIDDEN ? 'H' : id == CMD_THUMBS ? 'P' : id == CMD_CHECKS ? 'V' : id == CMD_DARK ? 'D' : id == CMD_ACCENT0 ? 'C' : id == CMD_SHORTCUT || id == CMD_SHORTCUT_HERE ? 'S' : id == CMD_ZOOM_IN || id == CMD_FONT_PLUS ? '+' : id == CMD_ZOOM_OUT || id == CMD_FONT_MINUS ? '-' : id == CMD_SETICON ? 'I' : id == CMD_NEWWIN ? 'W' :
        id == CMD_BACKUP ? 'B' : id == CMD_RESTORE_CFG ? 'R' : id == CMD_AUTO_RUN ? 'A' : id == CMD_SORT_NAME ? 'S' : '*';
    SelectObject(dc, S.font_big); RECT t = *r; textr(dc, l, &t, TC, c); SelectObject(dc, S.font);
}
static void draw_ribbon(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); int pill = st->round >= 9; RECT rt = LY.ribtabs; fillr(dc, &rt, p->tool); SelectObject(dc, S.font);
    for (int i = 0; i < 6; i++) { RECT r = LY.ribtab[i]; int act = i == C.ribbon_tab && C.ribbon_open, hot = in_rect(&r, g_mx, g_my);
        if (pill) { if (act) rrect(dc, &r, p->sel, p->sel, st->round > 12 ? (r.bottom - r.top) / 2 : st->round, 0); else if (hot) rrect(dc, &r, p->hover, p->hover, st->round > 12 ? (r.bottom - r.top) / 2 : st->round, 0); textr(dc, RIB_TABS[i], &r, TC, act ? p->sel_text : p->text); }
        else { if (hot && !act) fillr(dc, &r, p->hover); if (act) { RECT t = r; fillr(dc, &t, p->tool); fill4(dc, r.left, r.bottom - 3, r.right, r.bottom, p->accent); } textr(dc, RIB_TABS[i], &r, TC, act ? p->text : p->dim); } }
    RECT cr = LY.ribcol; if (in_rect(&cr, g_mx, g_my)) rrect(dc, &cr, p->hover, p->hover, 3, 0); int cx = (cr.left + cr.right) / 2, cy = (cr.top + cr.bottom) / 2; if (C.ribbon_open) { linec(dc, cx - 4, cy + 2, cx, cy - 2, p->dim, 2); linec(dc, cx, cy - 2, cx + 5, cy + 2, p->dim, 2); } else { linec(dc, cx - 4, cy - 2, cx, cy + 2, p->dim, 2); linec(dc, cx, cy + 2, cx + 5, cy - 2, p->dim, 2); }
    if (!C.ribbon_open) fill4(dc, 0, rt.bottom - 1, rt.right, rt.bottom, p->line);
    g_nrb = 0; if (!LY.has_strip) return; RECT sr = LY.strip; fillr(dc, &sr, mixc(p->tool, p->bg, 0.5)); fill4(dc, 0, sr.top, sr.right, sr.top + 1, p->line); fill4(dc, 0, sr.bottom - 1, sr.right, sr.bottom, p->line);
    RBI it[16]; int n = rb_items(C.ribbon_tab, it); int x = px(8); Tab *t = curtab(); int real = t->path[0] && !is_special(t->path); int ns = sel_count(t, NULL); SelectObject(dc, S.font_small);
    for (int i = 0; i < n && g_nrb < 24; i++) { int w = tw_(it[i].label, S.font_small) + px(20); if (w < px(58)) w = px(58); RECT b = { x, sr.top + px(4), x + w, sr.bottom - px(4) }; if (b.right > sr.right - px(4)) break;
        int id = it[i].id & 0x3FFF, en = 1; if (id == CMD_PASTE) en = S.clip.n > 0 && real; else if (id == CMD_NEWFOLDER || id == CMD_NEWFILE || id == CMD_SHORTCUT_HERE) en = real; else if (id == CMD_CUT || id == CMD_COPY || id == CMD_DELETE || id == CMD_COMPRESS_ZIP || id == CMD_COMPRESS_7Z || id == CMD_COMPRESS_TGZ || id == CMD_COMPRESS_RAR || id == CMD_SHORTCUT) en = ns > 0;
        else if (id == CMD_RENAME || id == CMD_PROPS || id == CMD_COPYPATH || id == CMD_OPENWITH || id == CMD_EXTRACT_MENU || id == CMD_ADMIN || id == CMD_FOLDERCOLOR || id == CMD_SETICON) en = ns == 1 || (id == CMD_COPYPATH && ns >= 1);
        int on = cmd_checked(id) && !(it[i].id & 0x4000), hot = in_rect(&b, g_mx, g_my) && en; if (on) rrect(dc, &b, p->sel, p->accent, st->round > 8 ? 6 : st->round, 1); else if (hot) rrect(dc, &b, p->hover, p->hover, st->round > 8 ? 6 : st->round, 0);
        RECT gr = { b.left, b.top, b.right, b.top + (b.bottom - b.top) * 6 / 10 }; COLORREF fg = en ? (on ? p->sel_text : p->text) : mixc(p->text, p->bg, 0.6); rb_glyph(dc, it[i].id, &gr, fg);
        RECT lr = { b.left + 1, gr.bottom - 2, b.right - 1, b.bottom }; textr(dc, it[i].label, &lr, TC, fg); g_rb[g_nrb] = b; g_rb_id[g_nrb] = en ? it[i].id : 0; g_nrb++; x += w + px(2);
        if (i < n - 1 && (id == CMD_PROPS || id == CMD_PASTE || id == CMD_ABOUT)) {} }
    SelectObject(dc, S.font);
}
static void draw_toolbar(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = curtab(); fillr(dc, &LY.navbar, st->layout == LAY_HEADER ? p->cap : p->tool); fill4(dc, 0, LY.navbar.bottom - 1, LY.navbar.right, LY.navbar.bottom, p->line); SelectObject(dc, S.font);
    tb_btn(dc, &LY.back, 0, t->hidx > 0, in_rect(&LY.back, g_mx, g_my)); tb_btn(dc, &LY.fwd, 1, t->hidx < t->hist.n - 1, in_rect(&LY.fwd, g_mx, g_my)); tb_btn(dc, &LY.up, 2, t->path[0] != 0, in_rect(&LY.up, g_mx, g_my)); tb_btn(dc, &LY.refresh, 3, 1, in_rect(&LY.refresh, g_mx, g_my));
    tb_btn(dc, &LY.menu, 4, 1, in_rect(&LY.menu, g_mx, g_my)); tb_btn(dc, &LY.setb, 11, 1, in_rect(&LY.setb, g_mx, g_my)); tb_btn(dc, &LY.cmdb, 10, 1, in_rect(&LY.cmdb, g_mx, g_my)); tb_btn(dc, &LY.osb, 12, 1, in_rect(&LY.osb, g_mx, g_my));
    int rr = st->round > 12 ? (LY.addr.bottom - LY.addr.top) / 2 : st->round; rrect(dc, &LY.addr, p->input, p->input_border, rr, 1); if (!S.addr_edit) draw_crumbs(dc);
    rrect(dc, &LY.search, p->input, p->input_border, rr, 1); RECT mg = { LY.search.left + px(4), LY.search.top, LY.search.left + px(4) + (LY.search.bottom - LY.search.top), LY.search.bottom }; tb_glyph(dc, 5, &mg, p->dim);
    int fh = LY.search.bottom - LY.search.top, bs = px(14); RECT cb = { LY.sub.left + px(2), LY.sub.top + (fh - bs) / 2, LY.sub.left + px(2) + bs, LY.sub.top + (fh - bs) / 2 + bs }; draw_check(dc, &cb, C.search_subfolders, in_rect(&LY.sub, g_mx, g_my), 0);
    SelectObject(dc, S.font_small); RECT sl = { cb.right + px(3), LY.sub.top, LY.sub.right, LY.sub.bottom }; textr(dc, L"Sub", &sl, TL, p->text); SelectObject(dc, S.font);
    if (in_rect(&LY.sdrop, g_mx, g_my)) rrect(dc, &LY.sdrop, p->hover, p->hover, 3, 0); int cx = (LY.sdrop.left + LY.sdrop.right) / 2, cy = (LY.sdrop.top + LY.sdrop.bottom) / 2; linec(dc, cx - 4, cy - 2, cx, cy + 2, p->dim, 2); linec(dc, cx, cy + 2, cx + 5, cy - 2, p->dim, 2);
    if (C.search_ext[0]) { RECT d = { LY.sdrop.left - px(8), cy - 3, LY.sdrop.left - px(2), cy + 3 }; ellipsec(dc, &d, p->accent, p->accent, 1); }
}
static void draw_tabs(HDC dc) {
    if (S.ntabs <= 1) return; const Pal *p = &S.pal; RECT tr = LY.tabs; fillr(dc, &tr, p->cap); SelectObject(dc, S.font); int W = tr.right - tr.left, tw = px(180), pw = px(40); if (S.ntabs * tw > W - pw) tw = (W - pw) / S.ntabs; int x = tr.left + px(4);
    for (int i = 0; i < S.ntabs; i++) { Tab *t = &S.tabs[i]; RECT r = { x, tr.top + px(3), x + tw - px(2), tr.bottom }; g_tabr[i] = r; int act = i == S.cur; COLORREF bg = act ? p->bg : (in_rect(&r, g_mx, g_my) ? p->hover : p->cap);
        rrect(dc, &r, bg, bg, 4, 0); if (act) fill4(dc, r.left + 2, r.bottom - 2, r.right - 2, r.bottom, p->accent);
        const WCHAR *n = !t->path[0] ? L"This PC" : is_special(t->path) ? special_label(t->path) : (base_name(t->path)[0] ? base_name(t->path) : t->path); RECT tx = { r.left + px(8), r.top, r.right - (t->pinned ? px(8) : px(24)), r.bottom }; textr(dc, n, &tx, TL, act ? p->text : p->dim);
        if (t->pinned) { RECT d = { r.right - px(12), (r.top + r.bottom) / 2 - 3, r.right - px(6), (r.top + r.bottom) / 2 + 3 }; ellipsec(dc, &d, p->accent, p->accent, 1); }
        else { int cx = r.right - px(12), cy = (r.top + r.bottom) / 2; COLORREF cc = in_rect(&r, g_mx, g_my) ? p->text : p->dim; linec(dc, cx - 3, cy - 3, cx + 4, cy + 4, cc, 1); linec(dc, cx - 3, cy + 3, cx + 4, cy - 4, cc, 1); } x += tw; }
    g_tabplus = (RECT){ x + px(2), tr.top + px(3), x + px(2) + px(28), tr.bottom }; tb_glyph(dc, 6, &g_tabplus, in_rect(&g_tabplus, g_mx, g_my) ? p->text : p->dim); fill4(dc, 0, tr.bottom - 1, tr.right, tr.bottom, p->line);
}
static void sort_arrow(HDC dc, int cx, int cy, COLORREF c, int up) { POINT t[3]; if (up) { t[0] = (POINT){ cx - 4, cy + 2 }; t[1] = (POINT){ cx + 4, cy + 2 }; t[2] = (POINT){ cx, cy - 3 }; } else { t[0] = (POINT){ cx - 4, cy - 2 }; t[1] = (POINT){ cx + 4, cy - 2 }; t[2] = (POINT){ cx, cy + 3 }; } poly(dc, t, 3, c); }
/* drag/rubber state (declared here: used by drawing) */
static struct { int rb_on, rb_x0, rb_y0, rb_x1, rb_y1; int drag_pend, dragging, drag_x0, drag_y0, drop_item, drop_nav; SL base; } g_in;
static BOOL (WINAPI *p_alpha)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static void fill_alpha(HDC dc, const RECT *r, COLORREF c, int a) {
    int w = r->right - r->left, h = r->bottom - r->top; if (w <= 0 || h <= 0) return; if (!p_alpha) { HMODULE m = LoadLibraryW(L"msimg32.dll"); if (m) *(FARPROC *)&p_alpha = GetProcAddress(m, "AlphaBlend"); }
    if (!p_alpha) { framer(dc, r, c); return; } HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, 1, 1); HGDIOBJ o = SelectObject(m, b); SetPixel(m, 0, 0, c); BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)a, 0 }; p_alpha(dc, r->left, r->top, w, h, m, 0, 0, 1, 1, bf); SelectObject(m, o); DeleteObject(b); DeleteDC(m);
}
static void draw_content(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = curtab(); int m = view_mode(); int budget = 10; g_thumb_more = 0; fillr(dc, &LY.content, p->bg); clamp_scroll(); int rr = st->round > 8 ? 6 : st->round;
    if (m == 0) { int c[5]; col_x(LY.body.right - LY.body.left, c); RECT h = LY.hdr; fillr(dc, &h, p->hdr_bg); static const WCHAR *lab[4] = { L"Name", L"Date modified", L"Type", L"Size" }; static const WCHAR *key[4] = { L"name", L"date", L"type", L"size" }; SelectObject(dc, S.font_small);
        for (int i = 0; i < 4; i++) { RECT r = { h.left + c[i], h.top, h.left + c[i + 1], h.bottom }; RECT tr = { r.left + px(8) + (i == 0 ? gutter() : 0), r.top, r.right - px(18), r.bottom }; textr(dc, t->searching && i == 2 ? L"Location" : lab[i], &tr, TL | (i == 3 ? DT_RIGHT : 0), p->hdr);
            if (!wcscmp(C.sort, key[i])) sort_arrow(dc, r.right - px(10), (r.top + r.bottom) / 2, p->hdr, C.asc); if (i) fill4(dc, r.left, r.top + 3, r.left + 1, r.bottom - 3, p->line); }
        fill4(dc, h.left, h.bottom - 1, h.right, h.bottom, p->line); }
    HRGN rg = CreateRectRgn(LY.body.left, LY.body.top, LY.body.right, LY.body.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
    int first = 0, last = t->nv - 1; if (m == 0) { int rh = px(st->row_h); first = t->scroll / rh; last = (t->scroll + LY.body.bottom - LY.body.top) / rh; if (last >= t->nv) last = t->nv - 1; }
    int c[5]; col_x(LY.body.right - LY.body.left, c); int gt = gutter();
    for (int vi = first < 0 ? 0 : first; vi <= last; vi++) {
        Item *it = &t->it[t->view[vi]]; RECT r = item_rect(vi); if (r.bottom < LY.body.top || r.top > LY.body.bottom) continue; int hov = in_rect(&r, g_mx, g_my) && in_rect(&LY.body, g_mx, g_my) && !g_in.dragging; COLORREF bg = p->bg, fg = p->text;
        if (m == 0 && st->zebra && (vi & 1)) bg = p->zebra; if (it->sel) { bg = p->sel; fg = p->sel_text; } else if (hov) bg = p->hover;
        if (g_in.dragging && vi == g_in.drop_item) { bg = p->accent; fg = contrast(p->accent); }
        if (bg != p->bg) { if (m == 0) { RECT q = r; if (rr > 2) { q.left += px(4); q.right -= px(4); } rrect(dc, &q, bg, bg, rr > 2 ? rr : 0, 0); } else rrect(dc, &r, bg, bg, rr, 0); }
        if (vi == t->focus) { RECT q = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }; HPEN pn = CreatePen(PS_DOT, 1, it->sel ? fg : p->dim); HGDIOBJ op = SelectObject(dc, pn), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc, q.left, q.top, q.right, q.bottom); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pn); }
        if (m == 0) {
            int isz = px(18); int iy = r.top + (r.bottom - r.top - isz) / 2; SelectObject(dc, S.font); draw_item_icon(dc, it, r.left + px(10) + gt, iy, isz, 0, &budget, 0);
            RECT nr = { r.left + px(10) + gt + isz + px(8), r.top, r.left + c[1] - px(4), r.bottom }; textr(dc, it->name, &nr, TL, fg); WCHAR b[64]; RECT dr = { r.left + c[1] + px(8), r.top, r.left + c[2] - px(4), r.bottom }; fmt_date(&it->mt, b, 64); textr(dc, b, &dr, TL, it->sel ? fg : p->dim);
            RECT tr = { r.left + c[2] + px(8), r.top, r.left + c[3] - px(4), r.bottom }; textr(dc, it->type, &tr, TL, it->sel ? fg : p->dim); RECT sr = { r.left + c[3], r.top, r.left + c[4] - px(12), r.bottom };
            if (!it->dir || it->kind == 1) { if (it->size || !it->dir) fmt_bytes(it->size, b, 64); else b[0] = 0; textr(dc, b, &sr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, it->sel ? fg : p->dim); }
            if (C.checkboxes && it->kind != 1 && (it->sel || (in_rect(&r, g_mx, g_my) && in_rect(&LY.body, g_mx, g_my)))) { RECT cb = check_rect(vi); draw_check(dc, &cb, it->sel, in_rect(&cb, g_mx, g_my), 0); }
        } else if (m == 1) {
            int isz = px(C.icon_size); draw_item_icon(dc, it, r.left + (r.right - r.left - isz) / 2, r.top + px(6), isz, 1, &budget, 1); SelectObject(dc, S.font_small);
            RECT nr = { r.left + px(3), r.top + isz + px(10), r.right - px(3), r.bottom - px(2) }; textr(dc, it->name, &nr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, fg);
            if (C.checkboxes && it->kind != 1 && (it->sel || (in_rect(&r, g_mx, g_my) && in_rect(&LY.body, g_mx, g_my)))) { RECT cb = check_rect(vi); draw_check(dc, &cb, it->sel, in_rect(&cb, g_mx, g_my), 1); }
        } else {
            int isz = px(40); draw_item_icon(dc, it, r.left + px(8), r.top + (r.bottom - r.top - isz) / 2, isz, 1, &budget, 1); int x0 = r.left + isz + px(16); WCHAR b[64];
            RECT n1 = { x0, r.top + px(4), r.right - px(24), r.top + (r.bottom - r.top) / 2 + 2 }; SelectObject(dc, S.font); textr(dc, it->name, &n1, TL, fg); SelectObject(dc, S.font_small); RECT n2 = { x0, n1.bottom - 2, r.right - px(4), r.bottom - px(2) };
            if (it->dir) wfmt(b, 64, L"%ls", it->type); else { WCHAR s[32]; fmt_bytes(it->size, s, 32); wfmt(b, 64, L"%ls - %ls", it->type, s); } textr(dc, b, &n2, TL, it->sel ? fg : p->dim);
            if (C.checkboxes && it->kind != 1 && (it->sel || (in_rect(&r, g_mx, g_my) && in_rect(&LY.body, g_mx, g_my)))) { RECT cb = check_rect(vi); draw_check(dc, &cb, it->sel, in_rect(&cb, g_mx, g_my), 1); }
        }
    }
    if (!t->nv) { SelectObject(dc, S.font); RECT r = LY.body; r.top += px(30); textr(dc, S.status_msg[0] && !t->searching ? S.status_msg : (t->searching ? L"Searching..." : (t->filter[0] ? L"No matching items" : L"This folder is empty")), &r, DT_CENTER | DT_SINGLELINE, p->dim); }
    if (g_in.rb_on) { RECT q = { g_in.rb_x0 < g_in.rb_x1 ? g_in.rb_x0 : g_in.rb_x1, 0, g_in.rb_x0 < g_in.rb_x1 ? g_in.rb_x1 : g_in.rb_x0, 0 }; int y0 = g_in.rb_y0 - t->scroll + LY.body.top, y1 = g_in.rb_y1 - t->scroll + LY.body.top; q.top = y0 < y1 ? y0 : y1; q.bottom = y0 < y1 ? y1 : y0; q.left += LY.body.left; q.right += LY.body.left;
        fill_alpha(dc, &q, p->accent, 60); framer(dc, &q, p->accent); }
    SelectClipRgn(dc, NULL);
    int th = total_h(), vh = LY.sb.bottom - LY.sb.top; fillr(dc, &LY.sb, p->bg);
    if (th > vh && vh > 0) { int tl = vh * vh / th; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = th - vh; int ty = LY.sb.top + (mx ? (int)((double)t->scroll / mx * rng) : 0); RECT tr = { LY.sb.left + px(3), ty, LY.sb.right - px(3), ty + tl }; rrect(dc, &tr, S.dragsb ? mixc(p->sb_thumb, p->text, 0.3) : p->sb_thumb, p->sb_thumb, 4, 0); }
}
static void draw_nav(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = curtab(); RECT nr = LY.nav; fillr(dc, &nr, p->side); fill4(dc, nr.right - 1, nr.top, nr.right, nr.bottom, p->line);
    HRGN rg = CreateRectRgn(nr.left, nr.top, nr.right - 1, nr.bottom); SelectClipRgn(dc, rg); DeleteObject(rg); int rh = px(st->nav_row), y = nr.top + px(4) - S.nav_scroll, rr = st->round > 8 ? 6 : st->round;
    if (!S.nnav || g_nav_dirty) nav_rebuild();
    for (int i = 0; i < S.nnav; i++, y += rh) { NavEnt *e = &S.nav[i]; RECT r = { nr.left + px(4), y, nr.right - px(6), y + rh }; if (r.bottom < nr.top || r.top > nr.bottom) continue;
        int cur = wieq(e->path, t->path) && (!e->root || t->path[0] == ':' || !t->path[0]); int hov = i == S.hover_nav; COLORREF fg = p->text;
        if (g_in.dragging && i == g_in.drop_nav) { rrect(dc, &r, p->accent, p->accent, rr, 0); fg = contrast(p->accent); } else if (cur) { rrect(dc, &r, p->nav_sel, p->nav_sel, rr, 0); fg = p->nav_sel_text; } else if (hov) rrect(dc, &r, p->hover, p->hover, rr, 0);
        int ix = r.left + px(e->root ? 6 : 24) + e->depth * px(14), isz = px(16), iy = y + (rh - isz) / 2;
        if (e->root) { int open = !S.nav_open[e->sect], ax = r.left + px(8), ay = y + rh / 2; if (e->sect < 4) { if (open) { linec(dc, ax - 3, ay - 2, ax, ay + 2, p->dim, 1); linec(dc, ax, ay + 2, ax + 4, ay - 2, p->dim, 1); } else { linec(dc, ax - 2, ay - 3, ax + 2, ay, p->dim, 1); linec(dc, ax + 2, ay, ax - 2, ay + 4, p->dim, 1); } }
            ix = r.left + px(20); SelectObject(dc, S.font_big); paint_glyph(dc, e->glyph, ix, iy, isz, cur ? fg : p->accent); }
        else { SelectObject(dc, S.font); if (e->exp >= 0) { int ax = ix - px(12), ay = y + rh / 2; if (e->exp) { linec(dc, ax - 3, ay - 2, ax, ay + 2, p->dim, 1); linec(dc, ax, ay + 2, ax + 4, ay - 2, p->dim, 1); } else { linec(dc, ax - 2, ay - 3, ax + 2, ay, p->dim, 1); linec(dc, ax + 2, ay, ax - 2, ay + 4, p->dim, 1); } }
            if (e->glyph == 5) paint_glyph(dc, 5, ix, iy, isz, fg); else paint_folder(dc, ix, iy, isz, folder_col(e->path, st->folder)); }
        RECT tx = { ix + isz + px(8), y, r.right - px(4), y + rh }; textr(dc, e->label, &tx, TL, fg); }
    SelectClipRgn(dc, NULL);
}
static void draw_preview(HDC dc) {
    if (LY.preview.right - LY.preview.left < 2) return; const Pal *p = &S.pal; RECT pr = LY.preview; fillr(dc, &pr, p->side); fill4(dc, pr.left, pr.top, pr.left + 1, pr.bottom, p->line);
    if (in_rect(&LY.split, g_mx, g_my)) fill4(dc, LY.split.left + px(2), LY.split.top, LY.split.left + px(4), LY.split.bottom, p->accent);
    HRGN rg = CreateRectRgn(pr.left + 1, pr.top, pr.right, pr.bottom); SelectClipRgn(dc, rg); DeleteObject(rg); PV *v = &g_pv;
    if (!S.prev.path[0]) { SelectObject(dc, S.font); textr(dc, L"Select a file to preview", &pr, TC, p->dim); SelectClipRgn(dc, NULL); g_pc_panel.nb = 0; return; }
    pv_layout(&g_pc_panel, pr, 0); WCHAR meta[220], sz[32], dt[40]; fmt_bytes(S.prev.size, sz, 32); fmt_date(&S.prev.mt, dt, 40);
    if (S.prev.dir) wfmt(meta, 220, L"%ls\n%ls", S.prev.type, dt); else wfmt(meta, 220, L"%ls - %ls\n%ls%ls%ls", S.prev.type, sz, dt, v->info[0] ? L"\n" : L"", v->info);
    pv_paint(dc, &g_pc_panel, base_name(S.prev.path), meta);
    if (v->kind == PK_MEDIA && g_vid.open && g_vid.video && !g_fullwnd) { RECT vr = g_pc_panel.body; vid_place(&vr); }
    SelectClipRgn(dc, NULL);
}
static void draw_status(HDC dc) {
    const Pal *p = &S.pal; Tab *t = curtab(); RECT r = LY.status; fillr(dc, &r, p->status); fill4(dc, r.left, r.top, r.right, r.top + 1, p->line); SelectObject(dc, S.font_small); u64 by; int ns = sel_count(t, &by); WCHAR m[400], b[40], b2[40];
    int nd = 0, nf = 0; u64 tot = 0; for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; if (x->dir) nd++; else { nf++; tot += x->size; } }
    if (S.status_msg[0] && (t->searching || InterlockedCompareExchange(&S.busy, 0, 0))) scpy(m, 400, S.status_msg);
    else { fmt_bytes(tot, b, 40); if (C.show_counts) wfmt(m, 400, L"%d folder%ls, %d file%ls  -  %ls", nd, nd == 1 ? L"" : L"s", nf, nf == 1 ? L"" : L"s", b); else wfmt(m, 400, L"%d item%ls", t->nv, t->nv == 1 ? L"" : L"s");
        if (ns) { fmt_bytes(by, b2, 40); wfmt(m + lstrlenW(m), 100, L"    |    %d selected%ls%ls", ns, by ? L"  " : L"", by ? b2 : L""); }
        if (!ns && t->path[0] && !is_special(t->path)) { u64 tt; u64 fr = free_space(t->path, &tt); if (tt) { fmt_bytes(fr, b2, 40); wfmt(m + lstrlenW(m), 100, L"    |    %ls free", b2); } } }
    RECT tr = { r.left + px(12), r.top, LY.views[0].left - px(8), r.bottom }; textr(dc, m, &tr, TL, p->dim); int cur = view_mode();
    for (int i = 0; i < 3; i++) { RECT v = LY.views[i]; if (cur == i) rrect(dc, &v, p->sel, p->sel, 3, 0); else if (in_rect(&v, g_mx, g_my)) rrect(dc, &v, p->hover, p->hover, 3, 0); tb_glyph(dc, 7 + i, &v, cur == i ? p->sel_text : p->dim); }
}
static void draw_client(HDC dc, int W, int H) {
    const Pal *p = &S.pal; RECT all = { 0, 0, W, H }; fillr(dc, &all, p->bg); do_layout(W, H); SetBkMode(dc, TRANSPARENT);
    draw_caption(dc); draw_ribbon(dc); draw_toolbar(dc); draw_tabs(dc); draw_nav(dc); draw_content(dc); draw_preview(dc); draw_status(dc);
    if (!g_maxed) { RECT wb = all; HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH); (void)nb; framer(dc, &wb, p->border); }
    if (g_in.dragging) { WCHAR t[300]; u64 by; int ns = sel_count(curtab(), &by); const WCHAR *dest = NULL; if (g_in.drop_item >= 0 && g_in.drop_item < curtab()->nv) dest = curtab()->it[curtab()->view[g_in.drop_item]].name; else if (g_in.drop_nav >= 0 && g_in.drop_nav < S.nnav) dest = S.nav[g_in.drop_nav].label;
        wfmt(t, 300, L"%ls %d item%ls%ls%ls", GetKeyState(VK_CONTROL) < 0 ? L"Copy" : L"Move", ns, ns == 1 ? L"" : L"s", dest ? L" to " : L"", dest ? dest : L""); SelectObject(dc, S.font); int tw = text_w(dc, t) + px(20), th = px(26); RECT r = { g_mx + px(14), g_my + px(10), g_mx + px(14) + tw, g_my + px(10) + th };
        if (r.right > W) { r.left -= r.right - W; r.right = W; } rrect(dc, &r, p->accent, p->accent, 5, 0); textr(dc, t, &r, TC, contrast(p->accent)); }
    if (S.toast[0] && GetTickCount() - S.toast_t < 3000) { SelectObject(dc, S.font); int tw = text_w(dc, S.toast) + px(36), th = px(34); RECT r = { (W - tw) / 2, LY.status.top - th - px(14), (W + tw) / 2, LY.status.top - px(14) }; rrect(dc, &r, p->dark ? RGB(70, 70, 70) : RGB(40, 40, 40), p->border, 8, 1); textr(dc, S.toast, &r, TC, RGB(255, 255, 255)); }
}

/* ========================================================================== */
/* COMMANDS, MENUS AND DIALOGS                                                */
/* ========================================================================== */
static void place_edits(void);
static void style_changed(void) { update_pal(); apply_fonts(); set_title(); g_nav_dirty = 1; cfg_save(); place_edits(); redraw(); }
static int exe_path(WCHAR *o, int cap) { return (int)GetModuleFileNameW(NULL, o, cap); }
static int run_at_login_enabled(void) { HKEY k; int on = C.run_at_login; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) { DWORD ty, n = 0; on = RegQueryValueExW(k, L"WinShellFM", NULL, &ty, NULL, &n) == ERROR_SUCCESS; RegCloseKey(k); } return on; }
static void set_run_at_login(int on) {
    C.run_at_login = on; HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) { WCHAR e[MAX_PATH + 4], q[MAX_PATH + 4]; exe_path(e, MAX_PATH); wfmt(q, MAX_PATH + 4, L"\"%ls\"", e); RegSetValueExW(k, L"WinShellFM", 0, REG_SZ, (BYTE *)q, (lstrlenW(q) + 1) * sizeof(WCHAR)); } else RegDeleteValueW(k, L"WinShellFM"); RegCloseKey(k);
}
static void run_autostart(int force) {
    if (S.autostart_done && !force) return; S.autostart_done = 1; if (!force && !C.autostart_enabled) return;
    for (int i = 0; i < C.autostart_apps.n; i++) { const WCHAR *p = C.autostart_apps.v[i]; if (path_exists(p)) { WCHAR d[MAX_PATH]; parent_of(p, d); ShellExecuteW(NULL, L"open", p, NULL, d[0] ? d : NULL, SW_SHOWNORMAL); } else fm_log("autostart: missing %ls", p); }
}
static void backup_settings(int restore) {
    WCHAR f[MAX_PATH] = L"winshell_fm_backup.json"; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = g_dlg_top ? g_dlg_top : S.hwnd; o.lpstrFilter = L"JSON (*.json)\0*.json\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.lpstrDefExt = L"json";
    if (restore) { o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST; if (!GetOpenFileNameW(&o)) return; } else { o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; if (!GetSaveFileNameW(&o)) return; }
    if (restore) { if (!CopyFileW(f, CONFIG_PATH, FALSE)) { toast(L"Could not restore settings"); return; } cfg_load(); update_pal(); apply_fonts(); place_edits(); g_nav_dirty = 1; reload(0); toast(L"Settings restored"); }
    else { cfg_save(); toast(CopyFileW(CONFIG_PATH, f, FALSE) ? L"Settings saved" : L"Could not save settings"); }
}
static void add_autostart(void) {
    WCHAR f[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = g_dlg_top ? g_dlg_top : S.hwnd; o.lpstrFilter = L"Programs (*.exe;*.bat;*.cmd;*.lnk)\0*.exe;*.bat;*.cmd;*.lnk\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o) && sl_find(&C.autostart_apps, f) < 0) { sl_add(&C.autostart_apps, f); cfg_save(); toast(L"Added to autostart"); }
}
static void sort_by(const WCHAR *k) { if (!wcscmp(C.sort, k)) C.asc = !C.asc; else { scpy(C.sort, 16, k); C.asc = 1; } cfg_save(); rebuild_view(curtab()); redraw(); }
static void set_pref_redraw(void) { cfg_save(); rebuild_view(curtab()); clamp_scroll(); place_edits(); redraw(); }
/* ---- shortcuts, run as admin ---- */
static int make_lnk(const WCHAR *target, const WCHAR *lnk) {
    IShellLinkW *sl = NULL; if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)) || !sl) return 0; int ok = 0; IPersistFile *pf = NULL; WCHAR dir[MAX_PATH];
    sl->lpVtbl->SetPath(sl, target); if (dir_exists(target)) lstrcpyW(dir, target); else parent_of(target, dir); if (dir[0]) sl->lpVtbl->SetWorkingDirectory(sl, dir);
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) && pf) { ok = SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE)); pf->lpVtbl->Release(pf); } sl->lpVtbl->Release(sl); return ok;
}
static void create_shortcuts(int desktop) {
    Tab *t = curtab(); SL s = { 0 }; collect_sel(t, &s); if (!s.n) { toast(L"Select a file or folder first"); free(s.v); return; } WCHAR dd[MAX_PATH]; int okc = 0;
    if (desktop) { if (!SHGetSpecialFolderPathW(NULL, dd, CSIDL_DESKTOPDIRECTORY, TRUE)) { WCHAR h[MAX_PATH]; home_dir(h); path_join(dd, MAX_PATH, h, L"Desktop"); CreateDirectoryW(dd, NULL); } } else scpy(dd, MAX_PATH, t->path);
    for (int i = 0; i < s.n; i++) { WCHAR nm[MAX_PATH], stem[MAX_PATH], out[MAX_PATH]; scpy(stem, MAX_PATH, base_name(s.v[i])); wfmt(nm, MAX_PATH, L"%ls - Shortcut.lnk", stem);
        if (desktop) wfmt(nm, MAX_PATH, L"%ls.lnk", stem); path_join(out, MAX_PATH, dd, nm); if (path_exists(out)) unique_new(dd, stem, L".lnk", out, MAX_PATH); if (make_lnk(s.v[i], out)) okc++; }
    for (int i = 0; i < s.n; i++) free(s.v[i]); free(s.v);
    WCHAR m[200]; if (okc) wfmt(m, 200, desktop ? L"Shortcut created on the Desktop (%d)" : L"Shortcut created (%d)", okc); else scpy(m, 200, L"Could not create the shortcut"); toast(m); if (!desktop) reload(1);
}
static void run_as_admin(Item *it) { if (!it) return; WCHAR d[MAX_PATH]; parent_of(it->path, d); HINSTANCE r = ShellExecuteW(S.hwnd, L"runas", it->path, NULL, d[0] ? d : NULL, SW_SHOWNORMAL); if ((INT_PTR)r <= 32) toast(L"Could not run as administrator"); }
/* ---- folder colours / custom icons ---- */
static void set_map(SL *l, const WCHAR *key, const WCHAR *val) {
    int kl = lstrlenW(key); for (int i = 0; i < l->n; i++) { const WCHAR *bar = wcsrchr(l->v[i], '|'); if (bar && (int)(bar - l->v[i]) == kl && !_wcsnicmp(l->v[i], key, kl)) { sl_del(l, i); break; } }
    if (val && val[0]) { WCHAR *e = (WCHAR *)malloc((kl + lstrlenW(val) + 2) * sizeof(WCHAR)); wsprintfW(e, L"%ls|%ls", key, val); sl_add(l, e); free(e); } cfg_save(); redraw();
}
static const u32 FOLDER_PALETTE[] = { 0xF8D775, 0xE95420, 0xE74C3C, 0xF39C12, 0x2ECC71, 0x1ABC9C, 0x3498DB, 0x8E5BD8, 0xE91E8C, 0x95A5A6, 0x34495E, 0x8B5A2B };
static const WCHAR *FOLDER_PAL_NAMES[] = { L"Yellow", L"Orange", L"Red", L"Amber", L"Green", L"Teal", L"Blue", L"Purple", L"Pink", L"Grey", L"Slate", L"Brown" };
static void apply_folder_color(int idx) {
    Tab *t = curtab(); SL s = { 0 }; collect_sel(t, &s); WCHAR hx[16]; if (idx >= 0) wfmt(hx, 16, L"#%06X", FOLDER_PALETTE[idx]);
    for (int i = 0; i < s.n; i++) if (dir_exists(s.v[i])) set_map(&C.folder_colors, s.v[i], idx >= 0 ? hx : NULL); for (int i = 0; i < s.n; i++) free(s.v[i]); free(s.v);
}
static void pick_icon(int scope) {   /* 0 this item, 1 all of this extension, 2 all folders, 3 remove for item/ext */
    Tab *t = curtab(); Item *f = focus_item(t); if (!f && scope != 2) return; WCHAR key[MAX_PATH];
    if (scope == 2) lstrcpyW(key, L"::folder"); else if (scope == 1) { const WCHAR *e = PathFindExtensionW(f->name); if (!*e) { toast(L"This file has no extension"); return; } lstrcpyW(key, e); } else lstrcpyW(key, f->path);
    WCHAR file[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = S.hwnd; o.lpstrFilter = L"Icons and images\0*.ico;*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return; set_map(&C.icon_map, key, file); memset(g_cicons, 0, sizeof g_cicons); toast(L"Custom icon set");
}
static void clear_icon(void) {
    Tab *t = curtab(); Item *f = focus_item(t); if (!f) return; set_map(&C.icon_map, f->path, NULL); const WCHAR *e = PathFindExtensionW(f->name); if (*e) set_map(&C.icon_map, e, NULL); if (f->dir) set_map(&C.icon_map, L"::folder", NULL); toast(L"Custom icon removed");
}
/* ---- Open with ---- */
typedef struct { WCHAR name[64], path[MAX_PATH]; } OWApp;
static OWApp g_ow[300]; static int g_now;
static void ow_add(const WCHAR *path, const WCHAR *name) {
    if (g_now >= 300 || !path[0] || !path_exists(path)) return; for (int i = 0; i < g_now; i++) if (wieq(g_ow[i].path, path)) return; OWApp *a = &g_ow[g_now++]; scpy(a->path, MAX_PATH, path);
    if (name && name[0]) scpy(a->name, 64, name); else { scpy(a->name, 64, base_name(path)); WCHAR *e = PathFindExtensionW(a->name); if (*e) *e = 0; if (a->name[0] >= 'a' && a->name[0] <= 'z') a->name[0] -= 32; }
}
static int cmp_ow(const void *a, const void *b) { return lstrcmpiW(((const OWApp *)a)->name, ((const OWApp *)b)->name); }
static void ow_collect(void) {
    g_now = 0; WCHAR win[MAX_PATH], p[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH); static const WCHAR *sys[] = { L"notepad.exe", L"write.exe", L"mspaint.exe", L"explorer.exe", L"regedit.exe", L"wordpad.exe", L"calc.exe" };
    for (int i = 0; i < 7; i++) { path_join(p, MAX_PATH, win, sys[i]); ow_add(p, NULL); } WCHAR s32[MAX_PATH]; GetSystemDirectoryW(s32, MAX_PATH); for (int i = 0; i < 7; i++) { path_join(p, MAX_PATH, s32, sys[i]); ow_add(p, NULL); }
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int r = 0; r < 2; r++) { HKEY k; if (RegOpenKeyExW(roots[r], L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0; i < 400; i++) { WCHAR sub[260]; DWORD sl = 260; if (RegEnumKeyExW(k, i, sub, &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break; HKEY k2; if (RegOpenKeyExW(k, sub, 0, KEY_READ, &k2) != ERROR_SUCCESS) continue;
            WCHAR v[MAX_PATH]; DWORD vl = sizeof v, ty; if (RegQueryValueExW(k2, NULL, NULL, &ty, (BYTE *)v, &vl) == ERROR_SUCCESS && (ty == REG_SZ || ty == REG_EXPAND_SZ)) { WCHAR x[MAX_PATH], q[MAX_PATH]; ExpandEnvironmentStringsW(v, x, MAX_PATH); scpy(q, MAX_PATH, x); PathUnquoteSpacesW(q); if (ext_in(q, L".exe.")) ow_add(q, NULL); } RegCloseKey(k2); } RegCloseKey(k); }
    for (int i = 0; i < C.assoc.n; i++) { const WCHAR *b = wcschr(C.assoc.v[i], '|'); if (b) ow_add(b + 1, NULL); } qsort(g_ow, g_now, sizeof(OWApp), cmp_ow);
}
static void launch_with(const WCHAR *app, const WCHAR *file) { WCHAR a[MAX_PATH + 4], d[MAX_PATH]; wfmt(a, MAX_PATH + 4, L"\"%ls\"", file); parent_of(file, d); HINSTANCE r = ShellExecuteW(S.hwnd, L"open", app, a, d[0] ? d : NULL, SW_SHOWNORMAL); if ((INT_PTR)r <= 32) toast(L"Could not start that program"); else sl_front(&C.recent_files, file, 20); }
static struct { WCHAR file[MAX_PATH], browse[MAX_PATH]; int always; } g_owd;
static void ow_build(Dlg *d) {
    WCHAR n[MAX_PATH + 40]; wfmt(n, MAX_PATH + 40, L"Choose a program to open \"%ls\"", base_name(g_owd.file)); dlg_note(d, n); for (int i = 0; i < g_now; i++) dlg_item(d, 1000 + i, g_ow[i].name, g_ow[i].path, 0);
    const WCHAR *e = PathFindExtensionW(g_owd.file); if (*e) { WCHAR t[100]; wfmt(t, 100, L"Always use the chosen program for %ls files", e); dlg_check(d, 5, t, &g_owd.always); }
}
static int ow_act(Dlg *d, Row *r, int id) {
    (void)d; (void)r; if (id == 9001) return 1; if (id == 9002) { WCHAR f[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = d->hw; o.lpstrFilter = L"Programs (*.exe;*.bat;*.cmd)\0*.exe;*.bat;*.cmd\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&o)) { scpy(g_owd.browse, MAX_PATH, f); return 1; } return 0; } return id >= 1000 ? 1 : 0;
}
static void open_with(Item *it) {
    if (!it || it->dir) return; ow_collect(); memset(&g_owd, 0, sizeof g_owd); scpy(g_owd.file, MAX_PATH, it->path); Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, L"Open with"); d.w = px(520); d.h = px(560); d.build = ow_build; d.act = ow_act; dlg_foot(&d, 9002, L"Browse...", 0); dlg_foot(&d, 9001, L"Cancel", 0);
    int rc = dlg_run(&d); WCHAR app[MAX_PATH] = L""; if (rc >= 1000 && rc - 1000 < g_now) scpy(app, MAX_PATH, g_ow[rc - 1000].path); else if (rc == 9002) scpy(app, MAX_PATH, g_owd.browse); if (!app[0]) return;
    if (g_owd.always) { const WCHAR *e = PathFindExtensionW(it->path); if (*e) set_map(&C.assoc, e, app); } launch_with(app, it->path);
}
/* ---- Properties ---- */
static void dir_size_walk(const WCHAR *dir, u64 *sz, int *nf, int *nd, int *budget, int depth) {
    if (depth > 20 || *budget <= 0) return; WCHAR pat[MAX_PATH + 4]; path_join(pat, MAX_PATH, dir, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue; (*budget)--; if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { (*nd)++; WCHAR c[MAX_PATH]; path_join(c, MAX_PATH, dir, fd.cFileName); dir_size_walk(c, sz, nf, nd, budget, depth + 1); } else { (*nf)++; *sz += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; } } while (*budget > 0 && FindNextFileW(h, &fd)); FindClose(h);
}
static Item g_propit;
static void props_build(Dlg *d) {
    Item *it = &g_propit; WCHAR b[300], s[40]; dlg_kv(d, L"Name", it->name); dlg_kv(d, L"Type", it->type); WCHAR par[MAX_PATH]; parent_of(it->path, par); dlg_kv(d, L"Location", par[0] ? par : L"-"); dlg_kv(d, L"Full path", it->path);
    if (it->dir) { u64 sz = 0; int nf = 0, nd = 0, bud = 30000; dir_size_walk(it->path, &sz, &nf, &nd, &bud, 0); fmt_bytes(sz, s, 40); wfmt(b, 300, L"%ls (%I64u bytes)%ls", s, (unsigned long long)sz, bud <= 0 ? L" - partial" : L""); dlg_kv(d, L"Size", b); wfmt(b, 300, L"%d files, %d folders", nf, nd); dlg_kv(d, L"Contains", b); }
    else { fmt_bytes(it->size, s, 40); wfmt(b, 300, L"%ls (%I64u bytes)", s, (unsigned long long)it->size); dlg_kv(d, L"Size", b); }
    WIN32_FILE_ATTRIBUTE_DATA ad; if (GetFileAttributesExW(it->path, GetFileExInfoStandard, &ad)) { fmt_date(&ad.ftCreationTime, b, 300); dlg_kv(d, L"Created", b); fmt_date(&ad.ftLastWriteTime, b, 300); dlg_kv(d, L"Modified", b); fmt_date(&ad.ftLastAccessTime, b, 300); dlg_kv(d, L"Accessed", b);
        wfmt(b, 300, L"%ls%ls%ls%ls", ad.dwFileAttributes & FILE_ATTRIBUTE_READONLY ? L"Read-only  " : L"", ad.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN ? L"Hidden  " : L"", ad.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM ? L"System  " : L"", ad.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE ? L"Archive" : L""); dlg_kv(d, L"Attributes", b[0] ? b : L"Normal"); }
}
static int props_act(Dlg *d, Row *r, int id) { (void)d; (void)r; return id == 1 ? 1 : 0; }
static void show_properties(Item *it) { if (!it || it->kind == 1) return; g_propit = *it; Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, L"Properties"); d.w = px(560); d.h = px(430); d.build = props_build; d.act = props_act; dlg_foot(&d, 1, L"Close", 1); dlg_run(&d); }
/* ---- About (first run + every 30 days) ---- */
#define FM_GITHUB L"https://github.com/Montydev786/Winlator-Wine-Graphical-Interface-Components-applications"
static int today_days(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; return (int)(u.QuadPart / 864000000000ULL); }
static void about_build(Dlg *d) {
    dlg_head(d, L"Files(Winlator/Wine\xD83C\xDF77)"); dlg_note(d, L"A themeable file manager for Winlator / Wine, written in plain C (Win32 + GDI)."); dlg_kv(d, L"Application", L"Files(Winlator/Wine\xD83C\xDF77)"); dlg_kv(d, L"Made & developed by", L"Monty"); dlg_kv(d, L"GitHub", FM_GITHUB);
    dlg_note(d, L"Support us and Give a \x2B50 Star."); dlg_kv(d, L"Looks", L"Windows 10/11/7/XP, macOS, Ubuntu, Zorin, Mint, KDE, ChromeOS, XFCE, Total Commander, Midnight Commander, Haiku, Cyber Neon, Directory Opus");
}
static int about_act(Dlg *d, Row *r, int id) { (void)d; (void)r; if (id == 2) { ShellExecuteW(NULL, L"open", FM_GITHUB, NULL, NULL, SW_SHOWNORMAL); return 0; } return id == 1 ? 1 : 0; }
static void show_about(void) { Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, L"About"); d.w = px(600); d.h = px(470); d.build = about_build; d.act = about_act; dlg_foot(&d, 2, L"\x2B50 Open GitHub", 1); dlg_foot(&d, 1, L"Close", 0); dlg_run(&d); }
/* ---- Styles dialog ---- */
static int g_st_acc, g_st_theme;
static void styles_build(Dlg *d) {
    dlg_head(d, L"Choose a look"); dlg_note(d, L"Every look gets its own window frame, title bar buttons, menus, dialogs and file-list style.");
    for (int k = 0; k < (int)NSTYLES; k++) { int i = STYLE_ORDER[k]; const Style *s = &OS_STYLES[i]; Row *r = dlg_row(d, R_SWATCH, 1000 + i, s->label); scpy(r->sub, 300, s->info); r->sel = i == style_index(C.os_style);
        int dk = C.style_dark && s->has_dark; const u32 *v = dk ? s->dark : s->light; if (i == 0) { const Theme *t = theme_by_name(C.theme); r->chips[0] = t->bg; r->chips[1] = t->alt; r->chips[2] = t->dark ? 0xFFFFFF : 0; r->chips[3] = 0x0078D4; r->chips[4] = s->folder; r->chips[5] = t->dark ? 0x3A3A3A : 0xE5E5E5; }
        else { r->chips[0] = v[0]; r->chips[1] = v[1]; r->chips[2] = v[5]; r->chips[3] = v[13]; r->chips[4] = v[15]; r->chips[5] = s->folder; } }
    dlg_head(d, L"Options"); dlg_check(d, 1, L"Dark variant (when the look has one)", &C.style_dark); dlg_check(d, 2, L"Use the look's own fonts", &C.style_fonts);
    static const WCHAR *const accn[8] = { L"Blue", L"Sky", L"Orange", L"Sage", L"Azure", L"Deep blue", L"Red", L"Violet" }; dlg_choice(d, 3, L"Accent colour", &g_st_acc, accn, 8);
    if (is_win10()) { static const WCHAR *const thn[6] = { L"light", L"dark", L"amoled", L"midnight", L"ocean", L"nord" }; dlg_choice(d, 4, L"Windows 10 theme", &g_st_theme, thn, 6); }
    dlg_btn(d, 5, L"Reset accent to the look's default");
}
static int styles_act(Dlg *d, Row *r, int id) {
    (void)d; (void)r;
    if (id >= 1000 && id < 1000 + (int)NSTYLES) { scpy(C.os_style, 16, OS_STYLES[id - 1000].key); style_changed(); return 0; } if (id == 3) { if (is_win10()) scpy(C.accent, 16, FM_ACCENTS[g_st_acc]); else scpy(C.style_accent, 16, FM_ACCENTS[g_st_acc]); style_changed(); return 0; }
    if (id == 4) { scpy(C.theme, 16, FM_THEMES[g_st_theme].name); style_changed(); return 0; } if (id == 5) { C.style_accent[0] = 0; style_changed(); return 0; } if (id == 1 || id == 2) { style_changed(); return 0; } return id == 9 ? 1 : 0;
}
static void show_styles(void) {
    g_st_acc = 0; for (int i = 0; i < 8; i++) if (wieq(is_win10() ? C.accent : C.style_accent, FM_ACCENTS[i])) g_st_acc = i; g_st_theme = 1; for (int i = 0; i < (int)NTHEMES; i++) if (wieq(C.theme, FM_THEMES[i].name)) g_st_theme = i;
    Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, L"Styles & Look"); d.w = px(720); d.h = px(660); d.build = styles_build; d.act = styles_act; dlg_foot(&d, 9, L"Done", 1); dlg_run(&d); style_changed();
}
/* ---- Settings dialog ---- */
static struct { int view, sortk, dirs, scale, login, theme; } ST;
static const WCHAR *const SZ_NAMES[4] = { L"Min", L"Mid", L"Large", L"Full screen" };
static void settings_commit(void) {
    static const WCHAR *vv[3] = { L"details", L"icons", L"tiles" }, *sk[4] = { L"name", L"date", L"type", L"size" }, *dd[3] = { L"first", L"last", L"mixed" };
    scpy(C.view, 16, vv[ST.view]); scpy(C.sort, 16, sk[ST.sortk]); scpy(C.sort_dirs, 8, dd[ST.dirs]); C.scale = ST.scale / 100.0; if (C.scale < 0.6) C.scale = 0.6; if (C.scale > 2.5) C.scale = 2.5; if (run_at_login_enabled() != ST.login) set_run_at_login(ST.login);
    update_pal(); apply_fonts(); set_title(); g_nav_dirty = 1; cfg_save(); rebuild_view(curtab()); clamp_scroll(); place_edits(); preview_update(); redraw();
}
static void settings_build(Dlg *d) {
    static const WCHAR *const vw[3] = { L"Details", L"Icons", L"Tiles" }, *const sk[4] = { L"Name", L"Date modified", L"Type", L"Size" }, *const dd[3] = { L"Folders first", L"Folders last", L"Mixed" }, *const vm[3] = { L"In the preview panel (paused)", L"Open full screen", L"Do not preview (play externally)" };
    switch (d->tab) {
    case 0: dlg_head(d, L"General"); dlg_check(d, 1, L"Restore tabs on start", &C.restore_session); dlg_check(d, 2, L"Show hidden files", &C.show_hidden); dlg_check(d, 3, L"Show folder / file counts and total size in the status bar", &C.show_counts);
        dlg_check(d, 4, L"Show \"Run as administrator\" in the context menu", &C.admin_menu); dlg_check(d, 5, L"Start with Windows (Run key)", &ST.login); dlg_check(d, 6, L"Keep the command strip (ribbon) open", &C.ribbon_open); break;
    case 1: dlg_head(d, L"Appearance"); dlg_btn(d, 20, L"Choose look (Styles dialog)..."); dlg_check(d, 21, L"Dark variant", &C.style_dark); dlg_check(d, 22, L"Use the look's own fonts", &C.style_fonts); dlg_spin(d, 23, L"Interface zoom", &ST.scale, 60, 250, 10, L"%"); dlg_spin(d, 24, L"Font size", &C.font_size, 9, 28, 1, L"pt");
        dlg_spin(d, 25, L"Icon size (icons view)", &C.icon_size, 32, 160, 16, L"px"); dlg_btn(d, 26, L"Document font..."); dlg_btn(d, 27, L"Add a font file for documents..."); break;
    case 2: dlg_head(d, L"View & files"); dlg_choice(d, 40, L"Default view", &ST.view, vw, 3); dlg_choice(d, 41, L"Sort by", &ST.sortk, sk, 4); dlg_check(d, 42, L"Ascending order", &C.asc); dlg_choice(d, 43, L"Folders", &ST.dirs, dd, 3); dlg_check(d, 44, L"Tree view in the navigation pane", &C.show_tree);
        dlg_check(d, 45, L"Selection tick boxes (hover tick in list, corner tick in grid)", &C.checkboxes); dlg_head(d, L"Thumbnails"); dlg_check(d, 46, L"Show thumbnails", &C.thumbnails); dlg_check(d, 47, L"Photos (jpg / png)", &C.thumb_img); dlg_check(d, 48, L"Video (play badge)", &C.thumb_vid); dlg_check(d, 49, L"Graphics (ico / bmp / gif / webp)", &C.thumb_gfx);
        dlg_head(d, L"Mouse"); dlg_check(d, 50, L"Drag files to move (hold Ctrl to copy)", &C.drag_drop); dlg_check(d, 51, L"Rubber-band (drag box) selection", &C.rubber); break;
    case 3: dlg_head(d, L"Preview panel"); dlg_check(d, 60, L"Show preview panel", &C.show_preview); dlg_choice(d, 61, L"Video / audio", &C.video_mode, vm, 3);
        dlg_head(d, L"Panel size per file type (drag the divider to fine-tune)"); dlg_choice(d, 62, L"Text", &C.pvsz0, SZ_NAMES, 4); dlg_choice(d, 63, L"Images", &C.pvsz1, SZ_NAMES, 4); dlg_choice(d, 64, L"Video / audio", &C.pvsz2, SZ_NAMES, 4); dlg_choice(d, 65, L"PDF", &C.pvsz3, SZ_NAMES, 4);
        dlg_choice(d, 66, L"Word", &C.pvsz4, SZ_NAMES, 4); dlg_choice(d, 67, L"Excel", &C.pvsz5, SZ_NAMES, 4); dlg_choice(d, 68, L"PowerPoint", &C.pvsz6, SZ_NAMES, 4); dlg_choice(d, 69, L"Archives", &C.pvsz7, SZ_NAMES, 4); break;
    case 4: dlg_head(d, L"Search"); dlg_check(d, 80, L"Search inside subfolders", &C.search_subfolders); dlg_spin(d, 81, L"Maximum results", &C.search_limit, 100, 20000, 100, NULL); dlg_btn(d, 82, L"Clear search history"); dlg_btn(d, 83, L"Clear pinned searches"); dlg_btn(d, 84, L"Clear extension filter"); break;
    case 5: dlg_head(d, L"Autostart - apps opened when Files starts"); dlg_check(d, 100, L"Enable autostart", &C.autostart_enabled); dlg_btn(d, 101, L"Add program or file..."); dlg_btn(d, 102, L"Run them now");
        for (int i = 0; i < C.autostart_apps.n && i < 40; i++) dlg_item(d, 1000 + i, base_name(C.autostart_apps.v[i]), C.autostart_apps.v[i], 0); if (C.autostart_apps.n) dlg_note(d, L"Click an entry to remove it."); break;
    case 6: dlg_head(d, L"Folder colours"); for (int i = 0; i < C.folder_colors.n && i < 60; i++) dlg_item(d, 2000 + i, base_name(C.folder_colors.v[i]), C.folder_colors.v[i], 0);
        dlg_head(d, L"Custom icons"); for (int i = 0; i < C.icon_map.n && i < 60; i++) dlg_item(d, 3000 + i, C.icon_map.v[i], L"Click to remove", 0); dlg_head(d, L"\"Open with\" associations"); for (int i = 0; i < C.assoc.n && i < 60; i++) dlg_item(d, 4000 + i, C.assoc.v[i], L"Click to remove", 0);
        dlg_note(d, L"Use the context menu (Folder colour, Custom icon) to add entries."); break;
    default: dlg_head(d, L"Backup"); dlg_btn(d, 120, L"Back up settings..."); dlg_btn(d, 121, L"Restore settings..."); dlg_head(d, L"About"); dlg_btn(d, 122, L"About Files(Winlator/Wine)..."); break;
    }
}
static int settings_act(Dlg *d, Row *r, int id) {
    (void)d; (void)r;
    if (id == 20) { show_styles(); return 0; } if (id == 26) { POINT pt; GetCursorPos(&pt); font_menu(pt.x, pt.y); return 0; } if (id == 27) { add_font_file(); return 0; } if (id == 82) sl_clear(&C.search_hist); else if (id == 83) sl_clear(&C.search_pins); else if (id == 84) { C.search_ext[0] = 0; rebuild_view(curtab()); }
    else if (id == 101) add_autostart(); else if (id == 102) run_autostart(1); else if (id >= 1000 && id < 1100) { if (id - 1000 < C.autostart_apps.n) sl_del(&C.autostart_apps, id - 1000); } else if (id >= 2000 && id < 2100) { if (id - 2000 < C.folder_colors.n) sl_del(&C.folder_colors, id - 2000); }
    else if (id >= 3000 && id < 3100) { if (id - 3000 < C.icon_map.n) sl_del(&C.icon_map, id - 3000); memset(g_cicons, 0, sizeof g_cicons); } else if (id >= 4000 && id < 4100) { if (id - 4000 < C.assoc.n) sl_del(&C.assoc, id - 4000); }
    else if (id == 120) { backup_settings(0); return 0; } else if (id == 121) { backup_settings(1); return 0; } else if (id == 122) { show_about(); return 0; } else if (id == 999) return 1;
    settings_commit(); return 0;
}
static void show_settings(void) {
    ST.view = !wcscmp(C.view, L"icons") ? 1 : !wcscmp(C.view, L"tiles") ? 2 : 0; ST.sortk = !wcscmp(C.sort, L"date") ? 1 : !wcscmp(C.sort, L"type") ? 2 : !wcscmp(C.sort, L"size") ? 3 : 0; ST.dirs = !wcscmp(C.sort_dirs, L"last") ? 1 : !wcscmp(C.sort_dirs, L"mixed") ? 2 : 0; ST.scale = (int)(C.scale * 100 + 0.5); ST.login = run_at_login_enabled();
    Dlg d; memset(&d, 0, sizeof d); scpy(d.title, 96, L"Settings"); d.w = px(780); d.h = px(620); d.build = settings_build; d.act = settings_act; d.ntab = 8; static const WCHAR *tn[8] = { L"General", L"Appearance", L"View & Files", L"Preview", L"Search", L"Autostart", L"Icons", L"Backup / About" };
    for (int i = 0; i < 8; i++) scpy(d.tabs[i], 40, tn[i]); dlg_foot(&d, 999, L"Close", 1); dlg_run(&d); settings_commit();
}

/* ========================================================================== */
/* MENUS (themed popups) + COMMAND DISPATCH                                    */
/* ========================================================================== */
#define mi(m, id, s, c, e) pm_add(m, id, s, c, e)
static const WCHAR *EXT_GROUPS[] = { L"", L".png.jpg.jpeg.gif.bmp.webp.ico.tif.tiff.", L".doc.docx.xls.xlsx.ppt.pptx.pdf.txt.rtf.odt.", L".mp4.avi.mkv.mov.wmv.webm.", L".mp3.wav.flac.ogg.m4a.wma.", L".zip.rar.7z.tar.gz.iso.", L".c.h.cpp.py.js.ts.html.css.json.xml.", L".exe.bat.cmd.msi.lnk." };
static const WCHAR *EXT_NAMES[] = { L"All files", L"Images", L"Documents", L"Video", L"Audio", L"Archives", L"Code / text", L"Programs" };
static void add_sort_menu(PMenu *s) {
    mi(s, CMD_SORT_NAME, L"Name", !wcscmp(C.sort, L"name"), 1); mi(s, CMD_SORT_DATE, L"Date modified", !wcscmp(C.sort, L"date"), 1); mi(s, CMD_SORT_TYPE, L"Type", !wcscmp(C.sort, L"type"), 1); mi(s, CMD_SORT_SIZE, L"Size", !wcscmp(C.sort, L"size"), 1);
    pm_sep(s); mi(s, CMD_ASC, L"Ascending", C.asc, 1); pm_sep(s); mi(s, CMD_DIRS_FIRST, L"Folders first", !wcscmp(C.sort_dirs, L"first"), 1); mi(s, CMD_DIRS_LAST, L"Folders last", !wcscmp(C.sort_dirs, L"last"), 1); mi(s, CMD_DIRS_MIXED, L"Mixed", !wcscmp(C.sort_dirs, L"mixed"), 1);
}
static void add_accent_menu(PMenu *ac) { for (int i = 0; i < 8; i++) { WCHAR l[16]; scpy(l, 16, FM_ACCENTS[i]); pm_swatch(ac, CMD_ACCENT0 + i, l, hex_color(FM_ACCENTS[i], 0), wieq(is_win10() ? C.accent : C.style_accent, FM_ACCENTS[i])); } if (!is_win10()) { pm_sep(ac); mi(ac, CMD_RESET_ACCENT, L"Look default", C.style_accent[0] == 0, 1); } }
static void add_compress_menu(PMenu *m) { mi(m, CMD_COMPRESS_ZIP, L"ZIP archive (.zip)", 0, 1); mi(m, CMD_COMPRESS_TAR, L"TAR archive (.tar)", 0, 1); mi(m, CMD_COMPRESS_TGZ, L"TAR.GZ archive (.tar.gz)", 0, 1); mi(m, CMD_COMPRESS_7Z, L"7z archive (needs 7-Zip)", 0, 1); mi(m, CMD_COMPRESS_RAR, L"RAR archive (needs WinRAR)", 0, 1); }
static void add_extract_menu(PMenu *m, int en) { mi(m, CMD_EXTRACT_HERE, L"Extract here", 0, en); mi(m, CMD_EXTRACT_NAME, L"Extract to \"<archive name>\\\"", 0, en); mi(m, CMD_EXTRACT_TO, L"Extract to folder...", 0, en); }
static PMenu *build_menu(int w) {
    PMenu *m = pm_new(); Tab *t = curtab(); int real = t->path[0] && !is_special(t->path); int ns = sel_count(t, NULL);
    if (w == 0) { mi(m, CMD_NEWTAB, L"New tab\tCtrl+T", 0, 1); mi(m, CMD_NEWFOLDER, L"New folder\tCtrl+Shift+N", 0, real); mi(m, CMD_NEWFILE, L"New text file", 0, real); pm_sep(m); mi(m, CMD_CMD, L"Open CMD here", 0, 1); mi(m, CMD_PROPS, L"Properties\tAlt+Enter", 0, ns == 1); mi(m, CMD_SETTINGS, L"Settings...", 0, 1); pm_sep(m); mi(m, CMD_EXIT, L"Exit", 0, 1); }
    else if (w == 1) { mi(m, CMD_CUT, L"Cut\tCtrl+X", 0, ns > 0); mi(m, CMD_COPY, L"Copy\tCtrl+C", 0, ns > 0); mi(m, CMD_PASTE, L"Paste\tCtrl+V", 0, S.clip.n > 0 && real); pm_sep(m); mi(m, CMD_SELALL, L"Select all\tCtrl+A", 0, 1); mi(m, CMD_SELNONE, L"Select none", 0, 1); mi(m, CMD_INVERT, L"Invert selection", 0, 1);
        pm_sep(m); mi(m, CMD_RENAME, L"Rename\tF2", 0, ns == 1); mi(m, CMD_DELETE, L"Delete\tDel", 0, ns > 0); mi(m, CMD_COPYPATH, L"Copy path", 0, ns == 1); mi(m, CMD_SHORTCUT, L"Create shortcut on Desktop", 0, ns > 0); mi(m, CMD_SHORTCUT_HERE, L"Create shortcut here", 0, ns > 0 && real); }
    else if (w == 2) { int vm = view_mode(); mi(m, CMD_VDET, L"Details", vm == 0, 1); mi(m, CMD_VICON, L"Icons", vm == 1, 1); mi(m, CMD_VTILE, L"Tiles", vm == 2, 1); pm_sep(m); add_sort_menu(pm_sub(m, L"Sort by")); pm_sep(m);
        mi(m, CMD_PREVIEW, L"Preview pane", C.show_preview, 1); mi(m, CMD_TREE, L"Tree view", C.show_tree, 1); mi(m, CMD_CHECKS, L"Selection tick boxes", C.checkboxes, 1); mi(m, CMD_HIDDEN, L"Show hidden files", C.show_hidden, 1); mi(m, CMD_THUMBS, L"Thumbnails", C.thumbnails, 1); mi(m, CMD_COUNTS, L"Counts in status bar", C.show_counts, 1);
        pm_sep(m); mi(m, CMD_ICON_PLUS, L"Larger icons", 0, 1); mi(m, CMD_ICON_MINUS, L"Smaller icons", 0, 1); mi(m, CMD_ZOOM_IN, L"Zoom interface in\tCtrl+=", 0, 1); mi(m, CMD_ZOOM_OUT, L"Zoom interface out\tCtrl+-", 0, 1); mi(m, CMD_PREVW, L"Cycle preview width", 0, C.show_preview); mi(m, CMD_RIBBON, L"Command strip", C.ribbon_open, 1); pm_sep(m); mi(m, CMD_REFRESH, L"Refresh\tF5", 0, 1); }
    else if (w == 3) { mi(m, CMD_BACK, L"Back\tAlt+Left", 0, t->hidx > 0); mi(m, CMD_FWD, L"Forward\tAlt+Right", 0, t->hidx < t->hist.n - 1); mi(m, CMD_UP, L"Up\tBackspace", 0, t->path[0] != 0); pm_sep(m); mi(m, CMD_HOME, cur_style()->sec[0], 0, 1); mi(m, CMD_RECENT, cur_style()->sec[1], 0, 1); mi(m, CMD_FAV, cur_style()->sec[2], 0, 1); mi(m, CMD_PC, cur_style()->sec[3], 0, 1); }
    else if (w == 4) { mi(m, CMD_STYLES, L"Styles dialog...", 0, 1); mi(m, CMD_OSNEXT, L"Switch to next OS theme", 0, 1); pm_sep(m);
        PMenu *sm = pm_sub(m, L"Quick look picker"); for (int i = 0; i < (int)NSTYLES; i++) mi(sm, CMD_STYLE0 + STYLE_ORDER[i], OS_STYLES[STYLE_ORDER[i]].label, STYLE_ORDER[i] == style_index(C.os_style), 1);
        if (is_win10()) { PMenu *th = pm_sub(m, L"Theme"); for (int i = 0; i < (int)NTHEMES; i++) mi(th, CMD_THEME0 + i, FM_THEMES[i].name, wieq(C.theme, FM_THEMES[i].name), 1); } else mi(m, CMD_DARK, L"Dark variant", C.style_dark, cur_style()->has_dark);
        add_accent_menu(pm_sub(m, L"Accent colour")); mi(m, CMD_STYLEFONTS, L"Use the look's fonts", C.style_fonts, 1); }
    else { PMenu *a = pm_sub(m, L"Autostart"); mi(a, CMD_AUTO_EN, L"Run chosen apps when Files starts", C.autostart_enabled, 1); mi(a, CMD_AUTO_ADD, L"Add program or file...", 0, 1); pm_sep(a);
        for (int i = 0; i < C.autostart_apps.n && i < 40; i++) { WCHAR l[MAX_PATH + 16]; wfmt(l, MAX_PATH + 16, L"Remove: %ls", base_name(C.autostart_apps.v[i])); mi(a, CMD_AUTOREM0 + i, l, 0, 1); } if (C.autostart_apps.n) pm_sep(a); mi(a, CMD_AUTO_RUN, L"Run now", 0, C.autostart_apps.n > 0); mi(a, CMD_AUTO_LOGIN, L"Start with Windows (Run key)", run_at_login_enabled(), 1);
        pm_sep(m); mi(m, CMD_SUBFOLDERS, L"Search inside subfolders", C.search_subfolders, 1); mi(m, CMD_RESTORE, L"Restore tabs on start", C.restore_session, 1); mi(m, CMD_FONT_PLUS, L"Larger font", 0, 1); mi(m, CMD_FONT_MINUS, L"Smaller font", 0, 1); pm_sep(m); mi(m, CMD_BACKUP, L"Back up settings...", 0, 1); mi(m, CMD_RESTORE_CFG, L"Restore settings...", 0, 1); pm_sep(m); mi(m, CMD_SETTINGS, L"Settings...", 0, 1); mi(m, CMD_ABOUT, L"About", 0, 1); }
    return m;
}
static void main_menu(void) { PMenu *m = pm_new(); static const WCHAR *n[6] = { L"File", L"Edit", L"View", L"Go", L"Style", L"Tools" }; for (int i = 0; i < 6; i++) { PMenu *s = pm_sub(m, n[i]); PMenu *b = build_menu(i); for (int k = 0; k < b->n; k++) { PMItem *d = pm_push(s); *d = b->it[k]; } free(b->it); free(b); }
    popup_at(m, LY.menu.left, LY.menu.bottom); pm_free(m); }
static void add_ow_submenu(PMenu *m) { ow_collect(); PMenu *s = pm_sub(m, L"Open with"); for (int i = 0; i < g_now && i < 300; i++) mi(s, CMD_OW0 + i, g_ow[i].name, 0, 1); pm_sep(s); mi(s, CMD_OW_MORE, L"Choose another app...", 0, 1); }
static void item_menu(int x, int y) {
    Tab *t = curtab(); Item *f = focus_item(t); if (!f) return; PMenu *m = pm_new(); int ns = sel_count(t, NULL); int real = f->kind != 1; int arc = !f->dir && arc_kind_of(f->path) != ARC_NONE && !ext_in(f->path, L".docx.xlsx.pptx.");
    mi(m, CMD_OPEN, L"Open", 0, 1); if (!f->dir) add_ow_submenu(m); if (C.admin_menu && !f->dir && ext_in(f->path, L".exe.bat.cmd.msi.lnk.com.")) mi(m, CMD_ADMIN, L"Run as administrator", 0, 1); pm_sep(m);
    if (real) {
        if (arc) { add_extract_menu(pm_sub(m, L"Extract"), 1); }
        add_compress_menu(pm_sub(m, L"Compress to")); pm_sep(m); mi(m, CMD_CUT, L"Cut", 0, 1); mi(m, CMD_COPY, L"Copy", 0, 1); mi(m, CMD_COPYPATH, L"Copy path", 0, ns == 1); pm_sep(m);
        mi(m, CMD_RENAME, L"Rename", 0, ns == 1); mi(m, CMD_DELETE, L"Delete", 0, 1); pm_sep(m); mi(m, CMD_PIN, is_pinned(f) ? L"Unpin from top" : L"Pin to top", 0, 1);
        if (f->dir) { mi(m, CMD_FAVTOGGLE, sl_find(&C.favorites, f->path) >= 0 ? L"Remove from Favorites" : L"Add to Favorites", 0, 1); PMenu *c = pm_sub(m, L"Folder colour"); for (int i = 0; i < 12; i++) pm_swatch(c, CMD_COLOR0 + i, FOLDER_PAL_NAMES[i], RGB((FOLDER_PALETTE[i] >> 16) & 255, (FOLDER_PALETTE[i] >> 8) & 255, FOLDER_PALETTE[i] & 255), 0); pm_sep(c); mi(c, CMD_FOLDERCOLOR_CLEAR, L"Default colour", 0, 1); }
        PMenu *ic = pm_sub(m, L"Custom icon"); mi(ic, CMD_SETICON, L"For this item...", 0, 1); mi(ic, CMD_SETICON_EXT, L"For all files of this type...", 0, !f->dir); mi(ic, CMD_SETICON_DIR, L"For all folders...", 0, 1); pm_sep(ic); mi(ic, CMD_CLEARICON, L"Remove custom icon", 0, 1);
        pm_sep(m); mi(m, CMD_SHORTCUT, L"Create shortcut on Desktop", 0, 1); mi(m, CMD_SHORTCUT_HERE, L"Create shortcut here", 0, 1); pm_sep(m); mi(m, CMD_PROPS, L"Properties", 0, ns == 1);
    }
    popup_at(m, x, y); pm_free(m);
}
static void blank_menu(int x, int y) {
    Tab *t = curtab(); int real = t->path[0] && !is_special(t->path); PMenu *m = pm_new(); int vm = view_mode();
    PMenu *v = pm_sub(m, L"View"); mi(v, CMD_VDET, L"Details", vm == 0, 1); mi(v, CMD_VICON, L"Icons", vm == 1, 1); mi(v, CMD_VTILE, L"Tiles", vm == 2, 1); add_sort_menu(pm_sub(m, L"Sort by")); pm_sep(m);
    PMenu *nw = pm_sub(m, L"New"); mi(nw, CMD_NEWFOLDER, L"Folder", 0, real); mi(nw, CMD_NEWFILE, L"Text document", 0, real); mi(m, CMD_PASTE, L"Paste", 0, real && S.clip.n); mi(m, CMD_SELALL, L"Select all", 0, 1); pm_sep(m);
    mi(m, CMD_REFRESH, L"Refresh", 0, 1); mi(m, CMD_CMD, L"Open CMD here", 0, 1); mi(m, CMD_STYLES, L"Styles...", 0, 1); mi(m, CMD_SETTINGS, L"Settings...", 0, 1); popup_at(m, x, y); pm_free(m);
}
static void tab_menu(int idx, int x, int y) { g_tab_ctx = idx; PMenu *m = pm_new(); Tab *t = &S.tabs[idx]; mi(m, CMD_TABDUP, L"Duplicate tab", 0, 1); mi(m, CMD_TABPIN, t->pinned ? L"Unpin tab" : L"Pin tab", 0, 1); pm_sep(m); mi(m, CMD_TABCLOSE, L"Close tab", 0, !t->pinned); mi(m, CMD_TABOTHERS, L"Close other tabs", 0, S.ntabs > 1); popup_at(m, x, y); pm_free(m); }
static void search_menu(void) {
    PMenu *m = pm_new(); if (C.search_hist.n) { pm_head(m, L"Recent searches"); for (int i = 0; i < C.search_hist.n && i < 10; i++) mi(m, CMD_SRCH_HIST0 + i, C.search_hist.v[i], 0, 1); }
    if (C.search_pins.n) { pm_sep(m); pm_head(m, L"Pinned searches"); for (int i = 0; i < C.search_pins.n && i < 30; i++) { WCHAR l[300]; wfmt(l, 300, L"\x2605 %ls", C.search_pins.v[i]); mi(m, CMD_SRCH_PIN0 + i, l, 0, 1); } }
    pm_sep(m); PMenu *f = pm_sub(m, L"Filter by extension"); for (int i = 0; i < 8; i++) pm_radio(f, CMD_SRCH_EXT0 + i, EXT_NAMES[i], !wcscmp(C.search_ext, EXT_GROUPS[i]), 1);
    pm_sep(m); WCHAR cur[256]; GetWindowTextW(S.e_search, cur, 256); mi(m, CMD_SRCH_PINCUR, L"Pin current search", 0, cur[0] != 0 && sl_find(&C.search_pins, cur) < 0);
    if (C.search_pins.n) { PMenu *u = pm_sub(m, L"Unpin"); for (int i = 0; i < C.search_pins.n && i < 30; i++) mi(u, CMD_SRCH_UNPIN0 + i, C.search_pins.v[i], 0, 1); } mi(m, CMD_SRCH_CLEAR, L"Clear history", 0, C.search_hist.n > 0); mi(m, CMD_SUBFOLDERS, L"Search inside subfolders", C.search_subfolders, 1);
    popup_at(m, LY.search.left, LY.search.bottom); pm_free(m);
}
static void apply_search_text(void);
static void do_search_text(const WCHAR *q) { set_search_text(q); apply_search_text(); }
static void extract_dest(int mode) {
    Tab *t = curtab(); Item *f = focus_item(t); if (!f || f->dir || arc_kind_of(f->path) == ARC_NONE) { toast(L"Select an archive first"); return; } WCHAR dest[MAX_PATH], par[MAX_PATH]; parent_of(f->path, par);
    if (mode == 0) scpy(dest, MAX_PATH, par); else if (mode == 1) { WCHAR stem[MAX_PATH]; scpy(stem, MAX_PATH, base_name(f->path)); int l = lstrlenW(stem); if (l > 7 && !_wcsicmp(stem + l - 7, L".tar.gz")) stem[l - 7] = 0; else { WCHAR *e = PathFindExtensionW(stem); if (*e) *e = 0; } unique_name(par, stem, 1, dest, MAX_PATH); }
    else { BROWSEINFOW bi; memset(&bi, 0, sizeof bi); bi.hwndOwner = S.hwnd; bi.lpszTitle = L"Extract to folder"; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE; LPITEMIDLIST pl = SHBrowseForFolderW(&bi); if (!pl) return; BOOL ok = SHGetPathFromIDListW(pl, dest); CoTaskMemFree(pl); if (!ok) return; }
    start_extract(f->path, dest);
}
static void do_cmd(int id) {
    Tab *t = curtab(); Item *f = focus_item(t);
    if (id >= CMD_STYLE0 && id < CMD_STYLE0 + (int)NSTYLES) { scpy(C.os_style, 16, OS_STYLES[id - CMD_STYLE0].key); style_changed(); return; }
    if (id >= CMD_THEME0 && id < CMD_THEME0 + (int)NTHEMES) { scpy(C.theme, 16, FM_THEMES[id - CMD_THEME0].name); style_changed(); return; }
    if (id >= CMD_ACCENT0 && id < CMD_ACCENT0 + 8) { if (is_win10()) scpy(C.accent, 16, FM_ACCENTS[id - CMD_ACCENT0]); else scpy(C.style_accent, 16, FM_ACCENTS[id - CMD_ACCENT0]); style_changed(); return; }
    if (id >= CMD_AUTOREM0 && id < CMD_AUTOREM0 + 40) { int i = id - CMD_AUTOREM0; if (i < C.autostart_apps.n) { sl_del(&C.autostart_apps, i); cfg_save(); } return; }
    if (id >= CMD_COLOR0 && id < CMD_COLOR0 + 12) { apply_folder_color(id - CMD_COLOR0); return; }
    if (id >= CMD_OW0 && id < CMD_OW0 + 300) { if (f && id - CMD_OW0 < g_now) launch_with(g_ow[id - CMD_OW0].path, f->path); return; }
    if (id >= CMD_SRCH_HIST0 && id < CMD_SRCH_HIST0 + 10) { if (id - CMD_SRCH_HIST0 < C.search_hist.n) { WCHAR q[256]; scpy(q, 256, C.search_hist.v[id - CMD_SRCH_HIST0]); do_search_text(q); } return; }
    if (id >= CMD_SRCH_PIN0 && id < CMD_SRCH_PIN0 + 30) { if (id - CMD_SRCH_PIN0 < C.search_pins.n) { WCHAR q[256]; scpy(q, 256, C.search_pins.v[id - CMD_SRCH_PIN0]); do_search_text(q); } return; }
    if (id >= CMD_SRCH_EXT0 && id < CMD_SRCH_EXT0 + 8) { scpy(C.search_ext, 64, EXT_GROUPS[id - CMD_SRCH_EXT0]); cfg_save(); rebuild_view(t); redraw(); return; }
    if (id >= CMD_SRCH_UNPIN0 && id < CMD_SRCH_UNPIN0 + 30) { if (id - CMD_SRCH_UNPIN0 < C.search_pins.n) { sl_del(&C.search_pins, id - CMD_SRCH_UNPIN0); cfg_save(); } return; }
    if (id >= CMD_TAB0 && id < CMD_TAB0 + 6) { if (C.ribbon_tab == id - CMD_TAB0 && C.ribbon_open) C.ribbon_open = 0; else { C.ribbon_tab = id - CMD_TAB0; C.ribbon_open = 1; } cfg_save(); place_edits(); redraw(); return; }
    id &= 0x3FFF;
    switch (id) {
    case CMD_NEWTAB: new_tab(NULL); break; case CMD_NEWWIN: new_tab(t->path); break; case CMD_NEWFOLDER: new_item(1); break; case CMD_NEWFILE: new_item(0); break; case CMD_CMD: open_cmd(); break; case CMD_EXIT: PostMessageW(S.hwnd, WM_CLOSE, 0, 0); break;
    case CMD_CUT: do_copy(1); break; case CMD_COPY: do_copy(0); break; case CMD_PASTE: do_paste(); break; case CMD_SELALL: select_all(); break; case CMD_INVERT: invert_sel(); break; case CMD_SELNONE: sel_clear(t); preview_update(); redraw(); break;
    case CMD_RENAME: begin_rename(); break; case CMD_DELETE: do_delete(); break; case CMD_COPYPATH: copy_path_text(f); break; case CMD_PROPS: show_properties(f); break;
    case CMD_OPEN: if (sel_count(t, NULL) > 1) { for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; if (x->sel && !x->dir) open_item(x); } } else open_item(f); break;
    case CMD_OPENWITH: if (f && !f->dir) { open_with(f); } break; case CMD_OW_MORE: open_with(f); break; case CMD_ADMIN: run_as_admin(f); break;
    case CMD_REFRESH: g_nav_dirty = 1; reload(1); break;
    case CMD_VDET: scpy(C.view, 16, L"details"); set_pref_redraw(); break; case CMD_VICON: scpy(C.view, 16, L"icons"); set_pref_redraw(); break; case CMD_VTILE: scpy(C.view, 16, L"tiles"); set_pref_redraw(); break;
    case CMD_PREVIEW: C.show_preview = !C.show_preview; if (C.show_preview) preview_update(); else preview_free(); set_pref_redraw(); break;
    case CMD_PREVW: C.preview_width = C.preview_width >= 700 ? 280 : C.preview_width + 140; g_pv_override = 1; set_pref_redraw(); break;
    case CMD_TREE: C.show_tree = !C.show_tree; g_nav_dirty = 1; set_pref_redraw(); break; case CMD_CHECKS: C.checkboxes = !C.checkboxes; set_pref_redraw(); break; case CMD_COUNTS: C.show_counts = !C.show_counts; set_pref_redraw(); break;
    case CMD_RIBBON: C.ribbon_open = !C.ribbon_open; set_pref_redraw(); break;
    case CMD_HIDDEN: C.show_hidden = !C.show_hidden; cfg_save(); reload(1); break; case CMD_THUMBS: C.thumbnails = !C.thumbnails; set_pref_redraw(); break;
    case CMD_SORT_NAME: if (id == (int)CMD_SORT_NAME && 0) break; sort_by(L"name"); break; case CMD_SORT_DATE: sort_by(L"date"); break; case CMD_SORT_TYPE: sort_by(L"type"); break; case CMD_SORT_SIZE: sort_by(L"size"); break;
    case CMD_ASC: C.asc = !C.asc; set_pref_redraw(); break; case CMD_DIRS_FIRST: scpy(C.sort_dirs, 8, L"first"); set_pref_redraw(); break; case CMD_DIRS_LAST: scpy(C.sort_dirs, 8, L"last"); set_pref_redraw(); break; case CMD_DIRS_MIXED: scpy(C.sort_dirs, 8, L"mixed"); set_pref_redraw(); break;
    case CMD_ICON_PLUS: C.icon_size = C.icon_size + 16 > 160 ? 160 : C.icon_size + 16; set_pref_redraw(); break; case CMD_ICON_MINUS: C.icon_size = C.icon_size - 16 < 32 ? 32 : C.icon_size - 16; set_pref_redraw(); break;
    case CMD_ZOOM_IN: C.scale = C.scale + 0.1 > 2.5 ? 2.5 : C.scale + 0.1; style_changed(); break; case CMD_ZOOM_OUT: C.scale = C.scale - 0.1 < 0.6 ? 0.6 : C.scale - 0.1; style_changed(); break;
    case CMD_FONT_PLUS: C.font_size = C.font_size + 1 > 28 ? 28 : C.font_size + 1; style_changed(); break; case CMD_FONT_MINUS: C.font_size = C.font_size - 1 < 9 ? 9 : C.font_size - 1; style_changed(); break;
    case CMD_BACK: go_back(); break; case CMD_FWD: go_fwd(); break; case CMD_UP: go_up(); break; case CMD_HOME: navigate(L"::quick", 1); break; case CMD_PC: navigate(L"", 1); break; case CMD_FAV: navigate(L"::fav", 1); break; case CMD_RECENT: navigate(L"::recent", 1); break;
    case CMD_DARK: C.style_dark = !C.style_dark; style_changed(); break; case CMD_RESET_ACCENT: C.style_accent[0] = 0; style_changed(); break; case CMD_STYLEFONTS: C.style_fonts = !C.style_fonts; style_changed(); break;
    case CMD_STYLES: show_styles(); break; case CMD_SETTINGS: show_settings(); break; case CMD_ABOUT: show_about(); break;
    case CMD_OSNEXT: { int i = style_index(C.os_style); int k = 0; for (int q = 0; q < (int)NSTYLES; q++) if (STYLE_ORDER[q] == i) k = q; scpy(C.os_style, 16, OS_STYLES[STYLE_ORDER[(k + 1) % NSTYLES]].key); style_changed(); WCHAR m[80]; wfmt(m, 80, L"Look: %ls", cur_style()->label); toast(m); break; }
    case CMD_ACCENT0: { PMenu *ac = pm_new(); add_accent_menu(ac); popup_at(ac, g_rb[0].left, LY.strip.bottom); pm_free(ac); break; }
    case CMD_SUBFOLDERS: C.search_subfolders = !C.search_subfolders; cfg_save(); if (t->filter[0]) { t->filter[0] = 0; apply_search_text(); } redraw(); break;
    case CMD_RESTORE: C.restore_session = !C.restore_session; cfg_save(); break; case CMD_BACKUP: backup_settings(0); break; case CMD_RESTORE_CFG: backup_settings(1); break;
    case CMD_AUTO_EN: C.autostart_enabled = !C.autostart_enabled; cfg_save(); break; case CMD_AUTO_ADD: add_autostart(); break; case CMD_AUTO_RUN: run_autostart(1); break; case CMD_AUTO_LOGIN: set_run_at_login(!run_at_login_enabled()); cfg_save(); break;
    case CMD_PIN: if (f) { int k = sl_find(&C.pins, f->path); if (k >= 0) sl_del(&C.pins, k); else sl_add(&C.pins, f->path); cfg_save(); rebuild_view(t); redraw(); } break;
    case CMD_FAVTOGGLE: if (f) { int k = sl_find(&C.favorites, f->path); if (k >= 0) sl_del(&C.favorites, k); else sl_add(&C.favorites, f->path); cfg_save(); g_nav_dirty = 1; redraw(); } break;
    case CMD_TABCLOSE: close_tab(g_tab_ctx); break; case CMD_TABPIN: if (g_tab_ctx < S.ntabs) { S.tabs[g_tab_ctx].pinned = !S.tabs[g_tab_ctx].pinned; redraw(); } break; case CMD_TABDUP: if (g_tab_ctx < S.ntabs) new_tab(S.tabs[g_tab_ctx].path); break;
    case CMD_TABOTHERS: for (int i = S.ntabs - 1; i >= 0; i--) if (i != g_tab_ctx && !S.tabs[i].pinned) { if (i < g_tab_ctx) g_tab_ctx--; close_tab(i); } break;
    case CMD_SHORTCUT: create_shortcuts(1); break; case CMD_SHORTCUT_HERE: create_shortcuts(0); break;
    case CMD_FOLDERCOLOR: { Item *fi = f; if (fi && fi->dir) { PMenu *c = pm_new(); for (int i = 0; i < 12; i++) pm_swatch(c, CMD_COLOR0 + i, FOLDER_PAL_NAMES[i], RGB((FOLDER_PALETTE[i] >> 16) & 255, (FOLDER_PALETTE[i] >> 8) & 255, FOLDER_PALETTE[i] & 255), 0); pm_sep(c); mi(c, CMD_FOLDERCOLOR_CLEAR, L"Default colour", 0, 1); POINT pt; GetCursorPos(&pt); int r = pm_track(c, pt.x, pt.y); pm_free(c); if (r) PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(r, 0), 0); } else toast(L"Select a folder first"); break; }
    case CMD_FOLDERCOLOR_CLEAR: apply_folder_color(-1); break;
    case CMD_SETICON: pick_icon(0); break; case CMD_SETICON_EXT: pick_icon(1); break; case CMD_SETICON_DIR: pick_icon(2); break; case CMD_CLEARICON: clear_icon(); break;
    case CMD_EXTRACT_HERE: extract_dest(0); break; case CMD_EXTRACT_NAME: extract_dest(1); break; case CMD_EXTRACT_TO: extract_dest(2); break;
    case CMD_EXTRACT_MENU: { PMenu *m = pm_new(); add_extract_menu(m, 1); POINT pt; GetCursorPos(&pt); int r = pm_track(m, pt.x, pt.y); pm_free(m); if (r) do_cmd(r); break; }
    case CMD_COMPRESS_ZIP: start_compress(L"zip"); break; case CMD_COMPRESS_TAR: start_compress(L"tar"); break; case CMD_COMPRESS_TGZ: start_compress(L"tgz"); break; case CMD_COMPRESS_7Z: start_compress(L"7z"); break; case CMD_COMPRESS_RAR: start_compress(L"rar"); break;
    case CMD_SRCH_PINCUR: { WCHAR q[256]; GetWindowTextW(S.e_search, q, 256); if (q[0] && sl_find(&C.search_pins, q) < 0) { sl_add(&C.search_pins, q); cfg_save(); toast(L"Search pinned"); } break; }
    case CMD_SRCH_CLEAR: sl_clear(&C.search_hist); cfg_save(); break;
    case CMD_PVFULL: pv_open_full(); break; case CMD_PVCLOSE: pv_close_full(); break;
    }
}

/* ========================================================================== */
/* EDIT CONTROLS, INPUT, WINDOW PROCEDURE, MAIN                               */
/* ========================================================================== */
static void place_edits(void) {
    if (!S.hwnd || !S.e_search || !S.ntabs) return; RECT rc; GetClientRect(S.hwnd, &rc); do_layout(rc.right, rc.bottom); int eh = px(C.font_size) + px(6); RECT s = LY.search, a = LY.addr; int ix = s.left + px(6) + (s.bottom - s.top);
    MoveWindow(S.e_search, ix, (s.top + s.bottom) / 2 - eh / 2, LY.sub.left - px(10) - ix, eh, TRUE); MoveWindow(S.e_addr, a.left + px(6), (a.top + a.bottom) / 2 - eh / 2, a.right - a.left - px(12), eh, TRUE);
}
static void show_addr(void) { Tab *t = curtab(); S.addr_edit = 1; SetWindowTextW(S.e_addr, (!t->path[0] || is_special(t->path)) ? L"" : t->path); place_edits(); ShowWindow(S.e_addr, SW_SHOW); SetFocus(S.e_addr); SendMessageW(S.e_addr, EM_SETSEL, 0, -1); redraw(); }
static void hide_addr(void) { S.addr_edit = 0; ShowWindow(S.e_addr, SW_HIDE); redraw(); }
static void begin_rename(void) {
    Tab *t = curtab(); Item *it = focus_item(t); if (!it || it->kind == 1 || sel_count(t, NULL) != 1) return; ensure_visible(t->focus); RECT r = item_rect(t->focus); int m = view_mode(), eh = px(C.font_size) + px(6); RECT e;
    if (m == 0) { int c[5]; col_x(LY.body.right - LY.body.left, c); e = (RECT){ r.left + px(10) + gutter() + px(26), r.top + (r.bottom - r.top - eh) / 2, r.left + c[1] - px(6), 0 }; } else if (m == 1) e = (RECT){ r.left + px(2), r.bottom - eh - px(4), r.right - px(2), 0 }; else e = (RECT){ r.left + px(56), r.top + px(6), r.right - px(6), 0 };
    S.rename_item = t->view[t->focus]; SetWindowTextW(S.e_rename, it->name); MoveWindow(S.e_rename, e.left, e.top, e.right - e.left, eh, TRUE); ShowWindow(S.e_rename, SW_SHOW); SetFocus(S.e_rename);
    int len = lstrlenW(it->name); const WCHAR *dot = wcsrchr(it->name, '.'); int stop = (!it->dir && dot && dot != it->name) ? (int)(dot - it->name) : len; SendMessageW(S.e_rename, EM_SETSEL, 0, stop);
}
static void rename_end(int commit) {
    if (S.rename_item < 0) return; Tab *t = curtab(); int idx = S.rename_item; S.rename_item = -1; WCHAR nn[256]; GetWindowTextW(S.e_rename, nn, 256); ShowWindow(S.e_rename, SW_HIDE); SetFocus(S.hwnd); if (!commit || idx >= t->n) return;
    Item *it = &t->it[idx]; int l = lstrlenW(nn); while (l && nn[l - 1] == ' ') nn[--l] = 0; if (!nn[0] || !wcscmp(nn, it->name)) return; if (wcspbrk(nn, L"\\/:*?\"<>|")) { toast(L"A name cannot contain \\ / : * ? \" < > |"); return; }
    WCHAR par[MAX_PATH], np[MAX_PATH]; parent_of(it->path, par); path_join(np, MAX_PATH, par, nn); if (path_exists(np) && !wieq(np, it->path)) { toast(L"An item with that name already exists"); return; } if (!MoveFileW(it->path, np)) { toast(L"Rename failed"); return; }
    int pk = sl_find(&C.pins, it->path); if (pk >= 0) { free(C.pins.v[pk]); C.pins.v[pk] = wdup(np); } reload(0); for (int i = 0; i < t->nv; i++) if (wieq(t->it[t->view[i]].path, np)) { sel_view(i, 0, 0); ensure_visible(i); break; }
}
static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) { PostMessageW(S.hwnd, WM_APP_EDITKEY, (WPARAM)h, wp == VK_RETURN); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_DOWN && h == S.e_search) { search_menu(); return 0; } if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KILLFOCUS) PostMessageW(S.hwnd, WM_APP_EDITKEY, (WPARAM)h, 2); return CallWindowProcW(S.edit_old, h, msg, wp, lp);
}
static void apply_search_text(void) {
    Tab *t = curtab(); WCHAR q[256]; GetWindowTextW(S.e_search, q, 256); if (!wcscmp(q, t->filter)) return; scpy(t->filter, 256, q); t->scroll = 0;
    if (!q[0]) { cancel_search(); t->searching = 0; reload(0); } else if (t->path[0] && !is_special(t->path) && C.search_subfolders) start_search(q); else { cancel_search(); t->searching = 0; rebuild_view(t); redraw(); }
}
static void on_editkey(HWND h, int act) {
    if (h == S.e_rename) { if (act == 1) rename_end(1); else if (act == 0) rename_end(0); else if (IsWindowVisible(h)) rename_end(1); }
    else if (h == S.e_addr) { if (!S.addr_edit) return; if (act == 1) { WCHAR p[MAX_PATH]; GetWindowTextW(h, p, MAX_PATH); hide_addr(); SetFocus(S.hwnd); navigate(p[0] ? p : L"", 1); } else { hide_addr(); if (act == 0) SetFocus(S.hwnd); } }
    else if (h == S.e_search) { if (act == 0) { set_search_text(L""); apply_search_text(); SetFocus(S.hwnd); } else if (act == 1) { apply_search_text(); WCHAR q[256]; GetWindowTextW(S.e_search, q, 256); if (q[0]) { sl_front(&C.search_hist, q, 20); cfg_save(); } SetFocus(S.hwnd); } }
}
/* ---- hit regions for hover redraw keys ---- */
static int region_at(int x, int y) {
    const RECT *rs[] = { &LY.back, &LY.fwd, &LY.up, &LY.refresh, &LY.menu, &LY.setb, &LY.cmdb, &LY.osb, &LY.sub, &LY.sdrop, &LY.ribcol, &LY.views[0], &LY.views[1], &LY.views[2], &LY.split };
    for (int i = 0; i < 15; i++) if (in_rect(rs[i], x, y)) return i + 1; for (int i = 0; i < 3; i++) if (in_rect(&LY.capbtn[i], x, y)) return 20 + i; for (int i = 0; i < 6; i++) if (in_rect(&LY.ribtab[i], x, y)) return 30 + i;
    for (int i = 0; i < g_nrb; i++) if (in_rect(&g_rb[i], x, y)) return 40 + i; if (S.ntabs > 1) { for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) return 70 + i; if (in_rect(&g_tabplus, x, y)) return 100; }
    for (int i = 0; i < S.ncrumbs; i++) if (in_rect(&S.crumbs[i].r, x, y)) return 110 + i; return 0;
}
static void scroll_by(int d) { Tab *t = curtab(); t->scroll += d; clamp_scroll(); redraw(); }
static void sb_drag_to(int y) { Tab *t = curtab(); int th = total_h(), vh = LY.sb.bottom - LY.sb.top; if (th <= vh) return; int tl = vh * vh / th; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = th - vh; if (rng <= 0) return; int ty = y - S.sb_off - LY.sb.top; if (ty < 0) ty = 0; if (ty > rng) ty = rng; t->scroll = (int)((double)ty / rng * mx); redraw(); }
static void click_header(int x) { int c[5]; col_x(LY.body.right - LY.body.left, c); static const WCHAR *k[4] = { L"name", L"date", L"type", L"size" }; int rx = x - LY.hdr.left; for (int i = 0; i < 4; i++) if (rx >= c[i] && rx < c[i + 1]) { sort_by(k[i]); return; } }
static int g_splitdrag, g_defer_vi = -1, g_pvcap;
static unsigned char *g_rbbase;
static void cap_action(int i) {
    int a = LY.capkind == 1 ? (i == 0 ? 2 : i == 1 ? 0 : 1) : i;   /* 0 min 1 max 2 close */
    if (a == 0) ShowWindow(S.hwnd, SW_MINIMIZE); else if (a == 1) ShowWindow(S.hwnd, IsZoomed(S.hwnd) ? SW_RESTORE : SW_MAXIMIZE); else PostMessageW(S.hwnd, WM_CLOSE, 0, 0);
}
static void rubber_update(int x, int y, int ctrl) {
    Tab *t = curtab(); g_in.rb_x1 = x - LY.body.left; g_in.rb_y1 = y - LY.body.top + t->scroll; if (y < LY.body.top) { t->scroll -= px(18); } else if (y > LY.body.bottom) { t->scroll += px(18); } clamp_scroll();
    int y0 = g_in.rb_y0 - t->scroll + LY.body.top, y1 = g_in.rb_y1 - t->scroll + LY.body.top; RECT q = { (g_in.rb_x0 < g_in.rb_x1 ? g_in.rb_x0 : g_in.rb_x1) + LY.body.left, y0 < y1 ? y0 : y1, (g_in.rb_x0 < g_in.rb_x1 ? g_in.rb_x1 : g_in.rb_x0) + LY.body.left, y0 < y1 ? y1 : y0 };
    for (int i = 0; i < t->nv; i++) { RECT r = item_rect(i); RECT in = r; if (view_mode() == 0) { in.left += px(6); in.right = in.left + px(260); } int hit = !(r.bottom < q.top || r.top > q.bottom) && !(in.right < q.left || in.left > q.right); Item *it = &t->it[t->view[i]]; if (it->kind == 1) continue; it->sel = (ctrl && g_rbbase && g_rbbase[i]) ? !hit || 1 : hit; if (ctrl && g_rbbase && g_rbbase[i]) it->sel = 1; }
    redraw();
}
static void do_drop(int ctrl) {
    Tab *t = curtab(); WCHAR dest[MAX_PATH] = L""; if (g_in.drop_item >= 0 && g_in.drop_item < t->nv) { Item *d = &t->it[t->view[g_in.drop_item]]; if (d->dir && !d->sel && d->kind != 1) scpy(dest, MAX_PATH, d->path); }
    else if (g_in.drop_nav >= 0 && g_in.drop_nav < S.nnav) { NavEnt *e = &S.nav[g_in.drop_nav]; if (e->path[0] && !is_special(e->path) && dir_exists(e->path)) scpy(dest, MAX_PATH, e->path); }
    if (!dest[0]) return; SL s = { 0 }; collect_sel(t, &s); int ok = s.n > 0; for (int i = 0; i < s.n; i++) if (under_path(dest, s.v[i])) ok = 0; if (ok) { WCHAR par[MAX_PATH]; parent_of(s.v[0], par); if (!ctrl && wieq(par, dest)) ok = 0; }
    if (ok) start_transfer(dest, &s, !ctrl); else toast(L"Cannot drop here"); sl_clear(&s); free(s.v);
}
static void ribbon_click(int i) {
    int id = g_rb_id[i]; if (!id) return; if (id == CMD_SORT_NAME + 0x4000) { PMenu *m = pm_new(); add_sort_menu(m); popup_at(m, g_rb[i].left, g_rb[i].bottom); pm_free(m); return; }
    if (id == CMD_ACCENT0 + 0x4000) { PMenu *m = pm_new(); add_accent_menu(m); popup_at(m, g_rb[i].left, g_rb[i].bottom); pm_free(m); return; }
    if (id == CMD_EXTRACT_MENU) { PMenu *m = pm_new(); add_extract_menu(m, 1); popup_at(m, g_rb[i].left, g_rb[i].bottom); pm_free(m); return; }
    if (id == CMD_FOLDERCOLOR) { Item *f = focus_item(curtab()); if (!f || !f->dir) { toast(L"Select a folder first"); return; } PMenu *c = pm_new(); for (int k = 0; k < 12; k++) pm_swatch(c, CMD_COLOR0 + k, FOLDER_PAL_NAMES[k], RGB((FOLDER_PALETTE[k] >> 16) & 255, (FOLDER_PALETTE[k] >> 8) & 255, FOLDER_PALETTE[k] & 255), 0); pm_sep(c); mi(c, CMD_FOLDERCOLOR_CLEAR, L"Default colour", 0, 1); popup_at(c, g_rb[i].left, g_rb[i].bottom); pm_free(c); return; }
    if (id == CMD_SETICON) { Item *f = focus_item(curtab()); PMenu *ic = pm_new(); mi(ic, CMD_SETICON, L"For this item...", 0, f != NULL); mi(ic, CMD_SETICON_EXT, L"For all files of this type...", 0, f && !f->dir); mi(ic, CMD_SETICON_DIR, L"For all folders...", 0, 1); pm_sep(ic); mi(ic, CMD_CLEARICON, L"Remove custom icon", 0, f != NULL); popup_at(ic, g_rb[i].left, g_rb[i].bottom); pm_free(ic); return; }
    if (id == CMD_OPENWITH) { Item *f = focus_item(curtab()); if (f && !f->dir) { PMenu *m = pm_new(); ow_collect(); for (int k = 0; k < g_now; k++) mi(m, CMD_OW0 + k, g_ow[k].name, 0, 1); pm_sep(m); mi(m, CMD_OW_MORE, L"Choose another app...", 0, 1); popup_at(m, g_rb[i].left, g_rb[i].bottom); pm_free(m); } return; }
    PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
}
static void on_lbutton(int x, int y, WPARAM wp) {
    Tab *t = curtab(); SetFocus(S.hwnd); int ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0; g_defer_vi = -1;
    for (int i = 0; i < 3; i++) if (in_rect(&LY.capbtn[i], x, y)) { cap_action(i); return; }
    for (int i = 0; i < 6; i++) if (in_rect(&LY.ribtab[i], x, y)) { do_cmd(CMD_TAB0 + i); return; } if (in_rect(&LY.ribcol, x, y)) { do_cmd(CMD_RIBBON); return; }
    if (LY.has_strip && in_rect(&LY.strip, x, y)) { for (int i = 0; i < g_nrb; i++) if (in_rect(&g_rb[i], x, y)) { ribbon_click(i); return; } return; }
    if (in_rect(&LY.back, x, y)) { go_back(); return; } if (in_rect(&LY.fwd, x, y)) { go_fwd(); return; } if (in_rect(&LY.up, x, y)) { go_up(); return; } if (in_rect(&LY.refresh, x, y)) { g_nav_dirty = 1; reload(1); return; }
    if (in_rect(&LY.menu, x, y)) { main_menu(); return; } if (in_rect(&LY.setb, x, y)) { do_cmd(CMD_SETTINGS); return; } if (in_rect(&LY.cmdb, x, y)) { open_cmd(); return; } if (in_rect(&LY.osb, x, y)) { do_cmd(CMD_OSNEXT); return; }
    if (in_rect(&LY.sub, x, y)) { do_cmd(CMD_SUBFOLDERS); return; } if (in_rect(&LY.sdrop, x, y)) { search_menu(); return; } if (in_rect(&LY.search, x, y)) { SetFocus(S.e_search); return; }
    if (in_rect(&LY.addr, x, y)) { for (int i = 0; i < S.ncrumbs; i++) if (in_rect(&S.crumbs[i].r, x, y)) { navigate(S.crumbs[i].path, 1); return; } show_addr(); return; }
    if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) { for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) { if (!S.tabs[i].pinned && x > g_tabr[i].right - px(24)) close_tab(i); else if (i != S.cur) switch_tab(i); return; } if (in_rect(&g_tabplus, x, y)) new_tab(NULL); return; }
    for (int i = 0; i < 3; i++) if (in_rect(&LY.views[i], x, y)) { do_cmd(CMD_VDET + i); return; }
    if (LY.split.right > LY.split.left && in_rect(&LY.split, x, y)) { g_splitdrag = 1; SetCapture(S.hwnd); return; }
    if (x < LY.nav.right && y >= LY.nav.top && y < LY.nav.bottom) { int i = nav_hit(x, y); if (i < 0) return; NavEnt *e = &S.nav[i];
        if (e->root && x < LY.nav.left + px(22) && e->sect < 4) { S.nav_open[e->sect] = !S.nav_open[e->sect]; nav_rebuild(); redraw(); return; }
        if (!e->root && e->exp >= 0 && x < LY.nav.left + px(24) + e->depth * px(14) + px(2)) { nav_toggle(i); return; } WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, e->path); navigate(p, 1); return; }
    if (LY.preview.right > LY.preview.left && in_rect(&LY.preview, x, y)) { POINT pt = { x, y }; ClientToScreen(S.hwnd, &pt); if (pv_click(&g_pc_panel, x, y, pt.x, pt.y + px(20))) { if (g_pc_panel.selecting || g_pc_panel.dragv || g_pc_panel.dragh) { g_pvcap = 1; SetCapture(S.hwnd); } redraw(); } return; }
    if (view_mode() == 0 && in_rect(&LY.hdr, x, y)) { click_header(x); return; }
    if (in_rect(&LY.sb, x, y)) { int th = total_h(), vh = LY.sb.bottom - LY.sb.top; if (th <= vh) return; int tl = vh * vh / th; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = th - vh, ty = LY.sb.top + (int)((double)t->scroll / mx * rng);
        if (y >= ty && y < ty + tl) { S.dragsb = 1; S.sb_off = y - ty; SetCapture(S.hwnd); } else scroll_by(y < ty ? -(vh - px(20)) : (vh - px(20))); return; }
    if (in_rect(&LY.body, x, y)) { int vi = hit_item(x, y);
        if (vi >= 0) { Item *it = &t->it[t->view[vi]]; if (C.checkboxes && it->kind != 1) { RECT cb = check_rect(vi); if (in_rect(&cb, x, y)) { sel_view(vi, 1, 0); return; } }
            if (it->sel && !ctrl && !shift && sel_count(t, NULL) > 1) { g_defer_vi = vi; t->focus = vi; } else sel_view(vi, ctrl, shift);
            if (C.drag_drop && it->kind != 1) { g_in.drag_pend = 1; g_in.drag_x0 = x; g_in.drag_y0 = y; SetCapture(S.hwnd); } }
        else if (C.rubber) { if (!ctrl && !shift) { sel_clear(t); } free(g_rbbase); g_rbbase = (unsigned char *)calloc(t->nv + 1, 1); for (int i = 0; i < t->nv; i++) g_rbbase[i] = t->it[t->view[i]].sel; g_in.rb_on = 1; g_in.rb_x0 = x - LY.body.left; g_in.rb_y0 = y - LY.body.top + t->scroll; g_in.rb_x1 = g_in.rb_x0; g_in.rb_y1 = g_in.rb_y0; SetCapture(S.hwnd); }
        else if (!ctrl && !shift) { sel_clear(t); preview_update(); redraw(); } }
}
static void on_lbuttonup(int x, int y) {
    Tab *t = curtab(); int was = 0;
    if (S.dragsb) { S.dragsb = 0; was = 1; } if (g_splitdrag) { g_splitdrag = 0; cfg_save(); was = 1; }
    if (g_pvcap) { g_pvcap = 0; pv_up(&g_pc_panel); was = 1; }
    if (g_in.rb_on) { g_in.rb_on = 0; free(g_rbbase); g_rbbase = NULL; was = 1; int f = -1; for (int i = 0; i < t->nv; i++) if (t->it[t->view[i]].sel) { f = i; break; } t->focus = f; t->anchor = f; preview_update(); }
    if (g_in.dragging) { g_in.dragging = 0; was = 1; do_drop(GetKeyState(VK_CONTROL) < 0); g_in.drop_item = g_in.drop_nav = -1; }
    else if (g_in.drag_pend) { g_in.drag_pend = 0; was = 1; if (g_defer_vi >= 0) sel_view(g_defer_vi, 0, 0); }
    g_defer_vi = -1; (void)x; (void)y; if (was) { ReleaseCapture(); redraw(); }
}
static void on_mousemove(int x, int y, WPARAM wp) {
    Tab *t = curtab(); if (g_splitdrag) { int W = LY.status.right; C.preview_width = (int)((W - x) / C.scale); if (C.preview_width < 160) C.preview_width = 160; g_pv_override = 1; place_edits(); redraw(); return; }
    if (g_pvcap) { pv_move(&g_pc_panel, x, y); return; } if (S.dragsb) { sb_drag_to(y); return; }
    if (g_in.rb_on) { rubber_update(x, y, (wp & MK_CONTROL) != 0); return; }
    if (g_in.drag_pend && (wp & MK_LBUTTON)) { if (!g_in.dragging && (abs(x - g_in.drag_x0) > px(6) || abs(y - g_in.drag_y0) > px(6))) { g_in.dragging = 1; g_defer_vi = -1; } }
    if (g_in.dragging) { g_in.drop_item = -1; g_in.drop_nav = -1; int vi = hit_item(x, y); if (vi >= 0) { Item *d = &t->it[t->view[vi]]; if (d->dir && !d->sel && d->kind != 1) g_in.drop_item = vi; } else if (x < LY.nav.right) { int ni = nav_hit(x, y); if (ni >= 0 && !S.nav[ni].root) g_in.drop_nav = ni; } redraw(); }
}
static void on_key(WPARAM vk) {
    Tab *t = curtab(); int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0; int m = view_mode(), cw = 1, ch = 1, per = 1; if (m) cell_geom(&cw, &ch, &per);
    int vis = (LY.body.bottom - LY.body.top) / (m == 0 ? px(cur_style()->row_h) : ch) * (m == 0 ? 1 : per); if (vis < 1) vis = 1; int nf = t->focus, step = 0;
    if (alt && vk == VK_LEFT) { go_back(); return; } if (alt && vk == VK_RIGHT) { go_fwd(); return; } if (alt && vk == VK_RETURN) { show_properties(focus_item(t)); return; }
    switch (vk) {
    case VK_UP: step = m ? -per : -1; break; case VK_DOWN: step = m ? per : 1; break; case VK_LEFT: if (m) step = -1; break; case VK_RIGHT: if (m) step = 1; break; case VK_PRIOR: step = -vis; break; case VK_NEXT: step = vis; break;
    case VK_HOME: nf = 0; break; case VK_END: nf = t->nv - 1; break; case VK_RETURN: if (ctrl) return; open_item(focus_item(t)); return;
    case VK_BACK: go_up(); return; case VK_F2: begin_rename(); return; case VK_F5: do_cmd(CMD_REFRESH); return; case VK_F11: if (g_pv.kind >= PK_TEXT) pv_open_full(); return; case VK_DELETE: do_delete(); return; case VK_F4: if (ctrl) close_tab(S.cur); return;
    case VK_TAB: if (ctrl && S.ntabs > 1) switch_tab((S.cur + (shift ? S.ntabs - 1 : 1)) % S.ntabs); return;
    case VK_ESCAPE: if (g_fullwnd) { pv_close_full(); return; } if (t->filter[0]) { set_search_text(L""); apply_search_text(); } else { sel_clear(t); preview_update(); redraw(); } return;
    case 'A': if (ctrl) select_all(); return; case 'C': if (ctrl) { if (g_pv.havesel) pv_copy(0); else do_copy(0); } return; case 'X': if (ctrl) do_copy(1); return; case 'V': if (ctrl) do_paste(); return; case 'T': if (ctrl) new_tab(NULL); return; case 'W': if (ctrl) close_tab(S.cur); return;
    case 'N': if (ctrl && shift) new_item(1); return; case 'L': if (ctrl) show_addr(); return; case 'F': if (ctrl) { SetFocus(S.e_search); SendMessageW(S.e_search, EM_SETSEL, 0, -1); } return;
    case VK_SPACE: if (g_pv.kind == PK_MEDIA) { vid_toggle(); redraw(); } return;
    case VK_OEM_PLUS: case VK_ADD: if (ctrl) do_cmd(CMD_ZOOM_IN); return; case VK_OEM_MINUS: case VK_SUBTRACT: if (ctrl) do_cmd(CMD_ZOOM_OUT); return; default: return; }
    if (!t->nv) return; if (step) nf = (t->focus < 0 ? (step > 0 ? -1 : t->nv) : t->focus) + step; if (nf < 0) nf = 0; if (nf >= t->nv) nf = t->nv - 1; sel_view(nf, 0, shift); ensure_visible(nf); redraw();
}
static void on_char(WCHAR c) {
    if (c < 32 || GetKeyState(VK_CONTROL) < 0) return; Tab *t = curtab(); DWORD now = GetTickCount(); if (now - S.type_t > 1000) S.typebuf[0] = 0; S.type_t = now; int l = lstrlenW(S.typebuf); if (l < 30) { S.typebuf[l] = c; S.typebuf[l + 1] = 0; }
    for (int i = 0; i < t->nv; i++) if (!_wcsnicmp(t->it[t->view[i]].name, S.typebuf, lstrlenW(S.typebuf))) { sel_view(i, 0, 0); ensure_visible(i); redraw(); return; }
}
static void save_session(void) {
    sl_clear(&C.session_tabs); sl_clear(&C.pinned_tabs); for (int i = 0; i < S.ntabs; i++) { sl_add(&C.session_tabs, S.tabs[i].path); if (S.tabs[i].pinned) sl_add(&C.pinned_tabs, S.tabs[i].path); } C.session_cur = S.cur;
    WINDOWPLACEMENT wp; wp.length = sizeof wp; if (GetWindowPlacement(S.hwnd, &wp)) { C.window_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left; C.window_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top; C.window_x = wp.rcNormalPosition.left; C.window_y = wp.rcNormalPosition.top; } cfg_save();
}
/* ---- back buffer ---- */
static HDC g_bdc; static HBITMAP g_bbm, g_bold; static int g_bw, g_bh;
static void bb_free(void) { if (g_bdc) { SelectObject(g_bdc, g_bold); DeleteObject(g_bbm); DeleteDC(g_bdc); g_bdc = NULL; g_bbm = NULL; } g_bw = g_bh = 0; }
static void bb_ensure(HDC ref, int w, int h) { if (g_bdc && g_bw == w && g_bh == h) return; bb_free(); g_bdc = CreateCompatibleDC(ref); g_bbm = CreateCompatibleBitmap(ref, w, h); g_bold = (HBITMAP)SelectObject(g_bdc, g_bbm); g_bw = w; g_bh = h; g_dirty = 1; }
static LRESULT hit_test(int sx, int sy) {
    RECT wr; GetWindowRect(S.hwnd, &wr); int x = sx - wr.left, y = sy - wr.top, W = wr.right - wr.left, H = wr.bottom - wr.top; int b = px(6);
    if (!g_maxed && !IsZoomed(S.hwnd)) { int l = x < b, r = x >= W - b, tp = y < b, bt = y >= H - b; if (tp && l) return HTTOPLEFT; if (tp && r) return HTTOPRIGHT; if (bt && l) return HTBOTTOMLEFT; if (bt && r) return HTBOTTOMRIGHT; if (l) return HTLEFT; if (r) return HTRIGHT; if (tp) return HTTOP; if (bt) return HTBOTTOM; }
    for (int i = 0; i < 3; i++) if (in_rect(&LY.capbtn[i], x, y)) return HTCLIENT; if (y < LY.caption.bottom) return HTCAPTION;
    if (y >= LY.ribtabs.top && y < LY.ribtabs.bottom) { int hit = 0; for (int i = 0; i < 6; i++) if (in_rect(&LY.ribtab[i], x, y)) hit = 1; if (in_rect(&LY.ribcol, x, y)) hit = 1; if (!hit) return HTCAPTION; }   /* empty ribbon-tab strip drags the window */
    if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom && x > g_tabplus.right + px(12)) return HTCAPTION;
    return HTCLIENT;
}
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) { S.hwnd = h; S.rename_item = -1; S.hover_nav = -1; g_in.drop_item = g_in.drop_nav = -1;
        S.e_addr = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL); S.e_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL);
        S.e_rename = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL); HWND es[3] = { S.e_addr, S.e_search, S.e_rename };
        for (int i = 0; i < 3; i++) { WNDPROC o = (WNDPROC)SetWindowLongPtrW(es[i], GWLP_WNDPROC, (LONG_PTR)edit_proc); if (!S.edit_old) S.edit_old = o; } return 0; }
    if (msg == WM_NCCALCSIZE) return 0; if (!S.ntabs) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_NCHITTEST: return hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); case WM_NCACTIVATE: redraw(); return TRUE; case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc); if (rc.right > 0 && rc.bottom > 0) { bb_ensure(dc, rc.right, rc.bottom); if (g_dirty) { g_dirty = 0; draw_client(g_bdc, rc.right, rc.bottom); }
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, g_bdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY); } EndPaint(h, &ps); return 0; }
    case WM_SIZE: if (wp == SIZE_MINIMIZED) return 0; g_maxed = wp == SIZE_MAXIMIZED; place_edits(); clamp_scroll(); g_dirty = 1; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_WINDOWPOSCHANGED: { WINDOWPOS *wpos = (WINDOWPOS *)lp; if (!(wpos->flags & SWP_NOSIZE) && !IsIconic(h)) { g_dirty = 1; } break; }
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; MONITORINFO mi_; memset(&mi_, 0, sizeof mi_); mi_.cbSize = sizeof mi_; if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi_)) { mm->ptMaxPosition.x = mi_.rcWork.left - mi_.rcMonitor.left; mm->ptMaxPosition.y = mi_.rcWork.top - mi_.rcMonitor.top; mm->ptMaxSize.x = mi_.rcWork.right - mi_.rcWork.left; mm->ptMaxSize.y = mi_.rcWork.bottom - mi_.rcWork.top; }
        mm->ptMinTrackSize.x = 640; mm->ptMinTrackSize.y = 420; return 0; }
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_RESTORE || (wp & 0xFFF0) == SC_MAXIMIZE) g_dirty = 1; break;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, S.pal.text); SetBkColor((HDC)wp, S.pal.input); return (LRESULT)S.br_input;
    case WM_COMMAND: if ((HWND)lp == S.e_search && HIWORD(wp) == EN_CHANGE) { if (!S.noevent) SetTimer(h, TIMER_SEARCH, 400, NULL); return 0; } if (!lp) { int id = LOWORD(wp); if (id == PB_PLAY + 5000) { vid_toggle(); redraw(); } else do_cmd(id); } return 0;
    case WM_APP_EDITKEY: on_editkey((HWND)wp, (int)lp); return 0; case WM_APP_REFRESH: redraw(); return 0; case WM_APP_ASK: { AskReq *a = (AskReq *)lp; a->result = fm_msgbox(a->text, a->title, a->flags); return 0; }
    case WM_APP_TASKDONE: InterlockedExchange(&S.busy, 0); reload(1); toast(S.status_msg); return 0;
    case WM_APP + 9: show_about(); return 0;
    case WM_TIMER:
        if (wp == TIMER_SEARCH) { KillTimer(h, TIMER_SEARCH); apply_search_text(); }
        else if (wp == TIMER_UI) { poll_search(); if (S.toast[0] && GetTickCount() - S.toast_t > 2900 && GetTickCount() - S.toast_t < 3500) redraw(); if (g_thumb_more) redraw(); }
        else if (wp == TIMER_NAV) { static DWORD lastmask; DWORD m = GetLogicalDrives(); if (m != lastmask) { lastmask = m; g_nav_dirty = 1; redraw(); } }
        else if (wp == TIMER_VID) { vid_poll(); if (g_fullwnd) InvalidateRect(g_fullwnd, NULL, FALSE); else if (g_pv.kind == PK_MEDIA) redraw(); }
        return 0;
    case WM_LBUTTONDOWN: on_lbutton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp); return 0;
    case WM_LBUTTONUP: on_lbuttonup(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_CAPTURECHANGED: if ((HWND)lp != h && (g_in.rb_on || g_in.dragging || g_in.drag_pend)) { g_in.rb_on = g_in.dragging = g_in.drag_pend = 0; redraw(); } return 0;
    case WM_LBUTTONDBLCLK: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); if (in_rect(&LY.caption, x, y)) return 0; int vi = hit_item(x, y); if (vi >= 0) { sel_view(vi, 0, 0); open_item(focus_item(curtab())); } else if (view_mode() == 0 && in_rect(&LY.hdr, x, y)) click_header(x); else if (in_rect(&LY.preview, x, y) && g_pv.kind >= PK_TEXT && !in_rect(&g_pc_panel.hdr, x, y)) pv_open_full(); return 0; }
    case WM_RBUTTONDOWN: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); SetFocus(h); int vi = hit_item(x, y); if (vi >= 0 && !curtab()->it[curtab()->view[vi]].sel) sel_view(vi, 0, 0); else if (vi >= 0) curtab()->focus = vi;
        if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) { for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) { tab_menu(i, x, y); return 0; } }
        else if (in_rect(&LY.ribtabs, x, y) || in_rect(&LY.strip, x, y)) { PMenu *m = pm_new(); mi(m, CMD_RIBBON, L"Show command strip", C.ribbon_open, 1); popup_at(m, x, y); pm_free(m); }
        else if (vi >= 0) item_menu(x, y); else if (in_rect(&LY.body, x, y)) { sel_clear(curtab()); preview_update(); redraw(); blank_menu(x, y); } return 0; }
    case WM_MBUTTONUP: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) { close_tab(i); break; } return 0; }
    case WM_MOUSEMOVE: { static unsigned hk; int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); g_mx = x; g_my = y; TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te);
        if (g_splitdrag || g_pvcap || S.dragsb || g_in.rb_on || g_in.drag_pend || g_in.dragging) { on_mousemove(x, y, wp); return 0; }
        int ni = nav_hit(x, y); if (x >= LY.nav.right) ni = -1; S.hover_nav = ni; unsigned k = (unsigned)region_at(x, y) * 1000003u + (unsigned)(hit_item(x, y) + 1) * 131u + (unsigned)(ni + 1);
        if (in_rect(&LY.preview, x, y) && g_pv.kind != PK_NONE) { for (int i = 0; i < g_pc_panel.nb; i++) if (in_rect(&g_pc_panel.b[i].r, x, y)) k = k * 31 + i + 1; }
        if (g_pv.kind == PK_MEDIA && in_rect(&g_pc_panel.mc, x, y)) k = k * 7 + (unsigned)(x / px(12)); if (C.checkboxes) { int vi = hit_item(x, y); if (vi >= 0) { RECT cb = check_rect(vi); if (in_rect(&cb, x, y)) k ^= 0x5bd1e995u; } }
        if (k != hk) { hk = k; redraw(); } return 0; }
    case WM_MOUSELEAVE: g_mx = g_my = -1; S.hover_nav = -1; redraw(); return 0;
    case WM_MOUSEWHEEL: { short d = GET_WHEEL_DELTA_WPARAM(wp); POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &pt);
        if (pt.x < LY.nav.right && pt.y >= LY.nav.top && pt.y < LY.nav.bottom) { int tot = S.nnav * px(cur_style()->nav_row), vh = LY.nav.bottom - LY.nav.top; S.nav_scroll -= d * px(cur_style()->nav_row) * 3 / 120; if (S.nav_scroll > tot - vh + px(8)) S.nav_scroll = tot - vh + px(8); if (S.nav_scroll < 0) S.nav_scroll = 0; redraw(); return 0; }
        if (LY.preview.right - LY.preview.left > 2 && pt.x >= LY.preview.left && pt.y >= LY.preview.top) { if (pv_wheel(&g_pc_panel, d, GetKeyState(VK_CONTROL) < 0, pt.x, pt.y)) return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && view_mode() == 1) { do_cmd(d > 0 ? CMD_ICON_PLUS : CMD_ICON_MINUS); return 0; } if (GetKeyState(VK_CONTROL) < 0) { do_cmd(d > 0 ? CMD_ZOOM_IN : CMD_ZOOM_OUT); return 0; } scroll_by(-d * px(cur_style()->row_h) * 3 / 120); return 0; }
    case WM_KEYDOWN: if (LY.preview.right > LY.preview.left && in_rect(&LY.preview, g_mx, g_my) && g_pv.npg && (wp == VK_OEM_PLUS || wp == VK_OEM_MINUS)) { pv_key(&g_pc_panel, wp); return 0; } on_key(wp); return 0;
    case WM_SYSKEYDOWN: if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN) { on_key(wp); return 0; } break; case WM_CHAR: on_char((WCHAR)wp); return 0; case WM_SETFOCUS: return 0;
    case WM_COPYDATA: { COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp; if (cd && cd->lpData && cd->cbData >= 2) new_tab((const WCHAR *)cd->lpData); if (IsIconic(h)) ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); return TRUE; }
    case WM_CLOSE: pv_close_full(); vid_close(); save_session(); DestroyWindow(h); return 0;
    case WM_DESTROY: cancel_search(); bb_free(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    (void)hp; (void)cmd; log_init(); detect_wine(); fm_log("Files (C) starting - Wine=%d", ON_WINE); CoInitialize(NULL);
    int argc = 0; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc); WCHAR open_path[MAX_PATH] = L"";
    for (int i = 1; i < argc; i++) { if (!wcscmp(argv[i], L"--open") && i + 1 < argc) scpy(open_path, MAX_PATH, argv[++i]); else if (argv[i][0] != '-' && !open_path[0]) scpy(open_path, MAX_PATH, argv[i]); }
    HANDLE mtx = CreateMutexW(NULL, FALSE, FM_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND w = FindWindowW(FM_CLASS, NULL); if (w) { if (open_path[0]) { COPYDATASTRUCT cd = { 0, (lstrlenW(open_path) + 1) * sizeof(WCHAR), open_path }; SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cd); } if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); } return 0; }
    cfg_init_paths(); cfg_load(); gdip_start(); doc_fonts_load(); pm_register();
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = FM_CLASS;
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE); if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); wc.hIconSm = wc.hIcon; RegisterClassExW(&wc);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT; if (C.window_x != CFG_UNSET && C.window_y != CFG_UNSET) { int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN); if (C.window_x > -C.window_w + 100 && C.window_x < sw - 100 && C.window_y > -20 && C.window_y < sh - 100) { x = C.window_x; y = C.window_y; } }
    HWND w = CreateWindowExW(WS_EX_APPWINDOW, FM_CLASS, L"Files(Winlator/Wine)", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN, x, y, C.window_w, C.window_h, NULL, NULL, hi, NULL);
    if (!w) { fm_log("CreateWindow failed %lu", GetLastError()); return 1; } update_pal(); apply_fonts(); int restored = 0;
    if (!open_path[0] && C.restore_session && C.session_tabs.n) { for (int i = 0; i < C.session_tabs.n && S.ntabs < FM_MAXTABS; i++) { const WCHAR *p = C.session_tabs.v[i]; if (p[0] && !is_special(p) && !dir_exists(p)) continue; tab_init(&S.tabs[S.ntabs], p); S.tabs[S.ntabs].pinned = sl_find(&C.pinned_tabs, p) >= 0; S.ntabs++; } if (S.ntabs) { restored = 1; S.cur = C.session_cur >= 0 && C.session_cur < S.ntabs ? C.session_cur : 0; } }
    if (!restored) { WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, open_path[0] ? open_path : C.last_dir); if (p[0]) norm_path(p); if (p[0] && !dir_exists(p)) p[0] = 0; if (!p[0]) scpy(p, MAX_PATH, L"::quick"); tab_init(&S.tabs[0], p); S.ntabs = 1; S.cur = 0; } else if (open_path[0]) new_tab(open_path);
    set_search_text(curtab()->filter); reload(0); set_title(); place_edits(); SetTimer(w, TIMER_UI, 250, NULL); SetTimer(w, TIMER_NAV, 2000, NULL);
    ShowWindow(w, show); UpdateWindow(w); run_autostart(0);
    { int td = today_days(); if (C.about_day <= 0 || td - C.about_day >= 30) { C.about_day = td; cfg_save(); PostMessageW(w, WM_APP + 9, 0, 0); } }
    fm_log("Files ready"); MSG m; while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); } CloseHandle(mtx); return (int)m.wParam;
}
