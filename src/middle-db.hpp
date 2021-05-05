#ifndef OSM2PGSQL_MIDDLE_DB_HPP
#define OSM2PGSQL_MIDDLE_DB_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2021 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "db-copy-mgr.hpp"
#include "middle.hpp"
#include "node-locations.hpp"
#include "node-persistent-cache.hpp"
#include "pgsql.hpp"
#include "template-repository.hpp"
#include "wkb.hpp"

#include <osmium/index/nwr_array.hpp>

#include <cstddef>
#include <memory>
#include <string>

class options_t;

enum class mode : bool
{
    import,
    update
};

struct db_store_options
{
    bool drop_tables = false;
    bool forward_dependencies = true;
    bool has_bucket_index = false;
    bool tags = true;
    bool attributes = false;
    bool untagged_nodes = true;
    bool locations = true;
    bool way_nodes = true;
    bool relation_members = true;
};

class middle_query_db_t : public middle_query_t
{
public:
    middle_query_db_t(std::string const &conninfo,
                      std::shared_ptr<node_locations_t> ram_cache,
                      std::shared_ptr<node_persistent_cache> persistent_cache,
                      template_repository_t const &templates,
                      db_store_options const &store_options);

    std::size_t nodes_get_list(osmium::WayNodeList *nodes) const override;

    bool way_get(osmid_t id, osmium::memory::Buffer *buffer) const override;

    std::size_t
    rel_members_get(osmium::Relation const &rel, osmium::memory::Buffer *buffer,
                    osmium::osm_entity_bits::type types) const override;

    bool relation_get(osmid_t id,
                      osmium::memory::Buffer *buffer) const override;

private:
    pg_conn_t m_db_connection;
    std::shared_ptr<node_locations_t> m_ram_cache;
    std::shared_ptr<node_persistent_cache> m_persistent_cache;
    db_store_options m_store_options;
};

class middle_db_t : public middle_t
{
public:
    middle_db_t(std::shared_ptr<thread_pool_t> thread_pool,
                options_t const *options);

    void start() override;
    void stop() override;

    void node(osmium::Node const &node) override;
    void way(osmium::Way const &way) override;
    void relation(osmium::Relation const &rel) override;

    void after_nodes() override;
    void after_ways() override;
    void after_relations() override;

    idlist_t get_ways_by_node(osmid_t osm_id) override;
    idlist_t get_rels_by_node(osmid_t osm_id) override;
    idlist_t get_rels_by_way(osmid_t osm_id) override;

    std::shared_ptr<middle_query_t> get_query_instance() override;

private:
    class table_desc
    {
    public:
        table_desc() = default;
        table_desc(osmium::item_type type, options_t const &options);

        std::string id() const noexcept
        {
            return osmium::item_type_to_name(m_type) + std::string{"s"};
        }

        std::string const &name() const noexcept { return m_copy_target->name; }

        std::shared_ptr<db_target_descr_t> const &copy_target() const noexcept
        {
            return m_copy_target;
        }

        task_result_t &task_primary_key() noexcept
        {
            return m_task_primary_key;
        }

        task_result_t &task_fw_dep_index() noexcept
        {
            return m_task_fw_dep_index;
        }

    private:
        std::shared_ptr<db_target_descr_t> m_copy_target;
        task_result_t m_task_primary_key;
        task_result_t m_task_fw_dep_index;
        osmium::item_type m_type = osmium::item_type::undefined;
    };

    void add_common_columns(osmium::OSMObject const &object);

    void node_set(osmium::Node const &node);
    void node_delete(osmid_t id);

    void way_set(osmium::Way const &way);
    void way_delete(osmid_t id);

    void relation_set(osmium::Relation const &rel);
    void relation_delete(osmid_t id);

    bool on_import() const noexcept { return m_mode == mode::import; }
    bool on_update() const noexcept { return m_mode == mode::update; }

    void override_opts_for_testing();
    void log_store_options();

    template_repository_t m_templates;

    osmium::nwr_array<table_desc> m_tables;

    std::string m_conninfo;
    pg_conn_t m_db_connection;

    std::shared_ptr<db_copy_thread_t> m_copy_thread;
    db_copy_mgr_t<db_deleter_by_id_t> m_db_copy;
    std::shared_ptr<node_locations_t> m_ram_cache;
    std::shared_ptr<node_persistent_cache> m_persistent_cache;
    std::size_t m_max_cache = 0;
    db_store_options m_store_options{};
    mode m_mode;
}; // class middle_db_t

#endif // OSM2PGSQL_MIDDLE_DB_HPP
