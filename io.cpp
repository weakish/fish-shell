/** \file io.c

Utilities for io redirection.

*/
#include "config.h"


#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <set>
#include <algorithm>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#include <unistd.h>
#include <fcntl.h>

#if HAVE_NCURSES_H
#include <ncurses.h>
#elif HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#else
#include <curses.h>
#endif

#if HAVE_TERM_H
#include <term.h>
#elif HAVE_NCURSES_TERM_H
#include <ncurses/term.h>
#endif

#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "exec.h"
#include "common.h"
#include "io.h"


io_data_t::~io_data_t()
{
}

void io_close_t::print() const
{
    fprintf(stderr, "close %d\n", fd);
}

void io_fd_t::print() const
{
    fprintf(stderr, "FD map %d -> %d\n", old_fd, fd);
}

io_fd_t::~io_fd_t()
{
    if (this->old_fd >= 0 && this->should_close)
    {
        exec_close(this->old_fd);
    }
}

void io_file_t::print() const
{
    fprintf(stderr, "file (%s)\n", filename_cstr);
}

void io_pipe_t::print() const
{
    fprintf(stderr, "pipe %d -> %d (input: %s)\n", this->old_fd, this->fd, this->fd == STDIN_FILENO ? "yes" : "no");
}

void io_buffer_t::print() const
{
    fprintf(stderr, "buffer %p (size %lu)\n", out_buffer_ptr(), (unsigned long) out_buffer_size());
}

void io_buffer_t::read()
{
    exec_close(pipe_fd[1]);

    if (io_mode == IO_BUFFER)
    {
        /*    if( fcntl( pipe_fd[0], F_SETFL, 0 ) )
            {
              wperror( L"fcntl" );
              return;
              }  */
        debug(4, L"io_buffer_t::read: blocking read on fd %d", pipe_fd[0]);
        while (1)
        {
            char b[4096];
            long l;
            l=read_blocked(pipe_fd[0], b, 4096);
            if (l==0)
            {
                break;
            }
            else if (l<0)
            {
                /*
                  exec_read_io_buffer is only called on jobs that have
                  exited, and will therefore never block. But a broken
                  pipe seems to cause some flags to reset, causing the
                  EOF flag to not be set. Therefore, EAGAIN is ignored
                  and we exit anyway.
                */
                if (errno != EAGAIN)
                {
                    debug(1,
                          _(L"An error occured while reading output from code block on file descriptor %d"),
                          pipe_fd[0]);
                    wperror(L"io_buffer_t::read");
                }

                break;
            }
            else
            {
                out_buffer_append(b, l);
            }
        }
    }
}

bool io_buffer_t::avoid_conflicts_with_io_chain(const io_chain_t &ios)
{
    bool result = pipe_avoid_conflicts_with_io_chain(this->pipe_fd, ios);
    if (! result)
    {
        wperror(L"dup");
    }
    return result;
}

io_buffer_t *io_buffer_t::create(int fd, const io_chain_t &conflicts)
{
    bool success = true;
    assert(fd >= 0);
    io_buffer_t *buffer_redirect = new io_buffer_t(fd);

    if (exec_pipe(buffer_redirect->pipe_fd) == -1)
    {
        debug(1, PIPE_ERROR);
        wperror(L"pipe");
        success = false;
    }
    else if (! buffer_redirect->avoid_conflicts_with_io_chain(conflicts))
    {
        // The above call closes the fds on error
        success = false;
    }
    else if (make_fd_nonblocking(buffer_redirect->pipe_fd[0]) != 0)
    {
        debug(1, PIPE_ERROR);
        wperror(L"fcntl");
        success = false;
    }

    if (! success)
    {
        delete buffer_redirect;
        buffer_redirect = NULL;
    }
    return buffer_redirect;
}

io_buffer_t::~io_buffer_t()
{
    if (pipe_fd[0] >= 0)
    {
        exec_close(pipe_fd[0]);
    }

    /*
      Dont free fd for writing. This should already be free'd before
      calling exec_read_io_buffer on the buffer
    */
}

void io_chain_t::remove(const shared_ptr<const io_data_t> &element)
{
    // See if you can guess why std::find doesn't work here
    for (io_chain_t::iterator iter = this->begin(); iter != this->end(); ++iter)
    {
        if (*iter == element)
        {
            this->erase(iter);
            break;
        }
    }
}

void io_chain_t::push_back(const shared_ptr<io_data_t> &element)
{
    // Ensure we never push back NULL
    assert(element.get() != NULL);
    std::vector<shared_ptr<io_data_t> >::push_back(element);
}

void io_chain_t::push_front(const shared_ptr<io_data_t> &element)
{
    assert(element.get() != NULL);
    this->insert(this->begin(), element);
}

void io_chain_t::append(const io_chain_t &chain)
{
    this->insert(this->end(), chain.begin(), chain.end());
}

void io_print(const io_chain_t &chain)
{
    if (chain.empty())
    {
        fprintf(stderr, "Empty chain %p\n", &chain);
        return;
    }

    fprintf(stderr, "Chain %p (%ld items):\n", &chain, (long)chain.size());
    for (size_t i=0; i < chain.size(); i++)
    {
        const shared_ptr<const io_data_t> &io = chain.at(i);
        if (io.get() == NULL)
        {
            fprintf(stderr, "\t(null)\n");
        }
        else
        {
            fprintf(stderr, "\t%lu: fd:%d, ", (unsigned long)i, io->fd);
            io->print();
        }
    }
}

/* If the given fd is used by the io chain, duplicates it repeatedly until an fd not used in the io chain is found, or we run out. If we return a new fd or an error, closes the old one. Any fd created is marked close-on-exec. Returns -1 on failure (in which case the given fd is still closed). */
static int move_fd_to_unused(int fd, const io_chain_t &io_chain)
{
    int new_fd = fd;
    if (fd >= 0 && io_chain.get_io_for_fd(fd).get() != NULL)
    {
        /* We have fd >= 0, and it's a conflict. dup it and recurse. Note that we recurse before anything is closed; this forces the kernel to give us a new one (or report fd exhaustion). */
        int tmp_fd;
        do
        {
            tmp_fd = dup(fd);
        } while (tmp_fd < 0 && errno == EINTR);
        
        assert(tmp_fd != fd);
        if (tmp_fd < 0)
        {
            /* Likely fd exhaustion. */
            new_fd = -1;
        }
        else
        {
            /* Ok, we have a new candidate fd. Recurse. If we get a valid fd, either it's the same as what we gave it, or it's a new fd and what we gave it has been closed. If we get a negative value, the fd also has been closed. */
            set_cloexec(tmp_fd);
            new_fd = move_fd_to_unused(tmp_fd, io_chain);
        }
        
        /* We're either returning a new fd or an error. In both cases, we promise to close the old one. */
        assert(new_fd != fd);
        int saved_errno = errno;
        exec_close(fd);
        errno = saved_errno;
    }
    return new_fd;
}

bool pipe_avoid_conflicts_with_io_chain(int fds[2], const io_chain_t &ios)
{
    bool success = true;
    for (int i=0; i < 2; i++)
    {
        fds[i] = move_fd_to_unused(fds[i], ios);
        if (fds[i] < 0)
        {
            success = false;
            break;
        }
    }
    
    /* If any fd failed, close all valid fds */
    if (! success)
    {
        int saved_errno = errno;
        for (int i=0; i < 2; i++)
        {
            if (fds[i] >= 0)
            {
                exec_close(fds[i]);
                fds[i] = -1;
            }
        }
        errno = saved_errno;
    }
    return success;
}

/* Return the last IO for the given fd */
shared_ptr<const io_data_t> io_chain_t::get_io_for_fd(int fd) const
{
    size_t idx = this->size();
    while (idx--)
    {
        const shared_ptr<const io_data_t> &data = this->at(idx);
        if (data->fd == fd)
        {
            return data;
        }
    }
    return shared_ptr<const io_data_t>();
}

shared_ptr<io_data_t> io_chain_t::get_io_for_fd(int fd)
{
    size_t idx = this->size();
    while (idx--)
    {
        const shared_ptr<io_data_t> &data = this->at(idx);
        if (data->fd == fd)
        {
            return data;
        }
    }
    return shared_ptr<io_data_t>();
}

/* The old function returned the last match, so we mimic that. */
shared_ptr<const io_data_t> io_chain_get(const io_chain_t &src, int fd)
{
    return src.get_io_for_fd(fd);
}

shared_ptr<io_data_t> io_chain_get(io_chain_t &src, int fd)
{
    return src.get_io_for_fd(fd);
}

io_chain_t::io_chain_t(const shared_ptr<io_data_t> &data) :
    std::vector<shared_ptr<io_data_t> >(1, data)
{

}

io_chain_t::io_chain_t() : std::vector<shared_ptr<io_data_t> >()
{
}

wcstring resolve_if_relative(const wcstring &path, const wcstring &cwd)
{
    if (path.empty())
    {
        return path;
    }
    else if (path.at(0) == L'/')
    {
        /* absolute */
        return path;
    }
    else
    {
        /* relative */
        wcstring full_path = cwd;
        append_path_component(full_path, path);
        return full_path;
    }
}

static pthread_mutex_t s_working_directory_lock = PTHREAD_MUTEX_INITIALIZER;;

wcstring working_directory_t::path() const
{
    scoped_lock locker(s_working_directory_lock);
    return this->cwd;
}

bool working_directory_t::valid() const
{
    scoped_lock locker(s_working_directory_lock);
    return this->fd >= 0;
}

int working_directory_t::open_relative(const wcstring &path, int flags, mode_t mode) const
{
    /* TODO: can use openat() here */
    if (path.empty())
    {
        errno = ENOENT;
        return -1;
    }
    return wopen_cloexec(this->resolve_if_relative(path), flags, mode);
}

void working_directory_t::change_to(const wcstring &path)
{
    assert(! path.empty());
    int new_fd = this->open_relative(path, O_RDONLY);
    this->change_to(new_fd, path);
}

wcstring working_directory_t::resolve_if_relative(const wcstring &path) const
{
    return ::resolve_if_relative(path, this->path());
}

void working_directory_t::change_to(int new_fd, const wcstring &path)
{
    scoped_lock locker(s_working_directory_lock);
    if (this->fd >= 0)
    {
        close(this->fd);
    }
    this->fd = new_fd;
    this->cwd = path;
}

working_directory_t::~working_directory_t()
{
    if (this->fd >= 0)
    {
        close(this->fd);
    }
}

working_directory_t::working_directory_t(const wcstring &path) : cwd(path), fd(wopen_cloexec(path, O_RDONLY))
{
}

