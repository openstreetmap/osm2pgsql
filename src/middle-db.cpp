/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2021 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "format.hpp"
#include "logging.hpp"
#include "middle-db.hpp"
#include "options.hpp"
#include "osmtypes.hpp"
#include "pgsql-helper.hpp"
#include "util.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/types_from_string.hpp>
#include <osmium/util/string.hpp>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>

static void setup_template_variables(template_repository_t *tmpl,
                                     options_t const &options)
{
    assert(tmpl);

    std::string const &schema = options.middle_dbschema;
    std::string const using_tablespace{options.tblsslim_index.empty()
                                           ? ""
                                           : "USING INDEX TABLESPACE " +
                                                 options.tblsslim_index};
    tmpl->set("prefix", options.prefix);
    tmpl->set("schemaname", schema.empty() ? "public" : schema);
    tmpl->set("schema", schema.empty() ? "" : ("\"" + schema + "\"."));
    tmpl->set("unlogged", options.droptemp ? "UNLOGGED" : "");
    tmpl->set("using_tablespace", using_tablespace);
    tmpl->set("data_tablespace", tablespace_clause(options.tblsslim_data));
    tmpl->set("index_tablespace", tablespace_clause(options.tblsslim_index));
    tmpl->set("way_node_index_id_shift",
              std::to_string(options.way_node_index_id_shift));
}

static void setup_templates(template_repository_t *tmpl, bool has_bucket_index)
{
    assert(tmpl);

    tmpl->add(
        "init",
        "SET client_min_messages = WARNING;\n"
        "DROP VIEW IF EXISTS {schema}osm2pgsql_indexes;\n"
        "DROP VIEW IF EXISTS {schema}osm2pgsql_tables;\n"
        "DROP TABLE IF EXISTS {schema}osm2pgsql_index_list;\n"
        "DROP TABLE IF EXISTS {schema}osm2pgsql_table_list;\n"
        "RESET client_min_messages;\n"
        "CREATE TABLE {schema}osm2pgsql_table_list ("
        "  table_id text NOT NULL,"
        "  table_name text NOT NULL,"
        "  sort_index serial2 NOT NULL,"
        "  with_attributes boolean NOT NULL"
        ");\n"
        "CREATE TABLE {schema}osm2pgsql_index_list ("
        "  table_id text NOT NULL,"
        "  index_id text NOT NULL,"
        "  index_name text NOT NULL,"
        "  sort_index serial2 NOT NULL,"
        "  started timestamp,"
        "  finished timestamp"
        ");\n"
        "CREATE VIEW {schema}osm2pgsql_tables AS"
        "  SELECT table_id, c.oid AS relid, table_name, with_attributes,"
        "         c.reltuples::bigint AS rows_estimate,"
        "         pg_table_size(table_name) AS size,"
        "         pg_size_pretty(pg_table_size(table_name)) AS size_pretty"
        "  FROM {schema}osm2pgsql_table_list l LEFT JOIN pg_class c"
        "     ON l.table_name = c.relname AND c.relnamespace = "
        "        (SELECT oid FROM pg_namespace WHERE nspname='{schemaname}')"
        "     ORDER BY sort_index;\n"
        "CREATE VIEW {schema}osm2pgsql_indexes AS"
        "  SELECT table_id, index_id, index_name,"
        "         to_char(started, 'YYYY:MM:DD HH24:MI:SS') AS started,"
        "         to_char(finished, 'YYYY:MM:DD HH24:MI:SS') AS finished,"
        "         to_char(finished - started, 'HH24:MI:SS') AS build_time,"
        "         CASE WHEN i.indexname IS NULL THEN NULL"
        "              ELSE pg_table_size(index_name)"
        "         END AS size,"
        "         CASE WHEN i.indexname IS NULL THEN NULL"
        "              ELSE pg_size_pretty(pg_table_size(index_name))"
        "         END AS size_pretty"
        "  FROM {schema}osm2pgsql_index_list l LEFT JOIN pg_indexes i"
        "     ON l.index_name = i.indexname AND i.schemaname='{schemaname}'"
        "     ORDER BY sort_index;\n");

    tmpl->add("drop", "SET client_min_messages = WARNING;\n"
                      "DROP VIEW IF EXISTS {schema}osm2pgsql_indexes;\n"
                      "DROP TABLE IF EXISTS {schema}osm2pgsql_index_list;\n"
                      "DROP VIEW IF EXISTS {schema}osm2pgsql_tables;\n"
                      "DROP TABLE IF EXISTS {schema}osm2pgsql_table_list;\n"
                      "RESET client_min_messages;\n");

    tmpl->add(".add_attribute_columns",
              "ALTER TABLE {schema}\"{prefix}_{table}\""
              "  ADD COLUMN created timestamp without time zone,"
              "  ADD COLUMN version int4,"
              "  ADD COLUMN changeset_id int4,"
              "  ADD COLUMN user_id int4,"
              "  ADD COLUMN user_name text;\n"
              "UPDATE {schema}osm2pgsql_table_list"
              "  SET with_attributes = true"
              "    WHERE table_id = '{table}';\n");

    tmpl->add(".name", "{prefix}_{table}");

    tmpl->add(".drop_table",
              "SET client_min_messages = WARNING;\n"
              "DROP TABLE IF EXISTS {schema}\"{prefix}_{table}\" CASCADE;\n"
              "RESET client_min_messages;\n");

    tmpl->add(".analyze_table", "ANALYZE {schema}\"{prefix}_{table}\";\n");

    tmpl->add(".add_primary_key",
              "UPDATE {schema}osm2pgsql_index_list SET started=now()"
              "  WHERE table_id = '{table}' AND index_id = 'pkey';\n"
              "ALTER TABLE {schema}\"{prefix}_{table}\""
              "  ADD PRIMARY KEY(id) {using_tablespace};\n");

    tmpl->add(".primary_key_index_finished",
              "UPDATE {schema}osm2pgsql_index_list SET finished=now()"
              "  WHERE table_id = '{table}' AND index_id = 'pkey';\n");

    tmpl->add(".create_table",
              "CREATE {unlogged} TABLE {schema}\"{prefix}_{table}\" ("
              "  id int8 NOT NULL"
              ") {data_tablespace};\n"
              "INSERT INTO {schema}osm2pgsql_table_list"
              "         (table_id, table_name, with_attributes)"
              "  VALUES ('{table}', '{prefix}_{table}', false);\n"
              "INSERT INTO {schema}osm2pgsql_index_list"
              "         (table_id, index_id, index_name)"
              "  VALUES ('{table}', 'pkey', '{prefix}_{table}_pkey');\n");

    tmpl->add(".alter_table_add_tags",
              "ALTER TABLE {schema}\"{prefix}_{table}\""
              "  ADD COLUMN tags jsonb;\n");

    tmpl->add("nodes.alter_table",
              "ALTER TABLE {schema}\"{prefix}_nodes\""
              "  ADD COLUMN geom geometry(POINT, 4326);\n");

    tmpl->add("nodes.prepare_query", "PREPARE get_node_list(int8[]) AS"
                                     "  SELECT id, ST_X(geom), ST_Y(geom)"
                                     "    FROM {schema}\"{prefix}_nodes\""
                                     "      WHERE id = ANY($1::int8[]);\n");

    tmpl->add("ways.alter_table",
              "ALTER TABLE {schema}\"{prefix}_ways\""
              "  ADD COLUMN nodes int8[] NOT NULL;\n"
              "INSERT INTO {schema}osm2pgsql_index_list"
              "         (table_id, index_id, index_name)"
              "  VALUES ('ways', 'nodes', '{prefix}_ways_nodes_idx'),"
              "         ('ways', 'nodes_bucket', "
              "                  '{prefix}_ways_nodes_bucket_idx');\n");

    tmpl->add("ways.prepare_query",
              "PREPARE get_way(int8) AS"
              "  SELECT * FROM {schema}\"{prefix}_ways\" WHERE id = $1;\n"
              "PREPARE get_way_list(int8[]) AS"
              "  SELECT id, nodes"
              "    FROM {schema}\"{prefix}_ways\""
              "      WHERE id = ANY($1::int8[]);\n");

    if (has_bucket_index) {
        tmpl->add("ways.prepare_fw_dep_lookups",
                  "PREPARE get_ways_by_node(int8) AS"
                  "  SELECT id FROM {schema}\"{prefix}_ways\" w"
                  "    WHERE $1 = ANY(nodes)"
                  "      AND {schema}\"{prefix}_index_bucket\"(w.nodes)"
                  "       && {schema}\"{prefix}_index_bucket\"(ARRAY[$1]);\n");

        tmpl->add("ways.create_fw_dep_indexes",
                  "CREATE OR REPLACE FUNCTION"
                  "    {schema}\"{prefix}_index_bucket\"(int8[])"
                  "  RETURNS int8[] AS $$\n"
                  "  SELECT ARRAY(SELECT DISTINCT"
                  "    unnest($1) >> {way_node_index_id_shift})\n"
                  "$$ LANGUAGE SQL IMMUTABLE;\n"
                  "UPDATE {schema}osm2pgsql_index_list SET started=now()"
                  "  WHERE table_id = 'ways' AND index_id = 'nodes_bucket';\n"
                  "CREATE INDEX \"{prefix}_ways_nodes_bucket_idx\""
                  "  ON {schema}\"{prefix}_ways\""
                  "  USING GIN ({schema}\"{prefix}_index_bucket\"(nodes))"
                  "  WITH (fastupdate = off) {index_tablespace};\n");

        tmpl->add("ways.fw_dep_indexes_finished",
                  "UPDATE {schema}osm2pgsql_index_list SET finished=now()"
                  "  WHERE table_id = 'ways' AND index_id = 'nodes_bucket';\n");
    } else {
        tmpl->add("ways.prepare_fw_dep_lookups",
                  "PREPARE get_ways_by_node(int8) AS"
                  "  SELECT id FROM {schema}\"{prefix}_ways\""
                  "    WHERE nodes && ARRAY[$1];\n");

        tmpl->add("ways.create_fw_dep_indexes",
                  "UPDATE {schema}osm2pgsql_index_list SET started=now()"
                  "  WHERE table_id = 'ways' AND index_id = 'nodes';\n"
                  "CREATE INDEX ON {schema}\"{prefix}_ways\" USING GIN (nodes)"
                  "  WITH (fastupdate = off) {index_tablespace};\n");

        tmpl->add("ways.fw_dep_indexes_finished",
                  "UPDATE {schema}osm2pgsql_index_list SET finished=now()"
                  "  WHERE table_id = 'ways' AND index_id = 'nodes';\n");
    }

    tmpl->add("relations.alter_table",
              "ALTER TABLE {schema}\"{prefix}_relations\""
              "  ADD COLUMN members jsonb;\n"
              "INSERT INTO {schema}osm2pgsql_index_list"
              "         (table_id, index_id, index_name)"
              "  VALUES ('relations', 'members',"
              "          '{prefix}_relations_members_idx');\n");

    tmpl->add("relations.prepare_query",
              "PREPARE get_rel(int8) AS"
              "  SELECT *"
              "    FROM {schema}\"{prefix}_relations\" WHERE id = $1;\n");

    tmpl->add("relations.prepare_fw_dep_lookups",
              "PREPARE get_relations_by_node(int8) AS"
              "  SELECT id FROM {schema}\"{prefix}_relations\""
              "    WHERE members @> ('[{{\"type\":\"node\", "
              "\"ref\":' || $1 || '}}]')::jsonb;\n"
              "PREPARE get_relations_by_way(int8) AS"
              "  SELECT id FROM {schema}\"{prefix}_relations\""
              "    WHERE members @> ('[{{\"type\":\"way\", "
              "\"ref\":' || $1 || '}}]')::jsonb;\n");

    tmpl->add(
        "relations.create_fw_dep_indexes",
        "UPDATE {schema}osm2pgsql_index_list SET started=now()"
        "  WHERE table_id = 'relations' AND index_id = 'members';\n"
        "CREATE INDEX ON {schema}\"{prefix}_relations\" USING GIN (members)"
        "  WITH (fastupdate = off) {index_tablespace};\n");

    tmpl->add("relations.fw_dep_indexes_finished",
              "UPDATE {schema}osm2pgsql_index_list SET finished=now()"
              "  WHERE table_id = 'relations' AND index_id='members';\n");
}

middle_db_t::table_desc::table_desc(osmium::item_type type,
                                    options_t const &options)
: m_copy_target(std::make_shared<db_target_descr_t>()), m_type(type)
{
    m_copy_target->name = fmt::format("{}_{}", options.prefix, id());
    m_copy_target->schema = options.middle_dbschema;
    m_copy_target->id = "id";
}

static void exec_commands(std::string const &conninfo,
                          std::vector<std::string> const &commands)
{
    pg_conn_t db_connection{conninfo};
    for (auto const &command : commands) {
        db_connection.exec(command);
    }
    db_connection.close();
}

namespace {

class TagsHandler
: public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, TagsHandler>
{
public:
    explicit TagsHandler(osmium::builder::TagListBuilder *builder)
    : m_builder(builder)
    {
        assert(builder);
    }

    bool Key(const char *str, rapidjson::SizeType length, bool)
    {
        m_key.append(str, length);
        return true;
    }

    bool String(const char *str, rapidjson::SizeType, bool)
    {
        m_builder->add_tag(m_key, str);
        m_key.clear();
        return true;
    }

private:
    osmium::builder::TagListBuilder *m_builder;
    std::string m_key;
};

template <typename T>
void pgsql_parse_tags(char const *string, T *object_builder)
{
    assert(string);
    assert(object_builder);

    osmium::builder::TagListBuilder builder{*object_builder};

    rapidjson::StringStream stream{string};
    TagsHandler handler{&builder};

    rapidjson::Reader reader;
    reader.Parse(stream, handler);
}

class MembersHandler
: public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, MembersHandler>
{
    enum member_key
    {
        none,
        type,
        ref,
        role
    };

public:
    explicit MembersHandler(osmium::builder::RelationMemberListBuilder *builder)
    : m_builder(builder)
    {
        assert(builder);
    }

    bool EndObject(rapidjson::SizeType)
    {
        if (m_ref == 0) {
            throw std::runtime_error{"No ref set in member in relation table"};
        } else if (m_type == osmium::item_type::undefined) {
            throw std::runtime_error{"No type set in member in relation table"};
        }
        m_builder->add_member(m_type, m_ref, m_role);
        m_role.clear();
        m_ref = 0;
        m_type = osmium::item_type::undefined;
        return true;
    }

    bool Key(const char *str, rapidjson::SizeType, bool)
    {
        if (!std::strcmp(str, "type")) {
            m_key = member_key::type;
        } else if (!std::strcmp(str, "ref")) {
            m_key = member_key::ref;
        } else if (!std::strcmp(str, "role")) {
            m_key = member_key::role;
        } else {
            throw std::runtime_error{
                "Invalid json key for member in relations table"};
        }
        return true;
    }

    bool Int(int value) noexcept
    {
        assert(m_key == member_key::ref);
        m_ref = value;
        return true;
    }

    bool Uint(unsigned int value) noexcept
    {
        assert(m_key == member_key::ref);
        m_ref = value;
        return true;
    }

    bool Int64(int64_t value) noexcept
    {
        assert(m_key == member_key::ref);
        m_ref = value;
        return true;
    }

    bool Uint64(uint64_t value) noexcept
    {
        assert(m_key == member_key::ref);
        m_ref = static_cast<osmid_t>(value);
        return true;
    }

    bool String(const char *str, rapidjson::SizeType length, bool)
    {
        assert(str);
        if (m_key == member_key::type) {
            if (length == 0) {
                throw std::runtime_error{
                    "Invalid member type in relations table"};
            }
            m_type = osmium::char_to_item_type(*str);
        } else if (m_key == member_key::role) {
            m_role.append(str, length);
        }
        return true;
    }

private:
    std::string m_role;
    osmium::builder::RelationMemberListBuilder *m_builder;
    osmid_t m_ref = 0;
    member_key m_key = member_key::none;
    osmium::item_type m_type = osmium::item_type::undefined;
};

void pgsql_parse_members(char const *string,
                         osmium::builder::RelationBuilder *relation_builder)
{
    assert(string);
    assert(relation_builder);

    osmium::builder::RelationMemberListBuilder builder{*relation_builder};

    rapidjson::StringStream stream{string};
    MembersHandler handler{&builder};

    rapidjson::Reader reader;
    reader.Parse(stream, handler);
}

void pgsql_parse_nodes(char const *string,
                       osmium::builder::WayBuilder *way_builder)
{
    assert(string);
    assert(way_builder);

    if (*string++ == '{') {
        osmium::builder::WayNodeListBuilder wnl_builder{*way_builder};
        while (*string != '}') {
            char *ptr = nullptr;
            wnl_builder.add_node_ref(std::strtoll(string, &ptr, 10));
            string = ptr;
            if (*string == ',') {
                ++string;
            }
        }
    }
}

} // anonymous namespace

void middle_db_t::node(osmium::Node const &node)
{
    if (node.deleted()) {
        node_delete(node.id());
    } else {
        if (on_update()) {
            node_delete(node.id());
        }
        node_set(node);
    }
}

void middle_db_t::way(osmium::Way const &way)
{
    if (way.deleted()) {
        way_delete(way.id());
    } else {
        if (on_update()) {
            way_delete(way.id());
        }
        way_set(way);
    }
}

void middle_db_t::relation(osmium::Relation const &relation)
{
    if (relation.deleted()) {
        relation_delete(relation.id());
    } else {
        if (on_update()) {
            relation_delete(relation.id());
        }
        relation_set(relation);
    }
}

void middle_db_t::add_common_columns(osmium::OSMObject const &object)
{
    m_db_copy.add_column(object.id());

    if (m_store_options.attributes) {
        m_db_copy.add_column(object.timestamp().to_iso());
        m_db_copy.add_column(object.version());
        m_db_copy.add_column(object.changeset());
        m_db_copy.add_column(object.uid());
        m_db_copy.add_column(object.user());
    }

    if (m_store_options.tags) {
        if (object.tags().empty()) {
            m_db_copy.add_null_column();
        } else {
            rapidjson::StringBuffer stream;
            rapidjson::Writer<rapidjson::StringBuffer> writer{stream};
            writer.StartObject();
            for (auto const &tag : object.tags()) {
                writer.String(tag.key());
                writer.String(tag.value());
            }
            writer.EndObject();
            m_db_copy.add_column(stream.GetString());
        }
    }
}

void middle_db_t::node_set(osmium::Node const &node)
{
    if (m_ram_cache && m_ram_cache->used_memory() < m_max_cache) {
        m_ram_cache->set(node.id(), node.location());
    }

    if (m_persistent_cache) {
        m_persistent_cache->set(node.id(), node.location());
    }

    if (m_store_options.untagged_nodes || !node.tags().empty()) {
        m_db_copy.new_line(m_tables.nodes().copy_target());
        add_common_columns(node);

        if (m_store_options.locations) {
            auto const &loc = node.location();
            m_db_copy.add_hex_geom(ewkb::create_point(loc.lon(), loc.lat()));
        } else {
            m_db_copy.add_null_column();
        }

        m_db_copy.finish_line();
    }
}

void middle_db_t::way_set(osmium::Way const &way)
{
    m_db_copy.new_line(m_tables.ways().copy_target());
    add_common_columns(way);

    if (m_store_options.way_nodes) {
        m_db_copy.new_array();
        for (auto const &wn : way.nodes()) {
            m_db_copy.add_array_elem(wn.ref());
        }
        m_db_copy.finish_array();
    } else {
        m_db_copy.add_null_column();
    }

    m_db_copy.finish_line();
}

void middle_db_t::relation_set(osmium::Relation const &relation)
{
    m_db_copy.new_line(m_tables.relations().copy_target());
    add_common_columns(relation);

    if (m_store_options.relation_members) {
        rapidjson::StringBuffer stream;
        rapidjson::Writer<rapidjson::StringBuffer> writer{stream};
        writer.StartArray();
        for (auto const &member : relation.members()) {
            writer.StartObject();
            writer.Key("type");
            writer.String(osmium::item_type_to_name(member.type()));
            writer.Key("ref");
            writer.Int64(member.ref());
            writer.Key("role");
            writer.String(member.role());
            writer.EndObject();
        }
        writer.EndArray();
        m_db_copy.add_column(stream.GetString());
    } else {
        m_db_copy.add_null_column();
    }

    m_db_copy.finish_line();
}

void middle_db_t::node_delete(osmid_t osm_id)
{
    assert(on_update());

    if (m_persistent_cache) {
        m_persistent_cache->set(osm_id, osmium::Location{});
    }

    m_db_copy.new_line(m_tables.nodes().copy_target());
    m_db_copy.delete_object(osm_id);
}

void middle_db_t::way_delete(osmid_t osm_id)
{
    assert(on_update());

    m_db_copy.new_line(m_tables.ways().copy_target());
    m_db_copy.delete_object(osm_id);
}

void middle_db_t::relation_delete(osmid_t osm_id)
{
    assert(on_update());

    m_db_copy.new_line(m_tables.relations().copy_target());
    m_db_copy.delete_object(osm_id);
}

std::size_t middle_query_db_t::nodes_get_list(osmium::WayNodeList *nodes) const
{
    assert(nodes);

    std::size_t count = 0;

    for (auto &nr : *nodes) {
        if (nr.location().valid()) {
            ++count;
        }

        if (count == nodes->size()) {
            return count;
        }
    }

    if (m_ram_cache) {
        for (auto &nr : *nodes) {
            if (!nr.location().valid()) {
                auto const location = m_ram_cache->get(nr.ref());
                if (location.valid()) {
                    nr.set_location(location);
                    ++count;
                }
            }
        }

        if (count == nodes->size()) {
            return count;
        }
    }

    if (m_persistent_cache) {
        for (auto &nr : *nodes) {
            if (!nr.location().valid()) {
                auto const location = m_persistent_cache->get(nr.ref());
                if (location.valid()) {
                    nr.set_location(location);
                    ++count;
                }
            }
        }

        return count;
    }

    util::string_id_list_t id_list;

    for (auto const &nr : *nodes) {
        if (!nr.location().valid()) {
            id_list.add(nr.ref());
        }
    }

    auto const res =
        m_db_connection.exec_prepared("get_node_list", id_list.get());
    std::unordered_map<osmid_t, osmium::Location> locs;
    for (int i = 0; i < res.num_tuples(); ++i) {
        locs.emplace(
            osmium::string_to_object_id(res.get_value(i, 0)),
            osmium::Location{std::strtod(res.get_value(i, 1), nullptr),
                             std::strtod(res.get_value(i, 2), nullptr)});
    }

    for (auto &nr : *nodes) {
        if (!nr.location().valid()) {
            auto const el = locs.find(nr.ref());
            if (el != locs.end()) {
                nr.set_location(el->second);
                ++count;
            }
        }
    }

    return count;
}

idlist_t middle_db_t::get_ways_by_node(osmid_t osm_id)
{
    return get_ids_from_db(&m_db_connection, "get_ways_by_node", osm_id);
}

idlist_t middle_db_t::get_rels_by_node(osmid_t osm_id)
{
    return get_ids_from_db(&m_db_connection, "get_relations_by_node", osm_id);
}

idlist_t middle_db_t::get_rels_by_way(osmid_t osm_id)
{
    return get_ids_from_db(&m_db_connection, "get_relations_by_way", osm_id);
}

template <typename BUILDER>
static void set_attributes(pg_result_t const &res, BUILDER &builder)
{
    if (!res.is_null(0, 1)) {
        std::string ts{res.get_value(0, 1)};
        assert(ts.size() == 19);
        ts[10] = 'T';
        ts += 'Z';
        builder.set_timestamp(ts.c_str());
    }
    if (!res.is_null(0, 2)) {
        builder.set_version(res.get_value(0, 2));
    }
    if (!res.is_null(0, 3)) {
        builder.set_changeset(res.get_value(0, 3));
    }
    if (!res.is_null(0, 4)) {
        builder.set_uid(res.get_value(0, 4));
    }
    if (!res.is_null(0, 5)) {
        builder.set_user(res.get_value(0, 5));
    }
}

bool middle_query_db_t::way_get(osmid_t id,
                                osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    auto const res = m_db_connection.exec_prepared("get_way", id);

    if (res.num_tuples() != 1) {
        return false;
    }

    {
        osmium::builder::WayBuilder builder{*buffer};
        builder.set_id(id);
        if (m_store_options.attributes) {
            set_attributes(res, builder);
        }

        auto const offset = int(m_store_options.attributes) * 5;
        pgsql_parse_tags(res.get_value(0, offset + 1), &builder);
        pgsql_parse_nodes(res.get_value(0, offset + 2), &builder);
    }

    buffer->commit();

    return true;
}

static std::size_t rel_way_members_get(pg_conn_t const &db_connection,
                                       osmium::Relation const &rel,
                                       osmium::memory::Buffer *buffer)
{
    assert(buffer);

    util::string_id_list_t id_list;

    for (auto const &m : rel.members()) {
        if (m.type() == osmium::item_type::way) {
            id_list.add(m.ref());
        }
    }

    if (id_list.empty()) {
        return 0;
    }

    auto const res = db_connection.exec_prepared("get_way_list", id_list.get());
    idlist_t const wayidspg = get_ids_from_result(res);

    // Match the list of ways coming from postgres in a different order
    // back to the list of ways given by the caller
    std::size_t outres = 0;
    for (auto const &m : rel.members()) {
        if (m.type() != osmium::item_type::way) {
            continue;
        }
        for (int j = 0; j < res.num_tuples(); ++j) {
            if (m.ref() == wayidspg[static_cast<std::size_t>(j)]) {
                {
                    osmium::builder::WayBuilder builder{*buffer};
                    builder.set_id(m.ref());
                    pgsql_parse_nodes(res.get_value(j, 1), &builder);
                }

                buffer->commit();
                ++outres;
                break;
            }
        }
    }

    return outres;
}

std::size_t
middle_query_db_t::rel_members_get(osmium::Relation const &rel,
                                   osmium::memory::Buffer *buffer,
                                   osmium::osm_entity_bits::type types) const
{
    assert(buffer);

    if (types == osmium::osm_entity_bits::way) {
        return rel_way_members_get(m_db_connection, rel, buffer);
    }

    // XXX TODO only works for ways!
    return 0;
}

bool middle_query_db_t::relation_get(osmid_t id,
                                     osmium::memory::Buffer *buffer) const
{
    assert(buffer);

    auto const res = m_db_connection.exec_prepared("get_rel", id);

    if (res.num_tuples() != 1) {
        return false;
    }

    {
        osmium::builder::RelationBuilder builder{*buffer};
        builder.set_id(id);
        if (m_store_options.attributes) {
            set_attributes(res, builder);
        }

        auto const offset = int(m_store_options.attributes) * 5;
        pgsql_parse_tags(res.get_value(0, offset + 1), &builder);
        pgsql_parse_members(res.get_value(0, offset + 2), &builder);
    }

    buffer->commit();

    return true;
}

void middle_db_t::after_nodes()
{
    m_db_copy.sync();

    auto &task = m_tables.nodes().task_primary_key();
    std::vector<std::string> commands{m_templates("nodes.analyze_table")};
    if (on_import()) {
        commands.push_back(m_templates("nodes.add_primary_key"));
        commands.push_back(m_templates("nodes.primary_key_index_finished"));
    }
    task.set(
        thread_pool().submit(std::bind(exec_commands, m_conninfo, commands)));

    if (!m_persistent_cache) {
        task.wait();
    }
}

void middle_db_t::after_ways()
{
    m_db_copy.sync();

    auto &task = m_tables.ways().task_primary_key();
    std::vector<std::string> commands{m_templates("ways.analyze_table")};
    if (on_import()) {
        commands.push_back(m_templates("ways.add_primary_key"));
        commands.push_back(m_templates("ways.primary_key_index_finished"));
    }
    task.set(
        thread_pool().submit(std::bind(exec_commands, m_conninfo, commands)));

    task.wait();
}

void middle_db_t::after_relations()
{
    m_db_copy.sync();

    // Release the copy thread and its database connection.
    m_copy_thread->finish();

    auto &task = m_tables.relations().task_primary_key();
    std::vector<std::string> commands{m_templates("relations.analyze_table")};
    if (on_import()) {
        commands.push_back(m_templates("relations.add_primary_key"));
        commands.push_back(m_templates("relations.primary_key_index_finished"));
    }
    task.set(
        thread_pool().submit(std::bind(exec_commands, m_conninfo, commands)));
}

middle_query_db_t::middle_query_db_t(
    std::string const &conninfo, std::shared_ptr<node_locations_t> ram_cache,
    std::shared_ptr<node_persistent_cache> persistent_cache,
    template_repository_t const &templates,
    db_store_options const &store_options)
: m_db_connection(conninfo), m_ram_cache(std::move(ram_cache)),
  m_persistent_cache(std::move(persistent_cache)),
  m_store_options(store_options)
{
    // Disable JIT and parallel workers as they are known to cause
    // problems when accessing the intarrays.
    m_db_connection.set_config("jit_above_cost", "-1");
    m_db_connection.set_config("max_parallel_workers_per_gather", "0");

    m_db_connection.exec(templates("nodes.prepare_query"));
    m_db_connection.exec(templates("ways.prepare_query"));
    m_db_connection.exec(templates("relations.prepare_query"));
}

void middle_db_t::override_opts_for_testing()
{
    char const *const middle_options = std::getenv("OSM2PGSQL_MIDDLE_OPTS");
    if (!middle_options) {
        return;
    }

    auto const opts = osmium::split_string(middle_options, ',', true);

    for (std::string opt : opts) {
        bool const choice = (opt[0] != '-');

        if (!choice) {
            opt.erase(0, 1);
        }

        if (opt == "untagged_nodes") {
            m_store_options.untagged_nodes = choice;
        } else if (opt == "tags") {
            m_store_options.tags = choice;
        } else if (opt == "attributes") {
            m_store_options.attributes = choice;
        } else if (opt == "locations") {
            m_store_options.locations = choice;
        } else if (opt == "way_nodes") {
            m_store_options.way_nodes = choice;
        } else if (opt == "relation_members") {
            m_store_options.relation_members = choice;
        } else {
            log_warn("Unknown middle option '{}'", opt);
        }
    }
}

void middle_db_t::log_store_options()
{
    log_debug("Middle 'db': ram_cache={} persistent_cache={}",
              m_ram_cache ? "yes" : "no", m_persistent_cache ? "yes" : "no");

    log_debug("Middle 'db' options:");
    log_debug("  drop_tables: {}", m_store_options.drop_tables);
    log_debug("  forward_dependencies: {}",
              m_store_options.forward_dependencies);
    log_debug("  untagged_nodes: {}", m_store_options.untagged_nodes);
    log_debug("  tags: {}", m_store_options.tags);
    log_debug("  attributes: {}", m_store_options.attributes);
    log_debug("  locations: {}", m_store_options.locations);
    log_debug("  way_nodes: {}", m_store_options.way_nodes);
    log_debug("  relation_members: {}", m_store_options.relation_members);
}

static bool check_bucket_index(pg_conn_t *db_connection,
                               std::string const &prefix)
{
    assert(db_connection);

    auto const res = db_connection->query(
        PGRES_TUPLES_OK,
        "SELECT relname FROM pg_class WHERE relkind='i' AND"
        "  relname = '{}_ways_nodes_bucket_idx';"_format(prefix));
    return res.num_tuples() > 0;
}

middle_db_t::middle_db_t(std::shared_ptr<thread_pool_t> thread_pool,
                         options_t const *options)
: middle_t(std::move(thread_pool)),
  m_conninfo(options->database_options.conninfo()), m_db_connection(m_conninfo),
  m_copy_thread(std::make_shared<db_copy_thread_t>(m_conninfo)),
  m_db_copy(m_copy_thread),
  m_mode(options->append ? mode::update : mode::import)
{
    assert(options);

    if (options->cache > 0) {
        m_ram_cache.reset(new node_locations_t{});
        m_max_cache = static_cast<std::size_t>(options->cache * 1024 * 1024);
    }

    if (!options->flat_node_file.empty()) {
        m_persistent_cache.reset(new node_persistent_cache{
            options->flat_node_file, options->droptemp});
        m_store_options.locations = false;
        m_store_options.untagged_nodes = false;
    }

    m_store_options.forward_dependencies = options->with_forward_dependencies;

    if (on_import()) {
        m_store_options.attributes = options->extra_attributes;
        m_store_options.drop_tables = options->droptemp;
        m_store_options.has_bucket_index = options->way_node_index_id_shift > 0;
    } else {
        auto const res = m_db_connection.query(
            PGRES_TUPLES_OK, "SELECT with_attributes"
                             "  FROM {}osm2pgsql_table_list LIMIT 1;"_format(
                                 options->middle_dbschema));
        if (res.num_tuples() == 0) {
            throw std::runtime_error{"invalid db schema"}; // XXX
        }
        char const *const with_attributes = res.get_value(0, 0);
        m_store_options.attributes = (with_attributes == std::string{"t"});
        m_store_options.has_bucket_index =
            check_bucket_index(&m_db_connection, options->prefix);

        if (!m_store_options.has_bucket_index &&
            options->with_forward_dependencies) {
            log_debug("You don't have a bucket index. See manual for details.");
        }
    }

    for (auto const type : {osmium::item_type::node, osmium::item_type::way,
                            osmium::item_type::relation}) {
        m_tables(type) = table_desc{type, *options};
    }

    override_opts_for_testing();
    log_store_options();

    setup_template_variables(&m_templates, *options);
}

void middle_db_t::start()
{
    setup_templates(&m_templates, m_store_options.has_bucket_index);

    if (on_update()) {
        // Disable JIT and parallel workers as they are known to cause
        // problems when accessing the intarrays.
        m_db_connection.set_config("jit_above_cost", "-1");
        m_db_connection.set_config("max_parallel_workers_per_gather", "0");

        // Prepare queries for finding dependent objects.
        m_db_connection.exec(m_templates("ways.prepare_fw_dep_lookups"));
        m_db_connection.exec(m_templates("relations.prepare_fw_dep_lookups"));
    } else {
        m_db_connection.exec(m_templates("init"));
        for (auto const &table : m_tables) {
            log_debug("Setting up table '{}'", table.name());
            m_db_connection.exec(m_templates(table.id() + ".drop_table"));
            m_db_connection.exec(m_templates(table.id() + ".create_table"));
            if (m_store_options.attributes) {
                m_db_connection.exec(
                    m_templates(table.id() + ".add_attribute_columns"));
            }
            if (m_store_options.tags) {
                m_db_connection.exec(
                    m_templates(table.id() + ".alter_table_add_tags"));
            }
            m_db_connection.exec(m_templates(table.id() + ".alter_table"));
        }
    }
}

void middle_db_t::stop()
{
    auto const mbyte = 1024 * 1024;

    if (m_ram_cache) {
        log_debug("Middle 'db': Node locations: size={} bytes={}M",
                  m_ram_cache->size(), m_ram_cache->used_memory() / mbyte);

        m_ram_cache.reset();
    } else {
        log_debug(
            "Middle 'db': No node locations stored in RAM (cache disabled)");
    }

    if (m_persistent_cache) {
        m_persistent_cache.reset();
    }

    for (auto &table : m_tables) {
        auto const run_time = table.task_primary_key().wait();
        log_info("Creating PK index for {} took {}", table.id(), run_time);
    }

    if (m_store_options.drop_tables) {
        // Dropping the tables is fast, so do it synchronously to guarantee
        // that the space is freed before creating the other indices.
        for (auto &table : m_tables) {
            util::timer_t timer;

            log_info("Dropping table '{}'", table.id());
            m_db_connection.exec(m_templates(table.id() + ".drop_table"));
            log_info("Done postprocessing on table '{}' in {}", table.id(),
                     util::human_readable_duration(timer.stop()));
        }
        m_db_connection.exec(m_templates("drop"));
    } else if (on_import() && m_store_options.forward_dependencies) {
        // Building the indexes takes time, so do it asynchronously.
        for (auto const type :
             {osmium::item_type::way, osmium::item_type::relation}) {
            auto &table = m_tables(type);
            std::vector<std::string> commands{
                m_templates(table.id() + ".create_fw_dep_indexes"),
                m_templates(table.id() + ".fw_dep_indexes_finished")};

            if (!commands.front().empty()) {
                log_info("Building index on table '{}'", table.name());
                table.task_fw_dep_index().set(thread_pool().submit(
                    std::bind(exec_commands, m_conninfo, commands)));
            }
        }
        for (auto &table : m_tables) {
            auto const duration = table.task_fw_dep_index().wait();
            if (duration.count() > 0) {
                log_info("Done postprocessing on table '{}' in {}",
                         table.name(), util::human_readable_duration(duration));
            }
        }
    }
}

std::shared_ptr<middle_query_t> middle_db_t::get_query_instance()
{
    return std::make_shared<middle_query_db_t>(m_conninfo, m_ram_cache,
                                               m_persistent_cache, m_templates,
                                               m_store_options);
}
