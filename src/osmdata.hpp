#ifndef OSM2PGSQL_OSMDATA_HPP
#define OSM2PGSQL_OSMDATA_HPP

#include <memory>
#include <vector>

#include "dependency-manager.hpp"
#include "options.hpp"
#include "osmtypes.hpp"

class output_t;
struct middle_t;
struct slim_middle_t;

class osmdata_t
{
public:
    osmdata_t(std::unique_ptr<dependency_manager_t> dependency_manager,
              std::shared_ptr<middle_t> mid,
              std::vector<std::shared_ptr<output_t>> outs,
              options_t const &options);

    void start() const;
    void flush() const;
    void stop() const;

    void node_add(osmium::Node const &node) const;
    void way_add(osmium::Way *way) const;
    void relation_add(osmium::Relation const &rel) const;

    void node_modify(osmium::Node const &node) const;
    void way_modify(osmium::Way *way) const;
    void relation_modify(osmium::Relation const &rel) const;

    void node_delete(osmid_t id) const;
    void way_delete(osmid_t id) const;
    void relation_delete(osmid_t id) const;

private:
    slim_middle_t &slim_middle() const noexcept;

    std::unique_ptr<dependency_manager_t> m_dependency_manager;
    std::shared_ptr<middle_t> m_mid;
    std::vector<std::shared_ptr<output_t>> m_outs;

    options_t const &m_options;
    bool m_with_extra_attrs;
};

#endif // OSM2PGSQL_OSMDATA_HPP
