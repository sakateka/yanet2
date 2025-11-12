#pragma once

#include "controlplane/config/zone.h"

#include "filter/filter.h"
#include "lib/fwstate/config.h"

// FIXME: make the structure private?
struct acl_module_config {
	struct cp_module cp_module;

	struct fwstate_config fwstate_cfg;
	struct filter filter;
};
