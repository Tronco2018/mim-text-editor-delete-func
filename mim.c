/*** INCLUDES ***/

// Feature test macros
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

/*** DEFINES ***/
#define MIM_VERSION "1.0.0"
#define MIM_TAB_SIZE 4
#define MIM_QUIT_TIMES 1

// Emulate CTRL + inputs (sets first three bits to 0 to emulate ASCII behaviour)
#define CTRL_KEY(k) ((k) & 0x1f)

// Key mappings
enum editorKey
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** DATA ***/

struct termios original_termios;

typedef struct erow
{
    // Size of actual characters
    int size;
    // Actual data
    char *chars;
    // Size of render string
    int rsize;
    // Data to render (formatted)
    char *render;
} erow;

struct editor_config
{
    // Cursor positions
    int cx, cy;
    // Cursor position on render
    int rx;
    // Row offset
    int rowoff;
    // Column offset
    int coloff;
    // Screen size
    int screenrows;
    int screencols;
    // Number of rows used
    int numrows;
    // Data in each row + size
    erow *row;
    // 0 = file unmodified, 1 = file modified
    int dirty;
    // Name of file opened in editor
    char *filename;
    // Status message below status bar
    char statusmsg[80];
    // Time when message was set
    time_t statusmsg_time;
    struct termios original_termios;
};

struct editor_config E;

/*** PROTOTYPES ***/

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt);

/*** TERMINAL ***/

/**
 * Handle fatal errors and exit program
 */
void die(const char *s)
{
    // Clean screen on exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

/**
 * Restore original terminal settings
 */
void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1)
        die("tcsetattar");
}

/**
 * Enter raw terminal mode for better keyboard input handling
 */
void enable_raw_mode()
{
    if (tcgetattr(STDERR_FILENO, &E.original_termios) == -1)
        die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.original_termios;
    /*
    Disable features by flipping flags
        ICRNL = CTRL + M (carriage return)
        IXON = CTRL + S / Q (advanced input)
        OPOST = output post-processing (\n -> \r\n)
        ECHO = repeats keyboard input
        ICANON = canonical mode (line-buffered input)
        IEXTEN = CTRL + V (literal character input)
        ISIG = CTRL + Z (SIGINT and SIGTSTP)
    */
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // Misc flags that we do because we used to
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= ~(CS8);

    // min num of bytes needed before read() can return
    raw.c_cc[VMIN] = 0;
    // 1/10 of a second
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

/**
 * Read a single key from keyboard input
 */
int editor_read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        // Cygwin returns -1 and errno = EAGAIN when read() times out
        // So we ignore that.
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    // Input is esc key
    // Immediately consume next bytes to check for special keys
    if (c == '\x1b')
    {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY; // [1~
                    case '2':
                        return END_KEY; // [2~
                    case '3':
                        return DEL_KEY; // [3~
                    case '5':
                        return PAGE_UP; // [5~
                    case '6':
                        return PAGE_DOWN; // [6~
                    case '7':
                        return HOME_KEY; // [7~
                    case '8':
                        return END_KEY; // [8~
                    default:
                        break;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP; // [A
                case 'B':
                    return ARROW_DOWN; // [B
                case 'C':
                    return ARROW_RIGHT; // [C
                case 'D':
                    return ARROW_LEFT; // [D
                case 'H':
                    return HOME_KEY; // [H
                case 'F':
                    return END_KEY; // [F
                default:
                    break;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY; // OH
            case 'F':
                return END_KEY; // OF
            default:
                break;
            }
        }

        // Return as is for unhandled cases
        return '\x1b';
    }
    else
    {
        return c;
    }
}

/**
 * Get current cursor position from terminal
 */
int get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    // Request cursor position (6) from Device Status Report (n)
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    return 0;
}

/**
 * Get terminal window dimensions
 */
int get_window_size(int *rows, int *cols)
{
    struct winsize ws;
    // Get size from ioctl into ws
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Fallback option:
        // position cursor to extreme right (999C) and bottom (999B)
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[998B", 12) != 12)
            return -1;
        // Failure
        return get_cursor_position(rows, cols);
    }
    else
    {
        // Update params
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** ROW OPERATIONS ***/

/**
 * Convert cursor x position to render x position accounting for tabs
 */
int editor_row_cx_to_rx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            rx += (MIM_TAB_SIZE - 1) - (rx % MIM_TAB_SIZE);
        rx++;
    }
    return rx;
}

/**
 * Update the render version of a row with proper tab handling
 */
void editor_update_row(erow *row)
{
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    // Tabs need max TAB_SIZE bytes, 1 is already in size so we add the rest
    row->render = malloc(row->size + tabs * (MIM_TAB_SIZE - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            // Append spaces until tabsize is hit
            while (idx % MIM_TAB_SIZE != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/**
 * Add a new row to the editor buffer
 */
void editor_insert_row(int at, char *s, size_t len)
{
    // Validate index at
    if (at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

/**
 * Free memory allocated for a row
 */
void editor_free_row(erow *row)
{
    free(row->chars);
    free(row->render);
}

/**
 * Delete a row at specified position
 */
void editor_del_row(int at)
{
    // Validate row index
    if (at < 0 || at > E.numrows)
        return;
    editor_free_row(&E.row[at]);
    // Shift rows [at+1] to [at]
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

/**
 * Insert a character into a row at specified position
 */
void editor_row_insert_char(erow *row, int at, int c)
{
    // Validate at index
    if (at < 0 || at > row->size)
        at = row->size;
    // +1 for new char, +1 for '\0'
    row->chars = realloc(row->chars, row->size + 2);
    // Shift from [at] to [at+1]
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    // Rerender row
    editor_update_row(row);
    E.dirty++;
}

/**
 * Append a string to end of specified row
 */
void editor_row_append_string(erow *row, char *s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    // Copy s at end of row
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

/**
 * Delete character at specified position in row
 */
void editor_row_del_char(erow *row, int at)
{
    if (at < 0 || at >= row->size)
        return;
    // Shift from [at+1] to [at]
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

/*** EDITOR OPERATIONS ***/

/**
 * Insert a character at current cursor position
 */
void editor_insert_char(int c)
{
    if (E.cy == E.numrows)
    {
        // Append a new row
        editor_insert_row(E.numrows, " ", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

/**
 * Insert a newline at current cursor position
 */
void editor_insert_newline()
{
    if (E.cx == 0)
    {
        editor_insert_row(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        // Insert a row below with the rest of the line contents
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        // Reinitialize cause insert row rellocs
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

/**
 * Delete character at current cursor position
 */
void editor_del_char()
{
    // Cursor past file, return
    if (E.cy == E.numrows)
        return;
    // Cursor top of file, return
    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.row[E.cy];
    // Character to the left, then delete
    if (E.cx > 0)
    {
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    }
    // If deleteing from first position, merge rows
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}
/*** FILE IO ***/

/**
 * Convert editor rows to a single string
 */
char *editor_rows_to_string(int *buflen)
{
    // Total length of text
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1; // +1 for \n

    // Set buflen to totlen to tell the caller of its size
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++)
    {
        // Loop thru, append each row to p + \n
        memcpy(p, E.row[j].chars, E.row[j].size);
        // Point addition to move to last
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}
/**
 * Open and read a file into the editor buffer
 */
void editor_open(char *filename)
{
    free(E.filename);
    // Duplicate string instead of taking the reference
    E.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp)
    {
        // File doesn't exist, just set the filename without creating the file
        // The file will be created when the user saves
        // This message is actually overwritten by the help message
        editor_set_status_message("New file: %s", filename);
        return;
    }

    char *line = NULL;
    // Hold size of allocated buff
    size_t linecap = 0;
    // Hold length of line read
    ssize_t linelen;
    // getline reads one line + ending newline
    // from file pointed by fp into memory pointed by line
    // and sets linecap to the size it read
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        // Trim all newlines and carriage returns
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

/**
 * Save current file to disk
 */
void editor_save()
{
    if (E.filename == NULL)
    {
        E.filename = editor_prompt("Save as: %s");
        if (E.filename == NULL)
        {
            editor_set_status_message("Save aborted");
            return;
        }
    }

    // Length of file string
    int len;
    // buf has a pointer to the converted memory that we free later
    char *buf = editor_rows_to_string(&len);

    // Check if file already exists to differentiate messages
    int file_exists = access(E.filename, F_OK) == 0;

    // O_READWRITE
    // O_CREATE file if doesn't exist
    // 0644 file perms if file is to be created
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate64(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;

                if (!file_exists)
                {
                    editor_set_status_message("New file created: %s. %d bytes written to disk", E.filename, len);
                }
                else
                {
                    editor_set_status_message("%d bytes written to disk", len);
                }
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("Failed to save! I/O error: %s", strerror(errno));
}
// Define a single string buffer to update at once
// Append buffer
struct abuf
{
    char *b;
    int len;
};

// Constructor
#define ABUT_INIT {NULL, 0}

/**
 * Append string to append buffer
 */
void ab_append(struct abuf *ab, const char *s, int len)
{
    // Realloc bigger mem as required
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    // Append and update buf
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

/**
 * Free memory used by append buffer
 */
void ab_free(struct abuf *ab)
{
    free(ab->b);
}

/*** OUTPUT ***/

/**
 * Update row and column offsets based on cursor position
 */
void editor_scroll()
{
    E.rx = E.cx;
    if (E.cy < E.numrows)
    {
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
    }
    // Cursor is above visible window, scroll up
    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    // Cursor is past visible window, scroll down
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    // Cursor is to the right of window, scroll right
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }

    // Cursor is to the left of window, scroll left
    if (E.rx >= E.coloff + E.screencols + 1)
    {
        E.coloff = E.rx - E.screencols + 2;
    }
}

/**
 * Draw each row of text in the editor
 */
void editor_draw_rows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows)
        {
            // Display welcome if nothing is in rows buff
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "MIM Text Editor -- version %s", MIM_VERSION);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                // Center
                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    ab_append(ab, " ", 1);

                ab_append(ab, welcome, welcomelen);
            }
            else
            {
                // Write a tilde
                ab_append(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            ab_append(ab, &E.row[filerow].render[E.coloff], len);
        }
        // Clean row as we write
        ab_append(ab, "\x1b[K", 3);

        // Append newline
        ab_append(ab, "\r\n", 2);
    }
}

/**
 * Draw the status bar at bottom of screen
 */
void editor_draw_status_bar(struct abuf *ab)
{
    // <Esc>[7m switches to invert color
    // m - Select Graphic Rendition, 7 for invert
    ab_append(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];

    // Name of file and no. of lines
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No name]",
                       E.numrows,
                       E.dirty ? "(modified)" : "");

    // From the right, current position
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    // Trim if bigger than screen
    if (len > E.screencols)
        len = E.screencols;

    ab_append(ab, status, len);
    while (len < E.screencols)
    {
        // Go till we hit the space where the edge of screen is
        // rlen characters long
        if (E.screencols - len == rlen)
        {
            ab_append(ab, rstatus, rlen);
            break;
        }
        else
        {
            ab_append(ab, " ", 1);
        }
        len++;
    }
    // Return to normal colors with <Esc>[m
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

/**
 * Draw the message bar below status bar
 */
void editor_draw_message_bar(struct abuf *ab)
{
    // Clear bar with K
    ab_append(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);

    // Trim for screen size
    if (msglen > E.screencols)
        msglen = E.screencols;

    // There is a message and time hasn't expired
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        ab_append(ab, E.statusmsg, msglen);
}

/**
 * Refresh the entire screen contents
 */
void editor_refresh_screen()
{
    editor_scroll();

    struct abuf ab = ABUT_INIT;

    // Hide cursor
    ab_append(&ab, "\x1b[?25l", 6);
    // Position cursor to top left with H, no args
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    // Draw the cursor at cy, cx
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    ab_append(&ab, buf, strlen(buf));

    // Show cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/**
 * Set status message with optional formatting
 */
void editor_set_status_message(const char *fmt, ...)
{
    va_list ap;
    // Call va_start on a va_list object with fmt to get the address of next argument
    va_start(ap, fmt);

    // format the string according to fmt and stores it in E.statusmsg
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);

    // clean up the va_list after we're done
    va_end(ap);

    // Pass null to time to get current time
    E.statusmsg_time = time(NULL);
}

/*** INPUT  ***/

/**
 * Prompt user for input and return entered text
 */
char *editor_prompt(char *prompt)
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (true)
    {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h' || c == BACKSPACE))
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b')
        {
            editor_set_status_message("");
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            // buf is not empty
            if (buflen != 0)
            {
                editor_set_status_message("");
                return buf;
            }
        }
        // If printable char, append to buff
        else if (!iscntrl(c) && c < 128)
        {
            // If buffer is filled, realloc double mem
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

/**
 * Move cursor based on arrow key input
 */
void editor_move_cursor(int key)
{
    // Check if the current row index is within the bounds of the number of rows
    // If it is out of bounds, set row to NULL.
    // Ensures that the cursor does not move beyond the available rows.

    erow *row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
            E.cx--;
        else if (E.cy > 0)
        {
            // Move up to previous line
            E.cy--;
            // At the end of line
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows)
            E.cy++;
        break;
    case ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
            E.cx++;
        else if (row && E.cx == row->size)
        {
            // Move to line below
            E.cy++;
            // At the beginning of line
            E.cx = 0;
        }
        break;
    default:
        break;
    }

    // Reset init row and do the same for horizontal
    row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

/**
 * Process keyboard input and handle special keys
 */
void editor_process_keypress()
{
    static int quit_times = MIM_QUIT_TIMES;
    int c = editor_read_key();

    // Reset quit_times to MIM_QUIT_TIMES if the key pressed is not CTRL-Q
    if (c != CTRL_KEY('q'))
    {
        quit_times = MIM_QUIT_TIMES;
    }

    switch (c)
    {
    // Enter key
    case '\r':
        editor_insert_newline();
        break;

    case CTRL_KEY('q'):
        // Clear screen
        if (E.dirty && quit_times > 0)
        {
            editor_set_status_message("WARNING: File has unsaved changes. Press CTRL-Q again to quit.");
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case CTRL_KEY('d'):
	if (E.cy < E.numrows) {
		editor_del_row(E.cy);
		if (E.cy >= E.numrows && E.numrows > 0){
			E.cy = E.numrows -1;
		}
		E.cx = 0;
	}
	break;
    case CTRL_KEY('s'):
        editor_save();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        // For del key, also move it to right
        if (c == DEL_KEY)
            editor_move_cursor(ARROW_RIGHT);
        editor_del_char();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        // Move cursor to top or bottom of screen
        // and then emulate an entire screen worth of up/down input
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows - 1;
        }

        int times = E.screenrows;
        while (times--)
            editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
    break;
    case ARROW_DOWN:
    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;

    // Ignore
    // CTRL-L used to be used for terminal refreshing
    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        // Insert every other keypress
        editor_insert_char(c);
        break;
    }
}

/**
 * Initialize editor state and terminal
 */
void init_editor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.numrows = 0;
    E.coloff = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
    // Reserve two space for status bar
    E.screenrows -= 2;
}

/**
 * Main entry point for the editor
 */
int main(int argc, char *argv[])
{
    enable_raw_mode();
    init_editor();
    if (argc >= 2)
    {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: CTRL+S to save | CTRL+Q to quit");

    while (true)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
