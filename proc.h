/** \file proc.h

    Prototypes for utilities for keeping track of jobs, processes and subshells, as
  well as signal handling functions for tracking children. These
  functions do not themselves launch new processes, the exec library
  will call proc to create representations of the running jobs as
  needed.

*/

#ifndef FISH_PROC_H
#define FISH_PROC_H

#include <wchar.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <list>

#include "util.h"
#include "io.h"
#include "common.h"
#include "parse_tree.h"
#include "job.h"

/**
   The status code use when a command was not found
*/
#define STATUS_UNKNOWN_COMMAND 127

/**
   The status code use when an unknown error occured during execution of a command
*/
#define STATUS_NOT_EXECUTABLE 126

/**
   The status code use when an unknown error occured during execution of a command
*/
#define STATUS_EXEC_FAIL 125

/**
   The status code use when a wildcard had no matches
*/
#define STATUS_UNMATCHED_WILDCARD 124

/**
   The status code used for normal exit in a  builtin
*/
#define STATUS_BUILTIN_OK 0

/**
   The status code used for erroneous argument combinations in a builtin
*/
#define STATUS_BUILTIN_ERROR 1

/**
   Types of processes
*/
enum process_type_t
{
    /**
       A regular external command
    */
    EXTERNAL,
    /**
       A builtin command
    */
    INTERNAL_BUILTIN,
    /**
       A shellscript function
    */
    INTERNAL_FUNCTION,

    /** A block of commands, represented as a node */
    INTERNAL_BLOCK_NODE,

    /**
       The exec builtin
    */
    INTERNAL_EXEC
};

enum
{
    JOB_CONTROL_ALL,
    JOB_CONTROL_INTERACTIVE,
    JOB_CONTROL_NONE,
}
;

/**
  A structure representing a single fish process. Contains variables
  for tracking process state and the process argument
  list. Actually, a fish process can be either a regular external
  process, an internal builtin which may or may not spawn a fake IO
  process during execution, a shellscript function or a block of
  commands to be evaluated by calling eval. Lastly, this process can
  be the result of an exec command. The role of this process_t is
  determined by the type field, which can be one of EXTERNAL,
  INTERNAL_BUILTIN, INTERNAL_FUNCTION, INTERNAL_EXEC.

  The process_t contains information on how the process should be
  started, such as command name and arguments, as well as runtime
  information on the status of the actual physical process which
  represents it. Shellscript functions, builtins and blocks of code
  may all need to spawn an external process that handles the piping
  and redirecting of IO for them.

  If the process is of type EXTERNAL or INTERNAL_EXEC, argv is the
  argument array and actual_cmd is the absolute path of the command
  to execute.

  If the process is of type INTERNAL_BUILTIN, argv is the argument
  vector, and argv[0] is the name of the builtin command.

  If the process is of type INTERNAL_FUNCTION, argv is the argument
  vector, and argv[0] is the name of the shellscript function.

*/
class process_t
{
private:

    null_terminated_array_t<wchar_t> argv_array;

    /* narrow copy of argv0 so we don't have to convert after fork */
    narrow_string_rep_t argv0_narrow;

    io_chain_t process_io_chain;

    /* No copying */
    process_t(const process_t &rhs);
    void operator=(const process_t &rhs);

public:

    process_t();
    ~process_t();

    /** Type of process. */
    enum process_type_t type;

    /* For internal block processes only, the node offset of the block */
    node_offset_t internal_block_node;

    /** Sets argv */
    void set_argv(const wcstring_list_t &argv)
    {
        argv_array.set(argv);
        argv0_narrow.set(argv.empty() ? L"" : argv[0]);
    }

    /** Returns argv */
    const wchar_t * const *get_argv(void) const
    {
        return argv_array.get();
    }
    const null_terminated_array_t<wchar_t> &get_argv_array(void) const
    {
        return argv_array;
    }

    /** Returns argv[idx] */
    const wchar_t *argv(size_t idx) const
    {
        const wchar_t * const *argv = argv_array.get();
        assert(argv != NULL);
        return argv[idx];
    }

    /** Returns argv[0], or NULL */
    const wchar_t *argv0(void) const
    {
        const wchar_t * const *argv = argv_array.get();
        return argv ? argv[0] : NULL;
    }

    /** Returns argv[0] as a char * */
    const char *argv0_cstr(void) const
    {
        return argv0_narrow.get();
    }

    /* IO chain getter and setter */
    const io_chain_t &io_chain() const
    {
        return process_io_chain;
    }

    void set_io_chain(const io_chain_t &chain)
    {
        this->process_io_chain = chain;
    }

    /** actual command to pass to exec in case of EXTERNAL or INTERNAL_EXEC. */
    wcstring actual_cmd;

    /** process ID */
    pid_t pid;
    
    /** Emulated process. Owned pointer or NULL. */
    emulated_process_t *eproc;

    /** File descriptor that pipe output should bind to */
    int pipe_write_fd;

    /** File descriptor that the _next_ process pipe input should bind to */
    int pipe_read_fd;

    /** true if process has completed */
    volatile int completed;

    /** true if process has stopped */
    volatile int stopped;

    /** reported status value */
    volatile int status;

    /** Special flag to tell the evaluation function for count to print the help information */
    int count_help_magic;

    /** Next process in pipeline. We own this and we are responsible for deleting it. */
    process_t *next;
#ifdef HAVE__PROC_SELF_STAT
    /** Last time of cpu time check */
    struct timeval last_time;
    /** Number of jiffies spent in process at last cpu time check */
    unsigned long last_jiffies;
#endif
};

/**
  Constants for the flag variable in the job struct
*/
enum
{
    /** Whether the user has been told about stopped job */
    JOB_NOTIFIED = 1 << 0,

    /** Whether this job is in the foreground */
    JOB_FOREGROUND = 1 << 1,

    /**
    Whether the specified job is completely constructed,
    i.e. completely parsed, and every process in the job has been
    forked, etc.
    */
    JOB_CONSTRUCTED = 1 << 2,

    /** Whether the specified job is a part of a subshell, event handler or some other form of special job that should not be reported */
    JOB_SKIP_NOTIFICATION = 1 << 3,

    /** Whether the exit status should be negated. This flag can only be set by the not builtin. */
    JOB_NEGATE = 1 << 4,

    /** Whether the job is under job control  */
    JOB_CONTROL = 1 << 5,

    /** Whether the job wants to own the terminal when in the foreground  */
    JOB_TERMINAL = 1 << 6
};

/**
  Whether this shell is attached to the keyboard at all
*/
extern int is_interactive_session;

/**
  Whether we are a login shell
*/
extern int is_login;

/**
  Whether we are running an event handler
*/
extern int is_event;

/**
   Whether a universal variable barrier roundtrip has already been
   made for the currently executing command. Such a roundtrip only
   needs to be done once on a given command, unless a universal
   variable value is changed. Once this has been done, this variable
   is set to 1, so that no more roundtrips need to be done.

   Both setting it to one when it should be zero and the opposite may
   cause concurrency bugs.
*/
bool get_proc_had_barrier();
void set_proc_had_barrier(bool flag);

/**
   If this flag is set, fish will never fork or run execve. It is used
   to put fish into a syntax verifier mode where fish tries to validate
   the syntax of a file but doesn't actually do anything.
  */
extern int no_exec;

#ifdef HAVE__PROC_SELF_STAT
/**
   Use the procfs filesystem to look up how many jiffies of cpu time
   was used by this process. This function is only available on
   systems with the procfs file entry 'stat', i.e. Linux.
*/
unsigned long proc_get_jiffies(process_t *p);

/**
   Update process time usage for all processes by calling the
   proc_get_jiffies function for every process of every job.
*/
void proc_update_jiffies();

#endif

/**
   Perform a set of simple sanity checks on the job list. This
   includes making sure that only one job is in the foreground, that
   every process is in a valid state, etc.
*/
void proc_sanity_check();

/**
   Send a process/job exit event notification. This function is a
   convenience wrapper around event_fire().
*/
class parser_t;
void proc_fire_event(parser_t &parser, const wchar_t *msg, int type, pid_t pid, int status);

/**
   Initializations
*/
void proc_init();

/**
   Format an exit status code as returned by e.g. wait into a fish exit code number as accepted by proc_set_last_status.
 */
int proc_format_status(int status);

#endif
