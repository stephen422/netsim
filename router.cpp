#include "router.h"
#include "queue.h"
#include "stb_ds.h"
#include <cassert>

static void dprintf(Router *r, const char *fmt, ...)
{
    printf("[@%3ld] [", r->eventq.curr_time());
    print_id(r->id);
    printf("] ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

static Event tick_event_from_id(Id id)
{
    return Event{id, [](Router &r) { r.tick(); }};
}

Channel::Channel(EventQueue &eq, const long dl, const Connection conn)
    : conn(conn), eventq(eq), delay(dl)
{
}

void Channel::put(Flit *flit)
{
    buf.push_back({eventq.curr_time() + delay, flit});
    eventq.reschedule(delay, tick_event_from_id(conn.dst.id));
}

void Channel::put_credit(const Credit &credit)
{
    buf_credit.push_back({eventq.curr_time() + delay, credit});
    eventq.reschedule(delay, tick_event_from_id(conn.src.id));
}

std::optional<Flit *> Channel::get()
{
    auto front = buf.cbegin();
    if (!buf.empty() && eventq.curr_time() >= front->first) {
        assert(eventq.curr_time() == front->first && "stagnant flit!");
        Flit *flit = front->second;
        buf.pop_front();
        return flit;
    } else {
        return {};
    }
}

std::optional<Credit> Channel::get_credit()
{
    auto front = buf_credit.cbegin();
    if (!buf_credit.empty() && eventq.curr_time() >= front->first) {
        assert(eventq.curr_time() == front->first && "stagnant flit!");
        buf_credit.pop_front();
        return front->second;
    } else {
        return {};
    }
}

void print_conn(const char *name, Connection conn)
{
    printf("%s: %d.%d.%d -> %d.%d.%d\n", name, conn.src.id.type,
           conn.src.id.value, conn.src.port, conn.dst.id.type,
           conn.dst.id.value, conn.dst.port);
}

Topology topology_create(void)
{
    return (Topology){
        .forward_hash = NULL,
        .reverse_hash = NULL,
    };
}

void topology_destroy(Topology *top)
{
    hmfree(top->forward_hash);
    hmfree(top->reverse_hash);
}

bool topology_connect(Topology *t, RouterPortPair input, RouterPortPair output)
{
    size_t inkey = RPHASH(&input);
    size_t outkey = RPHASH(&output);
    if (hmgeti(t->forward_hash, inkey) >= 0 ||
        hmgeti(t->reverse_hash, outkey) >= 0)
        // Bad connectivity: source or destination port is already connected
        return false;
    int uniq = hmlen(t->forward_hash);
    Connection conn = (Connection){.src = input, .dst = output, .uniq = uniq};
    hmput(t->forward_hash, inkey, conn);
    hmput(t->reverse_hash, outkey, conn);
    assert(hmgeti(t->forward_hash, inkey) >= 0);
    return true;
}

bool topology_connect_terminals(Topology *t, const int *ids)
{
    bool res = true;
    for (int i = 0; i < arrlen(ids); i++) {
        RouterPortPair src_port{src_id(ids[i]), 0};
        RouterPortPair dst_port{dst_id(ids[i]), 0};
        RouterPortPair rtr_port{rtr_id(ids[i]), 0};

        // Bidirectional channel
        res &= topology_connect(t, src_port, rtr_port);
        res &= topology_connect(t, rtr_port, dst_port);
        if (!res)
            return false;
    }
    return true;
}

// Port usage: 0:terminal, 1:counter-clockwise, 2:clockwise
bool topology_connect_ring(Topology *t, const int *ids)
{
    bool res = true;
    for (long i = 0; i < arrlen(ids); i++) {
        int l = ids[i];
        int r = ids[(i + 1) % arrlen(ids)];
        RouterPortPair lport{rtr_id(l), 2};
        RouterPortPair rport{rtr_id(r), 1};

        // Bidirectional channel
        res &= topology_connect(t, lport, rport);
        res &= topology_connect(t, rport, lport);
        if (!res)
            return false;
    }
    return true;
}

Topology topology_ring(int n)
{
    Topology top = topology_create();
    int *ids = NULL;
    bool res = true;

    for (int id = 0; id < n; id++)
        arrput(ids, id);

    // Inter-router channels
    res &= topology_connect_ring(&top, ids);
    // Terminal node channels
    res &= topology_connect_terminals(&top, ids);
    assert(res);

    arrfree(ids);
    return top;
}

Connection conn_find_forward(Topology *t, RouterPortPair out_port)
{
    size_t key = RPHASH(&out_port);
    ptrdiff_t idx = hmgeti(t->forward_hash, key);
    if (idx == -1)
        return not_connected;
    else
        return t->forward_hash[idx].value;
}

Connection conn_find_reverse(Topology *t, RouterPortPair in_port)
{
    size_t key = RPHASH(&in_port);
    ptrdiff_t idx = hmgeti(t->reverse_hash, key);
    if (idx == -1)
        return not_connected;
    else
        return t->reverse_hash[idx].value;
}

Flit *flit_create(FlitType t, int src, int dst, long p)
{
    Flit *flit = (Flit *)malloc(sizeof(Flit));
    RouteInfo ri = {src, dst, {}, 0};
    *flit = (Flit){.type = t, .route_info = ri, .payload = p};
    return flit;
}

void flit_destroy(Flit *flit)
{
    if (flit->route_info.path)
        arrfree(flit->route_info.path);
    free(flit);
}

void print_flit(const Flit *flit)
{
    printf("{%d.p%ld}", flit->route_info.src, flit->payload);
}

static InputUnit input_unit_create(int bufsize)
{
    Flit **buf = NULL;
    queue_init(buf, bufsize * 2);
    return (InputUnit){
        .global = STATE_IDLE,
        .next_global = STATE_IDLE,
        .route_port = -1,
        .output_vc = 0,
        .stage = PIPELINE_IDLE,
        .buf = buf,
        .st_ready = NULL,
    };
}

static OutputUnit output_unit_create(int bufsize)
{
    return (OutputUnit){
        .global = STATE_IDLE,
        .next_global = STATE_IDLE,
        .input_port = -1,
        .input_vc = 0,
        .credit_count = bufsize,
        .buf_credit = std::optional<Credit>{},
    };
}

Router::Router(EventQueue &eq, Alloc *fa, Stat &st, TopoDesc td, Id id_,
               int radix, const std::vector<Channel *> &in_chs,
               const std::vector<Channel *> &out_chs)
    : id(id_), eventq(eq), flit_allocator(fa), stat(st), top_desc(td),
      tick_event(tick_event_from_id(id_)), input_channels(in_chs),
      output_channels(out_chs), va_last_grant_input(0), sa_last_grant_input(0)
{
    // input_units = NULL;
    // output_units = NULL;
    for (int port = 0; port < radix; port++) {
        InputUnit iu = input_unit_create(input_buf_size);
        OutputUnit ou = output_unit_create(input_buf_size);
        input_units.push_back(iu);
        output_units.push_back(ou);
        // arrput(input_units, iu);
        // arrput(output_units, ou);
    }

    if (is_src(id) || is_dst(id)) {
        assert(input_units.size() == 1);
        assert(output_units.size() == 1);
        // assert(arrlen(input_units) == 1);
        // assert(arrlen(output_units) == 1);
        input_units[0].route_port = 0;
        output_units[0].input_port = 0;
    }
}

Router::~Router()
{
    for (int port = 0; port < get_radix(); port++) {
        queue_free(input_units[port].buf);
    }
}

void Router::do_reschedule()
{
    if (reschedule_next_tick && eventq.curr_time() != last_reschedule_tick) {
        eventq.reschedule(1, tick_event);
        // dbg() << "self-rescheduled to " << eventq.curr_time() + 1 <<
        // std::endl;
        // XXX: Hacky!
        last_reschedule_tick = eventq.curr_time();
    }
}

// Returns an stb array containing the series of routed output ports.
int *source_route_compute(TopoDesc td, int src_id, int dst_id)
{
    int *path = NULL;
    int total = td.k;
    int cw_dist = (dst_id - src_id + total) % total;
    if (cw_dist <= total / 2) {
        // Clockwise
        for (int i = 0; i < cw_dist; i++)
            arrput(path, 2);
        arrput(path, 0);
    } else {
        // Counterclockwise
        // TODO: if CW == CCW, pick random
        for (int i = 0; i < total - cw_dist; i++)
            arrput(path, 1);
        arrput(path, 0);
    }
    printf("Source route computation: %d -> %d : {", src_id, dst_id);
    for (long i = 0; i < arrlen(path); i++) {
        printf("%d,", path[i]);
    }
    printf("}\n");
    return path;
}

void Router::tick()
{
    // Make sure this router has not been already ticked in this cycle.
    if (eventq.curr_time() == last_tick) {
        // dbg() << "WARN: double tick! curr_time=" << eventq.curr_time()
        //       << ", last_tick=" << last_tick << std::endl;
        stat.double_tick_count++;
        return;
    }
    // assert(eventq.curr_time() != last_tick);

    reschedule_next_tick = false;

    // Different tick actions for different types of node.
    if (is_src(id)) {
        source_generate();
        // Source nodes also needs to manage credit in order to send flits at
        // the right time.
        credit_update();
        fetch_credit();
    } else if (is_dst(id)) {
        destination_consume();
        fetch_flit();
    } else {
        // Process each pipeline stage.
        // Stages are processed in reverse dependency order to prevent coherence
        // bug.  E.g., if a flit succeeds in route_compute() and advances to the
        // VA stage, and then vc_alloc() is called, it would then get processed
        // again in the same cycle.
        switch_traverse();
        switch_alloc();
        vc_alloc();
        route_compute();
        credit_update();
        fetch_credit();
        fetch_flit();

        // Self-tick autonomously unless all input ports are empty.
        // FIXME: redundant?
        // bool empty = true;
        // for (int i = 0; i < get_radix(); i++) {
        //     if (!input_units[i].buf.empty()) {
        //         empty = false;
        //         break;
        //     }
        // }
        // if (!empty) {
        //     mark_reschedule();
        // }
    }

    // Update the global state of each input/output unit.
    update_states();

    // Do the rescheduling at here once to prevent flooding the event queue.
    do_reschedule();

    last_tick = eventq.curr_time();
}

///
/// Pipeline stages
///

void Router::source_generate()
{
    OutputUnit *ou = &output_units[0];

    if (ou->credit_count <= 0) {
        dprintf(this, "Credit stall!\n");
        return;
    }

    // Handle flit_h = zalloc(flit_allocator);
    // Flit *flit = zptr(flit_h);
    Flit *flit = flit_create(FLIT_BODY, id.value, (id.value + 2) % 4,
                             flit_payload_counter);
    if (flit_payload_counter == 0) {
        flit->type = FLIT_HEAD;
        flit->route_info.path = source_route_compute(
            top_desc, flit->route_info.src, flit->route_info.dst);
        flit_payload_counter++;
    } else if (flit_payload_counter == 3 /* FIXME */) {
        flit->type = FLIT_TAIL;
        flit_payload_counter = 0;
    } else {
        flit_payload_counter++;
    }

    assert(get_radix() == 1);
    Channel *out_ch = output_channels[0];
    out_ch->put(flit);

    dprintf(this, "Credit decrement, credit=%d->%d\n", ou->credit_count,
            ou->credit_count - 1);
    ou->credit_count--;
    assert(ou->credit_count >= 0);

    flit_gen_count++;

    dprintf(this, "Flit created and sent: ");
    print_flit(flit);
    printf("\n");

    // TODO: for now, infinitely generate flits.
    mark_reschedule();
}

void Router::destination_consume()
{
    InputUnit *iu = &input_units[0];

    if (!queue_empty(iu->buf)) {
        Flit *flit = queue_front(iu->buf);
        dprintf(this, "Destination buf size=%zd\n", queue_len(iu->buf));
        dprintf(this, "Flit arrived: ");
        print_flit(flit);
        printf("\n");
        flit_destroy(flit);

        flit_arrive_count++;
        queue_pop(iu->buf);
        assert(queue_empty(iu->buf));

        Channel *in_ch = input_channels[0];
        in_ch->put_credit(Credit{});

        auto src_pair = in_ch->conn.src;
        dprintf(this, "Credit sent to {");
        print_id(src_pair.id);
        printf(", %d}\n", src_pair.port);

        // Self-tick autonomously unless all input ports are empty.
        mark_reschedule();
    }
}

void Router::fetch_flit()
{
    for (int iport = 0; iport < get_radix(); iport++) {
        Channel *ich = input_channels[iport];
        InputUnit *iu = &input_units[iport];
        auto flit_opt = ich->get();

        if (flit_opt) {
            dprintf(this, "Fetched flit ");
            Flit *flit = flit_opt.value();
            print_flit(flit);
            printf(", buf.size()=%zd\n", queue_len(iu->buf));

            // If the buffer was empty, this is the only place to kickstart the
            // pipeline.
            if (queue_empty(iu->buf)) {
                dprintf(this, "fetch_flit: buf was empty\n");
                // If the input unit state was also idle (empty != idle!), set
                // the stage to RC.
                if (iu->next_global == STATE_IDLE) {
                    // Idle -> RC transition
                    iu->next_global = STATE_ROUTING;
                    iu->stage = PIPELINE_RC;
                }

                mark_reschedule();
            }

            queue_put(iu->buf, flit);

            assert((size_t)queue_len(iu->buf) <= input_buf_size &&
                   "Input buffer overflow!");
        }
    }
}

void Router::fetch_credit()
{
    for (int oport = 0; oport < get_radix(); oport++) {
        OutputUnit *ou = &output_units[oport];
        Channel *och = output_channels[oport];
        auto credit_opt = och->get_credit();

        if (credit_opt) {
            dprintf(this, "Fetched credit, oport=%d\n", oport);
            ou->buf_credit = credit_opt.value();
            mark_reschedule();
        }
    }
}

void Router::credit_update()
{
    for (int oport = 0; oport < get_radix(); oport++) {
        OutputUnit *ou = &output_units[oport];
        if (ou->buf_credit) {
            dprintf(this, "Credit update! credit=%d->%d (oport=%d)\n",
                    ou->credit_count, ou->credit_count + 1, oport);
            // Upon credit update, the input and output unit receiving this
            // credit may or may not be in the CreditWait state.  If they are,
            // make sure to switch them back to the active state so that they
            // can proceed in the SA stage.
            //
            // This can otherwise be implemented in the SA stage itself,
            // switching the stage to Active and simultaneously commencing to
            // the switch allocation.  However, this implementation seems to
            // defeat the purpose of the CreditWait stage. This implementation
            // is what I think of as a more natural one.
            assert(ou->input_port != -1); // XXX: redundant?
            InputUnit *iu = &input_units[ou->input_port];
            if (ou->credit_count == 0) {
                if (ou->next_global == STATE_CREDWAIT) {
                    assert(iu->next_global == STATE_CREDWAIT);
                    iu->next_global = STATE_ACTIVE;
                    ou->next_global = STATE_ACTIVE;
                }
                mark_reschedule();
                dprintf(this, "credit update with kickstart! (iport=%d)\n",
                        ou->input_port);
            } else {
                dprintf(this, "credit update, but no kickstart (credit=%d)\n",
                        ou->credit_count);
            }

            ou->credit_count++;
            ou->buf_credit.reset();
        } else {
            // dbg() << "No credit update, oport=" << oport << std::endl;
        }
    }
}

void Router::route_compute()
{
    for (int port = 0; port < get_radix(); port++) {
        InputUnit *iu = &input_units[port];

        if (iu->global == STATE_ROUTING) {
            Flit *flit = queue_front(iu->buf);
            dprintf(this, "Route computation: ");
            print_flit(flit);
            printf("\n");
            assert(!queue_empty(iu->buf));

            // TODO: Simple algorithmic routing: keep rotating clockwise until
            // destination is met.
            // if (flit.route_info.dst == std::get<RtrId>(id).id) {
            //     // Port 0 is always connected to a terminal node
            //     iu->route_port = 0;
            // } else {
            //     int total = 4; /* FIXME: hardcoded */
            //     int cw_dist =
            //         (flit.route_info.dst - flit.route_info.src + total) %
            //         total;
            //     if (cw_dist <= total / 2) {
            //         // Clockwise is better
            //         iu->route_port = 2;
            //     } else {
            //         // TODO: if CW == CCW, pick random
            //         iu->route_port = 1;
            //     }
            // }

            assert(flit->route_info.idx < arrlenu(flit->route_info.path));
            dprintf(this, "RC: path size = %zd\n",
                    arrlen(flit->route_info.path));
            iu->route_port = flit->route_info.path[flit->route_info.idx];
            dprintf(this, "RC success for ");
            print_flit(flit);
            printf(" (idx=%zu, oport=%d)\n", flit->route_info.idx,
                   iu->route_port);
            flit->route_info.idx++;

            // RC -> VA transition
            iu->next_global = STATE_VCWAIT;
            iu->stage = PIPELINE_VA;
            mark_reschedule();
        }
    }
}

// This function expects the given output VC to be in the Idle state.
int Router::vc_arbit_round_robin(int out_port)
{
    // Debug: print contenders
    int *v = NULL;
    for (int i = 0; i < get_radix(); i++) {
        InputUnit *iu = &input_units[i];
        if (iu->global == STATE_VCWAIT && iu->route_port == out_port)
            arrput(v, i);
    }
    if (arrlen(v)) {
        dprintf(this, "VA: competing for oport %d from iports {", out_port);
        for (int i = 0; i < arrlen(v); i++)
            printf("%d,", v[i]);
        printf("}\n");
    }
    arrfree(v);

    int iport = (va_last_grant_input + 1) % get_radix();
    for (int i = 0; i < get_radix(); i++) {
        InputUnit *iu = &input_units[iport];
        if (iu->global == STATE_VCWAIT && iu->route_port == out_port) {
            // XXX: is VA stage and VCWait state the same?
            assert(iu->stage == PIPELINE_VA);
            va_last_grant_input = iport;
            return iport;
        }
        iport = (iport + 1) % get_radix();
    }
    // Indicates that there was no request for this VC.
    return -1;
}

// This function expects the given output VC to be in the Idle state.
int Router::sa_arbit_round_robin(int out_port)
{
    int iport = (sa_last_grant_input + 1) % get_radix();

    for (int i = 0; i < get_radix(); i++) {
        InputUnit *iu = &input_units[iport];

        if (iu->stage == PIPELINE_SA && iu->route_port == out_port &&
            iu->global == STATE_ACTIVE) {
            // dbg() << "SA: granted oport " << out_port << " to iport " <<
            // iport
            //       << std::endl;
            sa_last_grant_input = iport;
            return iport;
        } else if (iu->stage == PIPELINE_SA && iu->route_port == out_port &&
                   iu->global == STATE_CREDWAIT) {
            dprintf(this, "Credit stall! port=%d\n", iu->route_port);
        }

        iport = (iport + 1) % get_radix();
    }

    // Indicates that there was no request for this VC.
    return -1;
}

void Router::vc_alloc()
{
    // dbg() << "VC allocation\n";

    for (int oport = 0; oport < get_radix(); oport++) {
        OutputUnit *ou = &output_units[oport];

        // Only do arbitration for inactive output VCs.
        if (ou->global == STATE_IDLE) {
            // Arbitration
            int iport = vc_arbit_round_robin(oport);

            if (iport == -1) {
                // dbg() << "no pending VC request for oport=" << oport <<
                // std::endl;
            } else {
                InputUnit *iu = &input_units[iport];

                dprintf(this, "VA: success for ");
                print_flit(queue_front(iu->buf));
                printf(" from iport %d to oport %d\n", iport, oport);

                // We now have the VC, but we cannot proceed to the SA stage if
                // there is no credit.
                if (ou->credit_count == 0) {
                    dprintf(this, "VA: no credit, switching to CreditWait\n");
                    iu->next_global = STATE_CREDWAIT;
                    ou->next_global = STATE_CREDWAIT;
                } else {
                    iu->next_global = STATE_ACTIVE;
                    ou->next_global = STATE_ACTIVE;
                }

                // Record the input port to the Output unit.
                ou->input_port = iport;

                iu->stage = PIPELINE_SA;
                mark_reschedule();
            }
        }
    }
}

void Router::switch_alloc()
{
    for (int oport = 0; oport < get_radix(); oport++) {
        OutputUnit *ou = &output_units[oport];

        // Only do arbitration for output VCs that has available credits.
        if (ou->global == STATE_ACTIVE) {
            // Arbitration
            int iport = sa_arbit_round_robin(oport);

            if (iport == -1) {
                // dbg() << "no pending SA request!\n";
            } else {
                // SA success!
                InputUnit *iu = &input_units[iport];

                dprintf(this, "SA success for ");
                print_flit(queue_front(iu->buf));
                printf(" from iport %d to oport %d\n", iport, oport);

                // Input units in the active state *may* be empty, e.g. if
                // their body flits have not yet arrived.  Check that.
                assert(!queue_empty(iu->buf));
                // if (iu->buf.empty()) {
                //     continue;
                // }

                // The flit leaves the buffer here.
                Flit *flit = queue_front(iu->buf);

                dprintf(this, "Switch allocation success: ");
                print_flit(flit);
                printf("\n");

                assert(iu->global == STATE_ACTIVE);
                queue_pop(iu->buf);

                assert(!iu->st_ready); // XXX: harsh
                assert(flit);
                iu->st_ready = flit;

                assert(ou->global == STATE_ACTIVE);

                // Credit decrement
                dprintf(this, "Credit decrement, credit=%d->%d (oport=%d)\n",
                        ou->credit_count, ou->credit_count - 1, oport);
                assert(ou->credit_count > 0);
                ou->credit_count--;

                // SA -> ?? transition
                //
                // Set the next stage according to the flit type and credit
                // count.
                //
                // Note that switching state to CreditWait does NOT prevent the
                // subsequent ST to happen. The flit that has succeeded SA on
                // this cycle is transferred to iu->st_ready, and that is the
                // only thing that is visible to the ST stage.
                if (flit->type == FLIT_TAIL) {
                    ou->next_global = STATE_IDLE;
                    if (queue_empty(iu->buf)) {
                        iu->next_global = STATE_IDLE;
                        iu->stage = PIPELINE_IDLE;
                        dprintf(this, "SA: next state is Idle\n");
                    } else {
                        iu->next_global = STATE_ROUTING;
                        iu->stage = PIPELINE_RC;
                        dprintf(this, "SA: next state is Routing\n");
                    }
                    mark_reschedule();
                } else if (ou->credit_count == 0) {
                    dprintf(this, "SA: switching to CW\n");
                    iu->next_global = STATE_CREDWAIT;
                    ou->next_global = STATE_CREDWAIT;
                    dprintf(this, "SA: next state is CreditWait\n");
                } else {
                    iu->next_global = STATE_ACTIVE;
                    iu->stage = PIPELINE_SA;
                    dprintf(this, "SA: next state is Active\n");
                    mark_reschedule();
                }
                assert(ou->credit_count >= 0);
            }
        }
    }
}

void Router::switch_traverse()
{
    for (int iport = 0; iport < get_radix(); iport++) {
        InputUnit *iu = &input_units[iport];

        if (iu->st_ready) {
            Flit *flit = iu->st_ready;
            iu->st_ready = NULL;
            dprintf(this, "Switch traverse: ");
            print_flit(flit);
            printf("\n");

            // No output speedup: there is no need for an output buffer
            // (Ch17.3).  Flits that exit the switch are directly placed on the
            // channel.
            Channel *out_ch = output_channels[iu->route_port];
            out_ch->put(flit);
            auto dst_pair = out_ch->conn.dst;
            dprintf(this, "Flit ");
            print_flit(flit);
            printf(" sent to {");
            print_id(dst_pair.id);
            printf(", %d}\n", dst_pair.port);

            // With output speedup:
            // auto &ou = output_units[iu->route_port];
            // ou->buf.push_back(flit);

            // CT stage: return credit to the upstream node.
            Channel *in_ch = input_channels[iport];
            in_ch->put_credit(Credit{});
            auto src_pair = in_ch->conn.src;
            dprintf(this, "Credit sent to {");
            print_id(src_pair.id);
            printf(", %d}\n", src_pair.port);
        }
    }
}

void Router::update_states()
{
    bool changed = false;

    for (int port = 0; port < get_radix(); port++) {
        InputUnit *iu = &input_units[port];
        OutputUnit *ou = &output_units[port];
        if (iu->global != iu->next_global) {
            iu->global = iu->next_global;
            changed = true;
        }
        if (ou->global != ou->next_global) {
            if (ou->next_global == STATE_CREDWAIT && ou->credit_count > 0)
                assert(false);
            ou->global = ou->next_global;
            changed = true;
        }
    }

    // Reschedule whenever there is one or more state change.
    if (changed)
        mark_reschedule();
}
