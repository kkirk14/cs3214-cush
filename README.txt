Student Information
------------------------------------------------
Kevin Kirk (kkirk26)
Ryan Knight (rknight2020)


How to Execute the Shell
------------------------------------------------
Our cush is just a normal executable linux program and should be invoked as 
such. Specifying the -h option prints a usage message. Calling the program 
with no options will run the shell.


Important Notes
------------------------------------------------
All cush code is contained in the cush.c file. The python tests for our extra
builtins are provided in the /src/custom folder.


Description of Base Functionallity
------------------------------------------------
Our cush allows users to run commands from a terminal and manage their 
execution as they run. Our shell uses the notion of a job (more info below) 
to manage separate programs as a single unit.
Commands are invoked just as they are in bash - by specifying their path 
(relative or absolute) or an executable filename that exists somewhere in the 
PATH, followed by their arguments.

Our shell maintains the idea of background and foreground jobs. When a command
is invoked normally, the shell is run in the foreground, meaning that the shell
will give it terminal ownership and wait for it to terminate or stop. When a 
command is invoked with an ampersand ([COMMAND] &), it is run in the
background, meaning that the shell will start it but will retain terminal
ownership and immediately return to the prompt.

The following builtin commands are provided for managing jobs:
 - jobs: Prints a list of all active jobs the shell is currently overseeing.
         For each job, the job ID, job status, and arguments are printed.
 - fg: Given a valid job ID that corresponds to either a background or stopped
       job, the shell resumes the job (if necessary) and stalls until either 
       all of its processes terminate or are stopped.
 - bg: Given a valid job ID that corresponds to a stopped job, the shell resumes
       its execution in the background and will immediately return to the 
       prompt.
 - kill: Given a valid job ID, sends a SIGKILL signal to all processes in the 
         job and waits for them to terminate.
 - stop: Given a valid background job ID, sends a SIGSTOP to all processes in
         the job and returns to the prompt.
 - ^C and ^Z: Running a job in the foreground gives it terminal ownership, so
              pressing ctrl+C and ctrl+Z at the terminal will send SIGINT and 
              SIGSTP to the job's process group. The shell will respond to the 
              resulting status changes and return to the prompt if all
              processes terminate/stop properly. When these are invoked when
              the shell is at the prompt, then it will respond to these 
              signals.


Description of Extended Functionallity
------------------------------------------------
Our shell allows users to redirect the stdin and stdout of programs to/from 
files and other commands. 
File redirection can be done like this:
 - "[COMMAND] < file" redirects file's contents into the command's stdin
 - "[COMMAND] > file" redirects command's stdout to file
 - "[COMMAND] >> file" appends command's output to the end of file

I/O can be passed between commands using pipes: "[COMMAND] | [COMMAND]"
will start each command concurrently with the stdout of the first command
redirected to the stdin of the second command (via Linux pipes). These pipes
can be chained together to create a pipeline of multiple commands:
"[COMMAND] | [COMMAND] | [COMMAND]". Each of these pipelines constitutes a 
single job. Each invididual command is spawned as a separate process, and the 
shell provides the notion of a job to allow users to manage these separate
processes as one unit using the commands described above. 

The user can input a semicolon between jobs and they will be run sequentially
as if they typed in the first command and then the next.


List of Additional Builtins Implemented
------------------------------------------------
cd - Allows you to change your current working directory. Will go to home directory 
if none is specified.

History - Can use the history command to view all commands used within the session. 
Also allows you to use up and down arrow keys to change current command to a previous 
command. There are other abbreviations accepted such as !n, !-n, !!, !string, and !?string 
using GNU History Library.
