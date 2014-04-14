/** \file job.cpp

Utilities for keeping track of jobs.
*/


#include "config.h"
#include "job.h"
#include "parser.h"


/**
   Remove job from list of jobs
*/
static int job_remove(job_t *j)
{
    ASSERT_IS_MAIN_THREAD();
    return parser_t::principal_parser().job_remove(j);
}

void job_promote(job_t *job)
{
    ASSERT_IS_MAIN_THREAD();
    parser_t::principal_parser().job_promote(job);
}

/*
  Remove job from the job list and free all memory associated with
  it.
*/
void job_free(job_t * j)
{
    job_remove(j);
    delete j;
}


/* Basic thread safe job IDs. The vector consumed_job_ids has a true value wherever the job ID corresponding to that slot is in use. The job ID corresponding to slot 0 is 1. */
static pthread_mutex_t job_id_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<bool> consumed_job_ids;

job_id_t acquire_job_id(void)
{
    scoped_lock lock(job_id_lock);

    /* Find the index of the first 0 slot */
    std::vector<bool>::iterator slot = std::find(consumed_job_ids.begin(), consumed_job_ids.end(), false);
    if (slot != consumed_job_ids.end())
    {
        /* We found a slot. Note that slot 0 corresponds to job ID 1. */
        *slot = true;
        return (job_id_t)(slot - consumed_job_ids.begin() + 1);
    }
    else
    {
        /* We did not find a slot; create a new slot. The size of the vector is now the job ID (since it is one larger than the slot). */
        consumed_job_ids.push_back(true);
        return (job_id_t)consumed_job_ids.size();
    }
}

void release_job_id(job_id_t jid)
{
    assert(jid > 0);
    scoped_lock lock(job_id_lock);
    size_t slot = (size_t)(jid - 1), count = consumed_job_ids.size();

    /* Make sure this slot is within our vector and is currently set to consumed */
    assert(slot < count);
    assert(consumed_job_ids.at(slot) == true);

    /* Clear it and then resize the vector to eliminate unused trailing job IDs */
    consumed_job_ids.at(slot) = false;
    while (count--)
    {
        if (consumed_job_ids.at(count))
            break;
    }
    consumed_job_ids.resize(count + 1);
}

job_t *job_get(job_id_t id)
{
    ASSERT_IS_MAIN_THREAD();
    return parser_t::principal_parser().job_get(id);
}

job_t *job_get_from_pid(int pid)
{
    ASSERT_IS_MAIN_THREAD();
    return parser_t::principal_parser().job_get_from_pid(pid);
}


/*
   Return true if all processes in the job have stopped or completed.

   \param j the job to test
*/
int job_is_stopped(const job_t *j)
{
    process_t *p;

    for (p = j->first_process; p; p = p->next)
    {
        if (!p->completed && !p->stopped)
        {
            return 0;
        }
    }
    return 1;
}


/*
   Return true if the last processes in the job has completed.

   \param j the job to test
*/
bool job_is_completed(const job_t *j)
{
    assert(j->first_process != NULL);
    bool result = true;
    for (process_t *p = j->first_process; p != NULL; p = p->next)
    {
        if (! p->completed)
        {
            result = false;
            break;
        }
    }
    return result;
}

void job_set_flag(job_t *j, unsigned int flag, int set)
{
    if (set)
    {
        j->flags |= flag;
    }
    else
    {
        j->flags &= ~flag;
    }
}

int job_get_flag(const job_t *j, unsigned int flag)
{
    return !!(j->flags & flag);
}

int job_signal(job_t *j, int signal)
{
    pid_t my_pid = getpid();
    int res = 0;

    if (j->pgid != my_pid)
    {
        res = killpg(j->pgid, SIGHUP);
    }
    else
    {
        for (process_t *p = j->first_process; p; p=p->next)
        {
            if (! p->completed)
            {
                if (p->pid)
                {
                    if (kill(p->pid, SIGHUP))
                    {
                        res = -1;
                        break;
                    }
                }
            }
        }

    }

    return res;
}
