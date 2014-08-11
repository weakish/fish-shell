/** \file docopt_registration.cpp

Functions for handling the set of docopt descriptions

*/

#include "config.h"
#include "docopt_registration.h"
#include "common.h"
#include "parse_constants.h"
#include "parser.h"
#include "docopt/docopt_fish.h"
#include <map>
#include <vector>
#include <list>
#include <memory>

typedef docopt_fish::argument_parser_t<wcstring> docopt_parser_t;
typedef docopt_fish::error_t<wcstring> docopt_error_t;
typedef std::vector<docopt_error_t> docopt_error_list_t;

static void append_parse_error(parse_error_list_t *out_errors, size_t where, const wcstring &text)
{
    if (out_errors != NULL)
    {
        out_errors->resize(out_errors->size() + 1);
        parse_error_t *parse_err = &out_errors->back();
        parse_err->text = text;
        parse_err->code = parse_error_docopt;
        parse_err->source_start = where;
        parse_err->source_length = 0;
    }
}

// Name, value pair
struct registration_t {
    wcstring name;
    wcstring description;
    docopt_parser_t parser;
    
    registration_t(const wcstring &n, const wcstring &d) : name(n), description(d)
    {}
    registration_t()
    {}
    
    /* TODO: terribly hackish, eliminate this */
    registration_t(const registration_t &rhs) : name(rhs.name), description(rhs.description)
    {}
};

// Class that holds a mapping from command name to list of docopt descriptions
class doc_register_t {
    typedef std::list<registration_t> registration_list_t;
    typedef std::map<wcstring, registration_list_t> registration_map_t;
    registration_map_t cmd_to_registration;
    mutex_lock_t lock;
    
    // Looks for errors in the parser conditions
    bool validate_parser(const docopt_parser_t &parser, parse_error_list_t *out_errors)
    {
        bool success = true;
        const wcstring_list_t vars = parser.get_variables();
        parser_t error_detector(PARSER_TYPE_ERRORS_ONLY, false);
        for (size_t i=0; i < vars.size(); i++)
        {
            const wcstring &var = vars.at(i);
            const wcstring condition_string = parser.conditions_for_variable(var);
            if (! condition_string.empty())
            {
                wcstring local_err;
                if (error_detector.detect_errors_in_argument_list(condition_string, &local_err, L""))
                {
                    wcstring err_text = format_string(L"Condition '%ls' contained a syntax error:\n%ls", condition_string.c_str(), local_err.c_str());
                    // TODO: would be nice to have the actual position of the error
                    append_parse_error(out_errors, -1, err_text);
                    success = false;
                    break;
                }
            }
        }
        return success;
    }
    
    public:
    bool register_description(const wcstring &cmd_or_empty, const wcstring &name, const wcstring &description, parse_error_list_t *out_errors)
    {
        // Try to parse it
        docopt_parser_t parser;
        docopt_error_list_t errors;
        bool success = parser.set_doc(description, &errors);
        
        // Verify it
        success = success && validate_parser(parser, out_errors);

        // Translate errors from docopt to parse_error over
        if (out_errors != NULL)
        {
            for (size_t i=0; i < errors.size(); i++)
            {
                const docopt_error_t &doc_err = errors.at(i);
                append_parse_error(out_errors, doc_err.location, doc_err.text);
            }
        }

        // If the command is empty, we determine the command by inferring it from the doc, if there is one
        wcstring effective_cmd = cmd_or_empty;
        if (effective_cmd.empty())
        {
            const wcstring_list_t cmd_names = parser.get_command_names();
            if (cmd_names.empty())
            {
                append_parse_error(out_errors, 0, L"No command name found in docopt description");
            }
            else if (cmd_names.size() > 1)
            {
                const wchar_t *first = cmd_names.at(0).c_str();
                const wchar_t *second = cmd_names.at(1).c_str();
                const wcstring text = format_string(L"Multiple command names found in docopt description, such as '%ls' and '%ls'", first, second);
                append_parse_error(out_errors, 0, text);
            }
            else
            {
                assert(cmd_names.size() == 1);
                effective_cmd = cmd_names.front();
            }
        }
        success = success && ! effective_cmd.empty();
        
        if (success)
        {
            scoped_lock locker(lock);
            registration_list_t &regs = cmd_to_registration[effective_cmd];
            
            // Remove any with the same name
            registration_list_t::iterator iter = regs.begin();
            while (iter != regs.end())
            {
                if (iter->name == name)
                {
                    iter = regs.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
            
            // Append a new one
            regs.push_back(registration_t());
            registration_t *reg = &regs.back();
            reg->name = name;
            reg->description = description;
            reg->parser = parser;
        }
        return success;
    }
    
    wcstring_list_t copy_registered_descriptions(const wcstring &cmd)
    {
        wcstring_list_t result;
        scoped_lock locker(lock);
        registration_map_t::iterator where = cmd_to_registration.find(cmd);
        if (where != cmd_to_registration.end()) {
            const registration_list_t &regs = where->second;
            for (registration_list_t::const_iterator iter = regs.begin(); iter != regs.end(); ++iter)
            {
                result.push_back(iter->description);
            }
        }
        return result;
    }
    
    const docopt_parser_t *first_parser(const wcstring &cmd) const
    {
        const docopt_parser_t *result = NULL;
        ASSERT_IS_LOCKED(lock);
        registration_map_t::const_iterator where = this->cmd_to_registration.find(cmd);
        if (where != this->cmd_to_registration.end() && ! where->second.empty())
        {
            result = &where->second.front().parser;
        }
        return result;
    }

    std::vector<docopt_argument_status_t> validate_arguments(const wcstring &cmd, const wcstring_list_t &argv, docopt_fish::parse_flags_t flags)
    {
        scoped_lock locker(lock);
        std::vector<docopt_argument_status_t> result;
        const docopt_parser_t *p = this->first_parser(cmd);
        if (p)
        {
            const std::vector<docopt_fish::argument_status_t> tmp = p->validate_arguments(argv, flags);
            // Transform to our cover type
            result.reserve(tmp.size());
            for (size_t i=0; i < tmp.size(); i++)
            {
                result.push_back(static_cast<docopt_argument_status_t>(tmp.at(i)));
            }
        }
        return result;
    }

    wcstring_list_t suggest_next_argument(const wcstring &cmd, const wcstring_list_t &argv, docopt_fish::parse_flags_t flags)
    {
        scoped_lock locker(lock);
        wcstring_list_t result;
        const docopt_parser_t *p = this->first_parser(cmd);
        if (p)
        {
            result = p->suggest_next_argument(argv, flags);
        }
        return result;
    }

    wcstring conditions_for_variable(const wcstring &cmd, const wcstring &var)
    {
        scoped_lock locker(lock);
        wcstring result;
        const docopt_parser_t *p = this->first_parser(cmd);
        if (p)
        {
            result = p->conditions_for_variable(var);
        }
        return result;
    }

    wcstring description_for_option(const wcstring &cmd, const wcstring &option)
    {
        scoped_lock locker(lock);
        wcstring result;
        const docopt_parser_t *p = this->first_parser(cmd);
        if (p)
        {
            result = p->description_for_option(option);
        }
        return result;
    }

};
static doc_register_t default_register;

bool docopt_register_description(const wcstring &cmd, const wcstring &name, const wcstring &description, parse_error_list_t *out_errors)
{
    return default_register.register_description(cmd, name, description, out_errors);
}

wcstring_list_t docopt_copy_registered_descriptions(const wcstring &cmd)
{
    return default_register.copy_registered_descriptions(cmd);
}


std::vector<docopt_argument_status_t> docopt_validate_arguments(const wcstring &cmd, const wcstring_list_t &argv, docopt_parse_flags_t flags)
{
    return default_register.validate_arguments(cmd, argv, flags);
}

wcstring_list_t docopt_suggest_next_argument(const wcstring &cmd, const wcstring_list_t &argv, docopt_parse_flags_t flags)
{
    return default_register.suggest_next_argument(cmd, argv, flags);
}

wcstring docopt_conditions_for_variable(const wcstring &cmd, const wcstring &var)
{
    return default_register.conditions_for_variable(cmd, var);
}

wcstring docopt_description_for_option(const wcstring &cmd, const wcstring &option)
{
    return default_register.description_for_option(cmd, option);
}