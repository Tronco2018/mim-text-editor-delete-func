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

/*** DATA ***/

struct termios original_termios;

struct editor_config
{
    int screenrows;
    int screencols;
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
char editor_read_key()
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
    return c;
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
    // Position cursor to top left with H
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    // Draw rows and then go back to top left
    ab_append(&ab, "\x1b[H", 3);
    // Show cursor
    ab_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** INPUT  ***/
/**
 * editor_process_keypress - Wait for keypress from `editor_read_key` and handle special characters.
 */
void editor_process_keypress()
{
    char c = editor_read_key();
    switch (c)
    {
    case CTRL_KEY('q'):
        // Clear screen
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    default:
        break;
    }
}

/*** INIT ***/

void init_editor()
{
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
    while (true)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}