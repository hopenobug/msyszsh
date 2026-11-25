# msyszsh

English | [简体中文](README.zh-CN.md)

> A minimal C replacement for `msys2_shell.cmd`: run the MSYS2 login shell (zsh
> by default) directly in the current console — no `cmd.exe`, no new window.

It is equivalent to the two lines below, but needs only one exe and does not rely
on `cmd.exe` parsing or its quoting rules:

```bat
chcp 65001>nul
%MSYS2_ROOT%\msys2_shell.cmd -defterm -no-start -use-full-path -here -mingw64 -shell zsh %*
```

## Requirements

**The `MSYS2_ROOT` environment variable must be set** and point at the MSYS2
installation directory (the one that contains `usr\bin\msys-2.0.dll`). The program
does not guess any path: if the variable is missing or wrong, it prints an error
and exits.

```bat
:: cmd
set MSYS2_ROOT=C:\msys64
```

```sh
# bash / zsh
export MSYS2_ROOT=/c/msys64
```

## Build

Requires MinGW-w64 gcc and make (the MSYS2 `mingw64` toolchain or
<https://www.mingw-w64.org> both work).

```sh
make            # builds msyszsh.exe and strips the low-integrity label
make clean
```

After compiling, `make` automatically runs
`icacls msyszsh.exe /setintegritylevel Medium` (see "Troubleshooting" below).
This happens on every `make`, even when nothing was recompiled, because a freshly
written file inherits the low-integrity label from its directory again.

Default flags are `-O2 -Wall -Wextra`, linking `-lshell32 -ladvapi32`.
Do not add `-municode` — it would require `wWinMain`.

## Usage

```sh
msyszsh.exe                          # start zsh (login shell)
msyszsh.exe -c 'echo $MSYSTEM'       # remaining arguments go to the shell as-is
msyszsh.exe -shell bash -c 'ls'
msyszsh.exe --help
```

### Use it as the shell of a terminal emulator

Because it is a single exe that starts the login shell in the current console, it
works as the "shell" / "command line" of any terminal emulator that lets you set
one (Windows Terminal, ConEmu, Tabby, VS Code terminals, …). No `cmd.exe` wrapper
and no `chcp` needed.

Windows Terminal — add to the `profiles.list` array of `settings.json`:

```json
{
    "name": "MSYS2 zsh",
    "commandline": "C:\\tools\\msyszsh.exe",
    "startingDirectory": "%USERPROFILE%",
    "icon": "C:\\msys64\\msys2.ico"
}
```

Notes:

- Use the full path to the exe, and double the backslashes (JSON).
- `MSYS2_ROOT` must be set in the environment the terminal inherits (e.g. a user
  environment variable), since the emulator does not read your shell's `rc` files.
- Extra arguments can be appended in `commandline`, e.g.
  `"C:\\tools\\msyszsh.exe" -shell bash`.
- `startingDirectory` is honoured: the shell starts in that directory, and
  `Ctrl+C`/exit codes behave like any other shell profile.

### Options

| Option | Meaning |
| --- | --- |
| `-shell SHELL` | Login shell name, `zsh` by default (`bash`, `sh`, … are also fine; looked up in `<MSYS2_ROOT>\usr\bin`) |
| `-mingw64` / `-mingw32` / `-ucrt64` / `-clang64` / `-clangarm64` / `-msys[2]` | Sets `MSYSTEM`, default `MINGW64` |
| `-full-path` / `-use-full-path` | Exports the full Windows `PATH` (`MSYS2_PATH_TYPE=inherit`, on by default) |
| `-here` / `-defterm` / `-no-start` | Accepted for `msys2_shell.cmd` compatibility; the behaviour is built in, no effect |
| `-h` / `--help` / `-?` | Show help |
| `--` | Everything after it is passed to the shell |
| anything else | Passed to the shell (re-quoted per MSVCRT rules) |

### How it maps to the cmd script

| `msys2_shell.cmd` | This program |
| --- | --- |
| `chcp 65001` | `SetConsoleOutputCP/SetConsoleCP(CP_UTF8)`, original code page restored on exit |
| `-here` | Keeps the current working directory + `CHERE_INVOKING=enabled_from_arguments` |
| `-use-full-path` | `MSYS2_PATH_TYPE=inherit` |
| `-mingw64` | `MSYSTEM=MINGW64` |
| `-shell zsh` | Runs `<MSYS2_ROOT>\usr\bin\zsh.exe -l` |
| `-defterm` / `-no-start` | Nothing to do (this program never opens a new window) |
| `%*` | The remaining command line arguments |

The child's exit code becomes this program's exit code; `Ctrl+C` is left to the shell.

## Troubleshooting

### `MSYS2_ROOT is not set`

Set the variable as described in "Requirements" and run again.

### `fatal error - NtCreateDirectoryObject(\BaseNamedObjects\msys-2.0S5-...): 0xC0000022`

Not a bug in this program. If the directory containing the exe carries a
`Mandatory Label\Low Mandatory Level` label, the process is demoted to **low
integrity level** and is not allowed to create objects under `\BaseNamedObjects`,
so the MSYS2 runtime fails during initialisation.

```sh
# inspect
icacls msyszsh.exe
# fix
make fix-integrity
```

`make` already does this for you, so you normally do not need to run it by hand.

Alternatively, build or copy the exe into a directory without a low-integrity
label. The program also checks itself at startup and prints this hint when it is
running at low integrity level.

## Layout

```
msyszsh.c         the only source file
Makefile
README.md         this file
README.zh-CN.md   Chinese translation
```
