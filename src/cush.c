/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>
#include <time.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);

static char* custom_prompt = "\\! \\u@\\h in \\w";

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    //*com_num += 1; // the number of commands in the
                   // terminal should be improved
    int promptSize = strlen(custom_prompt); // determine how many characters are in the prompt
    char curr_char = * custom_prompt; // what commands we are available to choose
    time_t timer = time(NULL); // the time and date for the corresponding functions
    bool special = false; // what happen if you encounter backslash
    int count = 0; // record the the positions of characters in the custom prompt

    while (count < promptSize) {
        // If the backslash is already detected, we need to consider
        // several scenarios
        if (special) {
            switch(curr_char) {
            
                case 'u': 
                // print the user name
                printf("%s", getenv("USER"));
                break;

                case 'h':
                // print the hostname
                char host_name[33];
                gethostname(host_name, 32);
                host_name[32] = '\0';
                printf("%s", host_name);
                break;

                case 'w':
                // print the entire working directly path
                printf("%s", getenv("PWD"));
                break;

                case 'W':
                // print the current working directory
                printf("%s", basename(getenv("PWD")));
                break;
                
                case 'd':
                // display the date of the program
                struct tm tm1 = *localtime(&time);
                printf("%02d-%02d-%d", tm1.tm_mon + 1, tm1.tm_mday, tm1.tm_year + 10);
                break;
                
                case 'T':
                // display the current time of the program
                struct tm tm2 = *localtime(&time);
                printf("%02d-%02d", tm2.tm_hour, tm2.tm_min);
                break;

                case 'n':
                // print the new line character
                printf("\n");
                break;

                case 'c':
                // print the cash as efficiently as possible
                printf("cush");
                break;
                
                default:
                // we need to deal with the scenario that 
                // the curr_char, or slash, is not a special
                // character
                printf("\\");
                printc("%c", curr_char);
                break;
            }
            special = false;
        }
        else if (curr_char == "\\") {
            // If we encounter a normal situation,
            // we will be ready to determine whether
            // or not the latter symbol is a special character.
            special = true;
        }
        else {
            // We may print the current character.
            printf("%c", curr_char);
        }
        count++; // The position of the character increments by 1.
        
        curr_char = *(custom_prompt + count);
    }
    return strdup("");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
    int pid; // The pid in the job control
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

// an array used to store the jobs
static int stopped_job[MAXJOBS];

// a variable that can be set initially
static int stopped_job_num = 0;



static void added_stopped_job(int jid) 
{
    stopped_job[stopped_job_num] = jid;
    stopped_job_num++;
}

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);

    job -> pid = 0;
    if (pipe -> bg_job) {
        job -> status = BACKGROUND;
    }

    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic 
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}



static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */
    if (pid > 0) {
        
        struct job *job = NULL; // We are available to set a struct type that is
                                // NULL initially

        // We can take advantage of methods in list.h to solve
        // the problem efficiently.
        
        for (struct list_elem* e = list_begin(&job_list); 
             e != list_end(&job_list); 
             e = list_next(&job_list)) {
            
            // We can access the element with respect to list_entry
            job = list_entry(e, struct job, elem);

            // If their pid correspond to one another, we may break
            // the for loop and use the current job.
            if (job -> pid == pid) {
                break;
            }
            
            // Otherwise, we want to set job into NULL.
            job = NULL;
        }

                 
        if (job == NULL) {
            // If the job does not have a corresponding pid as the parameter pid did,
            // we will receive a fatal error
            utils_fatal_error("Error. There are no current jobs received from the signal.");
        }
        else {
            
            if (WIFEXITED(status)) {
               // What happen if the program is exited.
               // We decrement the variable number_of_processes alive in the job.
               job -> num_processes_alive--;
            }
            else if (WIFSIGNALED(status)) {
               // What happen if the child process received a signal that is terminated.
               // We receive a number that terminates the signal.
               int terNum = WTERMSIG(status);

               // Later, the terNum can be divided into several scenarios: aborted, floating
               // pointer exception, killed, segmentation fault, and terminated.
               if (terNum == 6) {
                  // The program is aborted.
                  utils_error("aborted\n");
               }
               else if (terNum == 8) {
                  // The program encounters a floating point exception.
                  utils_error("floating point exception\n");
               }
               else if (terNum == 9) {
                  // a killed signal errors.
                  utils_error("killed\n");
               }
               else if (terNum == 11) {
                  // The segmentation fault is more likely to produce.
                  utils_error("segmentation fault\n");
               }
               else if (terNum == 15) {
                  // The process is terminated.
                  utils_error("terminated\n");
               }

               // The number of processes that is alive decreases.
               job -> num_processes_alive--;
            }
            else if (WIFSTOPPED(status)) {
               // What happen if the child process receives a signal that is stopped.
            
               // The status of the job must be set STOPPED in the enumerator job_status by default.
               job -> status = STOPPED;

               // Later, the status of the process may be modified automatically.
               int stpNum = WSTOPSIG(status); 

               // If the status in the struct job is in the foreground stage, we will wish to save the
               // terminal state.
               if (job -> status == FOREGROUND) {
                    // We save the state of the terminal by calling &job -> saved_tty_state
                    termstate_save(&job ->saved_tty_state);
                    print_job(job);
               }
               else {
                   // The background stage can be depicted in two possibiltiies: (1) stpNum == SIGTTOU 
                   // | stpNum == SIGTTIN. (2) we print the job.
                   if (stpNum == SIGTTOU | stpNum == SIGTTIN) {
                        // If the stpNum is stopped, the job's status will need the terminal.
                        job -> status = NEEDSTERMINAL;
                   }
                   else {
                        print_job(job);
                   }
                }
                added_stopped_job(job -> jid);
            }
            // After each instruction is completed, we need to return the 
            // terminal back to shell
            termstate_give_terminal_back_to_shell();
        }
    }
    else {
        utils_fatal_error("Error in waiting for signal from the child process");

    }
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        struct ast_command_line * cline = ast_parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        ast_command_line_print(cline);      /* Output a representation of
                                               the entered command line */

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        ast_command_line_free(cline);
    }
    return 0;
}
