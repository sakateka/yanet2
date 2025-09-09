#include "agent.h"

#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

#include <errno.h>

#include "common/memory.h"
#include "common/strutils.h"

#include "controlplane/config/zone.h"
#include "dataplane/config/zone.h"

#include "api/agent.h"

#include <stdio.h>

struct yanet_shm *
yanet_shm_attach(const char *path) {
	int fd = open(path, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		return NULL;
	}

	struct stat stat;
	int rc = fstat(fd, &stat);
	if (rc == -1) {
		close(fd);
		return NULL;
	}

	void *ptr = mmap(
		NULL, stat.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0
	);
	close(fd);
	if (ptr == MAP_FAILED) {
		return NULL;
	}

	struct yanet_shm *shm = (struct yanet_shm *)ptr;
	return shm;
}

int
yanet_shm_detach(struct yanet_shm *shm) {
	// calculate total size of shared memory
	size_t size = 0;
	struct dp_config *dp_config = (struct dp_config *)shm;
	uint32_t instance_count = dp_config->instance_count;
	for (uint32_t instance_idx = 0; instance_idx < instance_count;
	     ++instance_idx) {
		size += dp_config->storage_size;
		dp_config = dp_config_nextk(dp_config, 1);
	}

	return munmap(shm, size);
}

struct dp_config *
yanet_shm_dp_config(struct yanet_shm *shm, uint32_t instance_idx) {
	return dp_config_nextk((struct dp_config *)shm, instance_idx);
}

uint32_t
yanet_shm_instance_count(struct yanet_shm *shm) {
	struct dp_config *dp_config = yanet_shm_dp_config(shm, 0);
	return dp_config->instance_count;
}

uint32_t
dataplane_instance_numa_idx(struct dp_config *dp_config) {
	return dp_config->numa_idx;
}

struct agent *
agent_attach(
	struct yanet_shm *shm,
	uint32_t instance_idx,
	const char *agent_name,
	size_t memory_limit
) {
	struct dp_config *dp_config = yanet_shm_dp_config(shm, instance_idx);

	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);

	cp_config_lock(cp_config);

	struct agent *new_agent = (struct agent *)memory_balloc(
		&cp_config->memory_context, sizeof(struct agent)
	);
	if (new_agent == NULL) {
		goto unlock;
	}
	memset(new_agent, 0, sizeof(struct agent));

	strtcpy(new_agent->name, agent_name, sizeof(new_agent->name));
	new_agent->memory_limit = memory_limit;
	SET_OFFSET_OF(&new_agent->dp_config, dp_config);
	SET_OFFSET_OF(&new_agent->cp_config, cp_config);
	new_agent->pid = getpid();

	struct cp_config_gen *config_gen = ADDR_OF(&cp_config->cp_config_gen);
	new_agent->gen = config_gen->gen;

	block_allocator_init(&new_agent->block_allocator);
	memory_context_init(
		&new_agent->memory_context,
		agent_name,
		&new_agent->block_allocator
	);

	/*
	 * FIXME: the code bellow tries to allocate memory_limit bytes
	 * using max possible chunk size what breaks allocator encapsulation.
	 * Alternative multi-alloc api should be implemented.
	 */
	uint64_t arena_count =
		(memory_limit + MEMORY_BLOCK_ALLOCATOR_MAX_SIZE - 1) /
		MEMORY_BLOCK_ALLOCATOR_MAX_SIZE;
	void **arenas = (void **)memory_balloc(
		&cp_config->memory_context, sizeof(void *) * arena_count
	);
	if (arenas == NULL) {
		agent_cleanup(new_agent);
		new_agent = NULL;
		goto unlock;
	}

	while (new_agent->arena_count < arena_count) {
		void *arena = memory_balloc(
			&cp_config->memory_context,
			MEMORY_BLOCK_ALLOCATOR_MAX_SIZE
		);
		if (arena == NULL) {
			agent_cleanup(new_agent);
			new_agent = NULL;
			goto unlock;
		}
		block_allocator_put_arena(
			&new_agent->block_allocator,
			arena,
			MEMORY_BLOCK_ALLOCATOR_MAX_SIZE
		);
		SET_OFFSET_OF(arenas + new_agent->arena_count, arena);
		new_agent->arena_count++;
	}
	SET_OFFSET_OF(&new_agent->arenas, arenas);

	struct cp_agent_registry *old_registry =
		ADDR_OF(&cp_config->agent_registry);
	bool found = false;
	for (uint64_t agent_idx = 0; agent_idx < old_registry->count;
	     ++agent_idx) {
		struct agent *old_agent =
			ADDR_OF(&old_registry->agents[agent_idx]);
		if (!strncmp(old_agent->name, agent_name, 80)) {
			found = true;
			SET_OFFSET_OF(
				&old_registry->agents[agent_idx], new_agent
			);
			SET_OFFSET_OF(&new_agent->prev, old_agent);
			break;
		}
	}

	if (!found) {
		new_agent->prev = NULL;
		struct cp_agent_registry *new_registry =
			(struct cp_agent_registry *)memory_balloc(
				&cp_config->memory_context,
				sizeof(struct cp_agent_registry) +
					(old_registry->count + 1) *
						sizeof(struct agent *)
			);
		if (new_registry == NULL) {
			agent_cleanup(new_agent);
			new_agent = NULL;
			goto unlock;
		}

		new_registry->count = old_registry->count + 1;
		for (uint64_t agent_idx = 0; agent_idx < old_registry->count;
		     ++agent_idx) {
			SET_OFFSET_OF(
				&new_registry->agents[agent_idx],
				ADDR_OF(&old_registry->agents[agent_idx])
			);
		}
		SET_OFFSET_OF(
			&new_registry->agents[new_registry->count - 1],
			new_agent
		);

		memory_bfree(
			&cp_config->memory_context,
			old_registry,
			sizeof(struct cp_agent_registry) +
				sizeof(struct agent *) * old_registry->count
		);

		SET_OFFSET_OF(&cp_config->agent_registry, new_registry);
	}

	struct agent *agent = new_agent;
	while (ADDR_OF(&agent->prev) != NULL) {
		struct agent *prev_agent = ADDR_OF(&agent->prev);

		if (prev_agent->loaded_module_count == 0) {
			SET_OFFSET_OF(&agent->prev, ADDR_OF(&prev_agent->prev));
			agent_cleanup(prev_agent);
			continue;
		}

		agent = ADDR_OF(&agent->prev);
	}

unlock:
	cp_config_unlock(cp_config);

	return new_agent;
}

void
agent_cleanup(struct agent *agent) {
	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);

	void **arenas = ADDR_OF(&agent->arenas);
	for (uint64_t arena_idx = 0; arena_idx < agent->arena_count;
	     ++arena_idx) {
		memory_bfree(
			&cp_config->memory_context,
			ADDR_OF(arenas + arena_idx),
			MEMORY_BLOCK_ALLOCATOR_MAX_SIZE
		);
	}
	memory_bfree(
		&cp_config->memory_context,
		arenas,
		sizeof(void *) * agent->arena_count
	);
	memory_bfree(&cp_config->memory_context, agent, sizeof(struct agent));
}

int
agent_detach(struct agent *agent) {
	(void)agent;
	// NOTE: Currently a no-op.
	return 0;
}

int
agent_update_modules(
	struct agent *agent, size_t module_count, struct cp_module **modules
) {
	int res = cp_config_update_modules(
		ADDR_OF(&agent->dp_config),
		ADDR_OF(&agent->cp_config),
		module_count,
		modules
	);

	agent_free_unused_modules(agent);
	agent_free_unused_agents(agent);

	return res;
}

int
agent_delete_module(
	struct agent *agent, const char *module_type, const char *module_name
) {
	struct dp_config *dp_config = ADDR_OF(&agent->dp_config);
	struct cp_config *cp_config = ADDR_OF(&agent->cp_config);

	uint64_t index;
	if (dp_config_lookup_module(dp_config, module_type, &index)) {
		return -1;
	}

	int res = cp_config_delete_module(
		dp_config, cp_config, index, module_name
	);

	agent_free_unused_modules(agent);
	agent_free_unused_agents(agent);

	return res;
}

int
agent_update_pipelines(
	struct agent *agent,
	size_t pipeline_count,
	struct pipeline_config *pipelines[]
) {
	return cp_config_update_pipelines(
		ADDR_OF(&agent->dp_config),
		ADDR_OF(&agent->cp_config),
		pipeline_count,
		pipelines
	);
}

int
agent_delete_pipeline(struct agent *agent, const char *pipeline_name) {
	return cp_config_delete_pipeline(
		ADDR_OF(&agent->dp_config),
		ADDR_OF(&agent->cp_config),
		pipeline_name
	);
}

struct pipeline_config *
pipeline_config_create(const char *name, uint64_t length) {
	struct pipeline_config *config = (struct pipeline_config *)malloc(
		sizeof(struct pipeline_config) +
		sizeof(struct module_config) * length
	);
	strtcpy(config->name, name, CP_PIPELINE_NAME_LEN);
	config->length = length;

	return config;
}

void
pipeline_config_free(struct pipeline_config *config) {
	free(config);
}

void
pipeline_config_set_module(
	struct pipeline_config *config,
	uint64_t index,
	const char *type,
	const char *name
) {
	strtcpy(config->modules[index].type,
		type,
		sizeof(config->modules[index].type));
	strtcpy(config->modules[index].name,
		name,
		sizeof(config->modules[index].name));
}

int
agent_update_devices(
	struct agent *agent,
	uint64_t device_count,
	struct cp_device_config *devices[]
) {
	return cp_config_update_devices(
		ADDR_OF(&agent->dp_config),
		ADDR_OF(&agent->cp_config),
		device_count,
		devices
	);
}

int
yanet_get_dp_module_info(
	struct dp_module_list_info *module_list,
	uint64_t index,
	struct dp_module_info *module_info
) {
	if (index >= module_list->module_count)
		return -1;
	*module_info = module_list->modules[index];
	return 0;
}

void
dp_module_list_info_free(struct dp_module_list_info *module_list_info) {
	free(module_list_info);
}

struct dp_module_list_info *
yanet_get_dp_module_list_info(struct dp_config *dp_config) {
	dp_config_lock(dp_config);

	struct dp_module_list_info *module_list_info =
		(struct dp_module_list_info *)malloc(
			sizeof(struct dp_module_list_info) +
			dp_config->module_count * sizeof(struct dp_module_info)
		);
	if (module_list_info == NULL)
		goto unlock;

	struct dp_module *modules = ADDR_OF(&dp_config->dp_modules);

	module_list_info->module_count = dp_config->module_count;
	for (uint64_t module_idx = 0; module_idx < dp_config->module_count;
	     ++module_idx) {
		strtcpy(module_list_info->modules[module_idx].name,
			modules[module_idx].name,
			sizeof(module_list_info->modules[module_idx].name));
	}

unlock:
	dp_config_unlock(dp_config);

	return module_list_info;
}

void
cp_module_list_info_free(struct cp_module_list_info *module_list_info) {
	free(module_list_info);
}

struct cp_module_list_info *
yanet_get_cp_module_list_info(struct dp_config *dp_config) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);

	struct cp_config_gen *config_gen = ADDR_OF(&cp_config->cp_config_gen);
	struct cp_module_registry *module_registry =
		&config_gen->module_registry;

	struct cp_module_list_info *module_list_info =
		(struct cp_module_list_info *)malloc(
			sizeof(struct cp_module_list_info) +
			sizeof(struct cp_module_info) *
				module_registry->registry.capacity
		);
	if (module_list_info == NULL)
		goto unlock;

	module_list_info->gen = config_gen->gen;
	module_list_info->module_count = 0;
	for (uint64_t module_idx = 0;
	     module_idx < module_registry->registry.capacity;
	     ++module_idx) {
		struct cp_module *cp_module =
			cp_config_gen_get_module(config_gen, module_idx);
		if (cp_module == NULL) {
			continue;
		}
		module_list_info->modules[module_list_info->module_count]
			.index = cp_module->type;
		strtcpy(module_list_info
				->modules[module_list_info->module_count]
				.config_name,
			cp_module->name,
			sizeof(module_list_info
				       ->modules[module_list_info->module_count]
				       .config_name));
		module_list_info->modules[module_list_info->module_count].gen =
			cp_module->gen;

		module_list_info->module_count += 1;
	}

unlock:
	cp_config_unlock(cp_config);

	return module_list_info;
}

int
yanet_get_cp_module_info(
	struct cp_module_list_info *module_list,
	uint64_t index,
	struct cp_module_info *module_info
) {
	if (index >= module_list->module_count)
		return -1;
	*module_info = module_list->modules[index];
	return 0;
}

void
cp_pipeline_list_info_free(struct cp_pipeline_list_info *pipeline_list_info) {
	for (uint64_t idx = 0; idx < pipeline_list_info->count; ++idx)
		free(pipeline_list_info->pipelines[idx]);
	free(pipeline_list_info);
}

struct cp_pipeline_list_info *
yanet_get_cp_pipeline_list_info(struct dp_config *dp_config) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);

	struct cp_config_gen *config_gen = ADDR_OF(&cp_config->cp_config_gen);
	struct registry *pipeline_registry =
		&config_gen->pipeline_registry.registry;

	struct cp_pipeline_list_info *pipeline_list_info =
		(struct cp_pipeline_list_info *)malloc(
			sizeof(struct cp_pipeline_list_info) +
			sizeof(struct cp_pipeline_info *) *
				pipeline_registry->capacity
		);
	if (pipeline_list_info == NULL)
		goto unlock;

	memset(pipeline_list_info,
	       0,
	       sizeof(struct cp_pipeline_list_info) +
		       sizeof(struct cp_pipeline_info *) *
			       pipeline_registry->capacity);
	for (uint64_t idx = 0; idx < pipeline_registry->capacity; ++idx) {
		struct cp_pipeline *cp_pipeline =
			cp_config_gen_get_pipeline(config_gen, idx);
		if (cp_pipeline == NULL) {
			continue;
		}

		struct cp_pipeline_info *pipeline_info =
			(struct cp_pipeline_info *)malloc(
				sizeof(struct cp_pipeline_info) +
				sizeof(uint64_t) * cp_pipeline->length
			);
		if (pipeline_info == NULL) {
			cp_pipeline_list_info_free(pipeline_list_info);
			pipeline_list_info = NULL;
			goto unlock;
		}

		strtcpy(pipeline_info->name,
			cp_pipeline->name,
			CP_PIPELINE_NAME_LEN);
		pipeline_info->length = cp_pipeline->length;
		for (uint64_t idx = 0; idx < cp_pipeline->length; ++idx) {
			pipeline_info->modules[idx] =
				cp_pipeline->modules[idx].index;
		}
		pipeline_list_info->pipelines[pipeline_list_info->count++] =
			pipeline_info;
	}

unlock:
	cp_config_unlock(cp_config);

	return pipeline_list_info;
}

int
yanet_get_cp_pipeline_info(
	struct cp_pipeline_list_info *pipeline_list_info,
	uint64_t index,
	struct cp_pipeline_info **pipeline_info
) {
	if (index >= pipeline_list_info->count)
		return -1;

	*pipeline_info = pipeline_list_info->pipelines[index];

	return 0;
}

int
yanet_get_cp_pipeline_module_info(
	struct cp_pipeline_info *pipeline_info,
	uint64_t index,
	uint64_t *config_index
) {
	if (index >= pipeline_info->length)
		return -1;

	*config_index = pipeline_info->modules[index];
	return 0;
}

void
cp_device_list_info_free(struct cp_device_list_info *device_list_info) {
	for (uint64_t idx = 0; idx < device_list_info->device_count; ++idx)
		free(device_list_info->devices[idx]);

	free(device_list_info);
}

static struct cp_device_info *
yanet_build_device_info(struct cp_device *device) {
	uint64_t prev_pipeline_id = -1;
	uint64_t pipeline_count = 0;
	for (uint64_t link_idx = 0; link_idx < device->pipeline_map_size;
	     ++link_idx) {
		if (prev_pipeline_id != device->pipeline_map[link_idx]) {
			pipeline_count++;
			prev_pipeline_id = device->pipeline_map[link_idx];
		}
	}

	size_t device_info_size =
		sizeof(struct cp_device_info) +
		sizeof(struct cp_device_pipeline_info) * pipeline_count;
	struct cp_device_info *device_info =
		(struct cp_device_info *)malloc(device_info_size);
	if (device_info == NULL) {
		return NULL;
	}
	memset(device_info, 0, device_info_size);

	device_info->pipeline_count = pipeline_count;
	strtcpy(device_info->name, device->name, CP_DEVICE_NAME_LEN);
	prev_pipeline_id = -1;
	pipeline_count = 0;
	for (uint64_t link_idx = 0; link_idx < device->pipeline_map_size;
	     ++link_idx) {
		if (prev_pipeline_id != device->pipeline_map[link_idx]) {
			pipeline_count++;
			prev_pipeline_id = device->pipeline_map[link_idx];
		}
		device_info->pipelines[pipeline_count - 1].pipeline_idx =
			device->pipeline_map[link_idx];
		device_info->pipelines[pipeline_count - 1].weight++;
	}

	return device_info;
}

struct cp_device_list_info *
yanet_get_cp_device_list_info(struct dp_config *dp_config) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);
	struct cp_config_gen *cp_config_gen =
		ADDR_OF(&cp_config->cp_config_gen);

	struct cp_device_registry *device_registry =
		&cp_config_gen->device_registry;

	size_t device_list_info_size =
		sizeof(struct cp_device_list_info) +
		sizeof(struct cp_device_info *) *
			device_registry->registry.capacity;
	struct cp_device_list_info *device_list_info =
		(struct cp_device_list_info *)malloc(device_list_info_size);
	if (device_list_info == NULL)
		goto unlock;

	memset(device_list_info, 0, device_list_info_size);
	device_list_info->gen = cp_config_gen->gen;
	device_list_info->device_count = 0;
	for (uint64_t idx = 0; idx < device_registry->registry.capacity;
	     ++idx) {
		struct cp_device *device =
			cp_config_gen_get_device(cp_config_gen, idx);
		if (device == NULL) {
			continue;
		}
		struct cp_device_info *device_info =
			yanet_build_device_info(device);
		if (device_info == NULL) {
			cp_device_list_info_free(device_list_info);
			device_list_info = NULL;
			goto unlock;
		}

		device_list_info->devices[device_list_info->device_count] =
			device_info;
		device_list_info->device_count++;
	}

unlock:
	cp_config_unlock(cp_config);

	return device_list_info;
}

struct cp_device_info *
yanet_get_cp_device_info(
	struct cp_device_list_info *device_list_info, uint64_t idx
) {
	if (idx >= device_list_info->device_count)
		return NULL;
	return device_list_info->devices[idx];
}

struct cp_device_pipeline_info *
yanet_get_cp_device_pipeline_info(
	struct cp_device_info *device_info, uint64_t idx
) {
	if (idx >= device_info->pipeline_count)
		return NULL;
	return device_info->pipelines + idx;
}

int
yanet_get_cp_agent_instance_info(
	struct cp_agent_info *agent_info,
	uint64_t index,
	struct cp_agent_instance_info **instance_info
) {
	if (index >= agent_info->instance_count) {
		errno = ERANGE;
		return -1;
	}

	*instance_info = agent_info->instances + index;

	return 0;
}

int
yanet_get_cp_agent_info(
	struct cp_agent_list_info *agent_list_info,
	uint64_t index,
	struct cp_agent_info **agent_info
) {
	if (index >= agent_list_info->count) {
		errno = ERANGE;
		return -1;
	}
	*agent_info = agent_list_info->agents[index];
	return 0;
}

void
cp_agent_list_info_free(struct cp_agent_list_info *agent_list_info) {
	for (uint64_t agent_idx = 0; agent_idx < agent_list_info->count;
	     ++agent_idx) {
		free(agent_list_info->agents[agent_idx]);
	}

	free(agent_list_info);
}

struct cp_agent_list_info *
yanet_get_cp_agent_list_info(struct dp_config *dp_config) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);

	struct cp_agent_registry *agent_registry =
		ADDR_OF(&cp_config->agent_registry);

	struct cp_agent_list_info *agent_list_info =
		(struct cp_agent_list_info *)malloc(
			sizeof(struct cp_agent_list_info) +
			sizeof(struct cp_agent_info *) * agent_registry->count
		);
	if (agent_list_info == NULL) {
		goto unlock;
	}
	agent_list_info->count = 0;

	for (uint64_t agent_idx = 0; agent_idx < agent_registry->count;
	     ++agent_idx) {
		struct agent *agent =
			ADDR_OF(&agent_registry->agents[agent_idx]);
		uint64_t instance_count = 1;
		struct agent *prev_agent = ADDR_OF(&agent->prev);
		while (prev_agent != NULL) {
			prev_agent = ADDR_OF(&prev_agent->prev);
			++instance_count;
		}

		struct cp_agent_info *agent_info = (struct cp_agent_info *)
			malloc(sizeof(struct cp_agent_info) +
			       sizeof(struct cp_agent_instance_info) *
				       instance_count);
		if (agent_info == NULL) {
			cp_agent_list_info_free(agent_list_info);
			agent_list_info = NULL;
			goto unlock;
		}

		strtcpy(agent_info->name, agent->name, sizeof(agent_info->name)
		);
		agent_info->instance_count = 0;
		while (agent_info->instance_count < instance_count) {
			struct cp_agent_instance_info *instance =
				agent_info->instances +
				agent_info->instance_count++;
			instance->pid = agent->pid;
			instance->memory_limit = agent->memory_limit;
			instance->allocated = agent->memory_context.balloc_size;
			instance->freed = agent->memory_context.bfree_size;
			instance->gen = agent->gen;
			agent = ADDR_OF(&agent->prev);
		}

		agent_list_info->agents[agent_list_info->count++] = agent_info;
	}

unlock:
	cp_config_unlock(cp_config);
	return agent_list_info;
}

struct cp_device_config *
cp_device_config_create(const char *name, uint64_t pipeline_count) {
	struct cp_device_config *config = (struct cp_device_config *)malloc(
		sizeof(struct cp_device_config) +
		sizeof(struct cp_pipeline_weight) * pipeline_count
	);

	if (config == NULL)
		return NULL;

	memset(config,
	       0,
	       sizeof(struct cp_device_config) +
		       sizeof(struct cp_pipeline_weight) * pipeline_count);
	strtcpy(config->name, name, CP_DEVICE_NAME_LEN);

	return config;
}

void
cp_device_config_free(struct cp_device_config *config) {
	free(config);
}

int
cp_device_config_add_pipeline(
	struct cp_device_config *device, const char *name, uint64_t weight
) {
	strtcpy(device->pipeline_weights[device->pipeline_weight_count].name,
		name,
		CP_PIPELINE_NAME_LEN);
	device->pipeline_weights[device->pipeline_weight_count].weight = weight;
	device->pipeline_weight_count += 1;

	return 0;
}

struct counter_handle_list *
yanet_get_pm_counters(
	struct dp_config *dp_config,
	const char *pipeline_name,
	const char *module_type,
	const char *module_name
) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);
	struct cp_config_gen *cp_config_gen =
		ADDR_OF(&cp_config->cp_config_gen);

	struct counter_registry *counter_registry;
	struct counter_storage *counter_storage;

	uint64_t module_type_index;
	if (dp_config_lookup_module(
		    dp_config, module_type, &module_type_index
	    )) {
		cp_config_unlock(cp_config);
		return NULL;
	}

	struct counter_storage *cs =
		cp_config_gen_get_pipeline_module_counter_storage(
			cp_config_gen,
			pipeline_name,
			module_type_index,
			module_name
		);

	if (cs == NULL) {
		cp_config_unlock(cp_config);
		return NULL;
	}
	counter_storage = cs;
	counter_registry = ADDR_OF(&counter_storage->registry);

	uint64_t count = counter_registry->count;
	struct counter_name *names = ADDR_OF(&counter_registry->names);

	cp_config_unlock(cp_config);

	struct counter_handle_list *list = (struct counter_handle_list *)malloc(
		sizeof(struct counter_handle_list) +
		sizeof(struct counter_handle) * count
	);

	if (list == NULL)
		return NULL;
	list->instance_count =
		ADDR_OF(&counter_storage->allocator)->instance_count;
	list->count = count;
	struct counter_handle *handlers = list->counters;

	for (uint64_t idx = 0; idx < count; ++idx) {
		strtcpy(handlers[idx].name, names[idx].name, 60);
		handlers[idx].size = names[idx].size;
		handlers[idx].gen = names[idx].gen;
		handlers[idx].value_handle =
			counter_get_value_handle(idx, counter_storage);
	}

	return list;
}

struct counter_handle_list *
yanet_get_pipeline_counters(
	struct dp_config *dp_config, const char *pipeline_name
) {
	struct cp_config *cp_config = ADDR_OF(&dp_config->cp_config);
	cp_config_lock(cp_config);
	struct cp_config_gen *cp_config_gen =
		ADDR_OF(&cp_config->cp_config_gen);

	struct counter_registry *counter_registry;
	struct counter_storage *counter_storage;

	struct counter_storage *cs = cp_config_gen_get_pipeline_counter_storage(
		cp_config_gen, pipeline_name
	);

	if (cs == NULL) {
		cp_config_unlock(cp_config);
		return NULL;
	}
	counter_storage = cs;
	counter_registry = ADDR_OF(&counter_storage->registry);

	uint64_t count = counter_registry->count;
	struct counter_name *names = ADDR_OF(&counter_registry->names);

	cp_config_unlock(cp_config);

	struct counter_handle_list *list = (struct counter_handle_list *)malloc(
		sizeof(struct counter_handle_list) +
		sizeof(struct counter_handle) * count
	);

	if (list == NULL)
		return NULL;
	list->instance_count =
		ADDR_OF(&counter_storage->allocator)->instance_count;
	list->count = count;
	struct counter_handle *handlers = list->counters;

	for (uint64_t idx = 0; idx < count; ++idx) {
		strtcpy(handlers[idx].name, names[idx].name, 60);
		handlers[idx].size = names[idx].size;
		handlers[idx].gen = names[idx].gen;
		handlers[idx].value_handle =
			counter_get_value_handle(idx, counter_storage);
	}

	return list;
}

struct counter_handle *
yanet_get_counter(struct counter_handle_list *counters, uint64_t idx) {
	if (idx >= counters->count)
		return NULL;
	return counters->counters + idx;
}

uint64_t
yanet_get_counter_value(
	struct counter_value_handle *value_handle,
	uint64_t value_idx,
	uint64_t worker_idx
) {
	return counter_handle_get_value(value_handle, worker_idx)[value_idx];
}

struct counter_handle_list *
yanet_get_worker_counters(struct dp_config *dp_config) {
	struct counter_registry *counter_registry = &dp_config->worker_counters;
	struct counter_storage *storage =
		ADDR_OF(&dp_config->worker_counter_storage);

	uint64_t count = counter_registry->count;
	struct counter_name *names = ADDR_OF(&counter_registry->names);

	struct counter_handle_list *list = (struct counter_handle_list *)malloc(
		sizeof(struct counter_handle_list) +
		sizeof(struct counter_handle) * count
	);

	if (list == NULL)
		return NULL;
	list->instance_count = ADDR_OF(&storage->allocator)->instance_count;
	list->count = count;
	struct counter_handle *handlers = list->counters;

	for (uint64_t idx = 0; idx < count; ++idx) {
		strtcpy(handlers[idx].name, names[idx].name, 60);
		handlers[idx].size = names[idx].size;
		handlers[idx].gen = names[idx].gen;
		handlers[idx].value_handle =
			counter_get_value_handle(idx, storage);
	}

	return list;
}

void
yanet_counter_handle_list_free(struct counter_handle_list *counters) {
	free(counters);
}

void
agent_free_unused_modules(struct agent *agent) {
	while (agent->unused_module != NULL) {
		struct cp_module *cp_module = ADDR_OF(&agent->unused_module);
		SET_OFFSET_OF(&agent->unused_module, ADDR_OF(&cp_module->prev));
		cp_module->free_handler(cp_module);
	}
}

void
agent_free_unused_agents(struct agent *agent) {
	if (agent == NULL) {
		return;
	}
	agent = ADDR_OF(&agent->prev);
	while (agent != NULL) {
		struct agent *prev_agent = ADDR_OF(&agent->prev);
		if (agent->loaded_module_count == 0) {
			agent_cleanup(agent);
		}
		agent = prev_agent;
	}
}