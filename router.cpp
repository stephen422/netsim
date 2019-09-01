#include "router.h"
#include <iostream>
#include <cassert>

Router::Router(EventQueue &eq, int id_, int radix,
               const std::vector<Topology::RouterPortPair> &dp)
    : eventq(eq), tick_event(id_, [](Router &r) { r.tick(); }),
      destination_ports(dp), id(id_) {
    std::cout << "Router::Router(id=" << id << ")\n";

    for (int port = 0; port < radix; port++) {
        input_units.emplace_back();
        output_units.emplace_back();
    }
}

void Router::put(int port, const Flit &flit) {
    std::cout << "Put!\n";
    auto &iu = input_units[port];

    // If the buffer was empty, set stage to RC, and kickstart the pipeline
    if (iu.buf.empty()) {
        // Idle -> RC transition
        iu.state.global = InputUnit::State::GlobalState::Routing;
        iu.stage = PipelineStage::RC;
        eventq.reschedule(1, tick_event);
    }

    iu.buf.push_back(flit);
}

void Router::tick() {
    // Make sure this router has not been already ticked in this cycle.
    assert(eventq.curr_time() != last_tick);
    reschedule_next_tick = false;

    std::cout << "Tick!\n";

    // Process each pipeline stage.
    // Stages are processed in reverse order to prevent coherence bug.  E.g.,
    // if a flit succeeds in route_compute() and advances to the VA stage, and
    // then vc_alloc() is called, it would then get processed again in the same
    // cycle.
    switch_traverse();
    switch_alloc();
    vc_alloc();
    route_compute();

    // Self-tick autonomously unless all input ports are empty.
    // FIXME: accuracy?
    bool empty = true;
    for (int i = 0; i < get_radix(); i++) {
        if (!input_units[i].buf.empty()) {
            empty = false;
        }
    }
    if (!empty) {
        reschedule_next_tick = true;
    }

    // Do the rescheduling at here once to prevent flooding the event queue.
    if (reschedule_next_tick) {
        eventq.reschedule(1, tick_event);
    }

    last_tick = eventq.curr_time();
}

///
/// Pipeline stages
///

void Router::route_compute() {
    for (int port = 0; port < get_radix(); port++) {
        auto &iu = input_units[port];
        if (iu.stage == PipelineStage::RC) {
            std::cout << "[" << port << "] route computation\n";
            assert(!iu.buf.empty());

            // TODO: simple routing: input port == output port
            iu.state.route = port;

            // RC -> VA transition
            iu.state.global = InputUnit::State::GlobalState::VCWait;
            iu.stage = PipelineStage::VA;
            reschedule_next_tick = true;
        }
    }
}

void Router::vc_alloc() {
    for (int port = 0; port < get_radix(); port++) {
        auto &iu = input_units[port];
        if (iu.stage == PipelineStage::VA) {
            std::cout << "[" << port << "] VC allocation\n";
            assert(!iu.buf.empty());

            // VA -> SA transition
            iu.state.global = InputUnit::State::GlobalState::Active;
            iu.stage = PipelineStage::SA;
            reschedule_next_tick = true;
        }
    }
}

void Router::switch_alloc() {
    for (int port = 0; port < get_radix(); port++) {
        auto &iu = input_units[port];
        if (iu.stage == PipelineStage::SA) {
            std::cout << "[" << port << "] switch allocation\n";
            assert(!iu.buf.empty());

            // SA -> ST transition
            iu.state.global = InputUnit::State::GlobalState::Active;
            iu.stage = PipelineStage::ST;
            reschedule_next_tick = true;
        }
    }
}

void Router::switch_traverse() {
    for (int port = 0; port < get_radix(); port++) {
        auto &iu = input_units[port];
        if (iu.stage == PipelineStage::ST) {
            std::cout << "[" << port << "] switch traverse\n";
            assert(!iu.buf.empty());

            Flit flit = iu.buf.front();
            iu.buf.pop_front();

            // No output speedup: there is no need for an output buffer
            // (Ch17.3).  Flits that exit the switch are directly placed on the
            // channel.
            auto dest = destination_ports[port];
            if (dest != Topology::not_connected) {
                // FIXME: link traversal time fixed to 1
                eventq.reschedule(1, Event{dest.first, [=](Router &r) {
                        r.put(dest.second, flit);
                        }});
            }

            // With output speedup:
            // auto &ou = output_units[iu.state.route];
            // ou.buf.push_back(flit);

            // ST -> ?? transition
            iu.state.global = InputUnit::State::GlobalState::Active;

            if (iu.buf.empty()) {
                iu.stage = PipelineStage::Idle;
            } else {
                // FIXME: what if the next flit is a head flit?
                reschedule_next_tick = true;
            }
        }
    }
}
