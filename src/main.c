#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "r2_tui_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

/* Terminal windows redraw every cell on each run, so the shared pool for
   recorded drawing has to hold several full screens. */
#define R2_TUI_CMD_POOL (2u << 20)
/* one event per input byte at most, so a whole read (a paste) fits; see in[] in main */
#define INPUT_MAX 256
#define R2_TUI_EVENT_QUEUE INPUT_MAX

#define R2_TUI_IMPLEMENTATION
#define R2_TUI_EDIT_IMPLEMENTATION
#include "r2_tui.h"
#include "r2_tui_edit.h"

#include "pty.h"
#include "vt.h"

#define MAX_COLS 400
#define MAX_ROWS 200

static uint8_t arena[R2_TUI_ARENA_SIZE(MAX_COLS, MAX_ROWS)];
static tui t;
static tui_posix p;
static int running = 1, quitting, windows, open_failed;

#define FS_NAME_MAX 128

/* the colours of a terminal's foreground and background */
#define TERM_FG TUI_BRIGHT_WHITE
#define TERM_BG TUI_BLUE
/* bytes read from one shell before the screen is redrawn */
#define TERM_READ_MAX 65536

typedef struct
{
    int used;
    tui_window *window;
    vt screen;
    pty_proc proc;
} terminal_t;

static terminal_t terminals[R2_TUI_MAX_WINDOWS];

//////////////////////////////////////////////////////

enum
{
    kDialogNone,
    kDialogOpen,
    kDialogSaveAs,
    kDialogDiscard,
    kDialogMessage
};

static void show_message(const char *text);

static void desktop_ui(tui *tt, tui_window *w, void *user);
static void term_ui(tui *tt, tui_window *w, void *user);

static void desktop_menus(tui *tt);

//////////////////////////////////////////////////////
// Terminal

static void term_reply(void *user, const char *bytes, size_t n)
{
    terminal_t *term = user;
    pty_write(&term->proc, bytes, n);
}

static int term_open(tui *tt)
{
    terminal_t *term = 0;
    char title[R2_TUI_TITLE_MAX];
    int i;

    for (i = 0; i < R2_TUI_MAX_WINDOWS; i++)
    {
        if (!terminals[i].used)
        {
            term = &terminals[i];
            break;
        }
    }
    if (term == 0)
    {
        return -1;
    }
    if (vt_init(&term->screen, 80, 24, tui_rune_width, term_reply, term) != 0)
    {
        return -1;
    }
    if (pty_spawn(&term->proc, 80, 24, &p.saved) != 0)
    {
        vt_free(&term->screen);
        return -1;
    }
    snprintf(title, sizeof(title), "Terminal %d", ++windows);
    term->window = tui_window_open(tt, title, (tui_rect){-1, -1, 82, 26}, TUI_WIN_ASK_CLOSE, term_ui, term);
    if (term->window == 0)
    {
        pty_close(&term->proc);
        vt_free(&term->screen);
        return -1;
    }
    term->used = 1;
    return 0;
}

static void term_close(terminal_t *term)
{
    if (!term->used)
    {
        return;
    }
    pty_close(&term->proc);
    vt_free(&term->screen);
    tui_window_close(&t, term->window);
    term->used = 0;
    term->window = 0;
}

static size_t put_utf8(char *out, uint32_t ch)
{
    return utf8_encode(ch, (uint8_t *)out);
}

/* the bytes xterm sends for a key */
static size_t encode_key(const vt *screen, const tui_event *ev, char *out)
{
    static const char kCursor[] = "ABDCHF";                       /* Up Down Left Right Home End */
    static const int kTilde[] = {15, 17, 18, 19, 20, 21, 23, 24}; /* F5..F12 */
    unsigned mods = ev->mods;
    int code = 0;
    char final = 0;
    size_t n = 0;

    /* xterm's modifier parameter is 1 + shift(1) + alt(2) + ctrl(4), the TUI_MOD_* bits */
    switch (ev->key)
    {
    case TUI_KEY_RUNE:
        if (mods & TUI_MOD_ALT)
            out[n++] = 0x1B;
        if (mods & TUI_MOD_CTRL)
        {
            rune c = ev->ch;
            if (c >= 'a' && c <= 'z')
                out[n++] = (char)(c - 'a' + 1);
            else if (c >= '@' && c <= '_')
                out[n++] = (char)(c - '@');
            else if (c == ' ' || c == '2')
                out[n++] = 0;
            else if (c == '?' || c == '8')
                out[n++] = 0x7F;
            else
                n += put_utf8(out + n, c);
            return n;
        }
        return n + put_utf8(out + n, ev->ch);
    case TUI_KEY_ENTER:
        if (mods & TUI_MOD_ALT)
            out[n++] = 0x1B;
        out[n++] = '\r';
        return n;
    case TUI_KEY_TAB:
        if (mods & TUI_MOD_SHIFT)
            return (size_t)sprintf(out, "\x1B[Z");
        if (mods & TUI_MOD_ALT)
            out[n++] = 0x1B;
        out[n++] = '\t';
        return n;
    case TUI_KEY_BACKSPACE:
        if (mods & TUI_MOD_ALT)
            out[n++] = 0x1B;
        out[n++] = (mods & TUI_MOD_CTRL) ? 0x08 : 0x7F;
        return n;
    case TUI_KEY_ESC:
        if (mods & TUI_MOD_ALT)
            out[n++] = 0x1B;
        out[n++] = 0x1B;
        return n;
    case TUI_KEY_UP:
    case TUI_KEY_DOWN:
    case TUI_KEY_LEFT:
    case TUI_KEY_RIGHT:
    case TUI_KEY_HOME:
    case TUI_KEY_END:
        final = kCursor[ev->key - TUI_KEY_UP];
        break;
    case TUI_KEY_INSERT:
        code = 2;
        break;
    case TUI_KEY_DELETE:
        code = 3;
        break;
    case TUI_KEY_PGUP:
        code = 5;
        break;
    case TUI_KEY_PGDN:
        code = 6;
        break;
    default:
        if (ev->key >= TUI_KEY_F1 && ev->key < TUI_KEY_F1 + 4)
            final = (char)('P' + (ev->key - TUI_KEY_F1));
        else if (ev->key >= TUI_KEY_F1 + 4 && ev->key <= TUI_KEY_F12)
            code = kTilde[ev->key - TUI_KEY_F1 - 4];
        else
            return 0;
        break;
    }
    if (code)
    {
        if (mods)
            return (size_t)sprintf(out, "\x1B[%d;%u~", code, mods + 1);
        return (size_t)sprintf(out, "\x1B[%d~", code);
    }
    if (mods)
        return (size_t)sprintf(out, "\x1B[1;%u%c", mods + 1, final);
    /* application cursor mode only changes the arrows, Home and End */
    if (screen->app_cursor || (final >= 'P' && final <= 'S'))
        return (size_t)sprintf(out, "\x1BO%c", final);
    return (size_t)sprintf(out, "\x1B[%c", final);
}

static tui_style cell_style(const vt_cell *c)
{
    unsigned fg = c->fg == VT_DEFAULT ? TERM_FG : c->fg;
    unsigned bg = c->bg == VT_DEFAULT ? TERM_BG : c->bg;
    /* VT_BOLD / UNDERLINE / REVERSE are the TUI_* bits */
    return TUI_STYLE(fg, bg, c->attr);
}

static int is_plain_blank(const vt_cell *c)
{
    return (c->ch == 0 || c->ch == ' ') && c->mark == 0 && c->bg == VT_DEFAULT && c->attr == 0;
}

static void draw_screen(tui *tt, const vt *screen, int cw, int ch)
{
    static char line[MAX_COLS * 8 + 8];
    int rows = screen->rows < ch ? screen->rows : ch;
    int cols = screen->cols < cw ? screen->cols : cw;
    int x, y;

    tui_fill(tt, (tui_rect){0, 0, cw, ch}, ' ', TUI_STYLE(TERM_FG, TERM_BG, 0));
    for (y = 0; y < rows; y++)
    {
        const vt_cell *row = vt_cell_at(screen, 0, y);
        int end = cols;
        while (end > 0 && is_plain_blank(&row[end - 1]))
        {
            end--;
        }
        /* one tui_text per run of cells in the same style */
        x = 0;
        while (x < end)
        {
            tui_style style = cell_style(&row[x]);
            int start = x;
            size_t len = 0;
            while (x < end && cell_style(&row[x]) == style)
            {
                const vt_cell *c = &row[x];
                if (c->ch != VT_CONT)
                {
                    len += put_utf8(line + len, c->ch ? c->ch : ' ');
                    if (c->mark)
                        len += put_utf8(line + len, c->mark);
                }
                else if (x == start || tui_rune_width(row[x - 1].ch) != 2)
                {
                    len += put_utf8(line + len, ' '); /* half of a wide character, cut off */
                }
                x++;
            }
            tui_text(tt, start, y, s8_from_bytes(line, (uint32_t)len), style);
        }
    }
    if (screen->cursor_visible && screen->x < cw && screen->y < ch)
    {
        tui_cursor(tt, screen->x, screen->y);
    }
}

static void term_ui(tui *tt, tui_window *w, void *user)
{
    terminal_t *term = user;
    const tui_event *ev;
    int cw, ch;

    if (tui_closing(tt))
    {
        term_close(term);
        return;
    }
    tui_size(tt, &cw, &ch);
    if (cw > 0 && ch > 0 && (cw != term->screen.cols || ch != term->screen.rows))
    {
        if (vt_resize(&term->screen, cw, ch) == 0)
        {
            pty_resize(&term->proc, cw, ch);
        }
    }
    if ((ev = tui_key(tt)) != 0)
    {
        char buf[32];
        size_t n = encode_key(&term->screen, ev, buf);
        if (n > 0)
        {
            pty_write(&term->proc, buf, n);
            tui_use_key(tt);
        }
    }
    draw_screen(tt, &term->screen, cw, ch);
    tui_status(tt, " ~Ctrl+\\ N~ New terminal  ~Ctrl+\\~ Commands  ~Ctrl+\\ Bksp~ Close"
                   "  ~Ctrl+]~ Search");
}

/* Read what the shells wrote. 1 if one of them has more waiting. */
static int term_pump(void)
{
    static uint8_t buf[16384];
    int more = 0, i;

    for (i = 0; i < R2_TUI_MAX_WINDOWS; i++)
    {
        terminal_t *term = &terminals[i];
        long total = 0, n = 0;
        if (!term->used)
        {
            continue;
        }
        while (total < TERM_READ_MAX && (n = pty_read(&term->proc, buf, sizeof(buf))) > 0)
        {
            vt_write(&term->screen, buf, (size_t)n);
            total += n;
        }
        if (total > 0)
        {
            tui_window_invalidate(&t, term->window);
            if (n > 0)
                more = 1;
        }
        if (term->screen.title_changed)
        {
            term->screen.title_changed = 0;
            if (term->screen.title[0])
                tui_window_set_title(&t, term->window, term->screen.title);
        }
        if (n < 0)
        {
            term_close(term); /* the shell exited */
        }
    }
    pty_reap();
    return more;
}

static void term_close_all(void)
{
    int i;
    for (i = 0; i < R2_TUI_MAX_WINDOWS; i++)
    {
        term_close(&terminals[i]);
    }
}

static void new_terminal(void)
{
    if (term_open(&t) != 0)
    {
        open_failed = 1;
        tui_window_invalidate(&t, &t.win[R2__TUI_DESKTOP]);
    }
}

//////////////////////////////////////////////////////

static void desktop_menus(tui *tt)
{
    static int about;

    tui_menu_begin(tt, "~F~ile");
    /* the shortcut is only shown: feed_input handles Ctrl+\ then N */
    if (tui_menu_item(tt, "~N~ew terminal", "Ctrl+\\ N"))
    {
        new_terminal();
    }
    tui_menu_separator(tt);
    if (tui_menu_item(tt, "E~x~it", "Ctrl+Q"))
    {
        quitting = 1;
    }
    tui_menu_end(tt);

    tui_menu_begin(tt, "~H~elp");
    if (tui_menu_item(tt, "~A~bout", "F1"))
    {
        about = 1;
    }
    tui_menu_end(tt);

    if (open_failed && tui_message(tt, "Terminal", "Couldn't open another terminal.", TUI_MB_OK))
    {
        open_failed = 0;
    }
    if (about && tui_message(tt, "About",
                             "τūī - Terminal Window Manager \n   ___ \n  (o  "
                             ")> \n  ( @@) \n  /   \\ \n  ^^ ^^ ",
                             TUI_MB_OK))
    {
        about = 0;
    }
    if (quitting)
    {
        unsigned r = tui_message(tt, "Quit", "Leave τūī?", TUI_MB_YES | TUI_MB_NO);
        if (r == TUI_MB_YES)
        {
            running = 0;
        }
        if (r)
        {
            quitting = 0;
        }
    }
}

//////////////////////////////////////////////////////

// This is the default menus when something is not in focus.
// similar to finder in macOS
static void desktop_ui(tui *tt, tui_window *w, void *user)
{
    (void)w;
    (void)user;
    desktop_menus(tt);
    tui_status(tt, " ~Ctrl+\\ N~ New terminal  ~Ctrl+\\~ Commands  ~Ctrl+]~ Search "
                   " ~Ctrl+Q~ Quit");
}

//////////////////////////////////////////////////////

static void resize(void)
{
    int cols, rows;
    if (tui_posix_size(&cols, &rows) != 0)
    {
        cols = 80, rows = 25;
    }
    tui_resize(&t, cols < MAX_COLS ? cols : MAX_COLS, rows < MAX_ROWS ? rows : MAX_ROWS);
}

/* Sleep until the keyboard or a shell has something, or ms pass. */
static void wait_input(int ms)
{
    fd_set in;
    struct timeval tv;
    int top = STDIN_FILENO, i;

    FD_ZERO(&in);
    FD_SET(STDIN_FILENO, &in);
    for (i = 0; i < R2_TUI_MAX_WINDOWS; i++)
    {
        if (terminals[i].used && terminals[i].proc.fd >= 0)
        {
            FD_SET(terminals[i].proc.fd, &in);
            if (terminals[i].proc.fd > top)
                top = terminals[i].proc.fd;
        }
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(top + 1, &in, 0, 0, &tv);
}

/* Ctrl+\ is the window command prefix, and r2_tui only binds menus to the
   key after it, so catch N here: feed up to each Ctrl+\ and look at the
   next byte while the prefix is waiting for it. */
static void feed_input(const uint8_t *in, size_t n, uint32_t now)
{
    size_t i = 0;
    while (i < n)
    {
        size_t j = i;
        if (t.chord && t.in_len == 0 && (in[i] == 'n' || in[i] == 'N'))
        {
            t.chord = 0;
            t.compose_dirty = 1;
            new_terminal();
            i++;
            continue;
        }
        while (j < n && in[j] != 0x1C)
        {
            j++;
        }
        if (j < n)
        {
            j++;
        }
        tui_feed(&t, in + i, j - i, now);
        tui_update(&t, now);
        i = j;
    }
}

int main(void)
{
    uint8_t in[INPUT_MAX], out[4096];

    if (tui_init(&t, arena, sizeof(arena), 80, 25) != 0)
    {
        return 1;
    }

    if (tui_posix_open(&p) != 0)
    {
        fprintf(stderr, "tui: needs a terminal\n");
        return 1;
    }
    resize();

    tui_set_desktop(&t, desktop_ui, 0);

    while (running)
    {
        uint32_t now = tui_posix_now_ms();
        size_t n;
        int more;
        if (tui_posix_resized())
        {
            resize();
        }

        n = tui_posix_read(in, sizeof(in));

        feed_input(in, n, now);
        more = term_pump();
        tui_update(&t, now);

        while ((n = tui_render(&t, out, sizeof(out))) > 0)
        {
            tui_posix_write(out, n);
        }

        if (running)
        {
            wait_input(more ? 0 : tui_input_pending(&t) ? (int)t.esc_ms : 1000);
        }
    }
    term_close_all();
    pty_reap();
    tui_posix_close(&p);
    return 0;
}
