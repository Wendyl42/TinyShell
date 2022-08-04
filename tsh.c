/* 
 * tsh - A tiny shell program with job control
 * 
 * Name: Yihua Liu
 * ID: 1800017857
 * 
 * This is a shell with basic functions. It can run programs
 * foreground or background, and typing ctrl-c or ctrl-z can
 * send signal to it. Besides, it has 3 built-in commands:
 * jobs, fg job and bg job. I/O redirection is also supported.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */

/* Default file permissions are DEF_MODE & ~DEF_UMASK */
/* $begin createmasks */
#define DEF_MODE   S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH
#define DEF_UMASK  S_IWGRP|S_IWOTH
/* $end createmasks */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
        BUILTIN_FG} builtins;
};

/* End global variables */

/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Function from csapp.c */
void Sigfillset(sigset_t *set);
void Sigemptyset(sigset_t *set);
void Sigaddset(sigset_t *set, int signum);
void Sigdelset(sigset_t *set, int signum);
int Sigsuspend(const sigset_t *set);
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
pid_t Fork(void);
void Execve(const char *filename, char *const argv[], char *const envp[]);
void Setpgid(pid_t pid, pid_t pgid);
int Open(const char *pathname, int flags, mode_t mode);
void Close(int fd);
void Sio_error(char s[]);

/* My helper functions */
int builtin_command(struct cmdline_tokens *tok);
void execute_quit();
void execute_fg(struct cmdline_tokens *tok, sigset_t *pprev);
void execute_bg(struct cmdline_tokens *tok);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 

void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
ssize_t sio_puts(char s[]);
ssize_t sio_putl(long v);
void sio_error(char s[]);


typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);


/*
 * main - The shell's main routine 
 */
int 
main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);

    /* Execute the shell's read/eval loop */
    while (1) {

        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);
        
        fflush(stdout);
        fflush(stdout);
    } 
    
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void 
eval(char *cmdline) 
{
    /* Declare variables */
    int bg, jid; /* Should the job run in bg or fg? */
    pid_t pid;           /* Process id */
    struct cmdline_tokens tok;
    sigset_t prev, mask_three;

    /* Initialize block sets */
    Sigemptyset(&mask_three);
    Sigaddset(&mask_three, SIGCHLD);
    Sigaddset(&mask_three, SIGINT);
    Sigaddset(&mask_three, SIGTSTP);

    /* Parse command line */
    if((bg = parseline(cmdline, &tok)) == -1) /* parsing error */
        return;
    if (tok.argv[0] == NULL) /* ignore empty lines */
        return;

    /* Handling commands */
    if(!builtin_command(&tok))
    {
        Sigprocmask(SIG_BLOCK, &mask_three, &prev); /* Block SIGCHLD */
        
        if((pid = Fork()) == 0)
        {
            /* Preparations */
            Sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock SIGCHLD in child process */
            Setpgid(0, 0); /* put child in new process group */
            /* restore default signal handler */
            signal(SIGCHLD, SIG_DFL); 
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            
            /* I/O redirection */
            if(tok.infile)
            {
                int fd_src = Open(tok.infile, O_RDONLY, 0);
                dup2(fd_src, STDIN_FILENO);
            }
            if(tok.outfile)
            {
                umask(DEF_UMASK);
                int fd_dst = Open(tok.outfile, O_CREAT | O_TRUNC | O_WRONLY, DEF_MODE);
                dup2(fd_dst, STDOUT_FILENO);
            }

            /* Child run user job */
            if(execve(tok.argv[0], tok.argv, environ) < 0)
            {
                sio_puts(tok.argv[0]);
                sio_puts("s: Command not found.\n");
            }
            exit(0);
        }
        /* Parent adds job */
        addjob(job_list, pid, bg + 1, cmdline);
        jid = pid2jid(pid);

        if(!bg) /* Child runs foreground */
        {
            while (pid == fgpid(job_list)) /* Parent waits for foreground job to terminate */
                Sigsuspend(&prev);
        }
        else /* Child runs background */
        {
            /* Print prompt message */
            sio_puts("[");
            sio_putl(jid);
            sio_puts("] (");
            sio_putl(pid);
            sio_puts(") ");
            sio_puts(cmdline);
            sio_puts("\n");
        }
        Sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock signals before return */
    }
    return;
}

/* Builtin_command - If first arg is a builtin command, run it and return true */
int builtin_command(struct cmdline_tokens *tok)
{
    /* Declare and initialize block sets */
    sigset_t mask, prev;
    Sigfillset(&mask);
    Sigprocmask(SIG_BLOCK, &mask, &prev);
    Sigprocmask(SIG_SETMASK, &prev, NULL);

    if(tok->builtins == BUILTIN_QUIT) /* Builtin command quit */
        execute_quit();
    else if(tok->builtins == BUILTIN_JOBS) /* Builtin command jobs */
    {
        
        if(tok->outfile) /* Output redirection */
        {
            umask(DEF_UMASK);
            int fd_dst = Open(tok->outfile, O_CREAT | O_TRUNC | O_WRONLY, DEF_MODE);
            listjobs(job_list, fd_dst);
            Close(fd_dst);
        }
        else
            listjobs(job_list, STDOUT_FILENO);
        return 1;
    }
    else if(tok->builtins == BUILTIN_FG) /* Builtin command fg job */
    {
        execute_fg(tok, &prev);
        return 1;
    }
    else if(tok->builtins == BUILTIN_BG) /* Builtin command bg job */
    {
        execute_bg(tok);
        return 1;
    }

    return 0;
}

/* execute_quit - execute build-in command quit */
void execute_quit()
{
    /* Declare and initialize variables */
    int status;
    sigset_t mask_all, prev;
    pid_t pid;
    Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev);
    
    /* Reap zombie child before quit */
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED))>0)
    {
        Sigprocmask(SIG_BLOCK, &mask_all, &prev); /* Block all signals */

        if (WIFSTOPPED(status)) /* Child is stopped */
            kill(-pid, SIGINT); /* Terminate the child */
        else /* Child terminated */
            deletejob(job_list, pid);

        /* Unblock signals */
        Sigprocmask(SIG_SETMASK, &prev, NULL);
    }
    exit(0);
}


/* execute_fg - execute build-in command fg job */
void execute_fg(struct cmdline_tokens *tok, sigset_t *pprev)
{
    /* Declare and initialize variables */
    char *ID_str = tok->argv[1];
    int jid, pid;
    struct job_t *target_job;

    if(tok->argc != 2) /* Invalid format */
    {
        sio_puts(tok->argv[0]);
        //sio_puts(" command requires PID or %%jobid argument\n");
        sio_puts(" please input one and only one ID argument\n");
        return;
    }

    /* Get target job by input id */
    if (ID_str[0] == '%') /* Input jid */
    {
        if(!(jid = atoi(ID_str + 1))) /* Jid is 0 or not a number */
        {
            sio_puts(tok->argv[0]);
            sio_puts(": argument must be a nonzero %%jobid\n");
            return;
        }
        target_job = getjobjid(job_list, jid);
        pid = target_job->pid;
        if(!target_job) /* Can not find the job*/
        {
            sio_puts("[");
            sio_putl(jid);
            sio_puts("]: job with this jid do not exist\n");
            return;
        }
    }
    else /* Input pid */
    {
        if(!(pid = atoi(ID_str))) /* Pid is 0 or not a number */
        {
            sio_puts(tok->argv[0]);
            sio_puts(": argument must be a nonzero PID\n");
            return;
        }
        target_job = getjobpid(job_list, pid);
        jid = pid2jid(pid);
        if(!target_job) /* Can not find the job*/
        {
            sio_puts("(");
            sio_putl(pid);
            sio_puts("): process with this pid do not exist\n");
            return;
        }
    }

    /* Handling job */
    if(target_job ->state == UNDEF) /* Job's state undefined */
    {
        sio_puts("error: trying to fg a process not exist\n");
        return;
    }
    if(target_job -> state == ST) /* Restart a stopped job */
        kill(-pid, SIGCONT);
    target_job->state = FG;
    while(fgpid(job_list)) /* Parent waits for foreground job to terminate */
        Sigsuspend(pprev);

    return;
}

/* execute_bg - execute build-in command bg job */
void execute_bg(struct cmdline_tokens *tok)
{
    /* Declare and initialize variables */
    char *ID_str = tok->argv[1];
    int jid, pid;
    struct job_t *target_job;

    if(tok->argc != 2) /* Invalid format */
    {
        sio_puts(tok->argv[0]);
        sio_puts(" please input one and only one ID argument\n");
        return;
    }

    /* Get target job by input id */
    if (ID_str[0] == '%') /* Input jid */
    {
        if(!(jid = atoi(ID_str + 1))) /* Jid is 0 or not a number */
        {
            sio_puts(tok->argv[0]);
            sio_puts(": argument must be a %%jobid\n");
            return;
        }
        target_job = getjobjid(job_list, jid);
        pid = target_job->pid;
        if(!target_job)
        {
            sio_puts("[");
            sio_putl(jid);
            sio_puts("]: job with this jid do not exist\n");
            return;
        }
    }
    else /* Input pid */
    {
        if(!(pid = atoi(ID_str)))
        {
            sio_puts(tok->argv[0]);
            sio_puts(": argument must be a PID\n");
            return;
        }
        target_job = getjobpid(job_list, pid);
        jid = pid2jid(pid);
        if(!target_job) /* Can not find the job*/
        {
            sio_puts("(");
            sio_putl(pid);
            sio_puts("): process with this pid do not exist\n");
            return;
        }
    }

    /* Handling job */
    if(target_job == UNDEF)
    {
        sio_puts("error: trying to bg a process not exist\n");
        return;
    }
    if(target_job -> state == ST) /* Restart a stopped job */
        kill(-pid, SIGCONT);
    target_job->state = BG;
    /* Print prompt message */
    sio_puts("[");
    sio_putl(target_job->jid);
    sio_puts("] (");
    sio_putl(target_job->pid);
    sio_puts(") ");
    sio_puts(target_job->cmdline);
    sio_puts("\n");

    return;
}


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];          /* holds local copy of command line */
    const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
    char *buf = array;                   /* ptr that traverses command line */
    char *next;                          /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to end of cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;                   /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
    } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void 
sigchld_handler(int sig) 
{
    /* Declare variables */
    int olderrno = errno, status;
    sigset_t mask_all, prev;
    pid_t pid;
    struct job_t *job;

    /* Initialize block sets */
    Sigfillset(&mask_all);

    /* Parent reaps zombie child */
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED))>0)
    {
        Sigprocmask(SIG_BLOCK, &mask_all, &prev); /* Block all signals */
        job = getjobpid(job_list, pid);

        if(WIFSTOPPED(status)) /* Child is stopped */
        {
            /* 
            * If child stopped by SIGTSTP from shell, then 
            * sigtstp_handler will deal with it. However, if 
            * it was stopped by a signal didn't caught by shell, 
            * then sigchld_handler should change the state 
            * and print message. If child's state did not 
            * change, it is because of a signal didn't caught
            * by shell. One example is trace 12, where the child
            * send itself SIGTSTP. In this case, handler should 
            * change the state and print prompt message.
            */
            if(job->state != ST) 
            {
                sio_puts("Job [");
                sio_putl(job->jid);
                sio_puts("] (");
                sio_putl(job->pid);
                sio_puts(") stopped by signal ");
                sio_putl(WSTOPSIG(status));
                sio_puts("\n");
                job->state = ST;
            }
        }
        else if (WIFSIGNALED(status)) /* Child terminated by a signal */
        {
            /* Print prompt message */
            sio_puts("Job ["  );
            sio_putl(job->jid);
            sio_puts("] (");
            sio_putl(job->pid);
            sio_puts(") terminated by signal ");
            sio_putl(WTERMSIG(status));
            sio_puts("\n");
            /* Delete job */
            deletejob(job_list, pid);
        }
        else if(WIFEXITED(status)) /* Child terminated normally */
            /* Just delete job */
            deletejob(job_list, pid);

        /* Unblock signals */
        Sigprocmask(SIG_SETMASK, &prev, NULL);
    }

    errno = olderrno;

    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void 
sigint_handler(int sig) 
{
    /* Declare and initialize variables */
    int olderrno = errno;
    sigset_t mask_all, prev;
    pid_t pid;
    pid = fgpid(job_list);
    Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev); /* Block all signals */

    if(pid) /* If foreground job exist */
        kill(-pid, sig);
    /* 
    * Since the shell will wait foreground job terminate,
    * so we just let sigchld_handler to delete jobs and
    * print message
    */
    Sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock signals */

    errno = olderrno;

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void 
sigtstp_handler(int sig) 
{
    /* Declare and initialize variables */
    int olderrno = errno, jid;
    sigset_t mask_all, prev;
    pid_t pid;
    struct job_t *job;
    pid = fgpid(job_list); /* Get foreground job's pid */
    jid = pid2jid(pid); /* Get foreground job's jid */
    job = getjobpid(job_list,pid); /* Get job */
    Sigfillset(&mask_all);
    Sigprocmask(SIG_BLOCK, &mask_all, &prev);
    
    if(pid) /* If foreground job exist */
    {
        /* 
        * The shell will wait foreground job terminate,
        * so we must print message and change the state
        * here. As for child stopped for other reasons,
        * we leave the job for sigchld_handler.
        */
        sio_puts("Job [");
        sio_putl(jid);
        sio_puts("] (");
        sio_putl(pid);
        sio_puts(") stopped by signal ");
        sio_putl(sig);
        sio_puts("\n");
        job->state = ST;
        /* Send signals */
        kill(-pid, sig);
        
    }
    Sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock signals */

    errno = olderrno;
    
    return;
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void 
sigquit_handler(int sig) 
{
    sio_error("Terminating after receipt of SIGQUIT signal\n");
}



/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void 
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int 
maxjid(struct job_t *job_list) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int 
deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int 
pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void 
listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE + 1];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/

/******************************
 * helper routines from csapp.c
 ******************************/
void Sigfillset(sigset_t *set)
{ 
    if (sigfillset(set) < 0)
	unix_error("Sigfillset error");
    return;
}

void Sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
	unix_error("Sigemptyset error");
    return;
}

void Sigaddset(sigset_t *set, int signum)
{
    if (sigaddset(set, signum) < 0)
	unix_error("Sigaddset error");
    return;
}

void Sigdelset(sigset_t *set, int signum)
{
    if (sigdelset(set, signum) < 0)
	unix_error("Sigdelset error");
    return;
}

int Sigsuspend(const sigset_t *set)
{
    int rc = sigsuspend(set); /* always returns -1 */
    if (errno != EINTR)
        unix_error("Sigsuspend error");
    return rc;
}

void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if (sigprocmask(how, set, oldset) < 0)
	unix_error("Sigprocmask error");
    return;
}

void Setpgid(pid_t pid, pid_t pgid) 
{
    int rc;
    if ((rc = setpgid(pid, pgid)) < 0)
	unix_error("Setpgid error");
    return;
}

void Execve(const char *filename, char *const argv[], char *const envp[]) 
{
    if (execve(filename, argv, envp) < 0)
	unix_error("Execve error");
}

pid_t Fork(void) 
{
    pid_t pid;

    if ((pid = fork()) < 0)
	unix_error("Fork error");
    return pid;
}

int Open(const char *pathname, int flags, mode_t mode) 
{
    int rc;

    if ((rc = open(pathname, flags, mode))  < 0)
	unix_error("Open error");
    return rc;
}

void Close(int fd) 
{
    int rc;

    if ((rc = close(fd)) < 0)
	unix_error("Close error");
}

void Sio_error(char s[])
{
    sio_error(s);
}

/**********************************
 * end helper routines from csapp.c
 **********************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void 
usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void 
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void 
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/* Private sio_functions */
/* sio_reverse - Reverse a string (from K&R) */
static void sio_reverse(char s[])
{
    int c, i, j;

    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

/* sio_ltoa - Convert long to base b string (from K&R) */
static void sio_ltoa(long v, char s[], int b) 
{
    int c, i = 0;
    
    do {  
        s[i++] = ((c = (v % b)) < 10)  ?  c + '0' : c - 10 + 'a';
    } while ((v /= b) > 0);
    s[i] = '\0';
    sio_reverse(s);
}

/* sio_strlen - Return length of string (from K&R) */
static size_t sio_strlen(char s[])
{
    int i = 0;

    while (s[i] != '\0')
        ++i;
    return i;
}

/* Public Sio functions */
ssize_t sio_puts(char s[]) /* Put string */
{
    return write(STDOUT_FILENO, s, sio_strlen(s));
}

ssize_t sio_putl(long v) /* Put long */
{
    char s[128];
    
    sio_ltoa(v, s, 10); /* Based on K&R itoa() */ 
    return sio_puts(s);
}

void sio_error(char s[]) /* Put error message and exit */
{
    sio_puts(s);
    _exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t 
*Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

