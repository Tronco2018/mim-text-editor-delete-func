# Mim

A simple terminal text editor implemented in C.

## Features

- Basic text editing capabilities
- Create new file or load existing
- Status bar with file information
- Helpful alert messages
- File modification tracking

## Usage

```bash
./mim [filename]
```

### Controls

- `Ctrl+Q`: Quit
- `Ctrl+S`: Save
- Arrow keys: Move cursor
- Page Up/Down: Scroll through document
- Home/End: Move to start/end of line
- Delete/Backspace: Delete characters

## Building

```bash
make
```

or directly with gcc

```bash
gcc -o mim mim.c
```

## Credits

This project is available under the [BSD 2-Clause License
](LICENSE).

This project follows the tutorial found at https://viewsourcecode.org/snaptoken/kilo/