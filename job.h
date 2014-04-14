/** \file job.h
  Support for running jobs
*/

#ifndef FISH_JOB_H
#define FISH_JOB_H

#include "common.h"
#include "io.h"
#include <list>

typedef int job_id_t;
job_id_t acquire_job_id(void);
void release_job_id(job_id_t jobid);

class job_manager_t
{
    
};

/**
    A struct represeting a job. A job is basically a pipeline of one
    or more processes and a couple of flags.
 */
class parser_t;
class process_t;
class job_t
{
    /**
        The original command which led to the creation of this
        job. It is used for displaying messages about job status
        on the terminal.
    */
    wcstring command_str;

    /* narrow copy so we don't have to convert after fork */
    narrow_string_rep_t command_narrow;

    /* The IO chain associated with the block */
    const io_chain_t block_io;

    /* No copying */
    job_t(const job_t &rhs);
    void operator=(const job_t &);

public:

    job_t(job_id_t jobid, const io_chain_t &bio);
    ~job_t();

    /** Returns whether the command is empty. */
    bool command_is_empty() const
    {
        return command_str.empty();
    }

    /** Returns the command as a wchar_t *. */
    const wchar_t *command_wcstr() const
    {
        return command_str.c_str();
    }

    /** Returns the command */
    const wcstring &command() const
    {
        return command_str;
    }

    /** Returns the command as a char *. */
    const char *command_cstr() const
    {
        return command_narrow.get();
    }

    /** Sets the command */
    void set_command(const wcstring &cmd)
    {
        command_str = cmd;
        command_narrow.set(cmd);
    }

    /**
        A linked list of all the processes in this job. We are responsible for deleting this when we are deallocated.
    */
    process_t *first_process;

    /**
        process group ID for the process group that this job is
        running in.
    */
    pid_t pgid;

    /**
        The saved terminal modes of this job. This needs to be
        saved so that we can restore the terminal to the same
        state after temporarily taking control over the terminal
        when a job stops.
    */
    struct termios tmodes;

    /**
       The job id of the job. This is a small integer that is a
       unique identifier of the job within this shell, and is
       used e.g. in process expansion.
    */
    const job_id_t job_id;

    /**
       Bitset containing information about the job. A combination of the JOB_* constants.
    */
    unsigned int flags;

    /* Returns the block IO redirections associated with the job. These are things like the IO redirections associated with the begin...end statement. */
    const io_chain_t &block_io_chain() const
    {
        return this->block_io;
    }

    /* Fetch all the IO redirections associated with the job */
    io_chain_t all_io_redirections() const;
};

typedef std::list<job_t *> job_list_t;


bool job_list_is_empty(void);

/** A class to aid iteration over jobs list.
    Note this is used from a signal handler, so it must be careful to not allocate memory.
*/
class job_iterator_t
{
    job_list_t * const job_list;
    job_list_t::iterator current, end;
public:

    void reset(void);

    job_t *next()
    {
        job_t *job = NULL;
        if (current != end)
        {
            job = *current;
            ++current;
        }
        return job;
    }

    job_iterator_t(job_list_t &jobs);
    job_iterator_t();
    size_t count() const;
};


/**
   Add the specified flag to the bitset of flags for the specified job
 */
void job_set_flag(job_t *j, unsigned int flag, int set);

/**
   Returns one if the specified flag is set in the specified job, 0 otherwise.
 */
int job_get_flag(const job_t *j, unsigned int flag);


/**
   The current job control mode.

   Must be one of JOB_CONTROL_ALL, JOB_CONTROL_INTERACTIVE and JOB_CONTROL_NONE
*/
extern int job_control_mode;


/**
   Remove the specified job
*/
void job_free(job_t* j);

/**
   Promotes a job to the front of the job list.
*/
void job_promote(job_t *job);

/**
  Return the job with the specified job id.
  If id is 0 or less, return the last job used.
*/
job_t *job_get(job_id_t id);

/**
  Return the job with the specified pid.
*/
job_t *job_get_from_pid(int pid);

/**
   Tests if the job is stopped
*/
int job_is_stopped(const job_t *j);

/**
   Tests if the job has completed, i.e. if the last process of the pipeline has ended.
*/
bool job_is_completed(const job_t *j);

/**
  Reassume a (possibly) stopped job. Put job j in the foreground.  If
  cont is true, restore the saved terminal modes and send the
  process group a SIGCONT signal to wake it up before we block.

  \param j The job
  \param cont Whether the function should wait for the job to complete before returning
*/
void job_continue(job_t *j, bool cont);

/**
   Notify the user about stopped or terminated jobs. Delete terminated
   jobs from the job list.

   \param interactive whether interactive jobs should be reaped as well
*/
int job_reap(bool interactive);

/**
   Signal handler for SIGCHLD. Mark any processes with relevant
   information.
*/
void job_handle_signal(int signal, siginfo_t *info, void *con);

/**
   Send the specified signal to all processes in the specified job.
*/
int job_signal(job_t *j, int signal);

/**
   Mark a process as failed to execute (and therefore completed)
*/
void job_mark_process_as_failed(const job_t *job, process_t *p);


#endif
