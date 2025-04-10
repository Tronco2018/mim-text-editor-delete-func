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
#include <string.h>

/*** DEFINES ***/
#define MIM_VERSION "0.0.1"

// Emulate CTRL + inputs (sets first three bits to 0 to emulate ASCII behaviour)
#define CTRL_KEY(k) ((k) & 0x1f)

// Key mappings
enum editorKey
{
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
    int size;
    char *chars;
} erow;

struct editor_config
{
    // Cursor positions
    int cx, cy;
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
    struct termios original_termios;
};

struct editor_config E;

/*** TERMINAL ***/

/**
 * Handle fatal errors.
 *
 * Print an error message provided by `perror()` and exit the program.
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
 * Enter raw terminal mode
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
 * Read a single key from standard input
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
 * Get current cursor position
 *
 * Request cursor position from terminal and parse the response
 * to determine current row and column.
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
 * Determine terminal window dimensions
 *
 * Try to get window size using ioctl, or fallback to positioning
 * cursor at bottom right and reading the position.
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
        // Update parmas
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** ROW OPERATIONS ***/

/**
 * Add a new row of text to the editor buffer
 *
 * Allocate memory for a new row and copy the provided string.
 */
void editor_append_row(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*** FILE IO ***/

/**
 * Open and read a file into the editor buffer
 *
 * Read file line by line and add each to the editor row buffer.
 */
void editor_open(char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("open");

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
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
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
 *
 * Reallocate memory as needed and append the string to the buffer.
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
 * Update row offset based on cursor position
 */
void editor_scroll()
{
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
    if (E.cx < E.coloff)
    {
        E.coloff = E.cx;
    }

    // Cursor is to the left of window, scroll left
    if (E.cx >= E.coloff + E.screencols + 1)
    {
        E.coloff = E.cx - E.screencols + 1;
    }
}

/**
 * Handle drawing each row of buffer of text
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
            int len = E.row[filerow].size - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            ab_append(ab, &E.row[filerow].chars[E.coloff], len);
        }
        // Clean row as we write
        ab_append(ab, "\x1b[K", 3);
        // If not last line, print newline
        if (y < E.screenrows - 1)
        {
            ab_append(ab, "\r\n", 2);
        }
    }
}

/**
 * Update the screen contents
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

    char buf[32];
    // Draw the cursor at cy, cx
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
    ab_append(&ab, buf, strlen(buf));

    // Show cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** INPUT  ***/

/**
 * Move the cursor using ARROW_{DIRECTION} keys
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
        else if(E.cy > 0) {
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
        if(row && E.cx < row->size)
            E.cx++;
        else if (row && E.cx == row->size) {
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
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

/**
 * Wait for keypress from `editor_read_key` and handle special characters
 */
void editor_process_keypress()
{
    int c = editor_read_key();
    switch (c)
    {
    case CTRL_KEY('q'):
        // Clear screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        E.cx = E.screencols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
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
    default:
        break;
    }
}

/**
 * Initialize editor state
 *
 * Set default values for editor state and get terminal window size.
 */
void init_editor()
{
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.numrows = 0;
    E.coloff = 0;
    E.row = NULL;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
}

/**
 * Main function to initialize the program
 */
int main(int argc, char *argv[])
{
    enable_raw_mode();
    init_editor();
    if (argc >= 2)
    {
        editor_open(argv[1]);
    }

    while (true)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
