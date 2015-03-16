/** \file builtin_set_color.cpp Functions defining the set_color builtin

Functions used for implementing the set_color builtin.

*/
#include "config.h"

#include "builtin.h"
#include "color.h"
#include "output.h"

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


/* We know about these buffers */
extern wcstring stdout_buffer, stderr_buffer;

/**
   Error message for invalid path operations
*/
#define BUILTIN_SET_PATH_ERROR L"%ls: Warning: path component %ls may not be valid in %ls.\n"

/**
   Hint for invalid path operation with a colon
*/
#define BUILTIN_SET_PATH_HINT L"%ls: Did you mean 'set %ls $%ls %ls'?\n"

/**
   Error for mismatch between index count and elements
*/
#define BUILTIN_SET_ARG_COUNT L"%ls: The number of variable indexes does not match the number of values\n"

static void print_colors(void)
{
    const wcstring_list_t result = rgb_color_t::named_color_names();
    size_t i;
    for (i=0; i < result.size(); i++)
    {
        stdout_buffer.append(result.at(i));
        stdout_buffer.push_back(L'\n');
    }
}

/* function we set as the output writer */
static std::string builtin_set_color_output;
static int set_color_builtin_outputter(char c)
{
    ASSERT_IS_MAIN_THREAD();
    builtin_set_color_output.push_back(c);
    return 0;
}

/**
   set_color builtin
*/
static const wchar_t * const g_set_color_usage =
    L"Usage:\n"
    L"       set_color [options] [<color>...]\n"
    L"\n"
    L"Options:\n"
    L"       -b <bgcolor>, --background <bgcolor>  sets the background color.\n"
    L"       -c, --print_colors  prints a list of all valid color names.\n"
    L"       -o, --bold  sets bold or extra bright mode.\n"
    L"       -u, --underline  sets underlined mode.\n"
    L"       -h, --help  displays a help message and exits.\n"
;

static int builtin_set_color(parser_t &parser, wchar_t **argv)
{
    /* Some code passes variables to set_color that don't exist, like $fish_user_whatever. As a hack, quietly return failure. */
    if (argv[1] == NULL)
    {
        return EXIT_FAILURE;
    }

    docopt_arguments_t args;
    int status;
    if (! parse_argv_or_show_help(parser, argv, &args, &status))
    {
        return status;
    }
    
    if (args.has(L"--print_colors"))
    {
        print_colors();
        return STATUS_BUILTIN_OK;
    }
    
    wcstring_list_t fgcolor_strs = args.get_list(L"<color>");
    const wchar_t *bgcolor_str = args.get_or_null(L"--background");
    bool bold = args.has(L"--bold");
    bool underline = args.has(L"--underline");
    int errret = -1;
    
    /* Remaining arguments are foreground color */
    std::vector<rgb_color_t> fgcolors;
    for (size_t i=0; i < fgcolor_strs.size(); i++)
    {
        rgb_color_t fg = rgb_color_t(fgcolor_strs.at(i));
        if (fg.is_none() || fg.is_ignore())
        {
            append_format(stderr_buffer, _(L"%ls: Unknown color '%ls'\n"), argv[0], argv[woptind]);
            return STATUS_BUILTIN_ERROR;
        }
        fgcolors.push_back(fg);
    }

    if (fgcolors.empty() && bgcolor_str == NULL && !bold && !underline)
    {
        append_format(stderr_buffer,
                      _(L"%ls: Expected an argument\n"),
                      argv[0]);
        return STATUS_BUILTIN_ERROR;
    }
    
    // #1323: We may have multiple foreground colors. Choose the best one.
    // If we had no foreground coor, we'll get none(); if we have at least one we expect not-none
    const rgb_color_t fg = best_color(fgcolors, output_get_color_support());
    assert(fgcolors.empty() || !(fg.is_none() || fg.is_ignore()));

    const rgb_color_t bg = rgb_color_t(bgcolor_str ? bgcolor_str : L"");
    if (bgcolor_str && (bg.is_none() || bg.is_ignore()))
    {
        append_format(stderr_buffer, _(L"%ls: Unknown color '%ls'\n"), argv[0], bgcolor_str);
        return STATUS_BUILTIN_ERROR;
    }

    /* Make sure that the term exists */
    if (cur_term == NULL && setupterm(0, STDOUT_FILENO, &errret) == ERR)
    {
        append_format(stderr_buffer, _(L"%ls: Could not set up terminal\n"), argv[0]);
        return STATUS_BUILTIN_ERROR;
    }

    /*
       Test if we have at least basic support for setting fonts, colors
       and related bits - otherwise just give up...
    */
    if (! exit_attribute_mode)
    {
        return STATUS_BUILTIN_ERROR;
    }

    /* Save old output function so we can restore it */
    int (* const saved_writer_func)(char) = output_get_writer();

    /* Set our output function, which writes to a std::string */
    builtin_set_color_output.clear();
    output_set_writer(set_color_builtin_outputter);

    if (bold)
    {
        if (enter_bold_mode)
            writembs(tparm(enter_bold_mode));
    }

    if (underline)
    {
        if (enter_underline_mode)
            writembs(enter_underline_mode);
    }

    if (bgcolor_str != NULL)
    {
        if (bg.is_normal())
        {
            write_color(rgb_color_t::black(), false /* not is_fg */);
            writembs(tparm(exit_attribute_mode));
        }
    }

    if (! fg.is_none())
    {
        if (fg.is_normal() || fg.is_reset())
        {
            write_color(rgb_color_t::black(), true /* is_fg */);
            writembs(tparm(exit_attribute_mode));
        }
        else
        {
            write_color(fg, true /* is_fg */);
        }
    }

    if (bgcolor_str != NULL)
    {
        if (! bg.is_normal() && ! bg.is_reset())
        {
            write_color(bg, false /* not is_fg */);
        }
    }

    /* Restore saved writer function */
    output_set_writer(saved_writer_func);

    /* Output the collected string */
    stdout_buffer.append(str2wcstring(builtin_set_color_output));
    builtin_set_color_output.clear();

    return status;
}

