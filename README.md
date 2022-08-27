# 小型shell：tsh


## 描述

本项目基于CS:APP Tsh lab，实现了一个小型的名为tsh的shell

## 功能特性

- 运行可执行程序，需要指示其具体路径，例如`tsh> /bin/ls -l -d`；可以通过`&`指示其在后台运行
- 运行内建指令，
  - `bg job`：让指示的job在后台恢复运行，`job`可以是PID或JID，下同
  - `fg job`：让指示的job在前台恢复运行
  - `quit`：退出tsh
  - `jobs`：列出所有后台job的信息
- 支持通过`<`与`>`进行I/O重定向，例如`tsh> /bin/cat < foo > bar`
- 按下`ctrl-c`(`ctrl-z`)能够让tsh给前台进程发送`SIGINT`(`SIGTSTP`)信号

## 实现内容

项目中需要自己实现的部分为`tsh.c`，具体来说，我们实现的部分包括：

- `void eval(char *cmdline)`：解析命令并执行，涉及自己实现的下列辅助函数：
  - `int builtin_command(struct cmdline_tokens *tok)`：检测内置命令
  - `void execute_quit()`：执行`quit`命令
  - `void execute_fg(struct cmdline_tokens *tok, sigset_t *pprev)`：执行`fg job`命令
  - `void execute_bg(struct cmdline_tokens *tok)`：执行`bg job`命令
- 3个信号处理函数，分别是：
  - `void sigchld_handler(int sig)`：捕获并处理SIGCHLD信号
  - `void sigtstp_handler(int sig)`：捕获并处理SIGTSTP信号
  - `void sigint_handler(int sig)`：捕获并处理SIGINT信号

## 要点概述

在实现该项目时，有一些值得注意的要点，简单罗列如下

- tsh需要维护一个job列表，存储有关所有job的各项信息，如PID、JID、当前状态
- tsh需要在合适的时机阻塞一些信号，特别是在信号处理函数内部
- 对于捕获的信号，我们必须区分清楚触发信号的多种可能。例如，子进程stop和terminate都会发送`SIGCHLD`给tsh，需要不同的处理方式
- 子进程的状态变化（例如变为stopped）可能由不同的信号导致，有一些信号不会被tsh捕获
- 由于信号不排队，tsh必须确保清理所有可能的僵尸子进程，而不是仅清理一个

## 编译与测试

首先通过`make clean`清除掉编译好的目标文件，然后通过`make`重新编译

如果想使用CS:APP tshlab提供的测试工具，可以直接执行`./sdriver`，它将测试所有的样例输入。如果想了解该工具的更多信息，请前往[CS:APP3e, Bryant and O'Hallaron (cmu.edu)](http://csapp.cs.cmu.edu/3e/labs.html)下载shell lab的writeup文件


如果想自己使用tsh，直接在命令行键入`./tsh`，看到命令提示符`tsh>`后即可尝试

## TODO

- 添加内部命令的输出重定向
- 管道
- `cd`切换工作路径
