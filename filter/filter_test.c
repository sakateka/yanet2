#include "ipfw.h"

#include <stdlib.h>

#include <endian.h>

int
main(int argc, char **argv)
{
	(void) argc;
	(void) argv;

	struct filter_action *actions;

	actions = (struct filter_action *)
		malloc(sizeof(struct filter_action) * 2);

	actions[0].net6.src_count = 2;
	actions[0].net6.srcs = (struct net6 *)
		malloc(sizeof(struct net6) * 2);
	actions[0].net6.srcs[0] =
		(struct net6){0, 0, 0x00000000000000C0, 0};
	actions[0].net6.srcs[1] =
		(struct net6){0x80, 0, 0x0000000000000080, 0};
	actions[0].net6.dst_count = 1;
	actions[0].net6.dsts = (struct net6 *)
		malloc(sizeof(struct net6) * 1);
	actions[0].net6.dsts[0] =
		(struct net6){0x0000000000000080, 0, 0x0000000000000080, 0};

	actions[0].transport.src_count = 1;
	actions[0].transport.srcs = (struct filter_port_range *)
		malloc(sizeof(struct filter_port_range) * 1);
	actions[0].transport.srcs[0] =
		(struct filter_port_range){0, 65535};

	actions[0].transport.dst_count = 2;
	actions[0].transport.dsts = (struct filter_port_range *)
		malloc(sizeof(struct filter_port_range) * 2);
	actions[0].transport.dsts[0] =
		(struct filter_port_range){htobe16(80), htobe16(80)};
	actions[0].transport.dsts[1] =
		(struct filter_port_range){htobe16(443), htobe16(443)};

	actions[0].action = 0;


	actions[1].net6.src_count = 1;
	actions[1].net6.srcs = (struct net6 *)
		malloc(sizeof(struct net6) * 1);
	actions[1].net6.srcs[0] =
		(struct net6){0, 0, 0, 0};
	actions[1].net6.dst_count = 1;
	actions[1].net6.dsts = (struct net6 *)
		malloc(sizeof(struct net6) * 1);
	actions[1].net6.dsts[0] =
		(struct net6){0, 0, 0, 0};

	actions[1].transport.src_count = 1;
	actions[1].transport.srcs = (struct filter_port_range *)
		malloc(sizeof(struct filter_port_range) * 1);
	actions[1].transport.srcs[0] =
		(struct filter_port_range){0, 65535};

	actions[1].transport.dst_count = 1;
	actions[1].transport.dsts = (struct filter_port_range *)
		malloc(sizeof(struct filter_port_range) * 1);
	actions[1].transport.dsts[0] =
		(struct filter_port_range){0, 65535};

	actions[1].action = 1;


	struct filter_compiler compiler;

	filter_compiler_init(&compiler, actions, 2);

	return 0;
}
