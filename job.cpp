/** \file job.cpp

Utilities for keeping track of jobs.
*/


#include "config.h"
#include "job.h"
#include "parser.h"
#include "iothread.h"


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

job_store_t::job_store_t() : needs_waitpid_gen_count(0), waitpid_thread_running(false)
{
    VOMIT_ON_FAILURE(pthread_mutex_init(&lock, NULL));
    VOMIT_ON_FAILURE(pthread_cond_init(&status_map_broadcaster, NULL));
}

job_store_t::~job_store_t()
{
    pthread_mutex_destroy(&lock);
    pthread_cond_destroy(&status_map_broadcaster);
}

job_store_t &job_store_t::global_store()
{
    static job_store_t global_job_store;
    return global_job_store;
}

static int background_do_wait_trampoline(job_store_t *store)
{
    return store->background_do_wait();
}

int job_store_t::background_do_wait()
{
    /* This lock stays locked with the exception of the waitpid() calls */
    int processes_reaped = 0;
    scoped_lock locker(lock);
    for (;;)
    {
        /* People should know we exist! */
        assert(this->waitpid_thread_running);
        
        /* Grab the current generation count */
        const uint32_t prewait_gen_count = this->needs_waitpid_gen_count;
        
        /* Unlock and then call waitpid */
        locker.unlock();
        
        int status = 1;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        const int err = (pid == -1 ? errno : 0);
        
        /* Lock again */
        locker.lock();
        
        /* Update data structures appropriately */
        if (pid >= 0)
        {
            status_map[pid] = status;
            processes_reaped++;
            
            // Announce our good news
            pthread_cond_broadcast(&status_map_broadcaster);
        }
        else if (err == ECHILD)
        {
            /* There are no child processes - we might be done! */
            if (prewait_gen_count == this->needs_waitpid_gen_count)
            {
                /* The client forked and then incremented the gen count. Because the gen count has not been incremented, we have seen all forks that occurred before this gen count. Therefore there are no more child processes. */
                break;
            }
            else
            {
                /* The gen count has been modified. Therefore another process has been forked. Go around again. */
            }
        }
        else if (err == EINTR)
        {
            /* Interrupted! */
            pthread_cond_broadcast(&status_map_broadcaster);
        }
    }
    
    /* We are exiting. Note that we are locked around waitpid_thread_running here. */
    waitpid_thread_running = false;
    
    return processes_reaped;
}

void job_store_t::note_needs_wait()
{
    scoped_lock locker(lock);
    
    // Increment needs_waitpid_gen_count. Its OK if it wraps to zero.
    needs_waitpid_gen_count++;
    
    if (! waitpid_thread_running)
    {
        waitpid_thread_running = true;
        iothread_perform<job_store_t>(background_do_wait_trampoline, NULL, this);
    }
}

int job_store_t::wait_for_job_in_parser(const parser_t &parser, pid_t *out_pid, int *out_status)
{
    const job_list_t &jobs = parser.job_list();

    int result_error = 0;
    pid_t result_pid = -1;
    int result_status = 0;
    
    scoped_lock locker(lock);
    while (result_pid == -1)
    {
        for (job_list_t::const_iterator iter = jobs.begin(); iter != jobs.end() && result_pid == -1; ++iter)
        {
            const job_t *j = *iter;
            for (const process_t *p = j->first_process; p != NULL && result_pid == -1; p=p->next)
            {
                if (p->pid > 0)
                {
                    pid_status_map_t::iterator where = status_map.find(p->pid);
                    if (where != status_map.end())
                    {
                        /* We found this pid in status_map. We acquire the value, so we remove it. */
                        result_pid = where->first;
                        result_status = where->second;
                        status_map.erase(where);
                    }
                }
            }
        }
        
        if (result_pid == -1)
        {
            // We did not get a PID. Wait for one to be added.
            // TODO: What if SIGHUP is delivered here? Should wait on a timer.
            pthread_cond_wait(&this->status_map_broadcaster, &this->lock);
        }
    }
    
    if (out_pid)
    {
        *out_pid = result_pid;
    }
    if (out_status)
    {
        *out_status = result_status;
    }

    return result_error;
}

pid_status_map_t job_store_t::acquire_statuses_for_jobs(const job_list_t &jobs)
{
    scoped_lock locker(lock);
    pid_status_map_t acquired_map;
    for (job_list_t::const_iterator iter = jobs.begin(); iter != jobs.end(); ++iter)
    {
        const job_t *j = *iter;
        for (const process_t *p = j->first_process; p; p=p->next)
        {
            if (p->pid > 0)
            {
                pid_status_map_t::iterator where = status_map.find(p->pid);
                if (where != status_map.end())
                {
                    /* We found this pid in status_map. Transfer the value from status_map to result. */
                    acquired_map.insert(*where);
                    status_map.erase(where);
                }
            }
        }
    }
    return acquired_map;
}
