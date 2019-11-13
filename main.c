#include "sim.h"
#include "router.h"
#include "queue.h"

int main(int argc, char **argv) {
    int debug = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-d"))
            debug = 1;
    }

    Topology top = topology_ring(4);

    Sim sim = sim_create(debug, 4, 4, 3, top);
    schedule(&sim.eventq, 0, tick_event_from_id(src_id(0)));
    schedule(&sim.eventq, 0, tick_event_from_id(src_id(1)));
    schedule(&sim.eventq, 0, tick_event_from_id(src_id(2)));
    // sim.eventq.schedule(0, tick_event_from_id(src_id(3)));

    // sim_run(&sim, 10000);

    Topology top2 = topology_torus(4, 3);
    topology_destroy(&top2);

    sim_report(&sim);

    sim_destroy(&sim);
    topology_destroy(&top);

    return 0;
}