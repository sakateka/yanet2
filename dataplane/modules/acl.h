#ifndef ACL_H
#define ACL_H

#include "module.h"

#include "filter/ipfw.h"

// FIXME: make the structure private?
struct acl_module_config {
	struct module_config config;

	struct filter_compiler filter;
};

// FIXME: make the structure private
struct acl_module {
	struct module module;
};

struct module *
new_module_acl();

#endif
