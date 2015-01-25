#ifndef FISH_IO_H
#define FISH_IO_H

#include "common.h"
#include <vector>

/**
   Describes what type of IO operation an io_data_t represents
*/
enum io_mode_t
{
    IO_FILE, IO_PIPE, IO_FD, IO_BUFFER, IO_CLOSE
};

/** Represents an FD redirection */
class io_data_t
{
private:
    /* No assignment or copying allowed */
    io_data_t(const io_data_t &rhs);
    void operator=(const io_data_t &rhs);

protected:
    io_data_t(io_mode_t m, int f) :
        io_mode(m),
        fd(f)
    {
    }

public:
    /** Type of redirect */
    const io_mode_t io_mode;
    /** FD to redirect */
    const int fd;

    virtual void print() const = 0;
    virtual ~io_data_t() = 0;
};

class io_close_t : public io_data_t
{
public:
    io_close_t(int f) :
        io_data_t(IO_CLOSE, f)
    {
    }

    virtual void print() const;
};

class io_fd_t : public io_data_t
{
protected:
    io_fd_t(enum io_mode_t mode, int f, int old, bool do_close) : io_data_t(mode, f), old_fd(old), user_supplied(false), should_close(do_close)
    {
    }
    
public:
    /** fd to redirect specified fd to. For example, in 2>&1, old_fd is 1, and io_data_t::fd is 2 */
    const int old_fd;
    
    /** Whether this redirection was supplied by a script. For example, 'cmd <&3' would have user_supplied set to true. But a redirection that comes about through transmogrification would not. */
    const bool user_supplied;
    
    /** Whether we close this fd on destruction */
    const bool should_close;
    
    virtual void print() const;

    io_fd_t(int f, int old, bool us, bool do_close = false) :
        io_data_t(IO_FD, f),
        old_fd(old),
        user_supplied(us),
        should_close(do_close)
    {
    }
    
    
    ~io_fd_t();
};

class io_file_t : public io_data_t
{
public:
    /** Filename, malloc'd. This needs to be used after fork, so don't use wcstring here. */
    const char * const filename_cstr;
    /** file creation flags to send to open */
    const int flags;

    virtual void print() const;

    io_file_t(int f, const wcstring &fname, int fl = 0) :
        io_data_t(IO_FILE, f),
        filename_cstr(wcs2str(fname)),
        flags(fl)
    {
    }

    virtual ~io_file_t()
    {
        free((void *)filename_cstr);
    }
};

/* An io_pipe always closes its fd */
class io_pipe_t : public io_fd_t
{
public:
    /* In process b for `a | b`, target_fd is STDIN_FILENO and pipe_fd is the pipe */
    io_pipe_t(int target_fd, int pipe_fd) : io_fd_t(IO_PIPE, target_fd, pipe_fd, true /* should_close */)
    {
        assert(target_fd >= 0);
        assert(pipe_fd >= 0);
    }
    
    virtual void print() const;
};

class io_chain_t;
class io_buffer_t : public io_data_t
{
private:
    /** buffer to save output in */
    std::vector<char> out_buffer;
    
public:
    int pipe_fd[2];

    /* Constructor is private, use io_buffer_t::create() below */
    io_buffer_t(int f): io_data_t(IO_BUFFER, f)
    {
        pipe_fd[0] = -1;
        pipe_fd[1] = -1;
    }

public:
    virtual void print() const;

    virtual ~io_buffer_t();

    /** Function to append to the buffer */
    void out_buffer_append(const char *ptr, size_t count)
    {
        out_buffer.insert(out_buffer.end(), ptr, ptr + count);
    }

    /** Function to get a pointer to the buffer */
    char *out_buffer_ptr(void)
    {
        return out_buffer.empty() ? NULL : &out_buffer.at(0);
    }

    const char *out_buffer_ptr(void) const
    {
        return out_buffer.empty() ? NULL : &out_buffer.at(0);
    }

    /** Function to get the size of the buffer */
    size_t out_buffer_size(void) const
    {
        return out_buffer.size();
    }
    
    /* Ensures that the pipes do not conflict with any fd redirections in the chain */
    bool avoid_conflicts_with_io_chain(const io_chain_t &ios);

    /**
       Close output pipe, and read from input pipe until eof.
    */
    void read();

    /**
       Create a IO_BUFFER type io redirection, complete with a pipe and a
       vector<char> for output. The default file descriptor used is STDOUT_FILENO
       for buffering. 

       \param fd the fd that will be mapped in the child process, typically STDOUT_FILENO
       \param conflicts A set of IO redirections. The function ensures that any pipe it makes
              does not conflict with an fd redirection in this list.
    */
    static io_buffer_t *create(int fd, const io_chain_t &conflicts);
};

class io_chain_t : public std::vector<shared_ptr<io_data_t> >
{
public:
    io_chain_t();
    io_chain_t(const shared_ptr<io_data_t> &);

    void remove(const shared_ptr<const io_data_t> &element);
    void push_back(const shared_ptr<io_data_t> &element);
    void push_front(const shared_ptr<io_data_t> &element);
    void append(const io_chain_t &chain);

    shared_ptr<const io_data_t> get_io_for_fd(int fd) const;
    shared_ptr<io_data_t> get_io_for_fd(int fd);
};


/**
   Return the last io redirection in the chain for the specified file descriptor.
*/
shared_ptr<const io_data_t> io_chain_get(const io_chain_t &src, int fd);
shared_ptr<io_data_t> io_chain_get(io_chain_t &src, int fd);

/* Given a pair of fds, if an fd is used by the given io chain, duplicate that fd repeatedly until we find one that does not conflict, or we run out of fds. Returns the new fds by reference, closing the old ones. If we get an error, returns false (in which case both fds are closed and set to -1). */
bool pipe_avoid_conflicts_with_io_chain(int fds[2], const io_chain_t &ios);

/** Class representing the output that a builtin can generate */
class output_stream_t
{
private:
    // no copying
    output_stream_t(const output_stream_t &s);
    void operator=(const output_stream_t &s);
    
    wcstring buffer;
    
public:
    output_stream_t()
    {
    }
    
    void append(const wcstring &s)
    {
        this->buffer.append(s);
    }

    void append(const wchar_t *s)
    {
        this->buffer.append(s);
    }
    
    void append(const wchar_t *s, size_t amt)
    {
        this->buffer.append(s, amt);
    }
    
    void push_back(wchar_t c)
    {
        this->buffer.push_back(c);
    }
    
    void append_format(const wchar_t *format, ...)
    {
        va_list va;
        va_start(va, format);
        append_formatv(this->buffer, format, va);
        va_end(va);
    }
    
    static output_stream_t *new_buffered_stream()
    {
        return new output_stream_t();
    }
    
    const wcstring &get_buffer() const
    {
        // Temporary
        return buffer;
    }
    
    bool empty() const
    {
        // Temporary
        return buffer.empty();
    }
};

struct io_streams_t
{
    output_stream_t stdout_stream;
    output_stream_t stderr_stream;
    
    // fd representing stdin. This is not closed by the destructor.
    int stdin_fd;
    
    // Indicates whether stdout and stderr are redirected (e.g. to a file or piped)
    bool out_is_redirected;
    bool err_is_redirected;
    
    // Actual IO redirections. This is only used by the source builtin. Unowned.
    const io_chain_t *io_chain;
    
    io_streams_t() : stdin_fd(-1), out_is_redirected(false), err_is_redirected(false), io_chain(NULL)
    {
    }
};

/** Print debug information about the specified IO redirection chain to stderr. */
void io_print(const io_chain_t &chain);

wcstring resolve_if_relative(const wcstring &path, const wcstring &cwd);

/** Special working directory support, that allows for multiple notions of cwd. This is mutable. */
class working_directory_t
{
    wcstring cwd;
    int fd;
    
public:
    wcstring path() const;
    bool valid() const;
    ~working_directory_t();
    working_directory_t(const wcstring &path);
    
    /* Changes to a new path, which may be relative to this path */
    void change_to(const wcstring &path);
    
    /* Changes to a new path, which must be absolute. Takes ownership of the fd. */
    void change_to(int fd, const wcstring &path);

    /* If the given path is relative, resolves it against our path */
    wcstring resolve_if_relative(const wcstring &path) const;

    /* Opens a path. If the path is relative, it's made relative to this path. The returned fd is marked CLOEXEC. */
    int open_relative(const wcstring &path, int flags, mode_t mode = 0) const;
};


#endif
