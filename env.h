/** \file env.h
  Prototypes for functions for setting and getting environment variables.
*/

#ifndef FISH_ENV_H
#define FISH_ENV_H

#include <wchar.h>
#include <map>

#include "util.h"
#include "io.h"
#include "common.h"

/* Flags that may be passed as the 'mode' in env_set / env_get_string */
enum
{
    /* Default mode */
    ENV_DEFAULT = 0,
    
    /** Flag for local (to the current block) variable */
    ENV_LOCAL = 1,
    
    /** Flag for exported (to commands) variable */
    ENV_EXPORT = 2,
    
    /** Flag for unexported variable */
    ENV_UNEXPORT = 16,
    
    /** Flag for global variable */
    ENV_GLOBAL = 4,
    
    /** Flag for variable update request from the user. All variable
       changes that are made directly by the user, such as those from the
       'set' builtin must have this flag set. */
    ENV_USER = 8,
    
    /** Flag for universal variable */
    ENV_UNIVERSAL = 32
};
typedef uint32_t env_mode_flags_t;

/**
   Error code for trying to alter read-only variable
*/
enum
{
    ENV_PERM = 1,
    ENV_SCOPE,
    ENV_INVALID
}
;

/* A struct of configuration directories, determined in main() that fish will optionally pass to env_init.
 */
struct config_paths_t
{
    wcstring data;      // e.g. /usr/local/share
    wcstring sysconf;   // e.g. /usr/local/etc
    wcstring doc;       // e.g. /usr/local/share/doc/fish
    wcstring bin;       // e.g. /usr/local/bin
};

/**
   Initialize environment variable data
*/
void env_init(const struct config_paths_t *paths = NULL);

/**
  Return the value of the variable with the specified name.  Returns 0
  if the key does not exist.  The returned string should not be
  modified or freed. The returned string is only guaranteed to be
  valid until the next call to env_get(), env_set(), env_push() or
  env_pop() takes place.
*/
//const wchar_t *env_get( const wchar_t *key );

class env_var_t : public wcstring
{
private:
    bool is_missing;
public:
    static env_var_t missing_var(void)
    {
        env_var_t result(L"");
        result.is_missing = true;
        return result;

    }

    env_var_t(const env_var_t &x) : wcstring(x), is_missing(x.is_missing) { }
    env_var_t(const wcstring & x) : wcstring(x), is_missing(false) { }
    env_var_t(const wchar_t *x) : wcstring(x), is_missing(false) { }
    env_var_t() : wcstring(L""), is_missing(false) { }

    bool missing(void) const
    {
        return is_missing;
    }

    bool missing_or_empty(void) const
    {
        return missing() || empty();
    }

    const wchar_t *c_str(void) const;

    env_var_t &operator=(const env_var_t &s)
    {
        is_missing = s.is_missing;
        wcstring::operator=(s);
        return *this;
    }

    bool operator==(const env_var_t &s) const
    {
        return is_missing == s.is_missing && static_cast<const wcstring &>(*this) == static_cast<const wcstring &>(s);
    }

    bool operator==(const wcstring &s) const
    {
        return ! is_missing && static_cast<const wcstring &>(*this) == s;
    }

    bool operator!=(const env_var_t &s) const
    {
        return !(*this == s);
    }

    bool operator!=(const wcstring &s) const
    {
        return !(*this == s);
    }

    bool operator==(const wchar_t *s) const
    {
        return ! is_missing && static_cast<const wcstring &>(*this) == s;
    }

    bool operator!=(const wchar_t *s) const
    {
        return !(*this == s);
    }
};

/* An environment_t is a base class for variable storage. The base provides a getter function only - no setting. */
class environment_t
{
    private:
    // No copying
    environment_t(const environment_t &);
    environment_t &operator=(const environment_t &);
    
    public:
    virtual env_var_t get(const wcstring &key, env_mode_flags_t mode = ENV_DEFAULT) const = 0;
    virtual wcstring_list_t get_names(env_mode_flags_t flags) const = 0;

    environment_t();
    virtual ~environment_t();
};

/* Class representing a function variable stack */
class env_node_t;

/* We use shared pointers to track nodes, because multiple stacks can reference the same node */
typedef shared_ptr<env_node_t> env_node_ref_t;

typedef unsigned export_generation_t;
#define EXPORT_GENERATION_INVALID ((export_generation_t)(-1))

class env_stack_t : public environment_t
{
    friend class parser_t;
    
    /** Bottom node on the function stack */
    const env_node_ref_t global;
    
    /** Top node on the function stack */
    env_node_ref_t top;
    
    /** The stack element that we must not pop */
    const env_node_ref_t boundary;
    
    /** Hackish: The last exit status. The parser sets this directly. */
    int exit_status;

    /** List of exported variables */
    null_terminated_array_t<char> export_array;
    
    /** Generation count for exported variable changes. A value of EXPORT_GENERATION_INVALID means it's stale. */
    export_generation_t changed_exported_generation;
    
    /** Child stacks */
    explicit env_stack_t(const env_stack_t &parent);
    
    env_node_t *get_node(const wcstring &key);
    
    /* Returns the next scope to search in order, respecting the new_scope flag, or NULL if we're done. */
    env_node_t *next_scope(env_node_t *scope);
    const env_node_t *next_scope(const env_node_t *scope) const;
    
    bool try_remove(env_node_t *n, const wcstring &key, int var_mode);
    bool local_scope_exports(env_node_t *n) const;
    void get_exported(const env_node_t *n, std::map<wcstring, wcstring> &h) const;
    void mark_changed_exported();
    bool has_changed_exported() const;
    
    public:
    env_stack_t();
    virtual ~env_stack_t();
    
    /**
       Set the value of the environment variable whose name matches key to val.

       \param key The key
       \param val The value
       \param mode The type of the variable. Can be any combination of ENV_GLOBAL, ENV_LOCAL, ENV_EXPORT and ENV_USER. If mode is zero, the current variable space is searched and the current mode is used. If no current variable with the same name is found, ENV_LOCAL is assumed.

       \returns 0 on sucess or an error code on failiure.

       The current error codes are:

       * ENV_PERM, can only be returned when setting as a user, e.g. ENV_USER is set. This means that the user tried to change a read-only variable.
       * ENV_SCOPE, the variable cannot be set in the given scope. This applies to readonly/electric variables set from the local or universal scopes, or set as exported.
       * ENV_INVALID, the variable value was invalid. This applies only to special variables.
    */
    int set(const wcstring &key, const wchar_t *val, env_mode_flags_t var_mode);
    
    /**
       Remove environment variable

       \param key The name of the variable to remove
       \param mode should be ENV_USER if this is a remove request from the user, 0 otherwise. If this is a user request, read-only variables can not be removed. The mode may also specify the scope of the variable that should be erased.

       \return zero if the variable existed, and non-zero if the variable did not exist
    */
    int remove(const wcstring &key, int var_mode);
    
    env_var_t get(const wcstring &key, env_mode_flags_t mode = ENV_DEFAULT) const;
    
    /**
       Returns true if the specified key exists. This can't be reliably done
       using env_get, since env_get returns null for 0-element arrays

       \param key The name of the variable to remove
       \param mode the scope to search in. All scopes are searched if unset
    */
    bool exist(const wchar_t *key, env_mode_flags_t mode) const;
    
    /** Push the variable stack. Used for implementing local variables for functions and for-loops. */
    void push(bool new_scope);
    
    /** Pop the variable stack. Used for implementing local variables for functions and for-loops. */
    void pop();
    
    /** Returns all variable names. */
    wcstring_list_t get_names(env_mode_flags_t flags) const;
    
    void update_export_array_if_necessary(bool recalc);
    const null_terminated_array_t<char> &get_export_array() const;
    
    /** Returns an array containing all exported variables in a format suitable for execv. */
    const char * const * env_export_arr(bool recalc);
    
    static const env_stack_t &empty();
};


/** Gets the variable with the specified name, or env_var_t::missing_var if it does not exist. */
env_var_t env_get_from_main(const wcstring &key);

env_var_t env_get_from_principal(const wcstring &key, env_mode_flags_t mode = ENV_DEFAULT);

/** Synchronizes all universal variable changes: writes everything out, reads stuff in */
void env_universal_barrier();

/** Update the PWD variable directory */
int env_set_pwd();

/* Returns the PWD with a terminating slash */
wcstring env_get_pwd_slash(const environment_t &vars);

class env_vars_snapshot_t : public environment_t
{
    std::map<wcstring, wcstring> vars;

public:
    env_vars_snapshot_t(const environment_t &env, const wchar_t * const * keys);

    env_var_t get(const wcstring &key, env_mode_flags_t mode = ENV_DEFAULT) const;
    wcstring_list_t get_names(env_mode_flags_t flags) const;

    // vars necessary for highlighting
    static const wchar_t * const highlighting_keys[];
};

extern bool g_log_forks;
extern int g_fork_count;

extern bool g_use_posix_spawn;

/**
 A variable entry. Stores the value of a variable and whether it
 should be exported.
 */
struct var_entry_t
{
    wcstring val; /**< The value of the variable */
    bool exportv; /**< Whether the variable should be exported */
    
    var_entry_t() : exportv(false) { }
};

typedef std::map<wcstring, var_entry_t> var_table_t;

#endif
