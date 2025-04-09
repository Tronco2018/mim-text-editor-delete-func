/*** INCLUDES ***/

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
} erows;

struct editor_config
{
    // Cursor positions
    int cx, cy;
    // Screen size
    int screenrows;
    int screencols;
    // Number of rows used
    int numrows;
    // Data in each row + size
    erows row;
    struct termios original_termios;
};

struct editor_config E;

/*** TERMINAL ***/

/**
 * die - Handle fatal errors.
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
 *
 * disable_raw_mode - Restore original terminal settings
 */
void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1)
        die("tcsetattar");
}

/**
 * enable_raw_mode - Enter raw terminal mode
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
 * editor_read_key - Read a single key from standard input
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

/*** FILE IO ***/
void editor_open()
{
    char *line = "Hi Mom!";
    ssize_t linelen = strlen(line);

    E.row.size = linelen;
    E.row.chars = malloc(linelen + 1);
    memcpy(E.row.chars, line, linelen);
    E.row.chars[linelen] = '\0';
    E.numrows = 1;
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

// Append len bytes of s to ab buffer
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

// Free ab buffer
void ab_free(struct abuf *ab)
{
    free(ab->b);
}

/*** OUTPUT ***/

/**
 * editor_draw_rows - Handle drawing each row of buffer of text
 */
void editor_draw_rows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        if (y >= E.numrows)
        {
            if (y == E.screenrows / 3)
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
            int len = E.row.size;
            if (len > E.screencols)
                len = E.screencols;
            ab_append(ab, E.row.chars, len);
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
 * editor_refresh_screen - Update the screen contents
 */
void editor_refresh_screen()
{
    struct abuf ab = ABUT_INIT;

    // Hide cursor
    ab_append(&ab, "\x1b[?25l", 6);
    // Position cursor to top left with H, no args
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    char buf[32];
    // Draw the cursor at cy, cx
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    ab_append(&ab, buf, strlen(buf));

    // Show cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** INPUT  ***/

/**
 * editor_move_cursor - Move the cursor using ARROW_{DIRECTION} keys
 */
void editor_move_cursor(int key)
{
    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
            E.cx--;
        break;
    case ARROW_DOWN:
        if (E.cy != E.screencols - 1)
            E.cy++;
        break;
    case ARROW_UP:
        if (E.cy != 0)
            E.cy--;
        break;
    case ARROW_RIGHT:
        if (E.cx != E.screenrows - 1)
            E.cx++;
        break;
    default:
        break;
    }
}

/**
 * editor_process_keypress - Wait for keypress from `editor_read_key` and handle special characters.
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

/*** INIT ***/

void init_editor()
{
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    if (get_window_size(&E.screenrows, &E.screencols) == -1)
        die("get_window_size");
}
/**
 * main - Main function to innit the program
 */
int main()
{
    enable_raw_mode();
    init_editor();
    editor_open();

    while (true)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
