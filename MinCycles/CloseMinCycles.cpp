// -*- C++ -*-  Copyright (c) Microsoft Corporation; see license.txt
#include "MinCycles/CloseMinCycles.h"

#include <atomic>

#include "libHh/MeshOp.h"  // gather_boundary(), edge_signed_dihedral_angle(), etc.
#include "libHh/Pqueue.h"
#include "libHh/Queue.h"
#include "libHh/Random.h"
#include "libHh/RangeOp.h"  // reverse()
#include "libHh/Set.h"
#include "libHh/Stat.h"
#include "libHh/Timer.h"

namespace hh {

// TODO:
// - Introduce ability to find dual cycle, so that we can always fill tunnels, e.g. buddha.orig.m .

// History:
//
// Ideas considered to counter inefficiency of would_be_nonseparating_cycle():
// - Are we missing some obvious e_join() which could be obtained by some transitive closure?
// - Somehow examine/maintain/traverse the topology of the active BFS front?
//    No, the presence of topological handles depends on topology beyond the visited BFS region!
//    e.g. wrap around the base of a "cow tail" versus around a torus.
// - As before, avoid testing would_be_nonseparating_cycle() when updating pq, but only when adding an edge.
//    crucial: how to avoid replacing a cycle v_vtouch() by a non-cycle v_vtouch() with lower search radius?
//    Maybe only test would_be_nonseparating_cycle() in that particular situation?
//    No, it would have had to be tested for the first case already in the pq, and that is no longer possible.
// - Go back to an approximate solution where would_be_nonseparating_cycle() is only tested when removing a vertex
//    from pq rather than entering the touch event in the pq.  This will lead to small violations of lb.
//    This may fail to find some small cycles (e.g. 4-edge cycle in dragon3.nf1000.m).
// - Make would_be_nonseparating_cycle() be approximate when entering touch events -- declare a cycle when
//    double-bfs traverses a sufficient number of edges.  YES, this is the course taken.  It seems quite effective.
//
// Earlier dead-end approach: vertex-based "visited" labels during search:
//  - Much easier.
//  - Can obtain shortest cycles with a cost measuring the number of edges in the cycle,
//    by detecting when vertex Voronoi regions first touch.
//    (In [Eck et al 1995], the test was whether a tile was homeomorphic to a disk, which was an efficient local test.
//     However, here we need a nonlocal property -- whether the cycle spans a topological handle.)
//  However, we cannot get the shortest _Euclidean_ cycle.
//  The problem is that we require two fronts of visited vertices, and the vertex Voronoi regions corresponding to
//  these visited vertices no longer encode the search region topology.
//  So, we instead went with dual-edge "joined" labels during search.
//  These labels indicate which Vertex Voronoi regions are joined together.
//
//  Note: After finding shortest cycle, we could keep expanding the BFS for a while so as to update the lower-bound
//  of additional vertices.  No, this does not help because it does not raise the lower-bound above zero.
//
// Example tunnel:
// MinCycles ~/data/mesh/torus10x10.m -frac_offset .1 | Filtermesh -proc remove_filled_faces | G3d - -key Dm -st torus
// Example handle:
// MkApp torus | Filtermesh -froma3d -triang | MinCycles - -frac_offset .1 | Filtermesh -proc remove_filled_faces | G3d - -key Dm -st torus

namespace {

constexpr int verb = 0;  // Diagnostic verbosity; 0 or 1.
constexpr bool sdebug = false;

// Allow inexact would_be_nonseparating_cycle() -> lower bounds might be wrong, but fast.
constexpr bool k_touch_approx = true;

const Point k_boundary_point_far_away = thrice(1e10f);  // To topologically fill mesh boundaries.

// Remember to initialize these sacs, including in cycle closing operations:
HH_SAC_ALLOCATE_FUNC(Mesh::MVertex, float, v_dist);    // Distance from seed vertex during BFS, or BIGFLOAT.
HH_SAC_ALLOCATE_FUNC(Mesh::MVertex, Vertex, v_vprev);  // Previous vertex during BFS.
HH_SAC_ALLOCATE_FUNC(Mesh::MEdge, int, e_bfsnum);      // For secondary BFS search.

const FlagMask eflag_joined = Mesh::allocate_Edge_flag();  // Records whether Vertex-Voronoi-regions are joined.

bool difference_is_within_relative_eps(float a, float b, float eps) { return abs(a - b) / (max(a, b) + 1e-10f) < eps; }

}  // namespace

CloseMinCycles::CloseMinCycles(GMesh& mesh, Options options) : _mesh(mesh), _options(std::move(options)) {
  if (_options.frac_offset) {
    const Bbox bbox{transform(_mesh.vertices(), [&](Vertex v) { return _mesh.point(v); })};
    _offset_magnitude = _options.frac_offset * bbox.max_side();
  }
}

inline Flag CloseMinCycles::e_joined(Edge e) { return _mesh.flags(e).flag(eflag_joined); }

// Re-initialize v_dist() and e_joined() on the region that was just searched.
void CloseMinCycles::flood_reinitialize(Vertex vseed) {
  v_dist(vseed) = BIGFLOAT;
  Queue<Vertex> queue;
  queue.enqueue(vseed);
  while (!queue.empty()) {
    Vertex vc = queue.dequeue();
    ASSERTX(v_dist(vc) == BIGFLOAT);
    for (Edge e : _mesh.edges(vc)) {
      e_joined(e) = false;
      Vertex vn = _mesh.opp_vertex(vc, e);
      if (v_dist(vn) != BIGFLOAT) {
        v_dist(vn) = BIGFLOAT;
        queue.enqueue(vn);
      }
    }
  }
}

// Given a cycle of edges described by a loop of vertices vertex_loop, close the associated topological handle by
//  (1) duplicating each vertex along the cycle, and
//  (2) closing each of the resulting two boundaries with a fan of triangle faces.
// Return the cycle of new corresponding vertices (which is in reverse order from vertex_loop).
Array<Vertex> CloseMinCycles::close_cycle(const CArrayView<Vertex> vertex_loop) {
  assertx(vertex_loop.num() >= 3);
  // First, create the new vertices by splitting the old ones, creating two mesh boundaries.
  Array<Vertex> new_vertices;
  new_vertices.reserve(vertex_loop.num());
  for_int(i, vertex_loop.num()) {
    // Careful: note the mixed references to arrays vertex_loop and new_vertices.
    Vertex vo = vertex_loop[i];
    Vertex vl = i ? new_vertices[i - 1] : vertex_loop.last();
    Vertex vr = i + 1 < vertex_loop.num() ? vertex_loop[i + 1] : new_vertices[0];
    Vertex vn = _mesh.split_vertex(vo, vl, vr, 0);  // New vertex.
    new_vertices.push(vn);
    // Copy vertex info.
    _mesh.flags(vn) = _mesh.flags(vo);
    Point p = _mesh.point(vo);
    if (0) {
      p += Vector(0.f, .0005f, 0.f);  // Some arbitrary displacement for debug visualization.
      // Optionally, we could look at local scale of mesh, estimate surface normals, and displace appropriately.
    }
    _mesh.set_point(vn, p);
    _mesh.set_string(vn, _mesh.get_string(vo));
    // Note that all corner (and their strings) are preserved in split_vertex().
    v_dist(vn) = 0.f;  // It can be any value != BIGFLOAT.
    // Note: Vertex vn is not entered into pqvlbsr, because its corresponding old vertex is already there.
    for (Edge e : _mesh.edges(vn)) e_bfsnum(e) = 0;  // Note that e_joined(e) will be initialized shortly.
    // On corners and faces, all sacs, strings, and flags are preserved.
  }
  // Next, fill the two boundaries with create_face() and center_split().
  reverse(new_vertices);  // Topologically, we must close the ring of new vertices in the opposite direction.
  Vec2<float> sum_dih = twice(0.f);  // Sum of edge dihedral angles after closure, to estimate handle vs. tunnel.
  Vec2<Vertex> center_vertices;
  for_int(fi, 2) {
    CArrayView<Vertex> va = fi == 0 ? vertex_loop : new_vertices;
    Face fn = _mesh.create_face(va);
    // We must assign string information to the inner corners if defined on the outer corners.
    for (Corner c : _mesh.corners(fn)) {
      Corner c2 = _mesh.ccw_corner(c);  // Arbitrarily choose one of the two adjacent corners.
      _mesh.set_string(c, _mesh.get_string(c2));
    }
    Vertex vn = _mesh.center_split_face(fn);
    center_vertices[fi] = vn;
    v_dist(vn) = 0.f;  // It can be any value != BIGFLOAT.
    // Clean-up / initialize the edge data.
    for (Face f : _mesh.faces(vn))
      for (Edge e : _mesh.edges(f)) e_bfsnum(e) = 0;      // Note that e_joined(e) will be initialized shortly.
    for (Vertex v : _mesh.vertices(vn)) v_dist(v) = 0.f;  // It can be any value != BIGFLOAT.
    // Heuristically characterize the loop as a handle or tunnel based on the geometric embedding.
    for (Face f : _mesh.faces(vn)) {
      Edge e = _mesh.opp_edge(vn, f);
      // Face f2 = _mesh.opp_face(f, e);
      const float dih = edge_signed_dihedral_angle(_mesh, e);
      // SHOW(dih);
      sum_dih[fi] += dih;
    }
  }
  const bool is_handle = sum(sum_dih) > 0.f;  // Else it is a tunnel.
  (is_handle ? _total_handles : _total_tunnels) += 1;
  for (Vertex vn : center_vertices) {
    if (_options.mark_edges_sharp) {
      // For all edges in the two cycles after the closure, label them with the "sharp" attribute.
      for (Face f : _mesh.faces(vn)) {
        Edge e = _mesh.opp_edge(vn, f);
        _mesh.update_string(e, "sharp", "");
      }
    }
    if (_options.mark_faces_filled) {
      // For all faces in the two fans after the closure, label them with the "filled" attribute.
      for (Face f : _mesh.faces(vn)) {
        _mesh.update_string(f, "filled", "");
        const char* key = is_handle ? "handle" : "tunnel";
        _mesh.update_string(f, key, "");
      }
      // Label the two vertices at the centers of the face fans.
      _mesh.update_string(vn, "filledcenter", "");
    }
    if (_options.frac_offset) {
      const Vector normal = Vnors(_mesh, vn).unique_nor();
      const Vector offset_vector = normal * _offset_magnitude * (is_handle ? 1.f : -1.f);
      for (Vertex v : concatenate(V(vn), _mesh.vertices(vn))) _mesh.set_point(v, _mesh.point(v) + offset_vector);
    }
  }
  {
    float len = 0.f;
    for_int(i, vertex_loop.num()) len +=
        _mesh.length(_mesh.edge(vertex_loop[i], vertex_loop[(i + 1) % vertex_loop.num()]));
    showdf("Closing cycle: edges=%-3d length=%-12g is_handle=%d\n", vertex_loop.num(), len, is_handle);
    if (verb) showdf(" (dih0=%.1f dih1=%.1f)\n", sum_dih[0], sum_dih[1]);
  }
  return new_vertices;
}

// Given Edge e adjacent to two already BFS-visited vertices, determine if connecting them would form a
// nonseparating cycle.
bool CloseMinCycles::would_be_nonseparating_cycle(Edge e12, bool exact) {
  HH_STIMER("__would_be_nonseparating_cycle");
  assertx(v_dist(_mesh.vertex1(e12)) != BIGFLOAT && v_dist(_mesh.vertex2(e12)) != BIGFLOAT);
  assertx(!e_joined(e12));
  e_joined(e12) = true;  // This must be undone before function return if the cycle is non-separating.
  // If the resulting components of !e_joined edges starting from the left and right sides of e12 is connected,
  // then we have a non-separating cycle.
  // To quickly detect small dead-ends, we perform two simultaneous BFS.
  bool connected;
  {
    Vec2<Queue<Corner>> queues;  // Initialized with two opposing half-edges.
    queues[0].enqueue(_mesh.corner(_mesh.vertex2(e12), _mesh.face1(e12)));
    queues[1].enqueue(_mesh.corner(_mesh.vertex1(e12), _mesh.face2(e12)));
    static std::atomic<int> g_bfsnum{0};  // Threadsafe.
    const int bfsnum = (++g_bfsnum) * 2;
    Vec2<Set<Edge>> sets;  // For slower algorithm.
    int count = 0;
    connected = [&] {
      for (;;) {
        const int thresh_count = 50;
        dummy_use(exact);
        if (k_touch_approx && !exact && count > thresh_count) return true;  // Return from lambda.
        for_int(i, 2) {
          // We quickly terminate if either of the queues is empty, declaring connected == false.
          if (queues[i].empty()) return false;  // Return from lambda.
          Corner cc = queues[i].dequeue();
          // Consider the two opposite edges of the face.
          for (Corner c : {_mesh.clw_corner(cc), _mesh.clw_corner(_mesh.ccw_face_corner(cc))}) {
            Edge e = _mesh.clw_face_edge(c);
            if (1) {  // About 3X faster.
              if (e_joined(e)) {
                ASSERTX(e_bfsnum(e) != bfsnum + 0 && e_bfsnum(e) != bfsnum + 1);
                continue;
              }
              if (e_bfsnum(e) == bfsnum + i) continue;
              if (e_bfsnum(e) == bfsnum + (1 - i)) return true;  // Return from lambda.
              e_bfsnum(e) = bfsnum + i;
            } else {
              if (e_joined(e)) {
                ASSERTX(!sets[0].contains(e) && !sets[1].contains(e));
                continue;
              }
              if (!sets[i].add(e)) continue;
              if (sets[1 - i].contains(e)) return true;  // Return from lambda.
            }
            queues[i].enqueue(c);
          }
        }
      }
    }();
    HH_SSTAT(Scount, count);
    if (0) {
      Vertex v1 = _mesh.vertex1(e12), v2 = _mesh.vertex2(e12);
      showdf("would_be_nonseparating_cycle v_dist(%d)=%g v_dist(%d)=%g e12=%g exact=%d connected=%d count=%d\n",
             _mesh.vertex_id(v1), v_dist(v1), _mesh.vertex_id(v2), v_dist(v2), _mesh.length(e12), exact, connected,
             count);
    }
  }
  if (connected) {
    e_joined(e12) = false;
    // In principle, if the cycle is non-separating (i.e. connected == true),
    //   then we could find the shortest "non-separating dual cycle".
    // This would let us characterize the second "dimension" of this topological handle [Wood et al. 2004],
    //   as an additional criterion in deciding whether to close the cycle, and
    //   if so decide whether to close the minimal handle or the minimal tunnel.
    // The above double-BFS provides one approximation to this shortest dual cycle.
    // Ideally, it should not be constrained to pass near edge e12.
  } else {
    if (0) {
      // Idea: when !is_cycle, we could discard smaller component, except want v_dist for later culling.
      // No, the smaller component could still itself contain a handle, so nothing can be discarded.
    }
    if (1) {  // Join the leaf paths using a zippering process.
      // Optional optimization to quickly join other nearby edges in a greedy way,
      // to reduce unnecessary subsequent calls to look_for_cycle().
      int count = 0;
      Vec2<Array<Edge>> esides;
      for_int(i, 2) {
        for (Edge e : _mesh.edges(_mesh.face(e12, i)))
          if (!e_joined(e)) esides[i].push(e);
      }
      for_int(i, 2) {
        if (esides[i].num() != 1) continue;
        Queue<Edge> queue;
        queue.enqueue(esides[i][0]);
        while (queue.length() == 1) {
          Edge ec = queue.dequeue();
          assertx(!e_joined(ec));
          assertx(v_dist(_mesh.vertex1(ec)) != BIGFLOAT);
          assertx(v_dist(_mesh.vertex2(ec)) != BIGFLOAT);
          e_joined(ec) = true;
          ++count;
          for (Face f : _mesh.faces(ec))
            for (Edge e : _mesh.edges(f))
              if (!e_joined(e)) queue.enqueue(e);
        }
      }
      HH_SSTAT(Szipper, count);
      if (0) showdf("joined %d additional edges\n", count);
    }
  }
  return connected;
}

// We have reached vertex v1 from vertex v2, both of which have already been visited.
// Determine if the associated cycle is non-separating (i.e. spans a topological handle).
// If it is, optionally process the cycle.
// If was_a_nonseparating_cycle, return the num_edges.
std::optional<int> CloseMinCycles::look_for_cycle(Vertex v1, Vertex v2, bool process, float verify_dist) {
  HH_STIMER("__look_for_cycle");
  if (verb) Warning("Looking for cycle");
  Edge e12 = _mesh.edge(v1, v2);
  assertx(!e_joined(e12));
  if (!would_be_nonseparating_cycle(e12, true)) {
    if (verb) Warning("not a cycle");
    return {};
  }
  Array<Edge> ecycle;
  {
    Vec2<Array<Edge>> epath;
    epath[0].push(e12);
    for_int(i, 2) {
      for (Vertex v = _mesh.vertex(e12, i);;) {
        Vertex vn = v_vprev(v);
        if (!vn) break;
        Edge e = _mesh.edge(v, vn);
        epath[i].push(e);
        v = vn;
      }
    }
    if (!assertw(epath[1].num())) return {};
    while (epath[0].last() == epath[1].last()) {
      if (process) Warning("Cycle was not minimal since trimming dart");
      epath[0].sub(1);
      epath[1].sub(1);
    }
    ecycle.push_array(std::move(epath[0]));
    ecycle.push_array(reverse(std::move(epath[1])));
  }
  const int num_edges = ecycle.num();
  if (process) {
    float len = 0.f;
    for (Edge e : ecycle) len += _mesh.length(e);
    if (0)
      showdf("Cycle edges=%d length=%g v1d=%g v2d=%g e12=%g\n",  //
             ecycle.num(), len, v_dist(v1), v_dist(v2), _mesh.length(e12));
    HH_SSTAT(Scyclene, ecycle.num());
    HH_SSTAT(Scyclelen, len);
    assertw(difference_is_within_relative_eps(len, verify_dist * 2.f, 1e-6f));
    // Convert from cycle of edges to cycle of vertices.
    Array<Vertex> vertex_loop;
    {  // Old vertices; vertex_loop[i] is between ecycle[i] and ecycle[i + 1].
      assertx(ecycle.num() >= 3);
      Vertex v0 = _mesh.vertex_between_edges(ecycle[0], ecycle[1]);
      vertex_loop.push(v0);
      for_intL(i, 1, ecycle.num()) vertex_loop.push(_mesh.opp_vertex(vertex_loop.last(), ecycle[i]));
      ASSERTX(_mesh.opp_vertex(vertex_loop.last(), ecycle[0]) == vertex_loop[0]);  // Verify ecycle is truly a cycle.
    }
    if (0) {
      SHOWL;
      for (Vertex v : vertex_loop) SHOW(_mesh.vertex_id(v));
    }
    Array<Vertex> new_vertices = close_cycle(vertex_loop);
    // Re-initialize v_dist() and e_joined() for that portion of the mesh disconnected from vseed.
    flood_reinitialize(new_vertices[0]);  // Pick any new vertex.
  }
  return {num_edges};
}

// Find the smallest size cycle containing vertex vseed -- report search radius (BIGFLOAT if no cycle found) and
// farthest vertex in cycle from vseed.
// If parameter `process` is true, modify the mesh to close the cycle.
// The caller must clean up e_joined() and v_dist() after this function completes.
auto CloseMinCycles::min_cycle_from_vertex(Vertex vseed, bool process) -> std::optional<MinCycleResult> {
  if (sdebug) {  // Verify that previous search has cleanly reinitialized all fields.
    Warning("sdebug");
    for (Edge e : _mesh.edges()) assertx(!e_joined(e));
    for (Vertex v : _mesh.vertices()) assertx(v_dist(v) == BIGFLOAT);
  }
  Map<Vertex, Vertex> map_vtouch;
  const auto v_vtouch = [&](Vertex v) -> Vertex& { return map_vtouch[v]; };
  // The priority queue on Vertex v contains two types of prioritized events:
  // (1) the BFS/Dijkstra advancing front (priority is Dijkstra path distance from vseed to v)
  //       through sequence of v_vprev(v) relationships.
  // (2) candidate cycle edges, where the priority is half the length of the cycle spanning
  //      (a) v through v_vprev(v) to vseed, combined with (b) v_vtouch(v) through v_vprev(v_vtouch(v)) to vseed.
  // These two event types are distinguished based on whether vertex v has been reached by BFS,
  //    i.e. v_dist(v) != BIGFLOAT for type (2).
  HPqueue<Vertex> hpq;
  {  // This first iteration is special.
    v_dist(vseed) = 0.f;
    v_vprev(vseed) = nullptr;
    for (Vertex v : _mesh.vertices(vseed)) {
      v_vprev(v) = vseed;
      hpq.enter(v, dist(_mesh.point(vseed), _mesh.point(v)));
    }
  }
  while (!hpq.empty()) {
    // Given the priority queue, pull out the next vertex, which may be of event type (1) or (2) as above.
    const float vdist = hpq.min_priority();
    Vertex vnew = hpq.remove_min();
    if (0)
      showf("pqmin: v=%d lb=%g v_dist(v)=%g v_vprev(v)=%d v_vtouch(v)=%d\n",  //
            _mesh.vertex_id(vnew), vdist, v_dist(vnew), v_dist(vnew) == BIGFLOAT ? _mesh.vertex_id(v_vprev(vnew)) : -1,
            v_dist(vnew) != BIGFLOAT ? _mesh.vertex_id(v_vtouch(vnew)) : -1);
    if (v_dist(vnew) != BIGFLOAT) {  // A candidate cycle edge.
      if (verb) Warning("Front is touching itself");
      if (e_joined(_mesh.edge(vnew, v_vtouch(vnew)))) {
        if (verb) Warning("joined in the meantime");
        continue;
      }
      if (auto result = look_for_cycle(vnew, v_vtouch(vnew), process, vdist)) {
        // We have found a cycle; exit from function.
        const float search_radius = vdist;
        Vertex farthest_vertex = v_vtouch(vnew);
        const int num_edges = *result;
        return MinCycleResult{search_radius, farthest_vertex, num_edges};
      }
      continue;  // Not a non-separating cycle; ignore this event.
    }
    // Dijkstra BFS visit of vnew, approached from vprev.
    Vertex vprev = v_vprev(vnew);
    {
      Edge e = _mesh.edge(vprev, vnew);
      v_dist(vnew) = vdist;
      ASSERTX(difference_is_within_relative_eps(vdist, v_dist(vprev) + _mesh.length(e), 1e-5f));
      ASSERTX(!e_joined(e));
      e_joined(e) = true;
    }
    // Update unvisited neighbors using ordinary BFS Dijkstra rules.
    for (Vertex v : _mesh.vertices(vnew)) {
      if (v_dist(v) != BIGFLOAT) continue;  // Already visited.
      const float elen = dist(_mesh.point(vnew), _mesh.point(v));
      const float possible_dist = v_dist(vnew) + elen;
      if (hpq.enter_update_if_smaller(v, possible_dist)) v_vprev(v) = vnew;
    }
    // Update unjoined visited neighbors for cycle events.
    {
      Vertex vccw, vclw;  // Most ccw|clw contiguous already-visited vertices.
      for (vccw = vprev;;) {
        ASSERTX(v_dist(vccw) != BIGFLOAT);
        Vertex vn = _mesh.ccw_vertex(vnew, vccw);
        if (vn == vprev || !e_joined(_mesh.edge(vccw, vn))) break;
        e_joined(_mesh.edge(vnew, vn)) = true;
        vccw = vn;
      }
      for (vclw = vprev;;) {
        ASSERTX(v_dist(vclw) != BIGFLOAT);
        Vertex vn = _mesh.clw_vertex(vnew, vclw);
        if (vn == vccw || !e_joined(_mesh.edge(vclw, vn))) break;
        e_joined(_mesh.edge(vnew, vn)) = true;
        vclw = vn;
      }
      Vertex vp = vccw;
      // Consider each vertex v not already in current advancing BFS front.
      for (Vertex v = _mesh.ccw_vertex(vnew, vp); v != vclw; vp = v, v = _mesh.ccw_vertex(vnew, vp)) {
        if (v_dist(v) == BIGFLOAT) {
          ASSERTX(!e_joined(_mesh.edge(vnew, v)));
        } else {
          Edge e = _mesh.edge(vnew, v);
          if (e_joined(e)) continue;  // May have been joined in the meantime during this loop.
          if (verb) Warning("Front is about to touch itself");
          if (!would_be_nonseparating_cycle(e, false)) {
            if (verb) Warning("Not would_be_nonseparating_cycle");
            continue;
          }
          // NOTE: must consider halfway point on potential cycle from vseed to vnew to v to vseed!
          ASSERTX(v_dist(v) <= v_dist(vnew));  // Visited earlier.
          const float elen = dist(_mesh.point(vnew), _mesh.point(v));
          const float possible_dist = (v_dist(vnew) + v_dist(v) + elen) * .5f;  // "Radius" (half of cycle length).
          assertw(possible_dist * (1.f + 1e-6f) >= v_dist(vnew));
          if (hpq.enter_update_if_smaller(v, possible_dist)) v_vtouch(v) = vnew;
        }
      }
    }
  }
  return {};
}

// Intuition:
//  We iteratively find the non-separating cycle with minimal length (where the cycle is a path of edges on the mesh,
//   i.e. not a true geodesic path that traverses across face interiors).
//  To find the minimal cycle, we consider the minimal cycle at each vertex.
//  To find the minimal cycle passing through a vertex, we compute a breadth-first search (BFS),
//   like Dijkstra's algorithm, but keep track of connected region as the front advances,
//   to detect when the front touches itself unexpectedly.
//  This indicates a potential non-separating cycle, which we verify using another BFS.
//  To speed up the process, we recognize that a single BFS actually lets us obtain a lower-bound on the
//   minimal cycle passing through any vertex traversed during the BFS search.
//  Intuitively, if the BFS covers a large mesh region before finding a cycle, then most of the vertices
//   in the search region cannot contain small cycles.
void CloseMinCycles::find_cycles() {
  HH_TIMER("_find_cycles");
  if (0) {  // For debug.
    // Results in 2 separate components, so not a topological handle.
    close_cycle(V(_mesh.id_vertex(50), _mesh.id_vertex(53), _mesh.id_vertex(59), _mesh.id_vertex(49)));
    return;
  }
  if (0) {  // For debug.
    const auto result = min_cycle_from_vertex(_mesh.id_vertex(49), true);
    SHOW(result->search_radius);
    for (Vertex v : _mesh.vertices())
      if (v_dist(v) != BIGFLOAT) showf("vdist(%d)=%g\n", _mesh.vertex_id(v), v_dist(v));
    return;
  }
  HPqueue<Vertex> pqvlbsr;  // Lower-bound on search radius for min cycle about vertex.
  pqvlbsr.reserve(_mesh.num_vertices());
  for (Vertex v : _mesh.vertices()) pqvlbsr.enter_unsorted(v, 0.f);
  if (0) pqvlbsr.sort();  // Sorting is unnecessary because all initial priority values are the same.
  Vertex vrand = _mesh.random_vertex(Random::G);  // Allow getenv_int("SEED_RANDOM") to vary the search.
  if (0) vrand = _mesh.id_vertex(53);             // For debug, select specific vertex.
  int nprocessed = 0;
  float ubsr = BIGFLOAT;  // Upper-bound on search radius for minimal cycle.
  int iter = 0;
  for (;;) {
    Vertex vseed = pqvlbsr.min();
    float lbsr = pqvlbsr.min_priority();
    if (vrand) {  // Override the choice of initial vertex.
      vseed = vrand;
      lbsr = pqvlbsr.retrieve(vseed);
      assertx(lbsr == 0.f);
      vrand = nullptr;
    }
    if (lbsr == BIGFLOAT) {
      showdf("No more cycles at all\n");
      break;
    }
    if (lbsr > _options.max_cycle_length / 2.f) {
      showdf("No more cycles of size <=%g\n", _options.max_cycle_length);
      break;
    }
    ++iter;
    const auto result = min_cycle_from_vertex(vseed, false);
    const float sr = result ? result->search_radius : BIGFLOAT;
    ubsr = min(ubsr, sr);  // If we find a cycle, possibly reduce the upper-bound on the minimal search radius.
    if (verb)
      showf("it=%-4d v=%-7d sr=%-12g nedges=%-4d lb=%-12g ub=%-12g\n",  //
            iter, _mesh.vertex_id(vseed), sr, (result ? result->num_edges : -1), lbsr, ubsr);
    if (!(sr * (1.f + 2e-7f) >= lbsr)) {
      SHOW((lbsr - sr) / sr - 1.f);
      assertx(sr >= lbsr);
    }
    if (!(ubsr * (1.f + 2e-7f) >= lbsr)) {
      SHOW((lbsr - ubsr) / ubsr - 1.f);
      assertx(ubsr >= lbsr);
    }
    if (0 && sr == BIGFLOAT) {  // This test is invalid if the mesh has multiple connected components.
      showdf("No more cycles at all\n");
      break;  // Efficiency shortcut: no need to clean up v_dist() and e_joined().
    }
    // Update pqvlbsr and re-initialize v_dist() and e_joined().
    {
      pqvlbsr.update(vseed, sr);  // Note: should never change again because it is the exact distance.
      // The following is like flood_reinitialize(vseed) but it also updates pqvlbsr.
      v_dist(vseed) = BIGFLOAT;
      Queue<Vertex> queue;
      queue.enqueue(vseed);
      while (!queue.empty()) {
        Vertex vc = queue.dequeue();
        ASSERTX(v_dist(vc) == BIGFLOAT);
        for (Edge e : _mesh.edges(vc)) {
          e_joined(e) = false;
          Vertex vn = _mesh.opp_vertex(vc, e);
          if (v_dist(vn) != BIGFLOAT) {
            // Vertex vn was found to have distance v_dist(vn) from vseed.
            // Because the minimal cycle about vseed has length sr * 2, we can infer that the minimal cycle
            //  about vn cannot be smaller than (sr - v_dist(vn)) * 2.
            float new_lower_bound_radius = sr - v_dist(vn);
            if (new_lower_bound_radius < 0.f) {
              if (0) SHOW(new_lower_bound_radius);
              assertx(new_lower_bound_radius > -sr * 1e-6f);
              new_lower_bound_radius = 0.f;
            }
            pqvlbsr.enter_update_if_greater(vn, new_lower_bound_radius);
            v_dist(vn) = BIGFLOAT;
            queue.enqueue(vn);
          }
        }
      }
      lbsr = pqvlbsr.min_priority();
      if (!(ubsr >= lbsr)) assertnever(SSHOW(_mesh.vertex_id(pqvlbsr.min()), lbsr));
    }
    if (!result) continue;  // No more cycles in this connected component of the mesh.
    // Process the cycle if its radius is within some fraction of the lower-bound minimal cycle radius lbsr.
    if (sr <= _options.frac_cycle_length * lbsr) {  // Was: "if (sr == lbsr)".
      if (verb) showdf("After %d iter, processing cycle of length %g\n", iter + 1, sr * 2.f);
      assertx(result);
      if (result->num_edges > _options.max_cycle_nedges) {
        showdf("Stopping because next cycle has %d>%d edges\n", result->num_edges, _options.max_cycle_nedges);
        break;
      }
      bool restart_at_farthest = true;  // May improve loop if _options.frac_cycle_length > 1.f (e.g. holes3.m).
      // if (!assertw(_options.frac_cycle_length > (1.f + 1e-6f))) restart_at_farthest = false;
      if (!(_options.frac_cycle_length > (1.f + 1e-6f))) restart_at_farthest = false;
      if (restart_at_farthest) vseed = result->farthest_vertex;
      const auto result2 = assertx(min_cycle_from_vertex(vseed, true));
      assertx(result2->search_radius <= sr * (1.f + 1e-6f));
      if (!restart_at_farthest) assertx(result2->search_radius == sr);
      assertw(result2->num_edges <= _options.max_cycle_nedges);
      flood_reinitialize(vseed);  // Again re-initialize v_dist() and e_joined().
      if (sdebug) {
        Warning("slow");
        for (Edge e : _mesh.edges()) assertx(!e_joined(e));
      }
      ++nprocessed;
      --_current_genus;
      ubsr = BIGFLOAT;
      if (nprocessed >= _options.num_cycles) {
        showdf("Processed requested %d cycles\n", _options.num_cycles);
        break;
      }
      if (_current_genus <= _options.genus) {
        showdf("Reduced genus to %d\n", _current_genus);
        break;
      }
    }
  }
  showdf("Computed total of %d iterations of BFS\n", iter);
}

// Wrap main function with set up and clean up.
void CloseMinCycles::compute() {
  assertx(_options.frac_cycle_length >= 1.f);
  assertx(_current_genus == std::numeric_limits<int>::max());
  if (_mesh.empty()) return;
  Array<Vertex> ar_boundary_centers;
  if (1) {  // Deal with mesh boundaries.
    HH_TIMER("_fillholes");
    Set<Edge> setbe;
    for (Edge e : _mesh.edges())
      if (_mesh.is_boundary(e)) setbe.enter(e);
    while (!setbe.empty()) {
      Edge e = setbe.get_one();
      const Queue<Edge> queuee = gather_boundary(_mesh, e);
      for (Edge ee : queuee) assertx(setbe.remove(ee));
      const Set<Face> setf = mesh_remove_boundary(_mesh, e);
      for (Face f : setf) {
        Vertex vnew = _mesh.center_split_face(f);
        _mesh.set_point(vnew, k_boundary_point_far_away);
        ar_boundary_centers.push(vnew);
      }
    }
    if (ar_boundary_centers.num()) showdf("Temporarily filled in %d boundaries\n", ar_boundary_centers.num());
  }
  for (Face f : _mesh.faces()) assertx(_mesh.is_triangle(f));
  if (0) {
    SHOW(mesh_genus_string(_mesh));
    assertx(_mesh.is_nice());
  }
  for (Vertex v : _mesh.vertices()) {
    assertx(_mesh.degree(v) > 0);  // No isolated vertices.
    v_dist(v) = BIGFLOAT;
    // v_vtouch(v) is undefined.
    // v_vprev(v) is undefined.
  }
  for (Edge e : _mesh.edges()) {
    e_joined(e) = false;
    e_bfsnum(e) = 0;
  }
  {
    HH_TIMER("__genus");
    const float fgenus = mesh_genus(_mesh);  // Somewhat slow implementation.
    assertx(fgenus == floor(fgenus));
    _current_genus = int(fgenus);
    assertx(_current_genus >= 0);
  }
  showdf("Starting with mesh of genus %d\n", _current_genus);
  find_cycles();
  showdf("Closed %d cycles (%d handles and %d tunnels), resulting in mesh of genus %d\n",  //
         _total_handles + _total_tunnels, _total_handles, _total_tunnels, _current_genus);
  for (Vertex vnew : ar_boundary_centers) {
    for (Face f : Array<Face>(_mesh.faces(vnew))) _mesh.destroy_face(f);
    _mesh.destroy_vertex(vnew);
  }
  ASSERTX(_current_genus == int(mesh_genus(_mesh)));
}

}  // namespace hh
