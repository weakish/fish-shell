/** \file docopt_registration.h
  Support for registering docopt descriptions of commands and functions
*/

#ifndef FISH_DOCOPT_REGISTRATION_H
#define FISH_DOCOPT_REGISTRATION_H

#include <wchar.h>

#include "../util.h"
#include "../common.h"
#include "../io.h"
#include "docopt_fish.h"

/* Given a command and a name, register a description */
void docopt_register_description(const wcstring &cmd, const wcstring &name, const wcstring &description);

/* Fetch registered descriptions */
wcstring_list_t docopt_copy_registered_descriptions(const wcstring &cmd);

/* Covers for docopt functions */
std::vector<docopt_fish::argument_status_t> docopt_validate_arguments(const wcstring &cmd, const wcstring_list_t &argv, docopt_fish::parse_flags_t flags = docopt_fish::flags_default);
wcstring_list_t docopt_suggest_next_argument(const wcstring &cmd, const wcstring_list_t &argv, docopt_fish::parse_flags_t flags = docopt_fish::flags_default);
wcstring docopt_conditions_for_variable(const wcstring &cmd, const wcstring &var);
wcstring docopt_description_for_option(const wcstring &cmd, const wcstring &option);


#endif
