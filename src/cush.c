/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
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
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);

extern char **environ;

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
    return strdup("> ");
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */

    /* Add additional fields here if needed. */
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

// an array used to store the jobs that are
// stopped
static int stopped_job[MAXJOBS];

// a variable that can be set initially to
// refer to the numer of jobs that are
// stopped
static int stopped_job_num = 0;

/*
 *
 * add the stopped job to the stopped job
 * array
 */
static void added_stopped_job(int jid)
{
    stopped_job[stopped_job_num] = jid;
    stopped_job_num++;
}

/*
 * remove the stopped job from the stopped
 * job array
 *
 */
static void remove_stopped_job(int jid)
{
    bool start = false; // I can use a variable named
                        // start of boolean type to
                        // resolve the problem.

    for (int i = 0; i < stopped_job_num; i++)
    {
        if (!start && jid == stopped_job[i])
        {
            // The job is detected
            start = true;
        }

        if (start)
        {
            // the position of the job that is not last
            if (i < stopped_job_num - 1)
            {
                // We need to move to the next position
                stopped_job[i] = stopped_job[i + 1];
            }

            // If this is the last position in the job array,
            // we must set it to zero.
            if (i == stopped_job_num - 1)
            {
                stopped_job[i] = 0;
            }
        }
    }

    if (start)
    {
        // reduce the number of jobs in the job array after
        // the execution of the array
        stopped_job_num--;
    }
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
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);

    job->jid = 0;
    if (pipe->bg_job)
    {
        job->status = BACKGROUND;
    }

    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
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
    switch (status)
    {
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
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
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

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
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

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
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
    if (pid > 0)
    {

        struct job *job = NULL; // We are available to set a struct type that is
                                // NULL initially

        // We can take advantage of methods in list.h to solve
        // the problem efficiently.

        int found = -1; // the initial value of job pid.

        for (struct list_elem *e = list_begin(&job_list);
             e != list_end(&job_list);
             e = list_next(e))
        {

            // We can access the element with respect to list_entry
            job = list_entry(e, struct job, elem);
            struct ast_command *com = NULL;
            // use a for loop to iterate every process in one job
            for (struct list_elem *p = list_begin(&job->pipe->commands);
                 p != list_end(&job->pipe->commands); p = list_next(p))
            {

                com = list_entry(p, struct ast_command, elem);
                // If their pid correspond to one another, we may break
                // the for loop and use the current job.

                if (com->pid == pid)
                {
                    found = job->jid;
                    break;
                }
            }

            if (found != -1)
            {
                break;
            }

            // Otherwise, we want to set job into NULL.
            job = NULL;
        }

        if (job == NULL)
        {
            // If the job does not have a corresponding pid as the parameter pid did,
            // we will receive a fatal error
            utils_fatal_error("Error There are no current jobs received from the signal.");
        }
        else
        {

            if (WIFEXITED(status))
            {
                // What happen if the program is exited.
                // We decrement the variable number_of_processes alive in the job.
                job->num_processes_alive--;
            }
            else if (WIFSIGNALED(status))
            {
                // What happen if the child process received a signal that is terminated.
                // We receive a number that terminates the signal.
                int terNum = WTERMSIG(status);

                // Later, the terNum can be divided into several scenarios: aborted, floating
                // pointer exception, killed, segmentation fault, and terminated.
                if (terNum == 6)
                {
                    // The program is aborted.
                    utils_error("aborted\n");
                }
                else if (terNum == 8)
                {
                    // The program encounters a floating point exception.
                    utils_error("floating point exception\n");
                }
                else if (terNum == 9)
                {
                    // a killed signal errors.
                    utils_error("killed\n");
                }
                else if (terNum == 11)
                {
                    // The segmentation fault is more likely to produce.
                    utils_error("segmentation fault\n");
                }
                else if (terNum == 15)
                {
                    // The process is terminated.
                    utils_error("terminated\n");
                }

                // The number of processes that is alive decreases.
                job->num_processes_alive--;
            }
            else if (WIFSTOPPED(status))
            {
                // What happen if the child process receives a signal that is stopped.

                // The status of the job must be set STOPPED in the enumerator job_status by default.
                job->status = STOPPED;

                // Later, the status of the process may be modified automatically.
                int stpNum = WSTOPSIG(status);

                // If the status in the struct job is in the foreground stage, we will wish to save the
                // terminal state.
                if (job->status == FOREGROUND)
                {
                    // We save the state of the terminal by calling &job -> saved_tty_state
                    termstate_save(&job->saved_tty_state);
                    print_job(job);
                }
                else
                {
                    // The background stage can be depicted in two possibiltiies: (1) stpNum == SIGTTOU
                    // | stpNum == SIGTTIN. (2) we print the job.
                    if (stpNum == SIGTTOU || stpNum == SIGTTIN)
                    {
                        // If the stpNum is stopped, the job's status will need the terminal.
                        job->status = NEEDSTERMINAL;
                    }
                    else
                    {
                        print_job(job);
                    }
                }
                added_stopped_job(job->jid);
            }
            // After each instruction is completed, we need to return the
            // terminal back to shell
            termstate_give_terminal_back_to_shell();
        }
    }
    else
    {
        utils_fatal_error("Error in waiting for signal from the child process");
    }
}

static void execute(struct ast_pipeline *currpipeline)
{

    // We would like to add jobs to the current pipeline
    struct job *job = add_job(currpipeline);

    // Right now, we should make some pipes, a technique used to
    // maintain the connection between two proceses.
    int size = list_size(&currpipeline->commands);
    int pipes[size][2];
    for (int i = 1; i < size + 1; i++)
    {
        pipe2(pipes[i], O_CLOEXEC);
    }

 /*   // The input file descriptor is currently passing into
    // pipeline.
    int input_fd = -1;
    while (currpipeline->iored_input != NULL)
    {
        // read the input file descriptor
        input_fd = open(currpipeline->iored_input, O_RDONLY);
    }

    // The output file descriptor is currently passing into
    // the pipeline.
    int output_fd = -1;
    while (currpipeline->iored_output != NULL)
    {

        if (currpipeline->append_to_output)
        {
            // the file descriptor can be written if appended successfully.
            output_fd = open(currpipeline->iored_output, O_WRONLY | O_CREAT | O_APPEND, 0750);
        }
        else
        {
            // the file descriptor can be written if it is failed to append.
            output_fd = open(currpipeline->iored_output, O_WRONLY | O_CREAT, 0750);
        }
    }
*/
//    int cmdnum = 0;
//    int pid = 0;

    signal_block(SIGCHLD);
    int success = -1;
    for (struct list_elem *e = list_begin(&currpipeline->commands); e != list_end(&currpipeline->commands);
         e = list_next(e))
    {
        job->num_processes_alive++;
        pid_t child; // a child is established.
        posix_spawn_file_actions_t file_actions;
        posix_spawn_file_actions_init(&file_actions);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);

        struct ast_command *command = list_entry(e, struct ast_command, elem);

        success = posix_spawnp(&child, command->argv[0], &file_actions, &attr, command->argv, environ);
        if (success != 0) {
            fprintf(stderr, "cush: %s: command not found\n", command->argv[0]);
            break;
        }
        command->pid = child;
    }
    if (success == 0) {
        wait_for_job(job);
    }
    if (job->status == FOREGROUND) {
        delete_job(job);
    }
}

static void getPath()
{
    int size = 200;
    char *buff = calloc(size + 1, sizeof(char));
    getcwd(buff, 200);
    fprintf(stdout, "cush in %s ", buff);
    free(buff);
}

int main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    // We would like to determine whether or not
    // the option is available to be used.
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

        /* Do not output a prompt unless shell's stdin is a terminal */

        getPath();
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        if (cmdline == NULL) /* User typed EOF */
            break;

        struct ast_command_line *cline = ast_parse_command_line(cmdline); // We would like to parse
                                                                          // each job, where
                                                                          // job contains multiple
                                                                          // pipelines.
        free(cmdline);                                                    // We would like to
        if (cline == NULL)                                                /* Error in command line */
            // If something goes wrong with pipeline, what are we supposed
            // to do
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            // If the command line does not contain pipelines, we
            // will be ready to free it.
            ast_command_line_free(cline);
            continue;
        }
        ast_command_line_print(cline); /* Output a representation of
                                          the entered command line */
        // We may focus on each pipeline
        for (struct list_elem *e = list_begin(&cline->pipes);
             e != list_end(&cline->pipes);
             e = list_next(e))
        {
            // We deal with pipe one-by-one.
            struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
            execute(pipe);
        }

        

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        //ast_command_line_free(cline);
        free(cline);
    }
    return 0;
}
