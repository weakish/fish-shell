/** \file docopt_registration.h
  Support for registering docopt descriptions of commands and functions
*/

#ifndef FISH_DOCOPT_REGISTRATION_H
#define FISH_DOCOPT_REGISTRATION_H

#include <wchar.h>

#include "util.h"
#include "common.h"
#include "io.h"

/* Given a command and a name, register a description */
void docopt_register_description(const wcstring &cmd, const wcstring &name, const wcstring &description);

/* Fetch registered descriptions */
wcstring_list_t docopt_copy_registered_descriptions(const wcstring &cmd);

#endif
