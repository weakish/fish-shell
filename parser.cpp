/** \file parser.c

The fish parser. Contains functions for parsing and evaluating code.

*/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <pwd.h>
#include <dirent.h>
#include <algorithm>

#include "fallback.h"
#include "util.h"

#include "common.h"
#include "wutil.h"
#include "proc.h"
#include "parser.h"
#include "parser_keywords.h"
#include "tokenizer.h"
#include "exec.h"
#include "wildcard.h"
#include "function.h"
#include "builtin.h"
#include "env.h"
#include "expand.h"
#include "reader.h"
#include "sanity.h"
#include "event.h"
#include "intern.h"
#include "parse_util.h"
#include "path.h"
#include "signal.h"
#include "complete.h"
#include "parse_tree.h"
#include "parse_execution.h"
#include "iothread.h"

/**
   Error message for tokenizer error. The tokenizer message is
   appended to this message.
*/
#define TOK_ERR_MSG _( L"Tokenizer error: '%ls'")

/**
   Error for evaluating in illegal scope
*/
#define INVALID_SCOPE_ERR_MSG _( L"Tried to evaluate commands using invalid block type '%ls'" )

/**
   Error for wrong token type
*/
#define UNEXPECTED_TOKEN_ERR_MSG _( L"Unexpected token of type '%ls'")

/**
   While block description
*/
#define WHILE_BLOCK N_( L"'while' block" )

/**
   For block description
*/
#define FOR_BLOCK N_( L"'for' block" )

/**
   Breakpoint block
*/
#define BREAKPOINT_BLOCK N_( L"Block created by breakpoint" )

/**
   If block description
*/
#define IF_BLOCK N_( L"'if' conditional block" )

/**
   Function definition block description
*/
#define FUNCTION_DEF_BLOCK N_( L"function definition block" )

/**
   Function invocation block description
*/
#define FUNCTION_CALL_BLOCK N_( L"function invocation block" )

/**
   Function invocation block description
*/
#define FUNCTION_CALL_NO_SHADOW_BLOCK N_( L"function invocation block with no variable shadowing" )

/**
   Switch block description
*/
#define SWITCH_BLOCK N_( L"'switch' block" )

/**
   Fake block description
*/
#define FAKE_BLOCK N_( L"unexecutable block" )

/**
   Top block description
*/
#define TOP_BLOCK N_( L"global root block" )

/**
   Command substitution block description
*/
#define SUBST_BLOCK N_( L"command substitution block" )

/**
   Begin block description
*/
#define BEGIN_BLOCK N_( L"'begin' unconditional block" )

/**
   Source block description
*/
#define SOURCE_BLOCK N_( L"Block created by the . builtin" )

/**
   Source block description
*/
#define EVENT_BLOCK N_( L"event handler block" )

/**
   Unknown block description
*/
#define UNKNOWN_BLOCK N_( L"unknown/invalid block" )


/**
   Datastructure to describe a block type, like while blocks, command substitution blocks, etc.
*/
struct block_lookup_entry
{

    /**
       The block type id. The legal values are defined in parser.h.
    */
    block_type_t type;

    /**
       The name of the builtin that creates this type of block, if any.
    */
    const wchar_t *name;

    /**
       A description of this block type
    */
    const wchar_t *desc;
}
;

/**
   List of all legal block types
*/
static const struct block_lookup_entry block_lookup[]=
{
    { WHILE, L"while", WHILE_BLOCK },
    { FOR, L"for", FOR_BLOCK },
    { IF, L"if", IF_BLOCK },
    { FUNCTION_DEF, L"function", FUNCTION_DEF_BLOCK },
    { FUNCTION_CALL, 0, FUNCTION_CALL_BLOCK },
    { FUNCTION_CALL_NO_SHADOW, 0, FUNCTION_CALL_NO_SHADOW_BLOCK },
    { SWITCH, L"switch", SWITCH_BLOCK },
    { FAKE, 0, FAKE_BLOCK },
    { TOP, 0, TOP_BLOCK },
    { SUBST, 0, SUBST_BLOCK },
    { BEGIN, L"begin", BEGIN_BLOCK },
    { SOURCE, L".", SOURCE_BLOCK },
    { EVENT, 0, EVENT_BLOCK },
    { BREAKPOINT, L"breakpoint", BREAKPOINT_BLOCK },
    { (block_type_t)0, 0, 0 }
};

// Given a file path, return something nicer. Currently we just "unexpand" tildes.
wcstring parser_t::user_presentable_path(const wcstring &path) const
{
    return replace_home_directory_with_tilde(path, this->vars());
}


parser_t::parser_t(enum parser_type_t type, const wcstring &cwd, bool errors) :
    parser_type(type),
    expected_thread(),
    show_errors(errors),
    cancellation_requested(false),
    is_within_fish_initialization(false),
    is_event_count(0),
    is_subshell_count(0),
    proc_last_bg_pid(-1)
{
    this->vars().event_handling_parser = this;
    this->vars().set_pwd(cwd);
}

/* Black magic */
parser_t::parser_t(const parser_t &parent) :
    parser_type(parent.parser_type),
    expected_thread(), // default means none
    show_errors(parent.show_errors),
    cancellation_requested(parent.cancellation_requested),
    is_within_fish_initialization(parent.is_within_fish_initialization),
    is_event_count(parent.is_event_count),
    is_subshell_count(parent.is_subshell_count),
    proc_last_bg_pid(parent.proc_last_bg_pid),
    interactive_filenames(parent.interactive_filenames),
    substitution_comamnd_lines(parent.substitution_comamnd_lines),
    forbidden_function(parent.forbidden_function),
    variable_stack(parent.variable_stack),
    block_stack_top(parent.block_stack_top)
{
    parent.assert_is_this_thread();
    this->vars().event_handling_parser = this;
}

static wcstring get_cwd()
{
    wchar_t dir_path[4096];
    wchar_t *res = wgetcwd(dir_path, 4096);
    return res ? res : L"/";
}

/* A pointer to the principal parser (which is a static local) */
static parser_t *s_principal_parser = NULL;

parser_t &parser_t::principal_parser(void)
{
    ASSERT_IS_NOT_FORKED_CHILD();
    ASSERT_IS_MAIN_THREAD();
    static parser_t parser(PARSER_TYPE_GENERAL, get_cwd(), true);
    if (! s_principal_parser)
    {
        parser.expected_thread = pthread_self();
        s_principal_parser = &parser;
    }
    return parser;
}

bool parser_t::is_principal() const
{
    return s_principal_parser != NULL && this == s_principal_parser;
}

void parser_t::assert_is_this_thread() const
{
    pthread_t invalid_thread = pthread_t();
    if (this->expected_thread != invalid_thread)
    {
        assert(pthread_equal(this->expected_thread, pthread_self()));
    }
}

/* A hacktastic function which enables getting access to the main thread environment from outside the main thread. This is used by the autoloading mechanisms (and ONLY by the autoloading mechanisms), since we do not yet support per-thread functions sets. */
const environment_t &parser_t::principal_environment()
{
    assert(s_principal_parser != NULL);
    return s_principal_parser->vars();
}

void parser_t::set_is_within_fish_initialization(bool flag)
{
    is_within_fish_initialization = flag;
}

void parser_t::push_is_event()
{
    assert_is_this_thread();
    assert(this->is_event_count + 1 > 0); // don't overflow
    this->is_event_count++;
}

void parser_t::pop_is_event()
{
    assert_is_this_thread();
    assert(this->is_event_count > 0);
    this->is_event_count--;
}

bool parser_t::get_is_event() const
{
    return this->is_event_count > 0;
}

void parser_t::push_is_subshell()
{
    assert_is_this_thread();
    assert(this->is_subshell_count + 1 > 0); // don't overflow
    this->is_subshell_count++;
}

void parser_t::pop_is_subshell()
{
    assert_is_this_thread();
    assert(this->is_subshell_count > 0);
    this->is_subshell_count--;
}

bool parser_t::get_is_subshell() const
{
    return this->is_subshell_count > 0;
}


void parser_t::skip_all_blocks(void)
{
    /* Tell all blocks to skip */
    if (s_principal_parser)
    {
        s_principal_parser->cancellation_requested = true;

        //write(2, "Cancelling blocks\n", strlen("Cancelling blocks\n"));
        for (size_t i=0; i < s_principal_parser->block_count(); i++)
        {
            s_principal_parser->block_at_index(i)->skip = true;
        }
    }
}

void parser_t::push_block(block_t *new_current)
{
    this->assert_is_this_thread();
    const enum block_type_t type = new_current->type();
    new_current->src_lineno = parser_t::get_lineno();

    const wchar_t *filename = parser_t::current_filename();
    if (filename != NULL)
    {
        new_current->src_filename = intern(filename);
    }

    const block_t *old_current = this->current_block();
    if (old_current && old_current->skip)
    {
        new_current->skip = true;
    }

    /*
      New blocks should be skipped if the outer block is skipped,
      except TOP ans SUBST block, which open up new environments. Fake
      blocks should always be skipped. Rather complicated... :-(
    */
    new_current->skip = old_current ? old_current->skip : 0;

    /*
      Type TOP and SUBST are never skipped
    */
    if (type == TOP || type == SUBST)
    {
        new_current->skip = 0;
    }

    /*
      Fake blocks and function definition blocks are never executed
    */
    if (type == FAKE || type == FUNCTION_DEF)
    {
        new_current->skip = 1;
    }

    new_current->job = 0;
    new_current->loop_status=LOOP_NORMAL;
    
    new_current->parent = this->block_stack_top;
    this->block_stack_top.reset(new_current); //takes ownership

    if ((new_current->type() != FUNCTION_DEF) &&
            (new_current->type() != FAKE) &&
            (new_current->type() != TOP))
    {
        this->vars().push(type == FUNCTION_CALL);
        new_current->wants_pop_env = true;
    }
}

void parser_t::pop_block()
{
    this->assert_is_this_thread();
    if (block_stack_top.get() == NULL)
    {
        debug(1,
              L"function %s called on empty block stack.",
              __func__);
        bugreport();
        return;
    }

    const block_ref_t old = this->block_stack_top;
    this->block_stack_top = this->block_stack_top->parent;

    if (old->wants_pop_env)
        this->vars().pop();
}

void parser_t::pop_block(const block_t *expected)
{
    assert(expected == this->current_block());
    this->pop_block();
}

const wchar_t *parser_t::get_block_desc(int block) const
{
    for (size_t i=0; block_lookup[i].desc; i++)
    {
        if (block_lookup[i].type == block)
        {
            return _(block_lookup[i].desc);
        }
    }
    return _(UNKNOWN_BLOCK);
}

wcstring parser_t::block_stack_description() const
{
    wcstring result;
    size_t idx = this->block_count();
    size_t spaces = 0;
    while (idx--)
    {
        if (spaces > 0)
        {
            result.push_back(L'\n');
        }
        for (size_t j=0; j < spaces; j++)
        {
            result.push_back(L' ');
        }
        result.append(this->block_at_index(idx)->description());
        spaces++;
    }
    return result;
}

const block_t *parser_t::block_at_index(size_t idx) const
{
    /* Clumsy linked list walking! 0 corresponds to the top element */
    size_t count = 0;
    const block_t *cursor = this->block_stack_top.get();
    while (cursor != NULL && count < idx)
    {
        cursor = cursor->parent.get();
        count++;
    }
    return cursor;
}

block_t *parser_t::block_at_index(size_t idx)
{
    size_t count = 0;
    block_t *cursor = this->block_stack_top.get();
    while (cursor != NULL && count < idx)
    {
        cursor = cursor->parent.get();
        count++;
    }
    return cursor;
}

const block_t *parser_t::current_block() const
{
    assert_is_this_thread();
    return this->block_stack_top.get();
}

block_t *parser_t::current_block()
{
    assert_is_this_thread();
    return this->block_stack_top.get();
}

bool parser_t::block_is_on_stack() const
{
    assert_is_this_thread();
    // We have a "block block" if any of the elements of our block stack are not TOP and not SUBST
    bool result = false;
    const block_t *cursor = this->current_block();
    while (cursor != NULL)
    {
        if (cursor->type() != TOP && cursor->type() != SUBST)
        {
            result = true;
            break;
        }
        cursor = cursor->parent.get();
    }
    return result;
}

void parser_t::forbid_function(const wcstring &function)
{
    this->assert_is_this_thread();
    forbidden_function.push_back(function);
}

void parser_t::allow_function()
{
    this->assert_is_this_thread();
    forbidden_function.pop_back();
}

const wchar_t *parser_t::current_interactive_filename() const
{
    assert_is_this_thread();
    return this->interactive_filenames.empty() ? NULL : this->interactive_filenames.back().c_str();
}

void parser_t::push_interactive_filename(const wcstring &str)
{
    assert_is_this_thread();
    this->interactive_filenames.push_back(str);
}

void parser_t::pop_interactive_filename()
{
    assert_is_this_thread();
    assert(! this->interactive_filenames.empty());
    this->interactive_filenames.pop_back();
}

bool parser_t::get_is_interactive() const
{
    assert_is_this_thread();
    /* An empty stack means false */
    bool result = false;
    if (! this->is_interactive_stack.empty())
    {
        result = this->is_interactive_stack.back();
    }
    return result;
}

void parser_t::push_is_interactive(bool new_val)
{
    assert_is_this_thread();
    bool old_val = this->get_is_interactive();
    this->is_interactive_stack.push_back(new_val);
    if (old_val != new_val && this->is_principal())
    {
        signal_set_handlers(new_val);
    }
}

/* Set is_interactive flag to the previous value. If needed, update signal handlers */
void parser_t::pop_is_interactive()
{
    assert_is_this_thread();
    assert(! this->is_interactive_stack.empty());
    bool old_val = this->get_is_interactive();
    this->is_interactive_stack.pop_back();
    bool new_val = this->get_is_interactive();
    if (old_val != new_val && this->is_principal())
    {
        signal_set_handlers(new_val);
    }
}

bool parser_t::get_substituted_commandline(wcstring *out)
{
    assert_is_this_thread();
    bool result = false;
    if (! this->substitution_comamnd_lines.empty())
    {
        out->assign(this->substitution_comamnd_lines.back());
        result = true;
    }
    return result;
}

void parser_t::push_substituted_commandline(const wcstring &s)
{
    assert_is_this_thread();
    this->substitution_comamnd_lines.push_back(s);
}

void parser_t::pop_substituted_commandline()
{
    assert_is_this_thread();
    assert(! this->substitution_comamnd_lines.empty());
    this->substitution_comamnd_lines.pop_back();
}


/**
   Print profiling information to the specified stream
*/
static void print_profile(const std::vector<profile_item_t*> &items,
                          FILE *out)
{
    for (size_t pos = 0; pos < items.size(); pos++)
    {
        const profile_item_t *me, *prev;
        size_t i;
        int my_time;

        me = items.at(pos);
        if (!me->skipped)
        {
            my_time=me->parse+me->exec;

            for (i=pos+1; i<items.size(); i++)
            {
                prev = items.at(i);
                if (prev->skipped)
                {
                    continue;
                }

                if (prev->level <= me->level)
                {
                    break;
                }

                if (prev->level > me->level+1)
                {
                    continue;
                }

                my_time -= prev->parse;
                my_time -= prev->exec;
            }

            if (me->cmd.size() > 0)
            {
                if (fwprintf(out, L"%d\t%d\t", my_time, me->parse+me->exec) < 0)
                {
                    wperror(L"fwprintf");
                    return;
                }

                for (i=0; i<me->level; i++)
                {
                    if (fwprintf(out, L"-") < 0)
                    {
                        wperror(L"fwprintf");
                        return;
                    }

                }
                if (fwprintf(out, L"> %ls\n", me->cmd.c_str()) < 0)
                {
                    wperror(L"fwprintf");
                    return;
                }

            }
        }
    }
}

void parser_t::emit_profiling(const char *path) const
{
    /* Save profiling information. OK to not use CLO_EXEC here because this is called while fish is dying (and hence will not fork) */
    FILE *f = fopen(path, "w");
    if (!f)
    {
        debug(1,
              _(L"Could not write profiling information to file '%s'"),
              path);
    }
    else
    {
        if (fwprintf(f,
                     _(L"Time\tSum\tCommand\n"),
                     profile_items.size()) < 0)
        {
            wperror(L"fwprintf");
        }
        else
        {
            print_profile(profile_items, f);
        }

        if (fclose(f))
        {
            wperror(L"fclose");
        }
    }
}

void parser_t::expand_argument_list(const wcstring &arg_list_src, const environment_t &vars, std::vector<completion_t> *output_arg_list)
{
    assert(output_arg_list != NULL);
    
    expand_flags_t eflags = 0;
    if (! show_errors)
        eflags |= EXPAND_NO_DESCRIPTIONS;
    if (this->parser_type != PARSER_TYPE_GENERAL)
        eflags |= EXPAND_SKIP_CMDSUBST;

    /* Parse the string as an argument list */
    parse_node_tree_t tree;
    if (! parse_tree_from_string(arg_list_src, parse_flag_none, &tree, NULL /* errors */, symbol_freestanding_argument_list))
    {
        /* Failed to parse. Here we expect to have reported any errors in test_args */
        return;
    }

    /* Get the root argument list */
    assert(! tree.empty());
    const parse_node_t *arg_list = &tree.at(0);
    assert(arg_list->type == symbol_freestanding_argument_list);

    /* Extract arguments from it */
    while (arg_list != NULL)
    {
        const parse_node_t *arg_node = tree.next_node_in_node_list(*arg_list, symbol_argument, &arg_list);
        if (arg_node != NULL)
        {
            const wcstring arg_src = arg_node->get_source(arg_list_src);
            /* Hackish. If we are a general parser, we allow command substitutions, in which case we have to pass ourselves to expand_string. Otherwise, we must pass the environment we were given. */
            int expand_result;
            if (this->parser_type == PARSER_TYPE_GENERAL)
            {
                expand_result = expand_string(arg_src, *this, *output_arg_list, eflags, NULL);
            }
            else
            {
                expand_result = expand_string(arg_src, vars, *output_arg_list, eflags, NULL);
            }
            if (expand_result == EXPAND_ERROR)
            {
                /* Failed to expand a string */
                break;
            }
        }
    }
}

void parser_t::stack_trace(size_t block_idx, wcstring &buff) const
{
    /*
      Check if we should end the recursion
    */
    if (block_idx >= this->block_count())
        return;

    const block_t *b = this->block_at_index(block_idx);

    if (b->type()==EVENT)
    {
        /*
          This is an event handler
        */
        const event_block_t *eb = static_cast<const event_block_t *>(b);
        wcstring description = event_get_desc(eb->event);
        append_format(buff, _(L"in event handler: %ls\n"), description.c_str());
        buff.append(L"\n");

        /*
          Stop recursing at event handler. No reason to believe that
          any other code is relevant.

          It might make sense in the future to continue printing the
          stack trace of the code that invoked the event, if this is a
          programmatic event, but we can't currently detect that.
        */
        return;
    }

    if (b->type() == FUNCTION_CALL || b->type() == FUNCTION_CALL_NO_SHADOW || b->type()==SOURCE || b->type()==SUBST)
    {
        /*
          These types of blocks should be printed
        */

        int i;

        switch (b->type())
        {
            case SOURCE:
            {
                const source_block_t *sb = static_cast<const source_block_t*>(b);
                const wchar_t *source_dest = sb->source_file;
                append_format(buff, _(L"from sourcing file %ls\n"), user_presentable_path(source_dest).c_str());
                break;
            }
            case FUNCTION_CALL:
            case FUNCTION_CALL_NO_SHADOW:
            {
                const function_block_t *fb = static_cast<const function_block_t*>(b);
                append_format(buff, _(L"in function '%ls'\n"), fb->name.c_str());
                break;
            }
            case SUBST:
            {
                append_format(buff, _(L"in command substitution\n"));
                break;
            }

            default: /* Can't get here */
                break;
        }

        const wchar_t *file = b->src_filename;

        if (file)
        {
            append_format(buff,
                          _(L"\tcalled on line %d of file %ls\n"),
                          b->src_lineno,
                          user_presentable_path(file).c_str());
        }
        else if (is_within_fish_initialization)
        {
            append_format(buff, _(L"\tcalled during startup\n"));
        }
        else
        {
            append_format(buff, _(L"\tcalled on standard input\n"));
        }

        if (b->type() == FUNCTION_CALL)
        {
            const function_block_t *fb = static_cast<const function_block_t *>(b);
            const process_t * const process = fb->process;
            if (process->argv(1))
            {
                wcstring tmp;

                for (i=1; process->argv(i); i++)
                {
                    if (i > 1)
                        tmp.push_back(L' ');
                    tmp.append(process->argv(i));
                }
                append_format(buff, _(L"\twith parameter list '%ls'\n"), tmp.c_str());
            }
        }

        append_format(buff, L"\n");
    }

    /*
      Recursively print the next block
    */
    parser_t::stack_trace(block_idx + 1, buff);
}

/**
   Returns the name of the currently evaluated function if we are
   currently evaluating a function, null otherwise. This is tested by
   moving down the block-scope-stack, checking every block if it is of
   type FUNCTION_CALL.
*/
const bool parser_t::is_function(wcstring *out_name) const
{
    bool result = false;
    for (size_t block_idx = 0; block_idx < this->block_count(); block_idx++)
    {
        const block_t *b = this->block_at_index(block_idx);
        if (b->type() == FUNCTION_CALL || b->type() == FUNCTION_CALL_NO_SHADOW)
        {
            const function_block_t *fb = static_cast<const function_block_t *>(b);
            result = true;
            if (out_name != NULL)
            {
                out_name->assign(fb->name);
            }
            break;
        }
        else if (b->type() == SOURCE)
        {
            /* If a function sources a file, obviously that function's offset doesn't contribute */
            break;
        }
    }
    return result;
}


int parser_t::get_lineno() const
{
    int lineno = -1;
    if (! execution_contexts.empty())
    {
        lineno = execution_contexts.back()->get_current_line_number();

        /* If we are executing a function, we have to add in its offset */
        wcstring function_name;
        if (this->is_function(&function_name))
        {
            lineno += function_get_definition_offset(function_name);
        }

    }
    return lineno;
}

const wchar_t *parser_t::current_filename() const
{
    this->assert_is_this_thread();

    for (size_t i=0; i < this->block_count(); i++)
    {
        const block_t *b = this->block_at_index(i);
        if (b->type() == FUNCTION_CALL || b->type() == FUNCTION_CALL_NO_SHADOW)
        {
            const function_block_t *fb = static_cast<const function_block_t *>(b);
            return function_get_definition_file(fb->name);
        }
        else if (b->type() == SOURCE)
        {
            const source_block_t *sb = static_cast<const source_block_t *>(b);
            return sb->source_file;
        }
    }
    
    return this->current_interactive_filename();
}

wcstring parser_t::current_line()
{
    if (execution_contexts.empty())
    {
        return wcstring();
    }
    const parse_execution_context_t *context = execution_contexts.back();
    assert(context != NULL);

    int source_offset = context->get_current_source_offset();
    if (source_offset < 0)
    {
        return wcstring();
    }

    const int lineno = this->get_lineno();
    const wchar_t *file = this->current_filename();

    wcstring prefix;

    /* If we are not going to print a stack trace, at least print the line number and filename */
    if (!get_is_interactive() || this->is_function(NULL))
    {
        if (file)
        {
            append_format(prefix, _(L"%ls (line %d): "), user_presentable_path(file).c_str(), lineno);
        }
        else if (is_within_fish_initialization)
        {
            append_format(prefix, L"%ls: ", _(L"Startup"), lineno);
        }
        else
        {
            append_format(prefix, L"%ls: ", _(L"Standard input"), lineno);
        }
    }

    bool is_interactive = get_is_interactive();
    bool skip_caret = is_interactive && ! this->is_function(NULL);

    /* Use an error with empty text */
    assert(source_offset >= 0);
    parse_error_t empty_error = {};
    empty_error.source_start = source_offset;

    wcstring line_info = empty_error.describe_with_prefix(context->get_source(), prefix, is_interactive, skip_caret);
    if (! line_info.empty())
    {
        line_info.push_back(L'\n');
    }

    parser_t::stack_trace(0, line_info);
    return line_info;
}

void parser_t::job_add(job_t *job)
{
    assert(job != NULL);
    assert(job->first_process != NULL);
    this->my_job_list.push_front(job);
}

bool parser_t::job_remove(job_t *j)
{
    job_list_t::iterator iter = std::find(my_job_list.begin(), my_job_list.end(), j);
    if (iter != my_job_list.end())
    {
        my_job_list.erase(iter);
        return true;
    }
    else
    {
        debug(1, _(L"Job inconsistency"));
        sanity_lose();
        return false;
    }
}

void parser_t::job_promote(job_t *job)
{
    job_list_t::iterator loc = std::find(my_job_list.begin(), my_job_list.end(), job);
    assert(loc != my_job_list.end());

    /* Move the job to the beginning */
    my_job_list.splice(my_job_list.begin(), my_job_list, loc);
}

job_t *parser_t::job_get(job_id_t id)
{
    job_iterator_t jobs(my_job_list);
    job_t *job;
    while ((job = jobs.next()))
    {
        if (id <= 0 || job->job_id == id)
            return job;
    }
    return NULL;
}

job_t *parser_t::job_get_from_pid(int pid)
{
    job_iterator_t jobs;
    job_t *job;
    while ((job = jobs.next()))
    {
        if (job->pgid == pid)
            return job;
    }
    return 0;
}

profile_item_t *parser_t::create_profile_item()
{
    profile_item_t *result = NULL;
    if (g_profiling_active)
    {
        result = new profile_item_t();
        profile_items.push_back(result);
    }
    return result;
}


int parser_t::eval(const wcstring &cmd, const io_chain_t &io, enum block_type_t block_type)
{
    /* Parse the source into a tree, if we can */
    parse_node_tree_t tree;
    parse_error_list_t error_list;
    if (! parse_tree_from_string(cmd, parse_flag_none, &tree, this->show_errors ? &error_list : NULL))
    {
        if (this->show_errors)
        {
            /* Get a backtrace */
            wcstring backtrace_and_desc;
            this->get_backtrace(cmd, error_list, &backtrace_and_desc);
            
            /* Print it */
            fprintf(stderr, "%ls", backtrace_and_desc.c_str());
        }
        
        return 1;
    }
    
    return this->eval(cmd, tree, 0, io, block_type);
}

int parser_t::eval(const wcstring &cmd, const parse_node_tree_t &tree, node_offset_t node, const io_chain_t &io, enum block_type_t block_type)
{
    CHECK_BLOCK(1);
    
    if (block_type != TOP && block_type != SUBST)
    {
        debug(1, INVALID_SCOPE_ERR_MSG, parser_t::get_block_desc(block_type));
        bugreport();
        return 1;
    }
    //print_stderr(block_stack_description());
    
    
    /* Determine the initial eval level. If this is the first context, it's -1; otherwise it's the eval level of the top context. This is sort of wonky because we're stitching together a global notion of eval level from these separate objects. A better approach would be some profile object that all contexts share, and that tracks the eval levels on its own. */
    int exec_eval_level = (execution_contexts.empty() ? -1 : execution_contexts.back()->current_eval_level());
    
    /* Append to the execution context stack */
    parse_execution_context_t *ctx = new parse_execution_context_t(tree, cmd, this, exec_eval_level);
    execution_contexts.push_back(ctx);
    
    /* Execute the first node */
    int result = 1;
    if (! tree.empty())
    {
        result = this->eval_block_node(node, io, block_type);
    }
    
    /* Clean up the execution context stack */
    assert(! execution_contexts.empty() && execution_contexts.back() == ctx);
    execution_contexts.pop_back();
    delete ctx;
    
    return 0;
}

int parser_t::eval_block_node(node_offset_t node_idx, const io_chain_t &io, enum block_type_t block_type)
{
    /* Paranoia. It's a little frightening that we're given only a node_idx and we interpret this in the topmost execution context's tree. What happens if two trees were to be interleaved? Fortunately that cannot happen (yet); in the future we probably want some sort of reference counted trees.
    */
    parse_execution_context_t *ctx = execution_contexts.back();
    assert(ctx != NULL);

    CHECK_BLOCK(1);

    /* Handle cancellation requests. If our block stack is currently empty, then we already did successfully cancel (or there was nothing to cancel); clear the flag. If our block stack is not empty, we are still in the process of cancelling; refuse to evaluate anything */
    if (this->cancellation_requested)
    {
        if (this->block_stack_top.get() != NULL)
        {
            return 1;
        }
        else
        {
            this->cancellation_requested = false;
        }
    }

    /* Only certain blocks are allowed */
    if ((block_type != TOP) &&
            (block_type != SUBST))
    {
        debug(1,
              INVALID_SCOPE_ERR_MSG,
              parser_t::get_block_desc(block_type));
        bugreport();
        return 1;
    }

    /* Not sure why we reap jobs here */
    job_reap(this, 0);

    /* Start it up */
    const block_t * const start_current_block = current_block();
    block_t *scope_block = new scope_block_t(block_type);
    this->push_block(scope_block);
    int result = ctx->eval_node_at_offset(node_idx, scope_block, io);

    /* Clean up the block stack */
    this->pop_block();
    while (start_current_block != current_block())
    {
        if (current_block() == NULL)
        {
            debug(0,
                  _(L"End of block mismatch. Program terminating."));
            bugreport();
            FATAL_EXIT();
            break;
        }
        this->pop_block();
    }

    /* Reap again */
    job_reap(this, 0);

    return result;
}

class child_eval_context_t
{
    public:
    parser_t parser;
    emulated_process_t *eproc; //unowned
    parse_node_tree_t tree;
    wcstring src;
    node_offset_t node_idx;
    io_chain_t io;
    enum block_type_t block_type;
    
    child_eval_context_t(const parser_t &parent) : parser(parent), eproc(NULL)
    {
    }
    
    int run_in_background()
    {
        this->parser.expected_thread = pthread_self();
        if (node_idx == NODE_OFFSET_INVALID)
        {
            parser.eval(src, io, block_type);
        }
        else
        {
            parser.eval(src, tree, node_idx, io, block_type);
        }
        int result = this->parser.get_last_status();
        if (this->eproc != NULL)
        {
            this->eproc->set_exit_status(result);
            this->eproc->mark_finished();
        }
        return result;
    }
    
    static int run_in_background(child_eval_context_t *self)
    {
        return self->run_in_background();
    }
};

static int run_child_parser_in_background(child_eval_context_t *ctx)
{
    return ctx->run_in_background();
}

static int run_child_parser_in_background_and_delete(child_eval_context_t *ctx)
{
    int ret = ctx->run_in_background();
    delete ctx;
    return ret;
}

int parser_t::evaluate_in_context_then_delete(child_eval_context_t *child_eval)
{
    bool sync = ! parser_concurrent_execution();
    iothread_perform(sync ? run_child_parser_in_background : run_child_parser_in_background_and_delete, child_eval);
    if (sync)
    {
        if (child_eval->eproc)
        {
            child_eval->eproc->wait_until_finished();
        }
        int result = child_eval->parser.get_last_status();
        this->set_last_status(result);
        delete child_eval;
        return result;
    }
    return -1;
}

int parser_t::eval_block_node_in_child(node_offset_t node_idx, emulated_process_t *eproc, const io_chain_t &io, enum block_type_t block_type)
{
    assert(eproc != NULL);
    
    /* Paranoia. It's a little frightening that we're given only a node_idx and we interpret this in the topmost execution context's tree. What happens if two trees were to be interleaved? Fortunately that cannot happen (yet); in the future we probably want some sort of reference counted trees.
    */
    parse_execution_context_t *ctx = this->execution_contexts.back();
    assert(ctx != NULL);

    CHECK_BLOCK(1);

    child_eval_context_t *child_eval = new child_eval_context_t(*this);
    child_eval->eproc = eproc;
    child_eval->tree = ctx->get_tree();
    child_eval->src = ctx->get_source();
    child_eval->node_idx = node_idx;
    child_eval->io = io;
    child_eval->block_type = block_type;
    return this->evaluate_in_context_then_delete(child_eval);
}

int parser_t::eval_in_child(const wcstring &src, emulated_process_t *eproc, const io_chain_t &io, enum block_type_t block_type)
{
    CHECK_BLOCK(1);
    child_eval_context_t *child_eval = new child_eval_context_t(*this);
    child_eval->eproc = eproc;
    child_eval->src = src;
    child_eval->node_idx = NODE_OFFSET_INVALID;
    child_eval->io = io;
    child_eval->block_type = block_type;
    return this->evaluate_in_context_then_delete(child_eval);
}

bool parser_t::detect_errors_in_argument_list(const wcstring &arg_list_src, wcstring *out, const wchar_t *prefix)
{
    bool errored = false;
    parse_error_list_t errors;

    /* Use empty string for the prefix if it's NULL */
    if (prefix == NULL)
    {
        prefix = L"";
    }

    /* Parse the string as an argument list */
    parse_node_tree_t tree;
    if (! parse_tree_from_string(arg_list_src, parse_flag_none, &tree, &errors, symbol_freestanding_argument_list))
    {
        /* Failed to parse. */
        errored = true;
    }

    if (! errored)
    {
        /* Get the root argument list */
        assert(! tree.empty());
        const parse_node_t *arg_list = &tree.at(0);
        assert(arg_list->type == symbol_freestanding_argument_list);

        /* Extract arguments from it */
        while (arg_list != NULL && ! errored)
        {
            const parse_node_t *arg_node = tree.next_node_in_node_list(*arg_list, symbol_argument, &arg_list);
            if (arg_node != NULL)
            {
                const wcstring arg_src = arg_node->get_source(arg_list_src);
                if (parse_util_detect_errors_in_argument(*arg_node, arg_src, &errors))
                {
                    errored = true;
                }
            }
        }
    }

    if (! errors.empty() && out != NULL)
    {
        out->assign(errors.at(0).describe_with_prefix(arg_list_src, prefix, false /* not interactive */, false /* don't skip caret */));
    }
    return errored;
}

void parser_t::get_backtrace(const wcstring &src, const parse_error_list_t &errors, wcstring *output) const
{
    assert(output != NULL);
    if (! errors.empty())
    {
        const parse_error_t &err = errors.at(0);

        const bool is_interactive = get_is_interactive();

        // Determine if we want to try to print a caret to point at the source error
        // The err.source_start <= src.size() check is due to the nasty way that slices work,
        // which is by rewriting the source (!)
        size_t which_line = 0;
        bool skip_caret = true;
        if (err.source_start != SOURCE_LOCATION_UNKNOWN && err.source_start <= src.size())
        {
            // Determine which line we're on
            which_line = 1 + std::count(src.begin(), src.begin() + err.source_start, L'\n');

            // Don't include the caret if we're interactive, this is the first line of text, and our source is at its beginning, because then it's obvious
            skip_caret = (is_interactive && which_line == 1 && err.source_start == 0);
        }

        wcstring prefix;
        const wchar_t *filename = this->current_filename();
        if (filename)
        {
            if (which_line > 0)
            {
                prefix = format_string(_(L"%ls (line %lu): "), user_presentable_path(filename).c_str(), which_line);
            }
            else
            {
                prefix = format_string(_(L"%ls: "), user_presentable_path(filename).c_str());
            }
        }
        else
        {
            prefix = L"fish: ";
        }

        const wcstring description = err.describe_with_prefix(src, prefix, is_interactive, skip_caret);
        if (! description.empty())
        {
            output->append(description);
            output->push_back(L'\n');
        }
        this->stack_trace(0, *output);
    }
}

block_t::block_t(block_type_t t) :
    block_type(t),
    skip(),
    tok_pos(),
    node_offset(NODE_OFFSET_INVALID),
    loop_status(LOOP_NORMAL),
    job(),
    src_filename(),
    src_lineno(),
    wants_pop_env(false),
    event_blocks()
{
}

block_t::~block_t()
{
}

wcstring block_t::description() const
{
    wcstring result;
    switch (this->type())
    {
        case WHILE:
            result.append(L"while");
            break;

        case FOR:
            result.append(L"for");
            break;

        case IF:
            result.append(L"if");
            break;

        case FUNCTION_DEF:
            result.append(L"function_def");
            break;

        case FUNCTION_CALL:
            result.append(L"function_call");
            break;

        case FUNCTION_CALL_NO_SHADOW:
            result.append(L"function_call_no_shadow");
            break;

        case SWITCH:
            result.append(L"switch");
            break;

        case FAKE:
            result.append(L"fake");
            break;

        case SUBST:
            result.append(L"substitution");
            break;

        case TOP:
            result.append(L"top");
            break;

        case BEGIN:
            result.append(L"begin");
            break;

        case SOURCE:
            result.append(L"source");
            break;

        case EVENT:
            result.append(L"event");
            break;

        case BREAKPOINT:
            result.append(L"breakpoint");
            break;

        default:
            append_format(result, L"unknown type %ld", (long)this->type());
            break;
    }

    if (this->src_lineno >= 0)
    {
        append_format(result, L" (line %d)", this->src_lineno);
    }
    if (this->src_filename != NULL)
    {
        append_format(result, L" (file %ls)", this->src_filename);
    }
    return result;
}

/* Various block constructors */

if_block_t::if_block_t() : block_t(IF)
{
}

event_block_t::event_block_t(const event_t &evt) :
    block_t(EVENT),
    event(evt)
{
}

function_block_t::function_block_t(const process_t *p, const wcstring &n, bool shadows) :
    block_t(shadows ? FUNCTION_CALL : FUNCTION_CALL_NO_SHADOW),
    process(p),
    name(n)
{
}

source_block_t::source_block_t(const wchar_t *src) :
    block_t(SOURCE),
    source_file(src)
{
}

for_block_t::for_block_t() : block_t(FOR)
{
}

while_block_t::while_block_t() : block_t(WHILE)
{
}

switch_block_t::switch_block_t() : block_t(SWITCH)
{
}

fake_block_t::fake_block_t() : block_t(FAKE)
{
}

scope_block_t::scope_block_t(block_type_t type) : block_t(type)
{
    assert(type == BEGIN || type == TOP || type == SUBST);
}

breakpoint_block_t::breakpoint_block_t() : block_t(BREAKPOINT)
{
}

bool parser_use_threads()
{
    return true;
}

bool parser_concurrent_execution()
{
    return parser_use_threads() && true;
}
