#ifndef VT_H
#define VT_H

/* vt - a small xterm-compatible terminal emulator. */

#include <stddef.h>
#include <stdint.h>

/* fg / bg: the terminal's default colour */
#define VT_DEFAULT 16u

#define VT_BOLD 1u
#define VT_UNDERLINE 2u
#define VT_REVERSE 4u

/* the right half of a wide character */
#define VT_CONT 0x1FFFFFu

#define VT_PARAMS_MAX 16
#define VT_OSC_MAX 256
#define VT_TITLE_MAX 64

typedef struct vt_cell
{
    uint32_t ch;   /* 0 is blank */
    uint32_t mark; /* one combining mark, or 0 */
    uint8_t fg, bg, attr;
} vt_cell;

typedef struct vt_cursor
{
    int x, y;
    uint8_t fg, bg, attr;
    uint8_t origin, wrap_pending;
    uint8_t charset[2], gl;
} vt_cursor;

typedef struct vt
{
    int cols, rows;
    vt_cell *cells; /* the screen in use: main or alt */
    vt_cell *main, *alt;
    uint8_t *tabs;

    int x, y;
    uint8_t fg, bg, attr;
    uint8_t wrap_pending;
    int top, bot;           /* scroll region, inclusive */
    uint8_t charset[2], gl; /* 0 ASCII, 1 DEC line drawing */
    vt_cursor saved;

    /* modes */
    uint8_t origin, autowrap, insert, cursor_visible, app_cursor, app_keypad, alt_active, bracketed_paste;

    /* parser */
    uint8_t state;
    int params[VT_PARAMS_MAX];
    int nparams;
    uint8_t priv;  /* '?', '>', '=' or 0 */
    uint8_t inter; /* intermediate byte or 0 */
    uint32_t utf8_cp;
    uint8_t utf8_need;
    char osc[VT_OSC_MAX];
    int osc_len;
    uint32_t last_ch; /* for REP */

    char title[VT_TITLE_MAX];
    uint8_t title_changed;

    int (*width)(uint32_t ch); /* columns a character takes: 0, 1 or 2 */
    void (*reply)(void *user, const char *bytes, size_t n);
    void *user;
} vt;

/* 0 on success, -1 if out of memory. width and reply must be set. */
int vt_init(vt *v, int cols, int rows, int (*width)(uint32_t ch),
            void (*reply)(void *user, const char *bytes, size_t n), void *user);
void vt_free(vt *v);
/* Keeps what fits, with the cursor line still on screen. 0 on success. */
int vt_resize(vt *v, int cols, int rows);
void vt_write(vt *v, const uint8_t *bytes, size_t n);

static inline vt_cell *vt_cell_at(const vt *v, int x, int y)
{
    return &v->cells[(size_t)y * (size_t)v->cols + (size_t)x];
}

#endif
