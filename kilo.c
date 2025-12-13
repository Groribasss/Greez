/*** includes ***/
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)
#define ABUFF_INIT {NULL, 0}
#define KILO_VERSION "0.0.1"
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY
};

/*** data ***/

typedef struct eRow {
    int size;
    char *chars;
} eRow;

struct editorConfig {
    struct termios orig_termios;
    int screen_rows;
    int screen_cols;
    int cx, cy;
    eRow *row;
    int numRows;
};
struct editorConfig E;

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = E.orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
            return '\x1b';
                }
                if (seq[2] == '`') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == '0') {
        switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
        }
    }
        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buff[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buff) - 1) {
        if (read(STDIN_FILENO, &buff[i], 1) != 1) break;
        if (buff[i] == 'R') break;
        i++;
    }
    buff[i] = '\0';
    if (buff[0] != '\x1b' || buff[1] != '[') return -1;
    if (sscanf(&buff[2], "%d;%d", rows, cols) != 2) return -1;
    return -1;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(eRow) * (E.numRows + 1));
    int at = E.numRows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numRows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
FILE *fp = fopen(filename, "r");
if (!fp) die("fopen");
char *line = NULL;
size_t lineCaps = 0;
ssize_t linelen;
while ((linelen = getline(&line, &lineCaps, fp)) != -1) {
while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
        linelen--;
        }
    editorAppendRow(line, linelen);
    }
free(line);
fclose(fp);
}

/*** append buffer ***/

struct aBuff {
    char *b;
    int len;
};

void abAppend(struct aBuff *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct aBuff *ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct aBuff *ab) {
    int y;
    for (y = 0; y < E.screen_rows; y++) {
        if (y >= E.numRows) {
        if (y == E.screen_rows / 3 && E.numRows == 0) {
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
            if (welcomelen > E.screen_cols) {
                welcomelen = E.screen_cols;
            }
            int padding = (E.screen_cols - welcomelen) / 2;
            if (padding) {
                abAppend(ab, "~", 1);
                padding--;
            }
            while (padding--) {
                abAppend(ab, " ", 1);
            }
            abAppend(ab, welcome, welcomelen);
        } else {
            abAppend(ab, "~", 1);
        }
    } else {
        int len = E.row[y].size;
        if (len > E.screen_cols) len = E.screen_cols;
        abAppend(ab, E.row[y].chars, len);
    }
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screen_rows - 1) {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    struct aBuff ab = ABUFF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buff, strlen(buff));
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor (int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screen_cols - 1) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.screen_rows - 1) {
                E.cy++;
            }
            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();
    switch(c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
            case HOME_KEY:
                E.cx = 0;
                break;
            case END_KEY:
                E.cx = E.screen_cols - 1;
                break;
            case PAGE_UP:
            case PAGE_DOWN:
            {
                int times = E.screen_rows;
                while (times--) {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        /*case PAGE_UP:
            E.cy = 0;
            break;
        case PAGE_DOWN:
            E.cy = E.screen_rows - 1;
            break;*/
    }
}

/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numRows = 0;
    E.row = NULL;
    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}