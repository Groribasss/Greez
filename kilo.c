/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f) //CTRL co,binations with other keys
#define ABUFF_INIT {NULL, 0} //empty buffer
#define KILO_VERSION "0.0.1" //outputs with "Welcome" message
#define KILO_TAB_STOP 8 //amount of "spaces" in 1 "tab"

enum editorKey {
    BACKSPACE = 127,
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

typedef struct eRow { // 1 string of a file
    int size; //string size
    char *chars; //content of string
    char *render; //rendered content of string
    int rsize; //rendered size of string
} eRow;

struct editorConfig {
    struct termios orig_termios; //contains general terminal interface information; defines the basic input, output, control and line discipline modes
    int rowoff; //file line number at the top of screen
    int coloff; //file collumn offset
    int screen_rows; //amount of file rows shown on the screen
    int screen_cols; //amount of file collumns shown on the screen
    int cx, cy, rx; //x (rx is x coordinate after rendering) and y coordinates of the cursor
    eRow *row; //1 string of a file
    int numRows; //amount of rows of a file
    char *filename; //file that user wants to open
    char statusmsg[80];
    time_t statusmsg_time;
};
struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void die(const char *s) { //if function aborts, program terminates and name of a bad function
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s); //error message
    exit(1); //unsuccesfull programm ending
}

void disableRawMode() { //disabling raw mode
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) die("tcsetattr");
}

void enableRawMode() { //enablina a mode where the terminal does not perform any processing or handling of the input and output
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

int editorReadKey() {//converts inputs (from the terminal point of view) to a button variables
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

int getCursorPosition(int *rows, int *cols) {//knows where is cursor stays
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

int getWindowSize(int *rows, int *cols) {//size of editor window
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

int editorRowCxToRx(eRow *row, int cx) {//makes sure that cursor recognizes each tab as 8 spaces
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(eRow *row) {//renders file line (tab = 8 spaces)
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') tabs++;
    }
    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);
    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {//pushes all file lines to row array
    E.row = realloc(E.row, sizeof(eRow) * (E.numRows + 1));
    int at = E.numRows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    E.numRows++;
}

void editorRowInsertChar(eRow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if (E.cy == E.numRows) {
        editorAppendRow("", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numRows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;
    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numRows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename) {//opens a file and grabs text from it
    E.filename = malloc (strlen (filename) + 1);
    if (E.filename != NULL) strcpy (E.filename,filename);
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

void editorSave() {
    if (E.filename == NULL) return;
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
            close(fd);
            free(buf);
            editorSetStatusMessage("%d bytes written to disk", len);
            return;
            }
        }
        close(fd);   
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/

struct aBuff {//buffer that consists of text from the file
    char *b;
    int len;
};

void abAppend(struct aBuff *ab, const char *s, int len) { //adds given strings to a buffer
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct aBuff *ab) { //clears the buffer
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numRows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screen_rows) {
        E.rowoff = E.cy - E.screen_rows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.rx >= E.coloff + E.screen_cols) {
        E.coloff = E.rx - E.screen_cols + 1;
    }
}

void editorDrawRows(struct aBuff *ab) { //outputs to terminal file text
    int y;
    for (y = 0; y < E.screen_rows; y++) {
        int fileRow = y + E.rowoff;
        if (fileRow >= E.numRows) {
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
        int len = E.row[fileRow].rsize - E.coloff;
        if (len < 0) len = 0;
        if (len > E.screen_cols) len = E.screen_cols;
        abAppend(ab, E.row[fileRow].render + E.coloff, len);
    }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct aBuff *ab) { //outputs to terminal status bar string
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", E.filename ? E.filename : "[No Name]", E.numRows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numRows);
    if (len > E.screen_cols) len = E.screen_cols;
    abAppend(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
        len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct aBuff *ab) { //outputs to terminal message bar string
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screen_cols) msglen = E.screen_cols;
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

void editorRefreshScreen() { //clears screen->puts in buffer all characters that must be shown->positions cursor according to coordinates->unpacks buffer and cleares it
    editorScroll();
    struct aBuff ab = ABUFF_INIT;
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    char buff[32];
    snprintf(buff, sizeof(buff), "\x1b[%d;%dH", E.cy + 1 - E.rowoff, E.rx + 1 - E.coloff);
    abAppend(&ab, buff, strlen(buff));
    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor (int key) {//cursor movement
    eRow *row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
        if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numRows) {
                E.cy++;
            }
            break;
    }
    row = (E.cy >= E.numRows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {//does different actions according to input from the user
    int c = editorReadKey();
    switch(c) {
        case '\r':
            /* TODO */
            break;
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case HOME_KEY:
                E.cx = 0;
                break;
        case END_KEY:
                if (E.cy < E.numRows) {
                    E.cx = E.row[E.cy].size;
                }
            case BACKSPACE:
            case CTRL_KEY('h'):
            case DEL_KEY:
                    /* TODO */
                break;
            case PAGE_UP:
            case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screen_rows - 1;
                    if (E.cy > E.numRows) {
                        E.cy = E.numRows;
                    }
                }
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editorInsertChar(c);
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

void initEditor() { //initialization of some variables
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numRows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) die("getWindowSize");
    E.screen_rows -= 2;
}

int main(int argc/*amount of arguments sended to programm*/, char *argv[]/*array that consists of programm arguments*/) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}