/** \file env.c
  Functions for setting and getting environment variables.
*/
#include "config.h"

#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <pwd.h>
#include <set>
#include <map>
#include <algorithm>

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

#if HAVE_LIBINTL_H
#include <libintl.h>
#endif

#include <errno.h>

#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "proc.h"
#include "common.h"
#include "env.h"
#include "sanity.h"
#include "expand.h"
#include "history.h"
#include "reader.h"
#include "parser.h"
#include "env_universal_common.h"
#include "input.h"
#include "event.h"
#include "path.h"

#include "complete.h"
#include "fish_version.h"

/** Value denoting a null string */
#define ENV_NULL L"\x1d"

/** Some configuration path environment variables */
#define FISH_DATADIR_VAR L"__fish_datadir"
#define FISH_SYSCONFDIR_VAR L"__fish_sysconfdir"
#define FISH_HELPDIR_VAR L"__fish_help_dir"
#define FISH_BIN_DIR L"__fish_bin_dir"

/* At init, we read all the environment variables from this array. */
extern char **environ;

/* This should be the same thing as \c environ, but it is possible only one of the two work... */
extern char **__environ;


bool g_log_forks = false;
bool g_use_posix_spawn = false; //will usually be set to true

// Big global lock that all environment modifications use
static pthread_mutex_t s_env_lock = PTHREAD_MUTEX_INITIALIZER;

/**
   Struct representing one level in the function variable stack
*/
class env_node_t
{
    private:
    /** Variable table */
    var_table_t env;
    
    public:
    /** Pointer to next level */
    const env_node_ref_t next;
    
    /**
      Does this node imply a new variable scope? If yes, all
      non-global variables below this one in the stack are
      invisible. If new_scope is set for the global variable node,
      the universe will explode.
    */
    const bool new_scope;
    
    /**
       Might this node contain any variables which are exported to subshells?
       This is probabilistic - if false, there are on exported variables, but if true, there may be one
    */
    bool exportv;


    env_node_t(bool is_new_scope, const env_node_ref_t &nxt) : next(nxt), new_scope(is_new_scope), exportv(false) { }

    /* Returns a pointer to the given entry if present, or NULL. */
    var_entry_t *find_entry(const wcstring &key);
    const var_entry_t *find_entry(const wcstring &key) const;
    
    /* Removes an entry */
    void remove_entry(const wcstring &key)
    {
        ASSERT_IS_LOCKED(s_env_lock);
        env.erase(key);
    }
    
    /* Returns a pointer to the given entry, creating it if necessary */
    var_entry_t *find_or_create_entry(const wcstring &key)
    {
        ASSERT_IS_LOCKED(s_env_lock);
        return &env[key];
    }
    
    const var_table_t &get_env() const
    {
        ASSERT_IS_LOCKED(s_env_lock);
        return env;
    }
    
    env_node_t *get_next()
    {
        return next.get();
    }
    
    const env_node_t *get_next() const
    {
        return next.get();
    }
};

env_stack_t::env_stack_t() : global(new env_node_t(false, env_node_ref_t())), top(global)
{
}

/* This creates a "child stack", not a copy. */
env_stack_t::env_stack_t(const env_stack_t &parent) : global(parent.global), top(parent.top), boundary(parent.top)
{
}

env_stack_t::~env_stack_t()
{
}

/* The stack associated with the main thread of execution */
static env_stack_t *main_stack()
{
    return &parser_t::principal_parser().vars();
}

/** Universal variables global instance. Initialized in env_init. */
static env_universal_t *s_universal_variables = NULL;

/* Getter for universal variables */
static env_universal_t *uvars()
{
    return s_universal_variables;
}

const env_stack_t &env_stack_t::empty()
{
    static const env_stack_t empty_result;
    return empty_result;
}

/* Helper class for storing constant strings, without needing to wrap them in a wcstring */

/* Comparer for const string set */
struct const_string_set_comparer
{
    bool operator()(const wchar_t *a, const wchar_t *b)
    {
        return wcscmp(a, b) < 0;
    }
};
typedef std::set<const wchar_t *, const_string_set_comparer> const_string_set_t;

/** Table of variables that may not be set using the set command. */
static const_string_set_t env_read_only;

static bool is_read_only(const wcstring &key)
{
    return env_read_only.find(key.c_str()) != env_read_only.end();
}

/**
   Table of variables whose value is dynamically calculated, such as umask, status, etc
*/
static const_string_set_t env_electric;

static bool is_electric(const wcstring &key)
{
    return env_electric.find(key.c_str()) != env_electric.end();
}

/**
   Exported variable array used by execv
*/
static null_terminated_array_t<char> export_array;

/**
   Flag for checking if we need to regenerate the exported variable
   array
*/
static bool has_changed_exported = true;
static void mark_changed_exported()
{
    has_changed_exported = true;
}

/**
   List of all locale variable names
*/
static const wchar_t * const locale_variable[] =
{
    L"LANG",
    L"LC_ALL",
    L"LC_COLLATE",
    L"LC_CTYPE",
    L"LC_MESSAGES",
    L"LC_MONETARY",
    L"LC_NUMERIC",
    L"LC_TIME",
    NULL
};


const var_entry_t *env_node_t::find_entry(const wcstring &key) const
{
    const var_entry_t *result = NULL;
    var_table_t::const_iterator where = env.find(key);
    if (where != env.end())
    {
        result = &where->second;
    }
    return result;
}

var_entry_t *env_node_t::find_entry(const wcstring &key)
{
    var_entry_t *result = NULL;
    var_table_t::iterator where = env.find(key);
    if (where != env.end())
    {
        result = &where->second;
    }
    return result;
}

env_node_t *env_stack_t::next_scope(env_node_t *scope)
{
    return scope->new_scope ? this->global.get() : scope->get_next();
}

const env_node_t *env_stack_t::next_scope(const env_node_t *scope) const
{
    return scope->new_scope ? this->global.get() : scope->get_next();
}

/**
   Return the current umask value.
*/
static mode_t get_umask()
{
    mode_t res;
    res = umask(0);
    umask(res);
    return res;
}

/** Checks if the specified variable is a locale variable */
static bool var_is_locale(const wcstring &key)
{
    for (size_t i=0; locale_variable[i]; i++)
    {
        if (key == locale_variable[i])
        {
            return true;
        }
    }
    return false;
}

/**
  Properly sets all locale information
*/
static void handle_locale()
{
    env_stack_t &vars = parser_t::principal_parser().vars();
    const env_var_t lc_all = vars.get(L"LC_ALL");
    const wcstring old_locale = wsetlocale(LC_MESSAGES, NULL);

    /*
      Array of locale constants corresponding to the local variable names defined in locale_variable
    */
    static const int cat[] =
    {
        0,
        LC_ALL,
        LC_COLLATE,
        LC_CTYPE,
        LC_MESSAGES,
        LC_MONETARY,
        LC_NUMERIC,
        LC_TIME
    }
    ;

    if (!lc_all.missing())
    {
        wsetlocale(LC_ALL, lc_all.c_str());
    }
    else
    {
        const env_var_t lang = vars.get(L"LANG");
        if (!lang.missing())
        {
            wsetlocale(LC_ALL, lang.c_str());
        }

        for (int i=2; locale_variable[i]; i++)
        {
            const env_var_t val = env_get_from_main(locale_variable[i]);

            if (!val.missing())
            {
                wsetlocale(cat[i], val.c_str());
            }
        }
    }

    const wcstring new_locale = wsetlocale(LC_MESSAGES, NULL);
    if (old_locale != new_locale)
    {

        /*
           Try to make change known to gettext. Both changing
           _nl_msg_cat_cntr and calling dcgettext might potentially
           tell some gettext implementation that the translation
           strings should be reloaded. We do both and hope for the
           best.
        */

        extern int _nl_msg_cat_cntr;
        _nl_msg_cat_cntr++;

        fish_dcgettext("fish", "Changing language to English", LC_MESSAGES);

        if (get_is_interactive())
        {
            debug(2, _(L"Changing language to English"));
        }
    }
}


/** React to modifying hte given variable */
static void react_to_variable_change(const wcstring &key)
{
    if (var_is_locale(key))
    {
        handle_locale();
    }
    else if (key == L"fish_term256" || key == L"fish_term24bit")
    {
        update_fish_color_support();
        reader_react_to_color_change();
    }
    else if (string_prefixes_string(L"fish_color_", key))
    {
        reader_react_to_color_change();
    }
}

/**
   Universal variable callback function. This function makes sure the
   proper events are triggered when an event occurs.
*/
static void universal_callback(fish_message_type_t type, const wchar_t *name, const wchar_t *val)
{
    const wchar_t *str = NULL;

    switch (type)
    {
        case SET:
        case SET_EXPORT:
        {
            str=L"SET";
            break;
        }
        
        case ERASE:
        {
            str=L"ERASE";
            break;
        }
        
        default:
            break;
    }

    if (str)
    {
        mark_changed_exported();

        event_t ev = event_t::variable_event(name);
        ev.arguments.push_back(L"VARIABLE");
        ev.arguments.push_back(str);
        ev.arguments.push_back(name);
        event_fire(&ev);
    }

    if (name)
        react_to_variable_change(name);
}

/**
   Make sure the PATH variable contains something
*/
static void setup_path()
{
    const env_var_t path = env_get_from_main(L"PATH");
    if (path.missing_or_empty())
    {
        const wchar_t *value = L"/usr/bin" ARRAY_SEP_STR L"/bin";
        env_set(L"PATH", value, ENV_GLOBAL | ENV_EXPORT);
    }
}

int env_set_pwd()
{
    wchar_t dir_path[4096];
    wchar_t *res = wgetcwd(dir_path, 4096);
    if (!res)
    {
        return 0;
    }
    env_set(L"PWD", dir_path, ENV_EXPORT | ENV_GLOBAL);
    return 1;
}

wcstring env_get_pwd_slash(const environment_t &vars)
{
    env_var_t pwd = vars.get(L"PWD");
    if (pwd.missing_or_empty())
    {
        return L"";
    }
    if (! string_suffixes_string(L"/", pwd))
    {
        pwd.push_back(L'/');
    }
    return pwd;
}

/* Here is the whitelist of variables that we colon-delimit, both incoming from the environment and outgoing back to it. This is deliberately very short - we don't want to add language-specific values like CLASSPATH. */
static bool variable_is_colon_delimited_array(const wcstring &str)
{
    return contains(str, L"PATH", L"MANPATH", L"CDPATH");
}

void env_init(const struct config_paths_t *paths /* or NULL */)
{
    env_stack_t * const vars = main_stack();
    
    /*
      env_read_only variables can not be altered directly by the user
    */
    const wchar_t * const ro_keys[] =
    {
        L"status",
        L"history",
        L"version",
        L"_",
        L"LINES",
        L"COLUMNS",
        L"PWD",
        //L"SHLVL", // will be inserted a bit lower down
        L"FISH_VERSION",
    };
    for (size_t i=0; i < sizeof ro_keys / sizeof *ro_keys; i++)
    {
        env_read_only.insert(ro_keys[i]);
    }

    /*
       Names of all dynamically calculated variables
       */
    env_electric.insert(L"history");
    env_electric.insert(L"status");
    env_electric.insert(L"umask");
    env_electric.insert(L"COLUMNS");
    env_electric.insert(L"LINES");

    /*
      Import environment variables
    */
    for (char **p = (environ ? environ : __environ); p && *p; p++)
    {
        const wcstring key_and_val = str2wcstring(*p); //like foo=bar
        size_t eql = key_and_val.find(L'=');
        if (eql == wcstring::npos)
        {
            // no equals found
            if (is_read_only(key_and_val) || is_electric(key_and_val)) continue;
            env_set(key_and_val, L"", ENV_EXPORT | ENV_GLOBAL);
        }
        else
        {
            wcstring key = key_and_val.substr(0, eql);
            if (is_read_only(key) || is_electric(key)) continue;
            wcstring val = key_and_val.substr(eql + 1);
            if (variable_is_colon_delimited_array(key))
            {
                std::replace(val.begin(), val.end(), L':', ARRAY_SEP);
            }

            env_set(key, val.c_str(), ENV_EXPORT | ENV_GLOBAL);
        }
    }

    /* Set the given paths in the environment, if we have any */
    if (paths != NULL)
    {
        env_set(FISH_DATADIR_VAR, paths->data.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_SYSCONFDIR_VAR, paths->sysconf.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_HELPDIR_VAR, paths->doc.c_str(), ENV_GLOBAL | ENV_EXPORT);
        env_set(FISH_BIN_DIR, paths->bin.c_str(), ENV_GLOBAL | ENV_EXPORT);
    }

    /*
      Set up the PATH variable
    */
    setup_path();

    /*
      Set up the USER variable
    */
    if (vars->get(L"USER").missing_or_empty())
    {
        const struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_name)
        {
            const wcstring uname = str2wcstring(pw->pw_name);
            vars->set(L"USER", uname.c_str(), ENV_GLOBAL | ENV_EXPORT);
        }
    }

    /*
      Set up the version variables
    */
    wcstring version = str2wcstring(get_fish_version());
    env_set(L"version", version.c_str(), ENV_GLOBAL);
    env_set(L"FISH_VERSION", version.c_str(), ENV_GLOBAL);

    /*
      Set up SHLVL variable
    */
    const env_var_t shlvl_str = vars->get(L"SHLVL");
    wcstring nshlvl_str = L"1";
    if (! shlvl_str.missing())
    {
        wchar_t *end;
        long shlvl_i = wcstol(shlvl_str.c_str(), &end, 10);
        while (iswspace(*end)) ++end; /* skip trailing whitespace */
        if (shlvl_i >= 0 && *end == '\0')
        {
            nshlvl_str = to_string<long>(shlvl_i + 1);
        }
    }
    env_set(L"SHLVL", nshlvl_str.c_str(), ENV_GLOBAL | ENV_EXPORT);
    env_read_only.insert(L"SHLVL");

    /* Set up the HOME variable */
    if (vars->get(L"HOME").missing_or_empty())
    {
        const env_var_t unam = vars->get(L"USER");
        char *unam_narrow = wcs2str(unam.c_str());
        struct passwd *pw = getpwnam(unam_narrow);
        if (pw->pw_dir != NULL)
        {
            const wcstring dir = str2wcstring(pw->pw_dir);
            env_set(L"HOME", dir.c_str(), ENV_GLOBAL | ENV_EXPORT);
        }
        free(unam_narrow);
    }

    /* Set PWD */
    env_set_pwd();

    /* Set up universal variables. The empty string means to use the deafult path. */
    assert(s_universal_variables == NULL);
    s_universal_variables = new env_universal_t(L"");
    s_universal_variables->load();

    /* Set g_log_forks */
    const env_var_t log_forks = vars->get(L"fish_log_forks");
    g_log_forks = ! log_forks.missing_or_empty() && from_string<bool>(log_forks);

    /* Set g_use_posix_spawn. Default to true. */
    const env_var_t use_posix_spawn = vars->get(L"fish_use_posix_spawn");
    g_use_posix_spawn = (use_posix_spawn.missing_or_empty() ? true : from_string<bool>(use_posix_spawn));

    /* Set fish_bind_mode to "default" */
    vars->set(FISH_BIND_MODE_VAR, DEFAULT_BIND_MODE, ENV_GLOBAL);

    /*
      Now that the global scope is fully initialized, add a toplevel local
      scope. This same local scope will persist throughout the lifetime of the
      fish process, and it will ensure that `set -l` commands run at the
      command-line don't affect the global scope.
    */
    vars->push(false);
}

env_node_t *env_stack_t::get_node(const wcstring &key)
{
    env_node_t *env = top.get();
    while (env != NULL)
    {
        if (env->find_entry(key) != NULL)
        {
            break;
        }
        
        env = this->next_scope(env);
    }
    return env;
}

int env_stack_t::set(const wcstring &key, const wchar_t *val, env_mode_flags_t var_mode)
{
    ASSERT_IS_MAIN_THREAD();
    
    scoped_lock locker(s_env_lock);
    bool has_changed_old = has_changed_exported;
    bool has_changed_new = false;
    int done=0;
    
    int is_universal = 0;
    
    if (val && contains(key, L"PWD", L"HOME"))
    {
        /* Canoncalize our path; if it changes, recurse and try again. */
        wcstring val_canonical = val;
        path_make_canonical(val_canonical);
        if (val != val_canonical)
        {
            return this->set(key, val_canonical.c_str(), var_mode);
        }
    }

    if ((var_mode & (ENV_LOCAL | ENV_UNIVERSAL)) && (is_read_only(key) || is_electric(key)))
    {
        return ENV_SCOPE;
    }
    if ((var_mode & ENV_EXPORT) && is_electric(key))
    {
        return ENV_SCOPE;
    }

    if ((var_mode & ENV_USER) && is_read_only(key))
    {
        return ENV_PERM;
    }
    
    if (key == L"umask")
    {
        wchar_t *end;
        
        /*
         Set the new umask
         */
        if (val && wcslen(val))
        {
            errno=0;
            long mask = wcstol(val, &end, 8);
            
            if (!errno && (!*end) && (mask <= 0777) && (mask >= 0))
            {
                umask(mask);
                /* Do not actually create a umask variable, on env_get, it will be calculated dynamically */
                return 0;
            }
        }

        return ENV_INVALID;
    }
    
    /*
     Zero element arrays are internaly not coded as null but as this
     placeholder string
     */
    if (!val)
    {
        val = ENV_NULL;
    }
    
    if (var_mode & ENV_UNIVERSAL)
    {
        const bool old_export = uvars() && uvars()->get_export(key);
        bool new_export;
        if (var_mode & ENV_EXPORT)
        {
            // export
            new_export = true;
        }
        else if (var_mode & ENV_UNEXPORT)
        {
            // unexport
            new_export = false;
        }
        else
        {
            // not changing the export
            new_export = old_export;
        }
        if (uvars())
        {
            uvars()->set(key, val, new_export);
            env_universal_barrier();
            if (old_export || new_export)
            {
                mark_changed_exported();
            }
        }
        is_universal = 1;
    }
    else
    {
        // Determine the node
        
        env_node_t *preexisting_node = this->get_node(key);
        bool preexisting_entry_exportv = false;
        if (preexisting_node != NULL)
        {
            const var_entry_t *entry = preexisting_node->find_entry(key);
            assert(entry != NULL);
            if (entry->exportv)
            {
                preexisting_entry_exportv = true;
                has_changed_new = true;
            }
        }
        
        env_node_t *node = NULL;
        if (var_mode & ENV_GLOBAL)
        {
            node = this->global.get();
        }
        else if (var_mode & ENV_LOCAL)
        {
            node = top.get();
        }
        else if (preexisting_node != NULL)
        {
            node = preexisting_node;
            
            if ((var_mode & (ENV_EXPORT | ENV_UNEXPORT)) == 0)
            {
                // use existing entry's exportv
                var_mode = preexisting_entry_exportv ? ENV_EXPORT : 0;
            }
        }
        else
        {
            if (! get_proc_had_barrier())
            {
                set_proc_had_barrier(true);
                env_universal_barrier();
            }

            if (uvars() && ! uvars()->get(key).missing())
            {
                bool exportv;
                if (var_mode & ENV_EXPORT)
                {
                    exportv = true;
                }
                else if (var_mode & ENV_UNEXPORT)
                {
                    exportv = false;
                }
                else
                {
                    exportv = uvars()->get_export(key);
                }

                uvars()->set(key, val, exportv);
                env_universal_barrier();
                is_universal = 1;
                
                done = 1;
                
            }
            else
            {
                /*
                 New variable with unspecified scope. The default
                 scope is the innermost scope that is shadowing,
                 which will be either the current function or the
                 global scope.
                 */
                node = top.get();
                while (node->next && !node->new_scope)
                {
                    node = node->get_next();
                }
            }
        }
        
        if (!done)
        {
            // Set the entry in the node
            // Note that operator[] accesses the existing entry, or creates a new one
            var_entry_t *entry = node->find_or_create_entry(key);
            if (entry->exportv)
            {
                // this variable already existed, and was exported
                has_changed_new = true;
            }
            entry->val = val;
            if (var_mode & ENV_EXPORT)
            {
                // the new variable is exported
                entry->exportv = true;
                node->exportv = true;
                has_changed_new = true;
            }
            else
            {
                entry->exportv = false;
            }
            
            if (has_changed_old || has_changed_new)
                mark_changed_exported();
        }
    }
    
    // Must not hold the lock around react_to_variable_change or event firing
    locker.unlock();
    
    if (!is_universal)
    {
        event_t ev = event_t::variable_event(key);
        ev.arguments.reserve(3);
        ev.arguments.push_back(L"VARIABLE");
        ev.arguments.push_back(L"SET");
        ev.arguments.push_back(key);
        
        //  debug( 1, L"env_set: fire events on variable %ls", key );
        event_fire(&ev);
        //  debug( 1, L"env_set: return from event firing" );
    }
    
    react_to_variable_change(key);
    
    return 0;
}

int env_set(const wcstring &key, const wchar_t *val, env_mode_flags_t var_mode)
{
    return main_stack()->set(key, val, var_mode);
}


/**
   Attempt to remove/free the specified key/value pair from the
   specified map.

   \return zero if the variable was not found, non-zero otherwise
*/
bool env_stack_t::try_remove(env_node_t *n, const wcstring &key, int var_mode)
{
    if (n == NULL)
    {
        return false;
    }

    var_entry_t *result = n->find_entry(key);
    if (result != NULL)
    {
        if (result->exportv)
        {
            mark_changed_exported();
        }
        n->remove_entry(key);
        return true;
    }

    if (var_mode & ENV_LOCAL)
    {
        return false;
    }

    if (n->new_scope)
    {
        return try_remove(this->global.get(), key, var_mode);
    }
    else
    {
        return try_remove(n->get_next(), key, var_mode);
    }
}


int env_stack_t::remove(const wcstring &key, int var_mode)
{
    ASSERT_IS_MAIN_THREAD();
    scoped_lock locker(s_env_lock);
    env_node_t *first_node;
    int erased = 0;

    if ((var_mode & ENV_USER) && is_read_only(key))
    {
        return 2;
    }

    first_node = this->top.get();

    if (!(var_mode & ENV_UNIVERSAL))
    {

        if (var_mode & ENV_GLOBAL)
        {
            first_node = this->global.get();
        }

        if (try_remove(first_node, key, var_mode))
        {
            event_t ev = event_t::variable_event(key);
            ev.arguments.push_back(L"VARIABLE");
            ev.arguments.push_back(L"ERASE");
            ev.arguments.push_back(key);

            event_fire(&ev);

            erased = 1;
        }
    }

    if (!erased &&
            !(var_mode & ENV_GLOBAL) &&
            !(var_mode & ENV_LOCAL))
    {
        erased = uvars() && uvars()->remove(key);
        if (erased)
        {
            env_universal_barrier();
        }
    }

    locker.unlock();
    react_to_variable_change(key);

    return !erased;
}

env_var_t env_get_from_principal(const wcstring &key, env_mode_flags_t mode)
{
    return parser_t::principal_parser().vars().get(key, mode);
}

const wchar_t *env_var_t::c_str(void) const
{
    assert(! is_missing);
    return wcstring::c_str();
}

env_var_t env_stack_t::get(const wcstring &key, env_mode_flags_t mode) const
{
    const bool has_scope = mode & (ENV_LOCAL | ENV_GLOBAL | ENV_UNIVERSAL);
    const bool search_local = !has_scope || (mode & ENV_LOCAL);
    const bool search_global = !has_scope || (mode & ENV_GLOBAL);
    const bool search_universal = !has_scope || (mode & ENV_UNIVERSAL);

    const bool search_exported = (mode & ENV_EXPORT) || !(mode & ENV_UNEXPORT);
    const bool search_unexported = (mode & ENV_UNEXPORT) || !(mode & ENV_EXPORT);

    /* Make the assumption that electric keys can't be shadowed elsewhere, since we currently block that in env_set() */
    if (is_electric(key))
    {
        if (!search_global) return env_var_t::missing_var();
        /* Big hack...we only allow getting the history on the main thread. Note that history_t may ask for an environment variable, so don't take the lock here (we don't need it) */
        if (key == L"history" && is_main_thread())
        {
            env_var_t result;

            history_t *history = reader_get_history();
            if (! history)
            {
                history = &history_t::history_with_name(L"fish");
            }
            if (history)
                history->get_string_representation(&result, ARRAY_SEP_STR);
            return result;
        }
        else if (key == L"COLUMNS")
        {
            return to_string(common_get_width());
        }
        else if (key == L"LINES")
        {
            return to_string(common_get_height());
        }
        else if (key == L"status")
        {
            return to_string(proc_get_last_status());
        }
        else if (key == L"umask")
        {
            return format_string(L"0%0.3o", get_umask());
        }
        // we should never get here unless the electric var list is out of sync
    }

    if (search_local || search_global) {
        /* Lock around a local region */
        scoped_lock lock(s_env_lock);

        const env_node_t *env = search_local ? top.get() : global.get();
        while (env != NULL)
        {
            const var_entry_t *entry = env->find_entry(key);
            if (entry != NULL && (entry->exportv ? search_exported : search_unexported))
            {
                if (entry->val == ENV_NULL)
                {
                    return env_var_t::missing_var();
                }
                else
                {
                    return entry->val;
                }
            }

            if (has_scope)
            {
                if (!search_global || env == global.get()) break;
                env = global.get();
            }
            else
            {
                env = this->next_scope(env);
            }
        }
    }

    if (!search_universal) return env_var_t::missing_var();

    /* Another big hack - only do a universal barrier on the main thread (since it can change variable values)
        Make sure we do this outside the env_lock because it may itself call env_get_string */
    if (is_main_thread() && ! get_proc_had_barrier())
    {
        set_proc_had_barrier(true);
        env_universal_barrier();
    }

    if (uvars())
    {
        env_var_t env_var = uvars()->get(key);
        if (env_var == ENV_NULL || !(uvars()->get_export(key) ? search_exported : search_unexported))
        {
            env_var = env_var_t::missing_var();
        }
        return env_var;
    }
    return env_var_t::missing_var();
}

bool env_stack_t::exist(const wchar_t *key, env_mode_flags_t mode) const
{
    const env_node_t *env;

    CHECK(key, false);

    const bool has_scope = mode & (ENV_LOCAL | ENV_GLOBAL | ENV_UNIVERSAL);
    const bool test_local = !has_scope || (mode & ENV_LOCAL);
    const bool test_global = !has_scope || (mode & ENV_GLOBAL);
    const bool test_universal = !has_scope || (mode & ENV_UNIVERSAL);

    const bool test_exported = (mode & ENV_EXPORT) || !(mode & ENV_UNEXPORT);
    const bool test_unexported = (mode & ENV_UNEXPORT) || !(mode & ENV_EXPORT);

    if (is_electric(key))
    {
        /*
        Electric variables all exist, and they are all global. A local or
        universal version can not exist. They are also never exported.
        */
        if (test_global && test_unexported)
        {
            return true;
        }
        return false;
    }

    if (test_local || test_global)
    {
        env = test_local ? this->top.get() : this->global.get();

        while (env)
        {
            const var_entry_t *entry = env->find_entry(key);

            if (entry != NULL)
            {
                return entry->exportv ? test_exported : test_unexported;
            }

            if (has_scope)
            {
                if (!test_global || env == this->global.get()) break;
                env = this->global.get();
            }
            else
            {
                env = this->next_scope(env);
            }
        }
    }

    if (test_universal)
    {
        if (! get_proc_had_barrier())
        {
            set_proc_had_barrier(true);
            env_universal_barrier();
        }

        if (uvars() && ! uvars()->get(key).missing())
        {
            return uvars()->get_export(key) ? test_exported : test_unexported;
        }
    }

    return 0;
}

/**
   Returns true if the specified scope or any non-shadowed non-global subscopes contain an exported variable.
*/
bool env_stack_t::local_scope_exports(env_node_t *n) const
{

    if (n == this->global.get())
        return false;

    if (n->exportv)
        return true;

    if (n->new_scope)
        return false;

    return this->local_scope_exports(n->get_next());
}

void env_stack_t::push(bool new_scope)
{
    const env_node_ref_t node = env_node_ref_t(new env_node_t(new_scope, top));
    if (new_scope)
    {
        if (local_scope_exports(top.get()))
            mark_changed_exported();
    }
    this->top = node;
}

void env_stack_t::pop()
{
    /* Don't pop past the boundary */
    assert(this->boundary.get() == NULL || this->boundary.get() != this->top.get());
    
    scoped_lock locker(s_env_lock);
    if (this->top != this->global)
    {
        int i;
        int locale_changed = 0;

        const env_node_ref_t killme = top;

        for (i=0; locale_variable[i]; i++)
        {
            const var_entry_t *result =  killme->find_entry(locale_variable[i]);
            if (result != NULL)
            {
                locale_changed = 1;
                break;
            }
        }

        if (killme->new_scope)
        {
            if (killme->exportv || local_scope_exports(killme->next.get()))
                mark_changed_exported();
        }

        top = top->next;

        var_table_t::const_iterator iter;
        for (iter = killme->get_env().begin(); iter != killme->get_env().end(); ++iter)
        {
            const var_entry_t &entry = iter->second;
            if (entry.exportv)
            {
                mark_changed_exported();
                break;
            }
        }
        
        locker.unlock();

        if (locale_changed)
            handle_locale();

    }
    else
    {
        debug(0,
              _(L"Tried to pop empty environment stack."));
        sanity_lose();
    }
}

env_var_t env_get_from_main(const wcstring &key)
{
    const environment_t &vars = parser_t::principal_environment();
    return vars.get(key);
}

/**
   Function used with to insert keys of one table into a set::set<wcstring>
*/
static void add_key_to_string_set(const var_table_t &envs, std::set<wcstring> *str_set, bool show_exported, bool show_unexported)
{
    var_table_t::const_iterator iter;
    for (iter = envs.begin(); iter != envs.end(); ++iter)
    {
        const var_entry_t &e = iter->second;

        if ((e.exportv && show_exported) ||
                (!e.exportv && show_unexported))
        {
            /* Insert this key */
            str_set->insert(iter->first);
        }

    }
}

wcstring_list_t env_stack_t::get_names(env_mode_flags_t flags) const
{
    scoped_lock locker(s_env_lock);

    wcstring_list_t result;
    std::set<wcstring> names;
    int show_local = flags & ENV_LOCAL;
    int show_global = flags & ENV_GLOBAL;
    int show_universal = flags & ENV_UNIVERSAL;

    const env_node_t *n = top.get();
    const bool show_exported = (flags & ENV_EXPORT) || !(flags & ENV_UNEXPORT);
    const bool show_unexported = (flags & ENV_UNEXPORT) || !(flags & ENV_EXPORT);

    if (!show_local && !show_global && !show_universal)
    {
        show_local =show_universal = show_global=1;
    }

    if (show_local)
    {
        while (n)
        {
            if (n == this->global.get())
                break;

            add_key_to_string_set(n->get_env(), &names, show_exported, show_unexported);
            if (n->new_scope)
                break;
            else
                n = n->next.get();

        }
    }

    if (show_global)
    {
        add_key_to_string_set(this->global->get_env(), &names, show_exported, show_unexported);
        if (show_unexported)
        {
            result.insert(result.end(), env_electric.begin(), env_electric.end());
        }
    }

    if (show_universal && uvars())
    {
        const wcstring_list_t uni_list = uvars()->get_names(show_exported, show_unexported);
        names.insert(uni_list.begin(), uni_list.end());
    }

    result.insert(result.end(), names.begin(), names.end());
    return result;
}

/**
  Get list of all exported variables
*/

void env_stack_t::get_exported(const env_node_t *n, std::map<wcstring, wcstring> &h) const
{
    ASSERT_IS_LOCKED(s_env_lock);
    if (!n)
        return;
    
    if (n->new_scope)
        this->get_exported(this->global.get(), h);
    else
        this->get_exported(n->next.get(), h);

    var_table_t::const_iterator iter;
    for (iter = n->get_env().begin(); iter != n->get_env().end(); ++iter)
    {
        const wcstring &key = iter->first;
        const var_entry_t &val_entry = iter->second;
        if (val_entry.exportv && (val_entry.val != ENV_NULL))
        {
            // Don't use std::map::insert here, since we need to overwrite existing values from previous scopes
            h[key] = val_entry.val;
        }
    }
}

/* Given a map from key to value, add values to out of the form key=value */
static void export_func(const std::map<wcstring, wcstring> &envs, std::vector<std::string> &out)
{
    out.reserve(out.size() + envs.size());
    std::map<wcstring, wcstring>::const_iterator iter;
    for (iter = envs.begin(); iter != envs.end(); ++iter)
    {
        const wcstring &key = iter->first;
        const std::string &ks = wcs2string(key);
        std::string vs = wcs2string(iter->second);

        /* Arrays in the value are ASCII record separator (0x1e) delimited. But some variables should have colons. Add those. */
        if (variable_is_colon_delimited_array(key))
        {
            /* Replace ARRAY_SEP with colon */
            std::replace(vs.begin(), vs.end(), (char)ARRAY_SEP, ':');
        }

        /* Put a string on the vector */
        out.push_back(std::string());
        std::string &str = out.back();
        str.reserve(ks.size() + 1 + vs.size());

        /* Append our environment variable data to it */
        str.append(ks);
        str.append("=");
        str.append(vs);
    }
}

void env_stack_t::update_export_array_if_necessary(bool recalc)
{
    scoped_lock locker(s_env_lock);
    
    ASSERT_IS_MAIN_THREAD();
    if (recalc && ! get_proc_had_barrier())
    {
        set_proc_had_barrier(true);
        env_universal_barrier();
    }

    if (has_changed_exported)
    {
        std::map<wcstring, wcstring> vals;
        size_t i;

        debug(4, L"env_export_arr() recalc");

        get_exported(top.get(), vals);

        if (uvars())
        {
            const wcstring_list_t uni = uvars()->get_names(true, false);
            for (i=0; i<uni.size(); i++)
            {
                const wcstring &key = uni.at(i);
                const env_var_t val = uvars()->get(key);

                if (! val.missing() && val != ENV_NULL)
                {
                    // Note that std::map::insert does NOT overwrite a value already in the map,
                    // which we depend on here
                    vals.insert(std::pair<wcstring, wcstring>(key, val));
                }
            }
        }

        std::vector<std::string> local_export_buffer;
        export_func(vals, local_export_buffer);
        this->export_array.set(local_export_buffer);
        has_changed_exported=false;
    }
}

const null_terminated_array_t<char> &env_stack_t::get_export_array() const
{
    return export_array;
}

const char * const *env_export_arr(bool recalc)
{
    ASSERT_IS_MAIN_THREAD();
    main_stack()->update_export_array_if_necessary(recalc);
    return main_stack()->get_export_array().get();
}

env_vars_snapshot_t::env_vars_snapshot_t(const environment_t &env, const wchar_t * const *keys)
{
    ASSERT_IS_MAIN_THREAD();
    wcstring key;
    for (size_t i=0; keys[i]; i++)
    {
        key.assign(keys[i]);
        const env_var_t val = env.get(key);
        if (! val.missing())
        {
            vars[key] = val;
        }
    }
}


void env_universal_barrier()
{
    ASSERT_IS_MAIN_THREAD();
    if (uvars())
    {
        callback_data_list_t changes;
        bool changed = uvars()->sync(&changes);
        if (changed)
        {
            universal_notifier_t::default_notifier().post_notification();
        }
        
        /* Post callbacks */
        for (size_t i=0; i < changes.size(); i++)
        {
            const callback_data_t &data = changes.at(i);
            universal_callback(data.type, data.key.c_str(), data.val.c_str());
        }
    }
}

env_var_t env_vars_snapshot_t::get(const wcstring &key, env_mode_flags_t mode) const
{
    std::map<wcstring, wcstring>::const_iterator iter = vars.find(key);
    return (iter == vars.end() ? env_var_t::missing_var() : env_var_t(iter->second));
}

wcstring_list_t env_vars_snapshot_t::get_names(env_mode_flags_t flags) const
{
    wcstring_list_t result;
    std::map<wcstring, wcstring>::const_iterator iter = vars.begin();
    while (iter != vars.end())
    {
        result.push_back(iter->first);
        ++iter;
    }
    return result;
}

const wchar_t * const env_vars_snapshot_t::highlighting_keys[] = {L"PATH", L"CDPATH", L"fish_function_path", L"PWD", USER_ABBREVIATIONS_VARIABLE_NAME, NULL};

environment_t::environment_t() { }
environment_t::~environment_t() { }
