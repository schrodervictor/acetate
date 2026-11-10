#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>

#define KEY_ESC 27

#define MARGIN 2
#define TITLE_HEIGHT 6

/* Highest slide index, discovered at startup by counting slideN.txt files. */
int last_slide;

typedef struct win_struct {
        WINDOW *win;
        int height, width, starty, startx;
} win_t;

typedef struct pres_struct {
        win_t title;
        win_t content;
        int slide;
} pres_t;

typedef void (*transition_fn)(pres_t *from, pres_t *to, int direction);

typedef enum align_enum {
        ALIGN_CENTER,        /* each line centered on its own (default) */
        ALIGN_BLOCK_CENTER   /* block left-aligned, centered as a whole */
} align_t;

typedef struct slide_meta_struct {
        transition_fn transition;
        align_t align;
} slide_meta_t;

slide_meta_t read_slide_meta(const char* filename);

win_t make_win(int height, int width, int starty, int startx) {
        win_t box = {
                .height = height,
                .width = width,
                .starty = starty,
                .startx = startx
        };

        box.win = newwin(height, width, starty, startx);

        return box;
}

void draw_box(WINDOW *win) {
        wborder(win, '|', '|', '-', '-', '+', '+', '+', '+');
}

typedef struct color_struct {
        char* name;
        short color;
} color_t;

static const color_t MARKUP_COLORS[] = {
        { "red",     COLOR_RED },
        { "green",   COLOR_GREEN },
        { "yellow",  COLOR_YELLOW },
        { "blue",    COLOR_BLUE },
        { "magenta", COLOR_MAGENTA },
        { "cyan",    COLOR_CYAN },
        { "white",   COLOR_WHITE }
};

#define MARKUP_COLOR_COUNT ((int)(sizeof(MARKUP_COLORS) / sizeof(MARKUP_COLORS[0])))

/* Extra pairs for the matrix rain, allocated past the markup pairs. */
#define MATRIX_TRAIL_PAIR (MARKUP_COLOR_COUNT + 1)
#define MATRIX_HEAD_PAIR  (MARKUP_COLOR_COUNT + 2)

void init_colors() {
        int i;

        if (!has_colors()) {
                return;
        }

        start_color();
        use_default_colors();

        for (i = 0; i < MARKUP_COLOR_COUNT; i++) {
                init_pair(i + 1, MARKUP_COLORS[i].color, -1);
        }

        init_pair(MATRIX_TRAIL_PAIR, COLOR_GREEN, -1);
        init_pair(MATRIX_HEAD_PAIR, COLOR_WHITE, -1);
}

int color_pair_for(const char* name, int len) {
        int i;

        for (i = 0; i < MARKUP_COLOR_COUNT; i++) {
                const char* candidate = MARKUP_COLORS[i].name;
                if ((int)strlen(candidate) == len
                                && strncmp(name, candidate, len) == 0) {
                        return i + 1;
                }
        }

        return 0;
}

/*
 * If s points at a valid markup tag, returns its length (through the closing
 * ']') and writes the color pair to activate into *pair (0 resets to default).
 * Returns 0 when s is not a recognized tag, so text like arr[0] is left intact.
 */
int parse_tag(const char* s, int* pair) {
        if (*s != '[') {
                return 0;
        }

        const char* end = strchr(s, ']');
        if (end == NULL) {
                return 0;
        }

        const char* name = s + 1;
        int len = end - name;

        if (len > 0 && name[0] == '/') {
                *pair = 0;
                return end - s + 1;
        }

        int found = color_pair_for(name, len);
        if (found == 0) {
                return 0;
        }

        *pair = found;
        return end - s + 1;
}

int visible_width(const char* s) {
        int width = 0;
        int pair;

        while (*s) {
                int tag = parse_tag(s, &pair);
                if (tag > 0) {
                        s += tag;
                } else {
                        /* count only UTF-8 lead/ASCII bytes, not continuation bytes */
                        if (((unsigned char)*s & 0xC0) != 0x80) {
                                width++;
                        }
                        s++;
                }
        }

        return width;
}

void render_markup(WINDOW* win, const char* s) {
        int pair;
        int current = 0;

        while (*s) {
                int tag = parse_tag(s, &pair);
                if (tag > 0) {
                        if (current != 0) {
                                wattroff(win, COLOR_PAIR(current));
                        }
                        current = pair;
                        if (current != 0) {
                                wattron(win, COLOR_PAIR(current));
                        }
                        s += tag;
                } else {
                        waddch(win, (unsigned char)*s);
                        s++;
                }
        }

        if (current != 0) {
                wattroff(win, COLOR_PAIR(current));
        }
}

/*
 * A line starting with [!center] is centered on its own, regardless of the
 * slide's alignment.
 */
int center_marker_len(const char* line) {
        const char* marker = "[!center]";
        int len = (int)strlen(marker);

        return strncmp(line, marker, len) == 0 ? len : 0;
}

/*
 * Widest visible line in a slide file, ignoring frontmatter and markup.
 */
int max_line_width(char* filename) {
        char buffer[1024];
        int size, width, max = 0;
        int in_frontmatter = 1;

        FILE *fp = fopen(filename, "r");
        if (fp == NULL) {
                return 0;
        }

        while (fgets(buffer, sizeof(buffer), fp)) {
                if (in_frontmatter && buffer[0] == '!') {
                        continue;
                }
                in_frontmatter = 0;

                if (center_marker_len(buffer)) {
                        continue;
                }

                size = strlen(buffer);
                if (size > 0 && buffer[size - 1] == '\n') {
                        buffer[size - 1] = '\0';
                }

                width = visible_width(buffer);
                if (width > max) {
                        max = width;
                }
        }

        fclose(fp);
        return max;
}

void set_text_from_file(WINDOW *win, char* filename, int row, align_t align) {
        char buffer[1024];
        int size, rows, cols;
        int in_frontmatter = 1;
        int base_col = 0;

        getmaxyx(win, rows, cols);

        if (align == ALIGN_BLOCK_CENTER) {
                base_col = (cols - max_line_width(filename)) / 2;
        }

        FILE *fp = fopen(filename, "r");

        if (fp == NULL) {
                return;
        }

        while (fgets(buffer, sizeof(buffer), fp)) {
                char* text;
                int block, col;

                if (in_frontmatter && buffer[0] == '!') {
                        continue;
                }
                in_frontmatter = 0;

                size = strlen(buffer);
                if (size > 0 && buffer[size - 1] == '\n') {
                        buffer[--size] = '\0';
                }

                text = buffer + center_marker_len(buffer);
                block = align == ALIGN_BLOCK_CENTER && text == buffer;

                col = block
                        ? base_col
                        : (cols - visible_width(text)) / 2;

                wmove(win, row++, col);
                render_markup(win, text);
        }

        fclose(fp);
}

void set_title(pres_t *presentation) {
        int rows, cols;

        getmaxyx(stdscr, rows, cols);
        (void)rows;

        win_t title = make_win(TITLE_HEIGHT, cols - 2 * MARGIN, 1, MARGIN);

        attron(A_BOLD);
        set_text_from_file(title.win, "title.txt", 0, ALIGN_CENTER);
        attroff(A_BOLD);

        presentation->title = title;
}

void set_content(pres_t *presentation, int slide) {
        int rows, cols;
        char filename[12] = {};

        getmaxyx(stdscr, rows, cols);

        win_t content = make_win(
                rows - presentation->title.height - 3,
                cols - 2 * MARGIN,
                presentation->title.height + 1,
                MARGIN
        );

        snprintf(filename, sizeof(filename), "slide%d.txt", slide);
        slide_meta_t meta = read_slide_meta(filename);
        set_text_from_file(content.win, filename, 2, meta.align);

        draw_box(content.win);

        presentation->content = content;
}

void set_signature() {
        int rows, cols;
        char* sig = "Victor Schroeder - 21.07.2026";

        getmaxyx(stdscr, rows, cols);
        move(rows - 2, cols - strlen(sig) - 2);
        printw("%s", sig);
}

void set_pagination(int slide) {
        int rows, cols;

        getmaxyx(stdscr, rows, cols);
        (void)cols;
        move(rows - 2, 2);
        printw("%d/%d", slide, last_slide);
}

pres_t build_slide(int slide) {
        pres_t presentation;

        presentation.slide = slide;
        set_title(&presentation);
        set_content(&presentation, slide);

        return presentation;
}

void destroy_slide(pres_t *presentation) {
        delwin(presentation->title.win);
        delwin(presentation->content.win);
}

/* Draws the fixed chrome plus a slide's windows, with no animation. */
void present(pres_t *presentation) {
        erase();
        draw_box(stdscr);
        set_pagination(presentation->slide);
        set_signature();

        refresh();

        /* erase() blanks the cells under the windows without marking the
         * windows dirty, so force a full repaint of both. */
        touchwin(presentation->title.win);
        touchwin(presentation->content.win);
        wrefresh(presentation->title.win);
        wrefresh(presentation->content.win);
}

/*
 * Copies a window onto stdscr with its left edge at screen_col, clipped to the
 * horizontal band [clip_left, clip_right]. copywin does the clipping, so the
 * window may be positioned partially or fully off that band.
 */
void blit_win(win_t *box, int screen_col, int clip_left, int clip_right) {
        int top = box->starty;
        int dmincol = screen_col < clip_left ? clip_left : screen_col;
        int dmaxcol = screen_col + box->width - 1;

        if (dmaxcol > clip_right) {
                dmaxcol = clip_right;
        }
        if (dmincol > dmaxcol) {
                return;
        }

        copywin(box->win, stdscr, 0, dmincol - screen_col,
                        top, dmincol, top + box->height - 1, dmaxcol, FALSE);
}

/*
 * Non-blocking peek at the input queue. Returns 1 if a key is waiting (pushing
 * it back for the main loop), so an animation can skip to its final frame when
 * the user is navigating faster than it plays.
 */
int input_pending() {
        int ch;

        nodelay(stdscr, TRUE);
        ch = getch();
        nodelay(stdscr, FALSE);

        if (ch == ERR) {
                return 0;
        }

        ungetch(ch);
        return 1;
}

void transition_cut(pres_t *from, pres_t *to, int direction) {
        (void)from;
        (void)direction;

        present(to);
}

/*
 * Pushes the content panels horizontally: the new slide enters from one side
 * while the old one exits the other. direction > 0 enters from the right.
 */
void transition_slide(pres_t *from, pres_t *to, int direction) {
        int rows, cols, f;
        int frames = 16;
        int home = to->content.startx;
        int clip_left = 1;
        int clip_right;

        getmaxyx(stdscr, rows, cols);
        (void)rows;
        clip_right = cols - 2;

        for (f = 1; f <= frames; f++) {
                int d = cols - (cols * f) / frames;
                int new_col = home + direction * d;
                int old_col = home + direction * (d - cols);

                if (input_pending()) {
                        break;
                }

                erase();
                draw_box(stdscr);
                set_pagination(to->slide);
                set_signature();
                blit_win(&to->title, to->title.startx, clip_left, clip_right);
                blit_win(&from->content, old_col, clip_left, clip_right);
                blit_win(&to->content, new_col, clip_left, clip_right);

                refresh();
                napms(16);
        }

        present(to);
}

#define MATRIX_MAX_COLS 512

static const char MATRIX_GLYPHS[] =
        "abcdefghijklmnopqrstuvwxyz0123456789@#$%&*+=-<>?";

#define MATRIX_GLYPH_COUNT ((int)(sizeof(MATRIX_GLYPHS) - 1))

char matrix_glyph() {
        return MATRIX_GLYPHS[rand() % MATRIX_GLYPH_COUNT];
}

/* Reads the visible character (without attributes) at a window cell. */
char cell_char(WINDOW *win, int row, int col) {
        return mvwinch(win, row, col) & A_CHARTEXT;
}

/*
 * Green "digital rain" wipe: a drop falls down every column of the content
 * region, showing the old slide ahead of each drop and the new slide behind
 * its tail, so the new content is left in place once the rain passes.
 */
void transition_matrix(pres_t *from, pres_t *to, int direction) {
        int top = to->content.starty;
        int left = to->content.startx;
        int height = to->content.height;
        int width = to->content.width;
        int start[MATRIX_MAX_COLS];
        int speed[MATRIX_MAX_COLS];
        int tail = height / 2;
        int c, r, f, frames;

        (void)direction;

        if (width > MATRIX_MAX_COLS) {
                width = MATRIX_MAX_COLS;
        }
        if (tail > 12) {
                tail = 12;
        }
        if (tail < 3) {
                tail = 3;
        }

        for (c = 0; c < width; c++) {
                start[c] = rand() % (height / 2 + 1);
                speed[c] = 1 + rand() % 2;
        }

        frames = height + height / 2 + tail + 2;

        for (f = 0; f < frames; f++) {
                if (input_pending()) {
                        break;
                }

                for (c = 0; c < width; c++) {
                        int head = (f - start[c]) * speed[c];

                        for (r = 0; r < height; r++) {
                                int distance = head - r;
                                chtype out;

                                if (distance < 0) {
                                        out = cell_char(from->content.win, r, c);
                                } else if (distance == 0) {
                                        out = matrix_glyph()
                                                | COLOR_PAIR(MATRIX_HEAD_PAIR) | A_BOLD;
                                } else if (distance <= tail) {
                                        out = matrix_glyph() | COLOR_PAIR(MATRIX_TRAIL_PAIR)
                                                | (distance <= tail / 2 ? A_BOLD : A_DIM);
                                } else {
                                        out = cell_char(to->content.win, r, c);
                                }

                                mvaddch(top + r, left + c, out);
                        }
                }

                set_pagination(to->slide);
                refresh();
                napms(45);
        }

        present(to);
}

#define FX_MAX_ROWS 128
#define FX_MAX_COLS 512

/* Per-cell schedule scratch, shared by the cell-based effects (not reentrant). */
static int fx_schedule[FX_MAX_ROWS][FX_MAX_COLS];

/*
 * Hollywood "decrypting" effect: every cell scrambles through random green
 * glyphs and then locks onto the new slide's character at a random frame.
 */
void transition_decrypt(pres_t *from, pres_t *to, int direction) {
        int top = to->content.starty;
        int left = to->content.startx;
        int height = to->content.height;
        int width = to->content.width;
        int r, c, f;
        int reveal_span = 30;
        int frames = reveal_span + 8;

        (void)from;
        (void)direction;

        if (height > FX_MAX_ROWS) {
                height = FX_MAX_ROWS;
        }
        if (width > FX_MAX_COLS) {
                width = FX_MAX_COLS;
        }

        for (r = 0; r < height; r++) {
                for (c = 0; c < width; c++) {
                        fx_schedule[r][c] = rand() % reveal_span;
                }
        }

        for (f = 0; f < frames; f++) {
                if (input_pending()) {
                        break;
                }

                for (r = 0; r < height; r++) {
                        for (c = 0; c < width; c++) {
                                int lock = fx_schedule[r][c];
                                chtype out;

                                if (f < lock) {
                                        out = matrix_glyph()
                                                | COLOR_PAIR(MATRIX_TRAIL_PAIR);
                                } else if (f == lock) {
                                        out = cell_char(to->content.win, r, c)
                                                | COLOR_PAIR(MATRIX_HEAD_PAIR) | A_BOLD;
                                } else {
                                        out = cell_char(to->content.win, r, c);
                                }

                                mvaddch(top + r, left + c, out);
                        }
                }

                set_pagination(to->slide);
                refresh();
                napms(40);
        }

        present(to);
}

/*
 * Dissolve: cells flip from the old slide to the new one at random frames,
 * with a brief speckle of static at each cell's moment of change.
 */
void transition_dissolve(pres_t *from, pres_t *to, int direction) {
        int top = to->content.starty;
        int left = to->content.startx;
        int height = to->content.height;
        int width = to->content.width;
        int r, c, f;
        int frames = 30;

        (void)direction;

        if (height > FX_MAX_ROWS) {
                height = FX_MAX_ROWS;
        }
        if (width > FX_MAX_COLS) {
                width = FX_MAX_COLS;
        }

        for (r = 0; r < height; r++) {
                for (c = 0; c < width; c++) {
                        fx_schedule[r][c] = rand() % (frames - 2);
                }
        }

        for (f = 0; f < frames; f++) {
                if (input_pending()) {
                        break;
                }

                for (r = 0; r < height; r++) {
                        for (c = 0; c < width; c++) {
                                int flip = fx_schedule[r][c];
                                chtype out;

                                if (f < flip) {
                                        out = cell_char(from->content.win, r, c);
                                } else if (f == flip) {
                                        out = matrix_glyph() | A_DIM;
                                } else {
                                        out = cell_char(to->content.win, r, c);
                                }

                                mvaddch(top + r, left + c, out);
                        }
                }

                set_pagination(to->slide);
                refresh();
                napms(40);
        }

        present(to);
}

/*
 * Cover: the new content slides in over the old, which stays put underneath.
 * direction > 0 brings the new slide in from the right.
 */
void transition_cover(pres_t *from, pres_t *to, int direction) {
        int rows, cols, f;
        int frames = 16;
        int home = to->content.startx;
        int clip_left = 1;
        int clip_right;

        getmaxyx(stdscr, rows, cols);
        (void)rows;
        clip_right = cols - 2;

        for (f = 1; f <= frames; f++) {
                int d = cols - (cols * f) / frames;
                int new_col = home + direction * d;

                if (input_pending()) {
                        break;
                }

                erase();
                draw_box(stdscr);
                set_pagination(to->slide);
                set_signature();
                blit_win(&to->title, to->title.startx, clip_left, clip_right);
                blit_win(&from->content, home, clip_left, clip_right);
                blit_win(&to->content, new_col, clip_left, clip_right);

                refresh();
                napms(16);
        }

        present(to);
}

/*
 * Uncover: the old content slides away to reveal the new one, which sits
 * still underneath. direction > 0 slides the old slide off to the left.
 */
void transition_uncover(pres_t *from, pres_t *to, int direction) {
        int rows, cols, f;
        int frames = 16;
        int home = to->content.startx;
        int clip_left = 1;
        int clip_right;

        getmaxyx(stdscr, rows, cols);
        (void)rows;
        clip_right = cols - 2;

        for (f = 1; f <= frames; f++) {
                int d = (cols * f) / frames;
                int old_col = home - direction * d;

                if (input_pending()) {
                        break;
                }

                erase();
                draw_box(stdscr);
                set_pagination(to->slide);
                set_signature();
                blit_win(&to->title, to->title.startx, clip_left, clip_right);
                blit_win(&to->content, home, clip_left, clip_right);
                blit_win(&from->content, old_col, clip_left, clip_right);

                refresh();
                napms(16);
        }

        present(to);
}

typedef struct transition_struct {
        char* name;
        transition_fn apply;
} transition_t;

static const transition_t TRANSITIONS[] = {
        { "cut",      transition_cut },
        { "slide",    transition_slide },
        { "matrix",   transition_matrix },
        { "decrypt",  transition_decrypt },
        { "dissolve", transition_dissolve },
        { "cover",    transition_cover },
        { "uncover",  transition_uncover }
};

#define TRANSITION_COUNT ((int)(sizeof(TRANSITIONS) / sizeof(TRANSITIONS[0])))

transition_fn transition_for(const char* name) {
        int i;

        for (i = 0; i < TRANSITION_COUNT; i++) {
                if (strcmp(TRANSITIONS[i].name, name) == 0) {
                        return TRANSITIONS[i].apply;
                }
        }

        return TRANSITIONS[0].apply;
}

align_t parse_align(const char* value) {
        if (strcmp(value, "block-center") == 0) {
                return ALIGN_BLOCK_CENTER;
        }

        return ALIGN_CENTER;
}

/* Reads the leading !key=value frontmatter block from a slide file. */
slide_meta_t read_slide_meta(const char* filename) {
        char buffer[1024];
        slide_meta_t meta = {
                .transition = transition_for("cut"),
                .align = ALIGN_CENTER
        };

        FILE *fp = fopen(filename, "r");
        if (fp == NULL) {
                return meta;
        }

        while (fgets(buffer, sizeof(buffer), fp)) {
                char* value;
                int size;

                if (buffer[0] != '!') {
                        break;
                }

                size = strlen(buffer);
                if (size > 0 && buffer[size - 1] == '\n') {
                        buffer[size - 1] = '\0';
                }

                value = strchr(buffer, '=');
                if (value == NULL) {
                        continue;
                }
                *value++ = '\0';

                if (strcmp(buffer + 1, "transition") == 0) {
                        meta.transition = transition_for(value);
                } else if (strcmp(buffer + 1, "align") == 0) {
                        meta.align = parse_align(value);
                }
        }

        fclose(fp);
        return meta;
}

/* Jumps straight to target, animating a single transition from the current
 * slide with the direction implied by the jump. */
void navigate(pres_t *current, int *slide, int target) {
        char filename[12] = {};
        slide_meta_t meta;
        pres_t next;
        int direction;

        if (target == *slide) {
                return;
        }

        direction = target > *slide ? 1 : -1;
        *slide = target;

        snprintf(filename, sizeof(filename), "slide%d.txt", target);
        meta = read_slide_meta(filename);
        next = build_slide(target);

        meta.transition(current, &next, direction);

        destroy_slide(current);
        *current = next;
}

/*
 * Reads a ':'-prefixed command line on the pagination row, starting at col,
 * echoing keystrokes until Enter (returns 1) or Esc (returns 0, aborting).
 * Supports backspace editing.
 */
int read_command(char *buf, int size, int col) {
        int rows, cols, ch;
        int len = 0;

        buf[0] = '\0';
        getmaxyx(stdscr, rows, cols);
        (void)cols;
        curs_set(TRUE);

        for (;;) {
                mvprintw(rows - 2, col, ":%s ", buf);
                move(rows - 2, col + 1 + len);
                refresh();

                ch = getch();

                if (ch == KEY_ESC) {
                        curs_set(FALSE);
                        return 0;
                }
                if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                        curs_set(FALSE);
                        return 1;
                }
                if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                        if (len > 0) {
                                buf[--len] = '\0';
                        }
                        continue;
                }
                if (len < size - 1 && ch >= 32 && ch < 127) {
                        buf[len++] = ch;
                        buf[len] = '\0';
                }
        }
}

/* Counts slideN.txt files to find the highest slide index available. */
int find_last_slide() {
        char filename[32];
        int n = 0;
        FILE *fp;

        for (;;) {
                snprintf(filename, sizeof(filename), "slide%d.txt", n);
                fp = fopen(filename, "r");
                if (fp == NULL) {
                        break;
                }
                fclose(fp);
                n++;
        }

        return n - 1;
}

int main(int argc, char *argv[])
{
        int ch;
        int slide = 0;
        pres_t current;

        last_slide = find_last_slide();

        setlocale(LC_ALL, "");
        initscr();
        cbreak();
        noecho();
        curs_set(false);
        keypad(stdscr, TRUE);
        init_colors();
        srand(time(NULL));

        current = build_slide(slide);
        clear();
        present(&current);

        while ((ch = getch())) {
                if (ch == KEY_ESC) {
                        break;
                }
                if (ch == KEY_RESIZE) {
                        clear();
                        destroy_slide(&current);
                        current = build_slide(slide);
                        present(&current);
                }
                if (ch == KEY_LEFT && slide > 0) {
                        navigate(&current, &slide, slide - 1);
                }
                if (ch == KEY_RIGHT && slide < last_slide) {
                        navigate(&current, &slide, slide + 1);
                }
                if (ch == ':') {
                        char cmd[64] = {};
                        char pagination[16] = {};
                        int target, col;

                        snprintf(pagination, sizeof(pagination), "%d/%d",
                                        slide, last_slide);
                        col = MARGIN + strlen(pagination) + 1;

                        if (read_command(cmd, sizeof(cmd), col)
                                        && sscanf(cmd, "goto %d", &target) == 1) {
                                if (target < 0) {
                                        target = 0;
                                }
                                if (target > last_slide) {
                                        target = last_slide;
                                }
                                navigate(&current, &slide, target);
                        }

                        present(&current);
                }
        }

        destroy_slide(&current);
        endwin();
        return 0;
}
