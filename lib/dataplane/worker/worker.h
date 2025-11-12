#include "dataplane/config/zone.h"

struct packet *
worker_packet_alloc(struct dp_worker *worker);

void
worker_packet_free(struct packet *packet);
