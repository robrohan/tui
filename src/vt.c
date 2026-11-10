#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    kGround,
    kEsc,
    kEscInter,
    kCsi,
    kOsc,
    kOscEsc,
    kString, /* DCS, SOS, PM, APC: ignored up to ST */
    kStringEsc
};

/* DEC special graphics, 0x5F..0x7E */
static const uint16_t kDecGraphics[32] = {0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
                                          0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
                                          0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
                                          0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};

/* xterm's 16 colours, for mapping 256-colour and RGB requests */
static const uint8_t kPalette[16][3] = {{0x00, 0x00, 0x00}, {0xCD, 0x00, 0x00}, {0x00, 0xCD, 0x00}, {0xCD, 0xCD, 0x00},
                                        {0x00, 0x00, 0xEE}, {0xCD, 0x00, 0xCD}, {0x00, 0xCD, 0xCD}, {0xE5, 0xE5, 0xE5},
                                        {0x7F, 0x7F, 0x7F}, {0xFF, 0x00, 0x00}, {0x00, 0xFF, 0x00}, {0xFF, 0xFF, 0x00},
                                        {0x5C, 0x5C, 0xFF}, {0xFF, 0x00, 0xFF}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF}};

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void reply(vt *v, const char *s)
{
    v->reply(v->user, s, strlen(s));
}

/* ---------------- cells ---------------- */

static vt_cell blank(const vt *v)
{
    vt_cell c;
    c.ch = 0;
    c.mark = 0;
    c.fg = VT_DEFAULT;
    c.bg = v->bg; /* erasing paints the current background, like xterm */
    c.attr = 0;
    return c;
}

static void clear_cells(vt *v, int x0, int y, int x1)
{
    vt_cell b = blank(v);
    int x;
    for (x = x0; x < x1; x++)
    {
        *vt_cell_at(v, x, y) = b;
    }
}

static void clear_rows(vt *v, int y0, int y1)
{
    int y;
    for (y = y0; y < y1; y++)
    {
        clear_cells(v, 0, y, v->cols);
    }
}

/* scroll rows top..bot up by n (n < 0: down) */
static void scroll(vt *v, int top, int bot, int n)
{
    size_t row = (size_t)v->cols;
    int height = bot - top + 1;
    if (n == 0 || height <= 0)
    {
        return;
    }
    if (n >= height || -n >= height)
    {
        clear_rows(v, top, bot + 1);
        return;
    }
    if (n > 0)
    {
        memmove(vt_cell_at(v, 0, top), vt_cell_at(v, 0, top + n), (size_t)(height - n) * row * sizeof(vt_cell));
        clear_rows(v, bot - n + 1, bot + 1);
    }
    else
    {
        n = -n;
        memmove(vt_cell_at(v, 0, top + n), vt_cell_at(v, 0, top), (size_t)(height - n) * row * sizeof(vt_cell));
        clear_rows(v, top, top + n);
    }
}

static void linefeed(vt *v)
{
    if (v->y == v->bot)
    {
        scroll(v, v->top, v->bot, 1);
    }
    else if (v->y < v->rows - 1)
    {
        v->y++;
    }
}

static void reverse_index(vt *v)
{
    if (v->y == v->top)
    {
        scroll(v, v->top, v->bot, -1);
    }
    else if (v->y > 0)
    {
        v->y--;
    }
}

/* writing over half of a wide character blanks the other half */
static void break_wide(vt *v, int x, int y)
{
    vt_cell *c = vt_cell_at(v, x, y);
    if (c->ch == VT_CONT && x > 0)
    {
        vt_cell_at(v, x - 1, y)->ch = 0;
    }
    if (x + 1 < v->cols && vt_cell_at(v, x + 1, y)->ch == VT_CONT)
    {
        vt_cell_at(v, x + 1, y)->ch = 0;
    }
}

static void print(vt *v, uint32_t ch)
{
    int w;
    vt_cell *c;

    if (v->charset[v->gl] && ch >= 0x5F && ch <= 0x7E)
    {
        ch = kDecGraphics[ch - 0x5F];
    }
    w = v->width(ch);
    if (w == 0)
    {
        /* a combining mark goes on the character before the cursor */
        int x = v->wrap_pending ? v->x : v->x - 1;
        if (x < 0)
        {
            return;
        }
        c = vt_cell_at(v, x, v->y);
        if (c->ch == VT_CONT && x > 0)
        {
            c = vt_cell_at(v, x - 1, v->y);
        }
        if (c->mark == 0)
        {
            c->mark = ch;
        }
        return;
    }
    if (w > v->cols)
    {
        return;
    }
    if (v->wrap_pending)
    {
        v->wrap_pending = 0;
        if (v->autowrap)
        {
            v->x = 0;
            linefeed(v);
        }
    }
    if (v->x + w > v->cols)
    {
        /* a wide character that doesn't fit on this line */
        if (!v->autowrap)
        {
            return;
        }
        clear_cells(v, v->x, v->y, v->cols);
        v->x = 0;
        linefeed(v);
    }
    if (v->insert)
    {
        vt_cell *row = vt_cell_at(v, 0, v->y);
        memmove(row + v->x + w, row + v->x, (size_t)(v->cols - v->x - w) * sizeof(vt_cell));
    }
    break_wide(v, v->x, v->y);
    if (w == 2)
    {
        break_wide(v, v->x + 1, v->y);
    }
    c = vt_cell_at(v, v->x, v->y);
    c->ch = ch;
    c->mark = 0;
    c->fg = v->fg;
    c->bg = v->bg;
    c->attr = v->attr;
    if (w == 2)
    {
        c[1] = c[0];
        c[1].ch = VT_CONT;
    }
    v->last_ch = ch;
    v->x += w;
    if (v->x >= v->cols)
    {
        v->x = v->cols - 1;
        v->wrap_pending = 1;
    }
}

/* ---------------- cursor ---------------- */

static void move_to(vt *v, int x, int y)
{
    int top = v->origin ? v->top : 0;
    int bot = v->origin ? v->bot : v->rows - 1;
    v->x = clampi(x, 0, v->cols - 1);
    v->y = clampi(y + top, top, bot);
    v->wrap_pending = 0;
}

static void save_cursor(vt *v)
{
    vt_cursor *s = &v->saved;
    s->x = v->x;
    s->y = v->y;
    s->fg = v->fg;
    s->bg = v->bg;
    s->attr = v->attr;
    s->origin = v->origin;
    s->wrap_pending = v->wrap_pending;
    s->charset[0] = v->charset[0];
    s->charset[1] = v->charset[1];
    s->gl = v->gl;
}

static void restore_cursor(vt *v)
{
    const vt_cursor *s = &v->saved;
    v->x = clampi(s->x, 0, v->cols - 1);
    v->y = clampi(s->y, 0, v->rows - 1);
    v->fg = s->fg;
    v->bg = s->bg;
    v->attr = s->attr;
    v->origin = s->origin;
    v->wrap_pending = s->wrap_pending;
    v->charset[0] = s->charset[0];
    v->charset[1] = s->charset[1];
    v->gl = s->gl;
}

static void tab(vt *v, int n)
{
    while (n > 0 && v->x < v->cols - 1)
    {
        v->x++;
        if (v->tabs[v->x])
        {
            n--;
        }
    }
    while (n < 0 && v->x > 0)
    {
        v->x--;
        if (v->tabs[v->x])
        {
            n++;
        }
    }
    v->wrap_pending = 0;
}

static void reset_tabs(vt *v)
{
    int x;
    for (x = 0; x < v->cols; x++)
    {
        v->tabs[x] = x > 0 && x % 8 == 0;
    }
}

static void soft_reset(vt *v)
{
    v->fg = v->bg = VT_DEFAULT;
    v->attr = 0;
    v->top = 0;
    v->bot = v->rows - 1;
    v->origin = 0;
    v->autowrap = 1;
    v->insert = 0;
    v->cursor_visible = 1;
    v->app_cursor = 0;
    v->app_keypad = 0;
    v->charset[0] = v->charset[1] = 0;
    v->gl = 0;
    v->wrap_pending = 0;
    save_cursor(v);
}

static void set_alt_screen(vt *v, int on)
{
    if (on == v->alt_active)
    {
        return;
    }
    v->alt_active = (uint8_t)on;
    v->cells = on ? v->alt : v->main;
}

static void full_reset(vt *v)
{
    set_alt_screen(v, 0);
    v->bracketed_paste = 0;
    soft_reset(v);
    reset_tabs(v);
    clear_rows(v, 0, v->rows);
    v->x = v->y = 0;
    save_cursor(v);
}

/* ---------------- colours ---------------- */

static uint8_t nearest(int r, int g, int b)
{
    int best = 0, i;
    long best_d = -1;
    for (i = 0; i < 16; i++)
    {
        long dr = r - kPalette[i][0], dg = g - kPalette[i][1], db = b - kPalette[i][2];
        long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

static uint8_t from_256(int n)
{
    static const int kLevels[6] = {0, 0x5F, 0x87, 0xAF, 0xD7, 0xFF};
    if (n < 16)
    {
        return (uint8_t)n;
    }
    if (n < 232)
    {
        n -= 16;
        return nearest(kLevels[n / 36], kLevels[(n / 6) % 6], kLevels[n % 6]);
    }
    n = 8 + (n - 232) * 10;
    return nearest(n, n, n);
}

/* 38 / 48: ;5;n or ;2;r;g;b. Returns the parameters used after i. */
static int extended_colour(const vt *v, int i, uint8_t *out)
{
    if (i + 1 < v->nparams && v->params[i + 1] == 5)
    {
        if (i + 2 < v->nparams)
        {
            *out = from_256(clampi(v->params[i + 2], 0, 255));
        }
        return 2;
    }
    if (i + 1 < v->nparams && v->params[i + 1] == 2)
    {
        if (i + 4 < v->nparams)
        {
            *out = nearest(clampi(v->params[i + 2], 0, 255), clampi(v->params[i + 3], 0, 255),
                           clampi(v->params[i + 4], 0, 255));
        }
        return 4;
    }
    return 0;
}

static void sgr(vt *v)
{
    int i;
    if (v->nparams == 0)
    {
        v->nparams = 1;
        v->params[0] = 0;
    }
    for (i = 0; i < v->nparams; i++)
    {
        int p = v->params[i];
        if (p == 0)
        {
            v->fg = v->bg = VT_DEFAULT;
            v->attr = 0;
        }
        else if (p == 1)
            v->attr |= VT_BOLD;
        else if (p == 4)
            v->attr |= VT_UNDERLINE;
        else if (p == 7)
            v->attr |= VT_REVERSE;
        else if (p == 22)
            v->attr &= (uint8_t)~VT_BOLD;
        else if (p == 24)
            v->attr &= (uint8_t)~VT_UNDERLINE;
        else if (p == 27)
            v->attr &= (uint8_t)~VT_REVERSE;
        else if (p >= 30 && p <= 37)
            v->fg = (uint8_t)(p - 30);
        else if (p == 38)
            i += extended_colour(v, i, &v->fg);
        else if (p == 39)
            v->fg = VT_DEFAULT;
        else if (p >= 40 && p <= 47)
            v->bg = (uint8_t)(p - 40);
        else if (p == 48)
            i += extended_colour(v, i, &v->bg);
        else if (p == 49)
            v->bg = VT_DEFAULT;
        else if (p >= 90 && p <= 97)
            v->fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107)
            v->bg = (uint8_t)(p - 100 + 8);
        /* dim, italic, blink, strike and the rest are ignored */
    }
}

/* ---------------- control sequences ---------------- */

static int param(const vt *v, int i, int def)
{
    return i < v->nparams && v->params[i] > 0 ? v->params[i] : def;
}

static void set_mode(vt *v, int on)
{
    int i;
    for (i = 0; i < v->nparams; i++)
    {
        int p = v->params[i];
        if (v->priv == '?')
        {
            switch (p)
            {
            case 1:
                v->app_cursor = (uint8_t)on;
                break;
            case 6:
                v->origin = (uint8_t)on;
                move_to(v, 0, 0);
                break;
            case 7:
                v->autowrap = (uint8_t)on;
                break;
            case 25:
                v->cursor_visible = (uint8_t)on;
                break;
            case 47:
            case 1047:
                if (!on && v->alt_active)
                {
                    clear_rows(v, 0, v->rows);
                }
                set_alt_screen(v, on);
                break;
            case 1048:
                if (on)
                    save_cursor(v);
                else
                    restore_cursor(v);
                break;
            case 1049:
                if (on)
                {
                    save_cursor(v);
                    set_alt_screen(v, 1);
                    clear_rows(v, 0, v->rows);
                }
                else
                {
                    set_alt_screen(v, 0);
                    restore_cursor(v);
                }
                break;
            case 2004:
                v->bracketed_paste = (uint8_t)on;
                break;
            default:
                break; /* mouse modes and the rest are ignored */
            }
        }
        else if (v->priv == 0 && p == 4)
        {
            v->insert = (uint8_t)on;
        }
    }
}

static void device_status(vt *v)
{
    char buf[32];
    int p = param(v, 0, 0);
    if (v->priv != 0 && v->priv != '?')
    {
        return;
    }
    if (p == 5)
    {
        reply(v, "\x1B[0n");
    }
    else if (p == 6)
    {
        int y = v->origin ? v->y - v->top : v->y;
        snprintf(buf, sizeof(buf), "\x1B[%s%d;%dR", v->priv == '?' ? "?" : "", y + 1, v->x + 1);
        reply(v, buf);
    }
}

static void csi(vt *v, uint8_t final)
{
    int n = param(v, 0, 1);

    if (v->inter == ' ' || v->inter == '"' || v->inter == '$')
    {
        return; /* cursor style, protection, rectangles: ignored */
    }
    if (v->inter == '!' && final == 'p')
    {
        soft_reset(v);
        return;
    }
    if (v->priv && final != 'h' && final != 'l' && final != 'c' && final != 'n' && final != 'J' && final != 'K')
    {
        return; /* xterm private extensions (modifyOtherKeys, ...) */
    }
    switch (final)
    {
    case '@':
        if (v->x < v->cols)
        {
            vt_cell *row = vt_cell_at(v, 0, v->y);
            n = clampi(n, 1, v->cols - v->x);
            memmove(row + v->x + n, row + v->x, (size_t)(v->cols - v->x - n) * sizeof(vt_cell));
            clear_cells(v, v->x, v->y, v->x + n);
        }
        v->wrap_pending = 0;
        break;
    case 'A':
        v->y = clampi(v->y - n, v->y >= v->top ? v->top : 0, v->rows - 1), v->wrap_pending = 0;
        break;
    case 'B':
    case 'e':
        v->y = clampi(v->y + n, 0, v->y <= v->bot ? v->bot : v->rows - 1), v->wrap_pending = 0;
        break;
    case 'C':
    case 'a':
        v->x = clampi(v->x + n, 0, v->cols - 1), v->wrap_pending = 0;
        break;
    case 'D':
        v->x = clampi(v->x - n, 0, v->cols - 1), v->wrap_pending = 0;
        break;
    case 'E':
        v->y = clampi(v->y + n, 0, v->y <= v->bot ? v->bot : v->rows - 1);
        v->x = 0, v->wrap_pending = 0;
        break;
    case 'F':
        v->y = clampi(v->y - n, v->y >= v->top ? v->top : 0, v->rows - 1);
        v->x = 0, v->wrap_pending = 0;
        break;
    case 'G':
    case '`':
        v->x = clampi(n - 1, 0, v->cols - 1), v->wrap_pending = 0;
        break;
    case 'H':
    case 'f':
        move_to(v, param(v, 1, 1) - 1, n - 1);
        break;
    case 'I':
        tab(v, n);
        break;
    case 'Z':
        tab(v, -n);
        break;
    case 'J':
        switch (param(v, 0, 0))
        {
        case 0:
            clear_cells(v, v->x, v->y, v->cols);
            clear_rows(v, v->y + 1, v->rows);
            break;
        case 1:
            clear_rows(v, 0, v->y);
            clear_cells(v, 0, v->y, v->x + 1);
            break;
        case 2:
            clear_rows(v, 0, v->rows);
            break;
        default:
            break; /* 3: scrollback, which we don't keep */
        }
        v->wrap_pending = 0;
        break;
    case 'K':
        switch (param(v, 0, 0))
        {
        case 0:
            clear_cells(v, v->x, v->y, v->cols);
            break;
        case 1:
            clear_cells(v, 0, v->y, v->x + 1);
            break;
        case 2:
            clear_cells(v, 0, v->y, v->cols);
            break;
        default:
            break;
        }
        v->wrap_pending = 0;
        break;
    case 'L':
    case 'M':
        if (v->y >= v->top && v->y <= v->bot)
        {
            scroll(v, v->y, v->bot, final == 'L' ? -n : n);
            v->x = 0;
        }
        v->wrap_pending = 0;
        break;
    case 'P': {
        vt_cell *row = vt_cell_at(v, 0, v->y);
        n = clampi(n, 1, v->cols - v->x);
        memmove(row + v->x, row + v->x + n, (size_t)(v->cols - v->x - n) * sizeof(vt_cell));
        clear_cells(v, v->cols - n, v->y, v->cols);
        v->wrap_pending = 0;
        break;
    }
    case 'S':
        scroll(v, v->top, v->bot, n);
        break;
    case 'T':
        scroll(v, v->top, v->bot, -n);
        break;
    case 'X':
        clear_cells(v, v->x, v->y, clampi(v->x + n, 0, v->cols));
        v->wrap_pending = 0;
        break;
    case 'b':
        if (v->last_ch)
        {
            n = clampi(n, 1, 65535);
            while (n-- > 0)
            {
                print(v, v->last_ch);
            }
        }
        break;
    case 'c':
        if (param(v, 0, 0) != 0)
            break;
        if (v->priv == 0)
            reply(v, "\x1B[?1;2c");
        else if (v->priv == '>')
            reply(v, "\x1B[>0;0;0c");
        break;
    case 'd':
        move_to(v, v->x, n - 1);
        break;
    case 'g':
        if (param(v, 0, 0) == 0)
            v->tabs[v->x] = 0;
        else if (param(v, 0, 0) == 3)
            memset(v->tabs, 0, (size_t)v->cols);
        break;
    case 'h':
        set_mode(v, 1);
        break;
    case 'l':
        set_mode(v, 0);
        break;
    case 'm':
        sgr(v);
        break;
    case 'n':
        device_status(v);
        break;
    case 'r': {
        int top = param(v, 0, 1) - 1, bot = param(v, 1, v->rows) - 1;
        bot = clampi(bot, 0, v->rows - 1);
        if (top < bot)
        {
            v->top = top;
            v->bot = bot;
            move_to(v, 0, 0);
        }
        break;
    }
    case 's':
        save_cursor(v);
        break;
    case 'u':
        restore_cursor(v);
        break;
    default:
        break; /* window ops (t) and the rest */
    }
}

static void esc(vt *v, uint8_t final)
{
    if (v->inter == '(' || v->inter == ')')
    {
        v->charset[v->inter == ')'] = final == '0';
        return;
    }
    if (v->inter)
    {
        return; /* ESC # 8 and friends */
    }
    switch (final)
    {
    case '7':
        save_cursor(v);
        break;
    case '8':
        restore_cursor(v);
        break;
    case 'D':
        linefeed(v);
        break;
    case 'E':
        v->x = 0;
        linefeed(v);
        break;
    case 'H':
        v->tabs[v->x] = 1;
        break;
    case 'M':
        reverse_index(v);
        break;
    case 'c':
        full_reset(v);
        break;
    case '=':
        v->app_keypad = 1;
        break;
    case '>':
        v->app_keypad = 0;
        break;
    default:
        break;
    }
    v->wrap_pending = 0;
}

static void osc(vt *v)
{
    int n = 0, i = 0;
    const char *text;
    v->osc[v->osc_len] = '\0';
    while (v->osc[i] >= '0' && v->osc[i] <= '9')
    {
        n = n * 10 + (v->osc[i++] - '0');
    }
    if (v->osc[i] != ';')
    {
        return;
    }
    text = v->osc + i + 1;
    if (n == 0 || n == 2)
    {
        snprintf(v->title, sizeof(v->title), "%s", text);
        v->title_changed = 1;
    }
    else if ((n == 10 || n == 11) && strcmp(text, "?") == 0)
    {
        /* programs ask for the colours to choose light or dark themes */
        reply(v, n == 10 ? "\x1B]10;rgb:e5e5/e5e5/e5e5\x1B\\" : "\x1B]11;rgb:0000/0000/0000\x1B\\");
    }
}

static void control(vt *v, uint8_t c)
{
    switch (c)
    {
    case 0x08:
        if (v->x > 0)
            v->x--;
        v->wrap_pending = 0;
        break;
    case 0x09:
        tab(v, 1);
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        linefeed(v);
        break;
    case 0x0D:
        v->x = 0;
        v->wrap_pending = 0;
        break;
    case 0x0E:
        v->gl = 1;
        break;
    case 0x0F:
        v->gl = 0;
        break;
    default:
        break; /* BEL and the rest */
    }
}

static void begin_sequence(vt *v, uint8_t state)
{
    v->state = state;
    v->nparams = 0;
    v->params[0] = 0;
    v->priv = 0;
    v->inter = 0;
    v->osc_len = 0;
}

static void ground_byte(vt *v, uint8_t c)
{
    if (c < 0x80)
    {
        v->utf8_need = 0;
        print(v, c);
        return;
    }
    if (c < 0xC0)
    {
        if (v->utf8_need == 0)
        {
            print(v, 0xFFFD);
            return;
        }
        v->utf8_cp = (v->utf8_cp << 6) | (c & 0x3Fu);
        if (--v->utf8_need == 0)
        {
            uint32_t cp = v->utf8_cp;
            print(v, (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) ? 0xFFFD : cp);
        }
        return;
    }
    if (v->utf8_need)
    {
        print(v, 0xFFFD);
    }
    if (c < 0xE0)
    {
        v->utf8_cp = c & 0x1Fu, v->utf8_need = 1;
    }
    else if (c < 0xF0)
    {
        v->utf8_cp = c & 0x0Fu, v->utf8_need = 2;
    }
    else if (c < 0xF8)
    {
        v->utf8_cp = c & 0x07u, v->utf8_need = 3;
    }
    else
    {
        v->utf8_need = 0;
        print(v, 0xFFFD);
    }
}

static void byte(vt *v, uint8_t c)
{
    /* string states swallow everything up to their terminator */
    switch (v->state)
    {
    case kOsc:
        if (c == 0x07)
        {
            osc(v);
            v->state = kGround;
        }
        else if (c == 0x1B)
            v->state = kOscEsc;
        else if (c >= 0x20 && v->osc_len < VT_OSC_MAX - 1)
            v->osc[v->osc_len++] = (char)c;
        return;
    case kOscEsc:
        osc(v);
        v->state = kGround;
        if (c != '\\')
        {
            byte(v, 0x1B);
            byte(v, c);
        }
        return;
    case kString:
        if (c == 0x1B)
            v->state = kStringEsc;
        else if (c == 0x07)
            v->state = kGround;
        return;
    case kStringEsc:
        v->state = c == '\\' ? kGround : kString;
        return;
    default:
        break;
    }

    if (c == 0x1B)
    {
        v->utf8_need = 0;
        begin_sequence(v, kEsc);
        return;
    }
    if (c == 0x18 || c == 0x1A)
    {
        v->state = kGround;
        return;
    }
    if (c < 0x20 || c == 0x7F)
    {
        if (c != 0x7F)
        {
            control(v, c);
        }
        return;
    }

    switch (v->state)
    {
    case kGround:
        ground_byte(v, c);
        break;
    case kEsc:
        if (c == '[')
            begin_sequence(v, kCsi);
        else if (c == ']')
            begin_sequence(v, kOsc);
        else if (c == 'P' || c == 'X' || c == '^' || c == '_')
            v->state = kString;
        else if (c >= 0x20 && c <= 0x2F)
        {
            v->inter = c;
            v->state = kEscInter;
        }
        else
        {
            esc(v, c);
            v->state = kGround;
        }
        break;
    case kEscInter:
        if (c >= 0x30)
        {
            esc(v, c);
            v->state = kGround;
        }
        break;
    case kCsi:
        if (c >= '0' && c <= '9')
        {
            if (v->nparams == 0)
                v->nparams = 1;
            if (v->params[v->nparams - 1] < 100000)
                v->params[v->nparams - 1] = v->params[v->nparams - 1] * 10 + (c - '0');
        }
        else if (c == ';' || c == ':')
        {
            if (v->nparams == 0)
                v->nparams = 1;
            if (v->nparams < VT_PARAMS_MAX)
                v->params[v->nparams++] = 0;
        }
        else if (c >= 0x3C && c <= 0x3F)
            v->priv = c;
        else if (c >= 0x20 && c <= 0x2F)
            v->inter = c;
        else if (c >= 0x40 && c <= 0x7E)
        {
            v->state = kGround;
            csi(v, c);
        }
        break;
    default:
        v->state = kGround;
        break;
    }
}

/* ---------------- public ---------------- */

static int alloc_screen(vt *v, int cols, int rows)
{
    size_t n = (size_t)cols * (size_t)rows;
    v->main = calloc(n, sizeof(vt_cell));
    v->alt = calloc(n, sizeof(vt_cell));
    v->tabs = calloc((size_t)cols, 1);
    if (!v->main || !v->alt || !v->tabs)
    {
        free(v->main);
        free(v->alt);
        free(v->tabs);
        return -1;
    }
    v->cells = v->alt_active ? v->alt : v->main;
    return 0;
}

int vt_init(vt *v, int cols, int rows, int (*width)(uint32_t ch),
            void (*reply_fn)(void *user, const char *bytes, size_t n), void *user)
{
    memset(v, 0, sizeof(*v));
    if (cols < 1 || rows < 1)
    {
        return -1;
    }
    v->cols = cols;
    v->rows = rows;
    v->width = width;
    v->reply = reply_fn;
    v->user = user;
    if (alloc_screen(v, cols, rows) != 0)
    {
        return -1;
    }
    full_reset(v);
    v->cells = v->main;
    clear_rows(v, 0, rows);
    v->cells = v->alt;
    clear_rows(v, 0, rows);
    v->cells = v->main;
    return 0;
}

void vt_free(vt *v)
{
    free(v->main);
    free(v->alt);
    free(v->tabs);
    v->main = v->alt = v->cells = 0;
    v->tabs = 0;
}

static void copy_screen(const vt_cell *from, int fcols, int frows, vt_cell *to, int cols, int rows, int shift)
{
    int y, x;
    for (y = 0; y < rows; y++)
    {
        for (x = 0; x < cols; x++)
        {
            vt_cell *c = &to[(size_t)y * (size_t)cols + (size_t)x];
            int fy = y + shift;
            if (fy < frows && x < fcols)
            {
                *c = from[(size_t)fy * (size_t)fcols + (size_t)x];
            }
            else
            {
                memset(c, 0, sizeof(*c));
                c->fg = c->bg = VT_DEFAULT;
            }
        }
        /* a wide character cut in half at the right edge */
        if (cols > 0 && to[(size_t)y * (size_t)cols + (size_t)cols - 1].ch != VT_CONT && cols < fcols &&
            y + shift < frows && from[(size_t)(y + shift) * (size_t)fcols + (size_t)cols].ch == VT_CONT)
        {
            to[(size_t)y * (size_t)cols + (size_t)cols - 1].ch = 0;
        }
    }
}

int vt_resize(vt *v, int cols, int rows)
{
    vt saved = *v;
    int shift;
    if (cols < 1 || rows < 1)
    {
        return -1;
    }
    if (cols == v->cols && rows == v->rows)
    {
        return 0;
    }
    if (alloc_screen(v, cols, rows) != 0)
    {
        *v = saved;
        return -1;
    }
    /* keep the cursor line on screen by dropping lines from the top */
    shift = saved.y - (rows - 1);
    if (shift < 0)
    {
        shift = 0;
    }
    copy_screen(saved.main, saved.cols, saved.rows, v->main, cols, rows, shift);
    copy_screen(saved.alt, saved.cols, saved.rows, v->alt, cols, rows, shift);
    free(saved.main);
    free(saved.alt);
    free(saved.tabs);
    v->cols = cols;
    v->rows = rows;
    reset_tabs(v);
    v->top = 0;
    v->bot = rows - 1;
    v->y = clampi(v->y - shift, 0, rows - 1);
    v->x = clampi(v->x, 0, cols - 1);
    v->wrap_pending = 0;
    v->saved.x = clampi(v->saved.x, 0, cols - 1);
    v->saved.y = clampi(v->saved.y - shift, 0, rows - 1);
    return 0;
}

void vt_write(vt *v, const uint8_t *bytes, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
    {
        byte(v, bytes[i]);
    }
}
