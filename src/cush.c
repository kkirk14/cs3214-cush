
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

#include <fcntl.h>
#include <signal.h>
#include <errno.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include "../posix_spawn/spawn.h"
#include "readline/history.h"


static void handle_child_status(pid_t pid, int status);

HIST_ENTRY **the_history_list;


/**
 * usage
 * Prints a message to stdout describing how to invoke this program.
 */
static void usage(char *progname) {
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}



/**
 * build_prompt
 * Build a prompt 
 */
static char *build_prompt(void) {
    return strdup("cush> ");
}



/* job_status enum */
enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */

    /* I added this */
    TERMINATED      /* all processes have terminated */
};



/* process status enum
   I don't think we need a TERMINATED status: when a process is terminated
   it will just be removed from the procs array. */
enum proc_status {
    PSTAT_RUNNING,
    PSTAT_STOPPED
};



/**
 * process struct
 */
typedef struct process {
    
    /* pid: Process' pid. */
    pid_t pid;

    /* status: Is this process running or stopped? */
    enum proc_status status;

    /* the command used to spawn this process */
    struct ast_command *command;

} process_t;



/**
 * job struct
 */
struct job {

    /* Link element for jobs list. */
    struct list_elem elem;

    /* The pipeline of commands this job represents */
    struct ast_pipeline *pipe;

    /* Job id. */
    int jid;

    /* Job status. */ 
    enum job_status status;

    /* The number of processes that we know to be alive */
    int  num_processes_alive;

    /* The state of the terminal when this job was stopped after having been 
       in foreground */
    struct termios saved_tty_state;  

    /* Add additional fields here if needed.
       Anything below this point was added by us - don't touch anything above. */

    /* pgid: Process group ID. All processes in the job will share this pgid. */
    pid_t pgid;

    /* procs: Pointer to heap-allocated array of process_t structs. There will
              be one entry in this array for each alive process in the job.
              This will have to be adjusted (memmove) each time a process dies.
              num_processes_alive can be used as the size of this array. */
    process_t *procs;
};



/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)


/* job_list: Linked list that will hold all active jobs. Accessing any job w/o
             using jid2job will be done by iterating through this list. */
static struct list job_list;


/* jid2job: Big array containing a pointer to the job struct for each active 
            job. jid2job[n] will point to the job struct for job w/ jid n. */
static struct job *jid2job[MAXJOBS];



/**
 * get_job_from_jid
 * Return job corresponding to jid 
 */
static struct job *get_job_from_jid(int jid) {
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}



/** 
 * add_job
 * Mallocs memory for a new job struct, initializes it, and adds it to job_list 
 * and jid2job. 
 * This doesn't completely initialize the job struct: for each process in the 
 * pipeline, we need to adjust num_processes_alive and any other members we 
 * define.
 * Return Value: A pointer to the job struct created.
 */ 
static struct job *add_job(struct ast_pipeline *pipe) {
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
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



/**
 * delete_job
 * Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void delete_job(struct job *job) {
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}



/**
 * get_status
 * Return Value: A string representation of the given job status.
 */
static const char *get_status(enum job_status status) {
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



/**
 * sigchld_handler
 * 
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
static void sigchld_handler(int sig, siginfo_t *info, void *_ctxt) {

    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}



/**
 * wait_for_job
 * 
 * Wait for all processes in this job to complete, or for
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
static void wait_for_job(struct job *job) {

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



/**
 * all_procs_stopped
 * Helper method for handle_stopped_child
 * Return Value: Non-zero (true) if all processes in job->procs are stopped, 
 *               zero (false) otherwise (there is an actively running process). 
 */
static char all_procs_stopped(struct job *job) {
    for (int i = 0; i < job->num_processes_alive; i++) {
        if (job->procs[i].status == PSTAT_RUNNING) {
            // There is a running process, return false
            return 0;
        }
    }
    return 1;
}



/**
 * find_pid
 * Searches through all processes in all jobs to find the process with the 
 * given pid. Returns the job this process belongs to and sets the 
 * process_t * at out_proc to point to the process_t struct.
 */
static struct job *find_pid(pid_t pid, process_t **out_proc) {

    // foreach job
    for (struct list_elem *job_l_elem = list_begin(&job_list);
                 job_l_elem != list_end(&job_list);
                 job_l_elem = list_next(job_l_elem)) {
        
        struct job *job = list_entry(job_l_elem, struct job, elem);
        for (int i = 0; i < job->num_processes_alive; i++) {
            if (job->procs[i].pid == pid) { // Found it
                *out_proc = &job->procs[i];
                return job;
            }
        }
    }

    // Nothing was found
    *out_proc = NULL;
    return NULL;
}



/**
 * handle_stopped_child
 */
static void handle_stopped_child(pid_t pid, 
                                 int status,
                                 struct job *job,
                                 process_t *proc) {

    // Check if fg job stopped by terminal problems (send SIGCONT and return)
    if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN) {
        
        if (job->status == FOREGROUND) {
            termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
            kill(-1 * job->pgid, SIGCONT);
            return;
        }
    }

    // Update process status
    proc->status = PSTAT_STOPPED;

    // If all procs in the job are stopped, then this job is now in the 
    // STOPPED/NEEDSTERMINAL state (adjust job->saved_tty_state and 
    // job->status)
    if (all_procs_stopped(job)) {
        if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
            job->status = NEEDSTERMINAL;
        else
            job->status = STOPPED;
        termstate_save(&job->saved_tty_state);
        print_job(job);
    }
}



/**
 * handle_terminated_child
 */
static void handle_terminated_child(pid_t pid, 
                                    int status, 
                                    struct job *job, 
                                    process_t *proc) {

    // If child was terminated by a signal: print a representative message
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("%s\n", strsignal(sig));
        fflush(stdout);
    }

    // Decrement job->num_processes_alive and remove the proc
    // from job's procs array.
    int procs_i = proc - job->procs;
    memmove(proc,
            proc + 1,
            sizeof(process_t) * (job->num_processes_alive - procs_i - 1));
    job->num_processes_alive--;

    // If num_processes_alive == 0, update job status
    if (job->num_processes_alive == 0) {

        // If foreground job, save the shell's new good termstate
        if (job->status == FOREGROUND) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                termstate_sample();
            job->status = TERMINATED;
        }

        // If num_processes_alive == 0 and this isn't the fg job, 
        // remove the job from data structures
        else {
            free(job->procs);
            list_remove(&job->elem);
            delete_job(job);
        }
    }

}



/**
 * handle_child_status
 * 
 * This is the big method we need to implement. It will be called both when a 
 * SIGCHLD is received and by wait_for_job when waiting for a foreground job.
 */
static void handle_child_status(pid_t pid, int status) {

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

    // Find the struct job and process_t for pid
    process_t *proc;
    struct job *job = find_pid(pid, &proc);
    if (job == NULL) { // This should (hopefully) never happen
        fprintf(stderr, "Received child status for unrecognized pid %d\n", pid);
        fflush(stderr);
        exit(1);
    }

    // If stopped, call handle_stopped_child
    if (WIFSTOPPED(status)) {
        handle_stopped_child(pid, status, job, proc);
    }

    // If terminated, call handle_terminated_child
    else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        handle_terminated_child(pid, status, job, proc);
    }

}



/* macros since I can never remember which pipe end is read and which is write */
#define PIPE_READ 0
#define PIPE_WRITE 1



/**
 * close_pipe
 * Little helper method for closing pipes
 */
static void close_pipe(int pipe[]) {
    if (pipe[PIPE_READ] > 2) {
        close(pipe[PIPE_READ]);
    }
    if (pipe[PIPE_WRITE] > 2) {
        close(pipe[PIPE_WRITE]);
    }
}



/**
 * exit_builtin
 * Called by main when user types "exit". Sends SIGKILL to all job pgroups and 
 * reaps them (this can be done by putting them in fg and calling 
 * wait_for_job), then exits.
 */
static void exit_builtin(int in_fd, int out_fd) {

    // Kill all jobs
    for (struct list_elem *jobs_l_elem = list_begin(&job_list);
         jobs_l_elem != list_end(&job_list);
         jobs_l_elem = list_next(jobs_l_elem)) {

        struct job *job = list_entry(jobs_l_elem, struct job, elem);
        
        job->status = FOREGROUND;
        kill(-1 * job->pgid, SIGKILL);
        wait_for_job(job);
    }

    // Exit
    exit(0);
}



/**
 * jobs_builtin
 * Called by shell loop when user types "jobs". Prints a list showing status
 * and args info for each active job.
 */
static void jobs_builtin(int in_fd, int out_fd) {
    for (struct list_elem *jobs_l_elem = list_begin(&job_list);
         jobs_l_elem != list_end(&job_list);
         jobs_l_elem = list_next(jobs_l_elem)) {

        struct job *job = list_entry(jobs_l_elem, struct job, elem);
        print_job(job);
    }
}



/**
 * kill_builtin
 * Sends SIGKILL to all processes in the job with the given jid.
 */
static void kill_builtin(char **argv) {
    
    int jid = atoi(argv[1]);
    if (jid < 1) {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
    struct job *job = get_job_from_jid(jid);
    if (job) {
        job->status = FOREGROUND;
        kill(-1 * job->pgid, SIGKILL);
        wait_for_job(job);
        free(job->procs);
        list_remove(&job->elem);
        delete_job(job);
    }
    else {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
}



/**
 * bg_builtin
 * Sends a SIGCONT to all processes in the given job and sets its status
 * to BACKGROUND.
 */
static void bg_builtin(char **argv) {
    int jid = atoi(argv[1]);
    if (jid < 1) {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
    struct job *job = get_job_from_jid(jid);
    if (job) {
        job->status = BACKGROUND;
        kill(-1 * job->pgid, SIGCONT);
        printf("[%d] %d\n", jid, job->pgid);
        fflush(stdout);
    }
    else {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
}



/**
 * fg_builtin
 * Sends a SIGCONT to all processes in the given job, sets its status to 
 * FOREGROUND, gives it terminal ownership, and waits for its completion.
 */
static void fg_builtin(char **argv) {

    int jid = atoi(argv[1]);
    if (jid < 1) {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
    struct job *job = get_job_from_jid(jid);
    if (job) {
        job->status = FOREGROUND;
        termstate_give_terminal_to(&job->saved_tty_state, job->pgid);
        kill(-1 * job->pgid, SIGCONT);
        print_cmdline(job->pipe);
        printf("\n");
        fflush(stdout);
        wait_for_job(job);
        if (job->status == TERMINATED) {
            free(job->procs);
            list_remove(&job->elem);
            delete_job(job);
        }
    }
    else {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
}



/**
 * stop_builtin
 */
static void stop_builtin(char **argv) {
    int jid = atoi(argv[1]);
    if (jid < 1) {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
    struct job *job = get_job_from_jid(jid);
    if (job) {
        kill(-1 * job->pgid, SIGSTOP);
    }
    else {
        printf("%s %s: No such job\n", argv[0], argv[1]);
        fflush(stdout);
    }
}

/**
 * history_builtin
 */
static void history_builtin(char **argv) {
    HISTORY_STATE *curhist = history_get_history_state();
    for (int i = 1; i <= curhist->length; i++) {
        printf("%d %s\n", i, curhist->entries[i-1]->line);
    }
}

/**
 * cd_builtin
 */
static void cd_builtin(char **argv) {
    // If no path is specified, go to the home directory
    if (argv[1] == NULL || strcmp(argv[1], "") == 0) {
        argv[1] = getenv("HOME");  // Use the HOME environment variable
    }

    // Attempt to change directory
    if (chdir(argv[1]) != 0) {
        perror("cd");  // If chdir fails, print an error message
    }
}

/**
 * setup_file_actions
 * Initializes a posix_spawn_file_actions_t for the creation of the process
 * that will run command. All the flags for I/O redirection are set here.
 * Note: posix_spawn_file_actions_destroy needs to be called on the returned 
 *       struct after it's been used.
 */
static posix_spawn_file_actions_t setup_file_actions(struct ast_pipeline *pipeline,
                                                     struct ast_command *command, 
                                                     int prev_pipe[], 
                                                     int new_pipe[]) {
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    // if this is the first file, redirect stdin to the iored_input file
    if (&command->elem == list_begin(&pipeline->commands) && 
        pipeline->iored_input != NULL) {
        
        posix_spawn_file_actions_addopen(&file_actions, 
                                         STDIN_FILENO, 
                                         pipeline->iored_input, 
                                         O_RDONLY, 0000);
    }
    if (&command->elem == list_rbegin(&pipeline->commands) && 
        pipeline->iored_output != NULL) {
        
        int o_flags = O_WRONLY | O_CREAT;
        if (pipeline->append_to_output)
            o_flags |= O_APPEND;
        posix_spawn_file_actions_addopen(&file_actions, 
                                         STDOUT_FILENO, 
                                         pipeline->iored_output, 
                                         o_flags, 0666);
    }

    // dup2 the pipes
    if (prev_pipe[PIPE_READ] > 2) {
        posix_spawn_file_actions_adddup2(&file_actions, 
                                         prev_pipe[PIPE_READ], 
                                         STDIN_FILENO);
    }
    if (new_pipe[PIPE_WRITE] > 2) {
        posix_spawn_file_actions_adddup2(&file_actions,
                                         new_pipe[PIPE_WRITE],
                                         STDOUT_FILENO);
    }

    // close the extra pipe fds
    if (prev_pipe[PIPE_READ] > 2) {
        posix_spawn_file_actions_addclose(&file_actions, prev_pipe[PIPE_READ]);
    }
    if (prev_pipe[PIPE_WRITE] > 2) {
        posix_spawn_file_actions_addclose(&file_actions, prev_pipe[PIPE_WRITE]);
    }
    if (new_pipe[PIPE_READ] > 2) {
        posix_spawn_file_actions_addclose(&file_actions, new_pipe[PIPE_READ]);
    }
    if (new_pipe[PIPE_WRITE] > 2) {
        posix_spawn_file_actions_addclose(&file_actions, new_pipe[PIPE_WRITE]);
    }

    // dup2 stderr to stdout if necessary
    if (command->dup_stderr_to_stdout) {
        posix_spawn_file_actions_adddup2(&file_actions, 
                                         STDOUT_FILENO, 
                                         STDERR_FILENO);
    }

    return file_actions;
}



/**
 * setup_spawnattr
 * Creates and initializes a posix_spawnattr_t struct to be used in the 
 * creation of the processes that will run commands from pipeline.
 * Note: posix_spawnattr_destroy needs to be called on the returned 
 *       struct after it's been used.
 */
static posix_spawnattr_t setup_spawnattr(struct ast_pipeline *pipeline,
                                         pid_t pgrp) {
    
    posix_spawnattr_t spawnattr;
    posix_spawnattr_init(&spawnattr);

    // Set pgroup
    posix_spawnattr_setflags(&spawnattr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&spawnattr, pgrp);

    // Set controlling terminal
    if (!pipeline->bg_job) {
        posix_spawnattr_tcsetpgrp_np(&spawnattr, termstate_get_tty_fd());
    }

    return spawnattr;
}



// DELETE LATER AND MAKE STATIC AGAIN IN termstate_management.c
//extern int shell_pgrp;


/**
 * shell_loop
 * Called by main to run the shell's read/eval loop. This is where all the 
 * job creation magic happens.
 */
static void shell_loop(char *envp[]) {

    int rc;

    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        char *expanded = NULL;
    
        int result = history_expand(cmdline, &expanded);

        // Check if history expansion happened
        if (result >= 0) {
            free(cmdline);
            cmdline = expanded;
        } else {
            printf("Error in history expansion\n");
        }
        
        //Make sure to add history of commands
        add_history(cmdline);

        // Always free the expanded string
        //free(expanded);
        
        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free (cmdline);
        if (cline == NULL)                  /* Error in command line */
            continue;

        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        //ast_command_line_print(cline);      /* Output a representation of
        //                                       the entered command line */

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        //ast_command_line_free(cline);

        // We will be modifying the job structures: we need to block SIGCHLD.
        signal_block(SIGCHLD);

        // foreach pipeline (job)
        for (struct list_elem *pipeline_l_elem = list_begin(&cline->pipes); 
             pipeline_l_elem != list_end (&cline->pipes); 
             pipeline_l_elem = list_next(pipeline_l_elem)) {
            
            struct job *job = NULL;
            struct ast_pipeline *pipeline = list_entry(pipeline_l_elem, 
                                                       struct ast_pipeline, 
                                                       elem);
            int prev_pipe[] = {STDIN_FILENO, -1};
            pid_t pgrp = 0;

            // foreach command
            for (struct list_elem *command_l_elem = list_begin(&pipeline->commands);
                 command_l_elem != list_end(&pipeline->commands);
                 command_l_elem = list_next(command_l_elem)) {
                
                struct ast_command *command = list_entry(command_l_elem,
                                                         struct ast_command,
                                                         elem);

                // Create pipe
                // Note: We read from prev_pipe[PIPE_READ] and write to 
                //       new_pipe[PIPE_WRITE]
                int new_pipe[] = {-1, STDOUT_FILENO};
                if (command_l_elem != list_rbegin(&pipeline->commands)) {
                    rc = pipe2(new_pipe, 0);
                    if (rc < 0) {
                        perror("pipe2 error");
                        exit_builtin(STDIN_FILENO, STDOUT_FILENO);
                    }
                }

                // Builtin: exit
                if (strcmp(command->argv[0], "exit") == 0) {
                    exit_builtin(prev_pipe[PIPE_READ], STDOUT_FILENO);
                }

                else if (strcmp(command->argv[0], "jobs") == 0) {
                    jobs_builtin(prev_pipe[PIPE_READ], new_pipe[PIPE_WRITE]);
                }

                else if (strcmp(command->argv[0], "kill") == 0) {
                    kill_builtin(command->argv);
                }

                else if (strcmp(command->argv[0], "bg") == 0) {
                    bg_builtin(command->argv);
                }
                
                else if (strcmp(command->argv[0], "fg") == 0) {
                    fg_builtin(command->argv);
                }

                else if (strcmp(command->argv[0], "stop") == 0) {
                    stop_builtin(command->argv);
                }

                else if (strcmp(command->argv[0], "history") == 0) {
                    history_builtin(command->argv);
                }
                
                else if (strcmp(command->argv[0], "cd") == 0) {
                    cd_builtin(command->argv);
                }

                // Not a builtin: execute external program
                else {
                    
                    // setup posix spawn file actions and attr structs
                    posix_spawn_file_actions_t file_actions = 
                        setup_file_actions(pipeline,
                                           command, 
                                           prev_pipe, 
                                           new_pipe);
                    posix_spawnattr_t spawnattr = setup_spawnattr(pipeline,
                                                                  pgrp);

                    // call posix_spawn
                    pid_t proc_pid;
                    int rc = posix_spawnp(&proc_pid,
                                          command->argv[0],
                                          &file_actions,
                                          &spawnattr,
                                          command->argv, 
                                          envp);
                    if (rc == ENOENT) {
                        printf("%s: No such file or directory\n", command->argv[0]);
                        fflush(stdout);
                    }
                    else if (rc != 0) {
                        fprintf(stderr, "posix_spawnp error: %d\n", rc);
                        fflush(stderr);
                        exit_builtin(STDIN_FILENO, STDOUT_FILENO);
                    }
                    else { // Process created successfully
                        
                        if (pgrp == 0) {
                            pgrp = proc_pid;
                        }

                        // Create job struct if necessary
                        if (job == NULL) {
                            job = add_job(pipeline);
                            job->pgid = pgrp;
                            job->procs = malloc(sizeof(struct job) * 
                                                list_size(&pipeline->commands));
                            job->status = 
                                pipeline->bg_job ? BACKGROUND : FOREGROUND;
                            termstate_save(&job->saved_tty_state);
                        }

                        // Add process to the job struct
                        process_t *proc = &job->procs[job->num_processes_alive];
                        proc->pid = proc_pid;
                        proc->status = PSTAT_RUNNING;
                        proc->command = command;
                        job->num_processes_alive++;
                    }

                    // close prev_pipe
                    close_pipe(prev_pipe);

                    // new_pipe is now prev_pipe
                    memcpy(prev_pipe, new_pipe, sizeof(int) * 2);
                }
            } // foreach command

            // Wait for job in fg
            if (!pipeline->bg_job && job) {
                termstate_give_terminal_to(&job->saved_tty_state, pgrp);
                wait_for_job(job);
                // Delete job struct
                if (job->status == TERMINATED) {
                    free(job->procs);
                    list_remove(&job->elem);
                    delete_job(job);
                }
            }

            else if (pipeline->bg_job && job) {
                printf("[%d] %d\n", job->jid, job->pgid);
                fflush(stdout);
            }

        } // foreach pipeline (job)


        // Unblock SIGCHLD so that we can reap children while waiting 
        // at the prompt (damn that sounds awful...)
        signal_unblock(SIGCHLD);
        
        // We're gonna return to the prompt - reclaim the terminal
        termstate_give_terminal_back_to_shell();
    }
}



/**
 * main
 * Execution starts here.
 */
int main(int ac, char *av[], char *envp[]) {

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
    using_history();

    
    shell_loop(envp);

    return 0;
}
