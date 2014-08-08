/** \file docopt_registration.h
  Support for registering docopt descriptions of commands and functions
*/

#ifndef FISH_DOCOPT_REGISTRATION_H
#define FISH_DOCOPT_REGISTRATION_H

#include <wchar.h>

#include "util.h"
#include "common.h"
#include "io.h"
#include <vector>

struct parse_error_t;
typedef std::vector<parse_error_t> parse_error_list_t;

/* Given a command and a name, register a description */
bool docopt_register_description(const wcstring &cmd, const wcstring &name, const wcstring &description, parse_error_list_t *out_errors);

/* Fetch registered descriptions */
wcstring_list_t docopt_copy_registered_descriptions(const wcstring &cmd);

/* Covers for docopt functions */
enum docopt_argument_status_t {
    status_invalid, // the argument doesn't work
    status_valid, // the argument works fine
    status_valid_prefix // the argument is a prefix of something that may work
};

enum docopt_parse_flags_t {
    flags_default = 0U,
    flag_generate_empty_args = 1U << 0,
    flag_match_allow_incomplete = 1U << 1,
    flag_resolve_unambiguous_prefixes = 1U << 2,
};

std::vector<docopt_argument_status_t> docopt_validate_arguments(const wcstring &cmd, const wcstring_list_t &argv, docopt_parse_flags_t flags = flags_default);
wcstring_list_t docopt_suggest_next_argument(const wcstring &cmd, const wcstring_list_t &argv, docopt_parse_flags_t flags = flags_default);
wcstring docopt_conditions_for_variable(const wcstring &cmd, const wcstring &var);
wcstring docopt_description_for_option(const wcstring &cmd, const wcstring &option);


#endif
