#ifndef DATA_PIPE_H
#define DATA_PIPE_H

struct data_pipe {
	volatile size_t *w_pos;
	volatile size_t *r_pos;
	volatile size_t *f_pos;

	void **data;
	size_t size;
};


typedef size_t (*data_pipe_handle_func)(
	void **item,
	size_t count,
	void *data);

static inline size_t
data_pipe_ring_handle(
	volatile size_t *from_pos,
	volatile size_t *to_pos,
	void **data,
	size_t size,
	data_pipe_handle_func handle_func,
	void *handle_func_data)
{
	size_t from = *from_pos;
	size_t to = *to_pos;

	size_t available = (size + to - from) % size;
	from %= size;
	/*
	 * Branch-less code: the first part is 1 if and only if we wrap
	 * around the ring size whereas the second part is size of the ring
	 * overflow.
	 */
	available -=
		(from + available) / size *
		(from + available - size);

	size_t handled = handle_func(
		data + from,
		available,
		handle_func_data);

	//FIXME write fence

	*from_pos += handled;

	return handled;
}


static inline size_t
data_pipe_push(
	struct data_pipe *data_pipe,
	data_pipe_handle_func push_func,
	void *push_func_data)
{
	return data_pipe_ring_handle(
		data_pipe->w_pos,
		data_pipe->f_pos,
		data_pipe->data,
		data_pipe->size,
		push_func,
		push_func_data);
}

static inline size_t
data_pipe_pop(
	struct data_pipe *data_pipe,
	data_pipe_handle_func pop_func,
	void *pop_func_data)
{
	return data_pipe_ring_handle(
		data_pipe->r_pos,
		data_pipe->w_pos,
		data_pipe->data,
		data_pipe->size,
		pop_func,
		pop_func_data);
}

static inline size_t
data_pipe_free(
	struct data_pipe *data_pipe,
	data_pipe_handle_func free_func,
	void *free_func_data)
{
	return data_pipe_ring_handle(
		data_pipe->f_pos,
		data_pipe->r_pos,
		data_pipe->data,
		data_pipe->size,
		free_func,
		free_func_data);
}

#endif
