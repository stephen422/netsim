#ifndef ROUTER_H
#define ROUTER_H

#include "event.h"
#include "stb_ds.h"
#include <deque>
#include <iostream>
#include <map>
#include <optional>

struct Stat {
    long double_tick_count{0};
};

struct RouterPortPair {
  Id id;
  int port;
  bool operator<(const RouterPortPair &b) const
  {
    return (id < b.id) || (id == b.id && port < b.port);
  }
  bool operator==(const RouterPortPair &b) const
  {
    return id == b.id && port == b.port;
  }
};

struct Connection {
  RouterPortPair src;
  RouterPortPair dst;
  /* FIXME */
  bool operator<(const Connection &b) const
  {
    return (src < b.src) || (src == b.src && dst < b.dst);
  }
  bool operator==(const Connection &b) const
  {
    return src == b.src && dst == b.dst;
  }
};

struct ConnectionHash {
  size_t key;
  Connection value;
};

#define RPHASH(ptr) (stbds_hash_bytes(ptr, sizeof(RouterPortPair), 0))

static const Connection not_connected = (Connection){
    .src =
        (RouterPortPair){
            .id = (Id){.type = ID_RTR, .value = -1},
            .port = -1,
        },
    .dst =
        (RouterPortPair){
            .id = (Id){.type = ID_RTR, .value = -1},
            .port = -1,
        },
};

long id_hash(Id id);

// Encodes channel connectivity in a bidirectional map.
// Supports runtime checking for connectivity error.
class Topology
{
public:
  static Topology ring(int n);

  Topology() = default;
  Topology(std::initializer_list<std::pair<RouterPortPair, RouterPortPair>>);

  Connection find_forward(RouterPortPair out_port)
  {
    size_t key = RPHASH(&out_port);
    ptrdiff_t idx = hmgeti(forward_hash, key);
    if (idx == -1)
      return not_connected;
    else
      return forward_hash[idx].value;
  }

  Connection find_reverse(RouterPortPair in_port)
  {
    size_t key = RPHASH(&in_port);
    ptrdiff_t idx = hmgeti(reverse_hash, key);
    if (idx == -1)
      return not_connected;
    else
      return reverse_hash[idx].value;
  }

  bool connect(RouterPortPair src, RouterPortPair dst);
  bool connect_terminals(const std::vector<int> &ids);
  bool connect_ring(const std::vector<int> &ids);

  ConnectionHash *forward_hash{NULL};
  ConnectionHash *reverse_hash{NULL};
};

enum TopoType {
    TOP_TORUS,
    TOP_FCLOS,
};

struct TopoDesc {
    TopoType type;
    int k; // side length for tori
    int r; // dimension for tori
};

/// Source-side all-in-one route computation.
std::vector<int> source_route_compute(TopoDesc td, int src_id, int dst_id);

enum FlitType {
    FLIT_HEAD,
    FLIT_BODY,
    FLIT_TAIL,
};

struct RouteInfo {
  int src;    // source node ID
  int dst{3}; // destination node ID
  std::vector<int> path;
  size_t idx{0};
}; // only contained in the head flit

/// Flit and credit encoding.
/// Follows Fig. 16.13.
struct Flit
{
  Flit(FlitType t, int src, int dst, long p) : type(t), payload(p)
  {
    route_info.src = src;
    route_info.dst = dst;
  }

  FlitType type;
  RouteInfo route_info;
  long payload;
};

std::ostream &operator<<(std::ostream &out, const Flit &flit);

struct Credit {
  // VC is omitted, as we only have one VC per a physical channel.
};

struct Channel
{
  Channel(EventQueue &eq, const long dl, const RouterPortPair s,
          const RouterPortPair d);

  void put(const Flit &flit);
  void put_credit(const Credit &credit);
  std::optional<Flit> get();
  std::optional<Credit> get_credit();

  RouterPortPair src;
  RouterPortPair dst;

  EventQueue &eventq;
  const long delay;
  std::deque<std::pair<long, Flit>> buf{};
  std::deque<std::pair<long, Credit>> buf_credit{};
};

// Custom printer for Flit.
std::ostream &operator<<(std::ostream &out, const Flit &flit);

// Pipeline stages.
enum PipelineStage {
  PIPELINE_IDLE,
  PIPELINE_RC,
  PIPELINE_VA,
  PIPELINE_SA,
  PIPELINE_ST,
};

// Global states of each input/output unit.
enum GlobalState {
  STATE_IDLE,
  STATE_ROUTING,
  STATE_VCWAIT,
  STATE_ACTIVE,
  STATE_CREDWAIT,
};

struct InputUnit {
  GlobalState global;
  GlobalState next_global;
  int route_port;
  int output_vc;
  // credit count is omitted; it can be found in the output
  // units instead.
  PipelineStage stage;
  std::deque<Flit> buf;
  std::optional<Flit> st_ready;
};

struct OutputUnit {
  GlobalState global;
  GlobalState next_global;
  int input_port;
  int input_vc;
  int credit_count; // FIXME: hardcoded
  // std::deque<Flit> buf;
  std::optional<Credit> buf_credit;
};

/// A node.  Despite its name, it can represent any of a router node, a source
/// node and a destination node.
class Router
{
public:
  Router(EventQueue &eq, Stat &st, TopoDesc td, Id id, int radix,
         const std::vector<Channel *> &in_chs,
         const std::vector<Channel *> &out_chs);
  // Router::tick_event captures pointer to 'this' in the Router's
  // constructor. To prevent invalidating the 'this' pointer, we should disallow
  // moving/copying of Router.
  Router(const Router &) = delete;
  Router(Router &&) = default;

  // Tick event
  void tick();

  void source_generate();
  void destination_consume();
  void fetch_flit();
  void fetch_credit();
  void credit_update();
  void route_compute();
  void vc_alloc();
  void switch_alloc();
  void switch_traverse();
  void update_states();

  // Allocators and arbiters
  int vc_arbit_round_robin(int out_port);
  int sa_arbit_round_robin(int out_port);

  // Misc
  const Event &get_tick_event() const { return tick_event; }
  int get_radix() const { return input_units.size(); }

  // Debug output stream
  std::ostream &dbg() const;

  // Mark self-reschedule on the next tick
  void mark_reschedule() { reschedule_next_tick = true; }
  void do_reschedule();

public:
  Id id;                     // router ID
  long flit_arrive_count{0}; // # of flits arrived for the destination node
  long flit_gen_count{0};    // # of flits generated for the destination node

  EventQueue &eventq; // reference to the simulator-global event queue
  Stat &stat;
  const TopoDesc top_desc;
  const Event tick_event; // self-tick event.
  const size_t input_buf_size{100};
  long last_tick{-1}; // record the last tick time to prevent double-tick in
                      // single cycle
  long last_reschedule_tick{-1}; // XXX: hacky?
  long flit_payload_counter{0};  // for simple payload generation
  bool reschedule_next_tick{
      false}; // marks whether to self-tick at the next cycle

  // Pointers to the input/output channels for each port.
  std::vector<Channel *> input_channels;
  std::vector<Channel *> output_channels;
  // Input/output units.
  std::vector<InputUnit> input_units;
  std::vector<OutputUnit> output_units;

  // Allocator variables.
  int va_last_grant_input;
  int sa_last_grant_input;
};

#endif
