#include <algorithm>
#include <cassert>
#include <tuple>
#include <vector>

#include <osmium/area/assembler.hpp>
#include <osmium/geom/wkb.hpp>

#include "osmium-builder.hpp"

namespace {

osmium::area::AssemblerConfig area_config;

inline double distance(osmium::geom::Coordinates p1,
                       osmium::geom::Coordinates p2)
{
    double dx = p1.x - p2.x;
    double dy = p1.y - p2.y;
    return std::sqrt(dx * dx + dy * dy);
}

inline osmium::geom::Coordinates interpolate(osmium::geom::Coordinates p1,
                                             osmium::geom::Coordinates p2,
                                             double frac)
{
    return osmium::geom::Coordinates(frac * (p1.x - p2.x) + p2.x,
                                     frac * (p1.y - p2.y) + p2.y);
}

template <typename ITERATOR>
inline void add_nodes_to_builder(osmium::builder::WayNodeListBuilder &builder,
                                 ITERATOR const &begin, ITERATOR const &end,
                                 bool skip_first)
{
    auto it = begin;
    if (skip_first) {
        ++it;
    }

    while (it != end) {
        if (it->location().valid()) {
            builder.add_node_ref(*it);
        }
        ++it;
    }
}

} // name space

namespace geom {

using WKBWriter = osmium::geom::detail::WKBFactoryImpl;

osmium_builder_t::wkb_t
osmium_builder_t::get_wkb_node(osmium::Location const &loc) const
{
    return m_writer.make_point(m_proj->reproject(loc));
}

osmium_builder_t::wkbs_t
osmium_builder_t::get_wkb_split(osmium::WayNodeList const &nodes)
{
    wkbs_t ret;

    double const split_at = m_proj->target_latlon() ? 1 : 100 * 1000;

    double dist = 0;
    osmium::geom::Coordinates prev_pt;
    m_writer.linestring_start();
    size_t curlen = 0;

    for (auto const &node : nodes) {
        if (!node.location().valid())
            continue;

        auto const this_pt = m_proj->reproject(node.location());
        if (prev_pt.valid()) {
            if (prev_pt == this_pt) {
                continue;
            }
            double const delta = distance(prev_pt, this_pt);

            // figure out if the addition of this point would take the total
            // length of the line in `segment` over the `split_at` distance.

            if (dist + delta > split_at) {
                size_t const splits =
                    (size_t)std::floor((dist + delta) / split_at);
                // use the splitting distance to split the current segment up
                // into as many parts as necessary to keep each part below
                // the `split_at` distance.
                osmium::geom::Coordinates ipoint;
                for (size_t j = 0; j < splits; ++j) {
                    double const frac =
                        ((double)(j + 1) * split_at - dist) / delta;
                    ipoint = interpolate(this_pt, prev_pt, frac);
                    m_writer.linestring_add_location(ipoint);
                    ret.push_back(m_writer.linestring_finish(curlen));
                    // start a new segment
                    m_writer.linestring_start();
                    m_writer.linestring_add_location(ipoint);
                }
                // reset the distance based on the final splitting point for
                // the next iteration.
                if (this_pt == ipoint) {
                    dist = 0;
                    m_writer.linestring_start();
                    curlen = 0;
                } else {
                    dist = distance(this_pt, ipoint);
                    curlen = 1;
                }
            } else {
                dist += delta;
            }
        }

        m_writer.linestring_add_location(this_pt);
        ++curlen;

        prev_pt = this_pt;
    }

    if (curlen > 1) {
        ret.push_back(m_writer.linestring_finish(curlen));
    }

    return ret;
}

osmium_builder_t::wkb_t
osmium_builder_t::get_wkb_polygon(osmium::Way const &way)
{
    osmium::area::Assembler assembler{area_config};

    m_buffer.clear();
    if (!assembler.make_area(way, m_buffer)) {
        return wkb_t();
    }

    auto wkbs = create_multipolygon(m_buffer.get<osmium::Area>(0));

    return wkbs.empty() ? wkb_t() : wkbs[0];
}

osmium_builder_t::wkbs_t
osmium_builder_t::get_wkb_multipolygon(osmium::Relation const &rel,
                                       osmium::memory::Buffer const &ways)
{
    wkbs_t ret;
    osmium::area::Assembler assembler{area_config};

    m_buffer.clear();
    if (assembler.make_area(rel, ways, m_buffer)) {
        ret = create_multipolygon(m_buffer.get<osmium::Area>(0));
    }

    return ret;
}

osmium_builder_t::wkbs_t
osmium_builder_t::get_wkb_multiline(osmium::memory::Buffer const &ways, bool)
{
    wkbs_t ret;

    // make a list of all endpoints
    using endpoint_t = std::tuple<osmium::object_id_type, size_t, bool>;
    std::vector<endpoint_t> endpoints;
    // and a list of way connections
    enum lmt : size_t
    {
        NOCONN = -1UL
    };
    std::vector<std::tuple<size_t, osmium::Way const *, size_t>> conns;

    // initialise the two lists
    for (auto const &w : ways.select<osmium::Way>()) {
        if (w.nodes().size() > 1) {
            endpoints.emplace_back(w.nodes().front().ref(), conns.size(), true);
            endpoints.emplace_back(w.nodes().back().ref(), conns.size(), false);
            conns.emplace_back(NOCONN, &w, NOCONN);
        }
    }
    // sort by node id
    std::sort(endpoints.begin(), endpoints.end());
    // now fill the connection list based on the sorted list
    {
        endpoint_t const *prev = nullptr;
        for (auto const &pt : endpoints) {
            if (prev) {
                if (std::get<0>(*prev) == std::get<0>(pt)) {
                    auto previd = std::get<1>(*prev);
                    auto ptid = std::get<1>(pt);
                    if (std::get<2>(*prev)) {
                        std::get<0>(conns[previd]) = ptid;
                    } else {
                        std::get<2>(conns[previd]) = ptid;
                    }
                    if (std::get<2>(pt)) {
                        std::get<0>(conns[ptid]) = previd;
                    } else {
                        std::get<2>(conns[ptid]) = previd;
                    }
                    prev = nullptr;
                    continue;
                }
            }

            prev = &pt;
        }
    }

    // XXX need to do non-split version
    size_t done_ways = 0;
    size_t todo_ways = conns.size();
    wkbs_t linewkbs;
    for (size_t i = 0; i < todo_ways; ++i) {
        if (!std::get<1>(conns[i]) || (std::get<0>(conns[i]) != NOCONN &&
                                       std::get<2>(conns[i]) != NOCONN)) {
            continue; // way already done or not the beginning of a segment
        }

        m_buffer.clear();
        osmium::builder::WayNodeListBuilder wnl_builder(m_buffer);
        size_t prev = NOCONN;
        size_t cur = i;

        do {
            auto &conn = conns[cur];
            assert(std::get<1>(conn));
            auto &nl = std::get<1>(conn)->nodes();
            bool skip_first = prev != NOCONN;
            bool forward = std::get<0>(conn) == prev;
            prev = cur;
            // add way nodes
            if (forward) {
                add_nodes_to_builder(wnl_builder, nl.cbegin(), nl.cend(),
                                     skip_first);
                cur = std::get<2>(conn);
            } else {
                add_nodes_to_builder(wnl_builder, nl.crbegin(), nl.crend(),
                                     skip_first);
                cur = std::get<0>(conn);
            }
            // mark way as done
            std::get<1>(conns[prev]) = nullptr;
            ++done_ways;
        } while (cur != NOCONN);

        // found a line end, create the wkbs
        m_buffer.commit();
        linewkbs = get_wkb_split(m_buffer.get<osmium::WayNodeList>(0));
        std::move(linewkbs.begin(), linewkbs.end(),
                  std::inserter(ret, ret.end()));
        linewkbs.clear();
    }

    if (done_ways < todo_ways) {
        // oh dear, there must be circular ways without an end
        // need to do the same shebang again
        for (size_t i = 0; i < todo_ways; ++i) {
            if (!std::get<1>(conns[i])) {
                continue; // way already done
            }

            m_buffer.clear();
            osmium::builder::WayNodeListBuilder wnl_builder(m_buffer);
            size_t prev = std::get<0>(conns[i]);
            size_t cur = i;
            bool skip_first = false;

            do {
                auto &conn = conns[cur];
                assert(std::get<1>(conn));
                auto &nl = std::get<1>(conn)->nodes();
                bool forward = std::get<0>(conn) == prev;
                prev = cur;
                if (forward) {
                    // add way forwards
                    add_nodes_to_builder(wnl_builder, nl.cbegin(), nl.cend(),
                                         skip_first);
                    cur = std::get<2>(conn);
                } else {
                    // add way backwards
                    add_nodes_to_builder(wnl_builder, nl.crbegin(), nl.crend(),
                                         skip_first);
                    cur = std::get<0>(conn);
                }
                // mark way as done
                std::get<1>(conns[prev]) = nullptr;
                skip_first = true;
            } while (cur != i);

            // found a line end, create the wkbs
            m_buffer.commit();
            linewkbs = get_wkb_split(m_buffer.get<osmium::WayNodeList>(0));
            std::move(linewkbs.begin(), linewkbs.end(),
                      std::inserter(ret, ret.end()));
            linewkbs.clear();
        }
    }

    return ret;
}

void osmium_builder_t::add_mp_points(const osmium::NodeRefList &nodes)
{
    osmium::Location last_location;
    for (const osmium::NodeRef &node_ref : nodes) {
        if (node_ref.location().valid() &&
            last_location != node_ref.location()) {
            last_location = node_ref.location();
            m_writer.multipolygon_add_location(
                m_proj->reproject(last_location));
        }
    }
}

osmium_builder_t::wkbs_t
osmium_builder_t::create_multipolygon(osmium::Area const &area)
{
    wkbs_t ret;

    // XXX need to split into polygons

    try {
        size_t num_polygons = 0;
        size_t num_rings = 0;
        m_writer.multipolygon_start();

        for (auto it = area.cbegin(); it != area.cend(); ++it) {
            if (it->type() == osmium::item_type::outer_ring) {
                auto &ring = static_cast<const osmium::OuterRing &>(*it);
                if (num_polygons > 0) {
                    m_writer.multipolygon_polygon_finish();
                }
                m_writer.multipolygon_polygon_start();
                m_writer.multipolygon_outer_ring_start();
                add_mp_points(ring);
                m_writer.multipolygon_outer_ring_finish();
                ++num_rings;
                ++num_polygons;
            } else if (it->type() == osmium::item_type::inner_ring) {
                auto &ring = static_cast<const osmium::InnerRing &>(*it);
                m_writer.multipolygon_inner_ring_start();
                add_mp_points(ring);
                m_writer.multipolygon_inner_ring_finish();
                ++num_rings;
            }
        }

        // if there are no rings, this area is invalid
        if (num_rings > 0) {
            m_writer.multipolygon_polygon_finish();
            ret.push_back(m_writer.multipolygon_finish());
        }

    } catch (osmium::geometry_error &e) { /* ignored */
    }

    return ret;
}

} // name space