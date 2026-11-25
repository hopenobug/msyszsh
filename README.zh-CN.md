# msyszsh

[English](README.md) | 简体中文

> `msys2_shell.cmd` 的最小 C 替代品：在当前控制台里直接启动 MSYS2 的登录 shell
> （默认 zsh），不用 cmd.exe、不弹新窗口。

等价于下面两行，但只需要一个 exe、不依赖 cmd 的解析和引号规则：

```bat
chcp 65001>nul
%MSYS2_ROOT%\msys2_shell.cmd -defterm -no-start -use-full-path -here -mingw64 -shell zsh %*
```

## 前置条件

**必须设置环境变量 `MSYS2_ROOT`**，指向 MSYS2 安装目录（即包含 `usr\bin\msys-2.0.dll` 的那一层）。
程序不做任何路径猜测，找不到就直接报错退出。

```bat
:: cmd
set MSYS2_ROOT=C:\msys64
```

```sh
# bash / zsh
export MSYS2_ROOT=/c/msys64
```

## 编译

需要 MinGW-w64 gcc 和 make（MSYS2 的 `mingw64` 工具链或 <https://www.mingw-w64.org> 均可）。

```sh
make            # 生成 msyszsh.exe，并自动修掉低完整性标签
make clean
```

`make` 编译完会自动执行 `icacls msyszsh.exe /setintegritylevel Medium`（原因见下方
「常见问题」）。即便只是重复 `make`、没有重新编译，这一步也会再跑一次，因为新生成的
文件会重新继承目录上的低完整性标签。

默认参数 `-O2 -Wall -Wextra`，链接 `-lshell32 -ladvapi32`。
注意不能加 `-municode`（会要求 `wWinMain`）。

## 用法

```sh
msyszsh.exe                          # 启动 zsh（登录 shell）
msyszsh.exe -c 'echo $MSYSTEM'       # 其余参数原样透传给 shell
msyszsh.exe -shell bash -c 'ls'
msyszsh.exe --help
```

### 选项

| 选项 | 含义 |
| --- | --- |
| `-shell SHELL` | 登录 shell 名，默认 `zsh`（也支持 `bash`、`sh` 等，位于 `<MSYS2_ROOT>\usr\bin`） |
| `-mingw64` / `-mingw32` / `-ucrt64` / `-clang64` / `-clangarm64` / `-msys[2]` | 设置 `MSYSTEM`，默认 `MINGW64` |
| `-full-path` / `-use-full-path` | 导出完整 Windows `PATH`（`MSYS2_PATH_TYPE=inherit`，默认开） |
| `-here` / `-defterm` / `-no-start` | 兼容 `msys2_shell.cmd` 保留，行为已内置，无副作用 |
| `-h` / `--help` / `-?` | 显示帮助 |
| `--` | 之后的全部参数当作 shell 参数 |
| 其他 | 透传给 shell（按 MSVCRT 引号规则重新加引号） |

### 与 cmd 脚本的对应关系

| `msys2_shell.cmd` | 本程序 |
| --- | --- |
| `chcp 65001` | `SetConsoleOutputCP/SetConsoleCP(CP_UTF8)`，退出时恢复原码页 |
| `-here` | 继承当前工作目录 + `CHERE_INVOKING=enabled_from_arguments` |
| `-use-full-path` | `MSYS2_PATH_TYPE=inherit` |
| `-mingw64` | `MSYSTEM=MINGW64` |
| `-shell zsh` | 执行 `<MSYS2_ROOT>\usr\bin\zsh.exe -l` |
| `-defterm` / `-no-start` | 无对应动作（本程序本来就不新开窗口） |
| `%*` | 其余命令行参数 |

子进程退出码作为本程序退出码返回；`Ctrl+C` 交给 shell 处理。

## 常见问题

### `MSYS2_ROOT is not set`

设置该环境变量后再运行，见上文「前置条件」。

### `fatal error - NtCreateDirectoryObject(\BaseNamedObjects\msys-2.0S5-...): 0xC0000022`

不是本程序的 bug。exe 所在目录如果带 `Mandatory Label\Low Mandatory Level` 标签，
进程会被降到**低完整性级别**，无权在 `\BaseNamedObjects` 建对象，MSYS2 运行时初始化即失败。

```sh
# 查看
icacls msyszsh.exe
# 修复
make fix-integrity
```

`make` 已经自动做了这件事，通常不用手动跑。

或者把 exe 编译/复制到没有低完整性标签的目录。程序启动时也会自检，处于低完整性级别会直接打印这条修复提示。

## 目录

```
msyszsh.c        唯一源文件
Makefile
README.md        英文说明
README.zh-CN.md  本文件
```
