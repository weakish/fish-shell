/** \file docopt_registration.cpp

Functions for handling the set of docopt descriptions

*/

#include "config.h"
#include "docopt_registration.h"
#include "common.h"
#include <map>
#include <vector>

// Name, value pair
struct registration_t {
    wcstring name;
    wcstring description;
    
    registration_t(const wcstring &n, const wcstring &d) : name(n), description(d)
    {}
};

// Class that holds a mapping from command name to list of docopt descriptions
class doc_register_t {
    std::map<wcstring, std::vector<registration_t> > cmd_to_registration;
    mutex_lock_t lock;
    
    public:
    void register_description(const wcstring &cmd, const wcstring &name, const wcstring &description)
    {
        scoped_lock locker(lock);
        std::vector<registration_t> &regs = cmd_to_registration[cmd];
        
        // Remove any with the same name
        for (size_t i=0; i < regs.size(); i++) {
            if (regs.at(i).name == name) {
                regs.erase(regs.begin() + i);
            }
        }
        
        // Append a new one
        regs.push_back(registration_t(name, description));
    }
    
    wcstring_list_t copy_registered_descriptions(const wcstring &cmd) {
        wcstring_list_t result;
        scoped_lock locker(lock);
        std::map<wcstring, std::vector<registration_t> >::iterator where = cmd_to_registration.find(cmd);
        if (where != cmd_to_registration.end()) {
            const std::vector<registration_t> &regs = where->second;
            for (size_t i=0; i < regs.size(); i++) {
                result.push_back(regs.at(i).description);
            }
        }
        return result;
    }
};
static doc_register_t default_register;

void docopt_register_description(const wcstring &cmd, const wcstring &name, const wcstring &description) {
    default_register.register_description(cmd, name, description);
}

wcstring_list_t docopt_copy_registered_descriptions(const wcstring &cmd) {
    return default_register.copy_registered_descriptions(cmd);
}
