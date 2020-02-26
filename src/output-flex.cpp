
#include "config.h"

#include "expire-tiles.hpp"
#include "geom-transform.hpp"
#include "lua-init.hpp"
#include "lua-utils.hpp"
#include "middle.hpp"
#include "options.hpp"
#include "osmtypes.hpp"
#include "output-flex.hpp"
#include "pgsql.hpp"
#include "reprojection.hpp"
#include "taginfo-impl.hpp"
#include "version.hpp"
#include "wkb.hpp"

#include <osmium/osm/types_from_string.hpp>

extern "C"
{
#include <lauxlib.h>
#include <lualib.h>
}

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

// Mutex used to coordinate access to Lua code
static std::mutex lua_mutex;

// Lua can't call functions on C++ objects directly. This macro defines simple
// C "trampoline" functions which are called from Lua which get the current
// context (the output_flex_t object) and call the respective function on the
// context object.
#define TRAMPOLINE(func_name, lua_name)                                        \
    static int lua_trampoline_##func_name(lua_State *lua_state)                \
    {                                                                          \
        try {                                                                  \
            return static_cast<output_flex_t *>(luaX_get_context(lua_state))   \
                ->func_name();                                                 \
        } catch (std::exception const &e) {                                    \
            return luaL_error(lua_state, "Error in '" #lua_name "': %s\n",     \
                              e.what());                                       \
        } catch (...) {                                                        \
            return luaL_error(lua_state,                                       \
                              "Unknown error in '" #lua_name "'.\n");          \
        }                                                                      \
    }

TRAMPOLINE(app_define_table, define_table)
TRAMPOLINE(app_mark, mark)
TRAMPOLINE(app_get_bbox, get_bbox)
TRAMPOLINE(table_name, name)
TRAMPOLINE(table_schema, schema)
TRAMPOLINE(table_add_row, add_row)
TRAMPOLINE(table_columns, columns)
TRAMPOLINE(table_tostring, __tostring)

static char const osm2pgsql_table_name[] = "osm2pgsql.table";
static char const osm2pgsql_object_metatable[] = "osm2pgsql.object_metatable";

static void push_osm_object_to_lua_stack(lua_State *lua_state,
                                         osmium::OSMObject const &object,
                                         bool with_attributes)
{
    assert(lua_state);

    /**
     * Table will have 7 fields (id, version, timestamp, changeset, uid, user,
     * tags) for all object types plus 2 (is_closed, nodes) for ways or 1
     * (members) for relations.
     */
    constexpr int const max_table_size = 9;

    lua_createtable(lua_state, 0, max_table_size);

    luaX_add_table_int(lua_state, "id", object.id());

    if (with_attributes) {
        if (object.version() != 0U) {
            luaX_add_table_int(lua_state, "version", object.version());
        } else {
            // This is a workaround, because the middle will give us the
            // attributes as pseudo-tags.
            char const *const val = object.tags()["osm_version"];
            if (val) {
                luaX_add_table_int(lua_state, "version",
                                   osmium::string_to_object_version(val));
            }
        }

        if (object.timestamp().valid()) {
            luaX_add_table_int(lua_state, "timestamp",
                               object.timestamp().seconds_since_epoch());
        } else {
            // This is a workaround, because the middle will give us the
            // attributes as pseudo-tags.
            char const *const val = object.tags()["osm_timestamp"];
            if (val) {
                auto const timestamp = osmium::Timestamp{val};
                luaX_add_table_int(lua_state, "timestamp",
                                   timestamp.seconds_since_epoch());
            }
        }

        if (object.changeset() != 0U) {
            luaX_add_table_int(lua_state, "changeset", object.changeset());
        } else {
            char const *const val = object.tags()["osm_changeset"];
            // This is a workaround, because the middle will give us the
            // attributes as pseudo-tags.
            if (val) {
                luaX_add_table_int(lua_state, "changeset",
                                   osmium::string_to_changeset_id(val));
            }
        }

        if (object.uid() != 0U) {
            luaX_add_table_int(lua_state, "uid", object.uid());
        } else {
            // This is a workaround, because the middle will give us the
            // attributes as pseudo-tags.
            char const *const val = object.tags()["osm_uid"];
            if (val) {
                luaX_add_table_int(lua_state, "uid",
                                   osmium::string_to_uid(val));
            }
        }

        if (object.user()[0] != '\0') {
            luaX_add_table_str(lua_state, "user", object.user());
        } else {
            // This is a workaround, because the middle will give us the
            // attributes as pseudo-tags.
            char const *const val = object.tags()["osm_user"];
            if (val) {
                luaX_add_table_str(lua_state, "user", val);
            }
        }
    }

    if (object.type() == osmium::item_type::way) {
        auto const &way = static_cast<osmium::Way const &>(object);
        luaX_add_table_bool(lua_state, "is_closed", way.is_closed());
        luaX_add_table_array(lua_state, "nodes", way.nodes(),
                             [&](osmium::NodeRef const &wn) {
                                 lua_pushinteger(lua_state, wn.ref());
                             });
    } else if (object.type() == osmium::item_type::relation) {
        auto const &relation = static_cast<osmium::Relation const &>(object);
        luaX_add_table_array(
            lua_state, "members", relation.members(),
            [&](osmium::RelationMember const &member) {
                lua_createtable(lua_state, 0, 3);
                std::array<char, 2> tmp{"x"};
                tmp[0] = osmium::item_type_to_char(member.type());
                luaX_add_table_str(lua_state, "type", &tmp[0]);
                luaX_add_table_int(lua_state, "ref", member.ref());
                luaX_add_table_str(lua_state, "role", member.role());
            });
    }

    lua_pushliteral(lua_state, "tags");
    lua_createtable(lua_state, 0, (int)object.tags().size());
    for (auto const &tag : object.tags()) {
        luaX_add_table_str(lua_state, tag.key(), tag.value());
    }
    lua_rawset(lua_state, -3);

    // Set the metatable of this object
    lua_pushlightuserdata(lua_state, (void *)osm2pgsql_object_metatable);
    lua_gettable(lua_state, LUA_REGISTRYINDEX);
    lua_setmetatable(lua_state, -2);
}

static bool str2bool(char const *str) noexcept
{
    return (std::strcmp(str, "yes") == 0) || (std::strcmp(str, "true") == 0);
}

static int str2direction(char const *str) noexcept
{
    if ((std::strcmp(str, "yes") == 0) || (std::strcmp(str, "true") == 0) ||
        (std::strcmp(str, "1") == 0)) {
        return 1;
    }

    if (std::strcmp(str, "-1") == 0) {
        return -1;
    }

    return 0;
}

static int sgn(int val) noexcept
{
    if (val > 0) {
        return 1;
    }
    if (val < 0) {
        return -1;
    }
    return 0;
}

void output_flex_t::write_column(
    db_copy_mgr_t<db_deleter_by_type_and_id_t> *copy_mgr,
    flex_table_column_t const &column)
{
    lua_getfield(lua_state(), -1, column.name().c_str());
    int const ltype = lua_type(lua_state(), -1);

    // A Lua nil value is always translated to a database NULL
    if (ltype == LUA_TNIL) {
        copy_mgr->add_null_column();
        lua_pop(lua_state(), 1);
        return;
    }

    if ((column.type() == table_column_type::sql) ||
        (column.type() == table_column_type::text)) {
        auto const *const str = lua_tolstring(lua_state(), -1, nullptr);
        if (!str) {
            throw std::runtime_error{"Invalid type '{}' for text column"_format(
                lua_typename(lua_state(), ltype))};
        }
        copy_mgr->add_column(str);
    } else if (column.type() == table_column_type::boolean) {
        switch (ltype) {
        case LUA_TBOOLEAN:
            copy_mgr->add_column(lua_toboolean(lua_state(), -1) != 0);
            break;
        case LUA_TNUMBER:
            copy_mgr->add_column(lua_tointeger(lua_state(), -1) != 0);
            break;
        case LUA_TSTRING:
            copy_mgr->add_column(
                str2bool(lua_tolstring(lua_state(), -1, nullptr)));
            break;
        default:
            throw std::runtime_error{
                "Invalid type '{}' for boolean column"_format(
                    lua_typename(lua_state(), ltype))};
        }
    } else if (column.type() == table_column_type::int2) {
        // cast here is on okay, because the database column is only 16bit
        copy_mgr->add_column((int16_t)lua_tointeger(lua_state(), -1));
    } else if (column.type() == table_column_type::int4) {
        // cast here is on okay, because the database column is only 32bit
        copy_mgr->add_column((int32_t)lua_tointeger(lua_state(), -1));
    } else if (column.type() == table_column_type::int8) {
        copy_mgr->add_column(lua_tointeger(lua_state(), -1));
    } else if (column.type() == table_column_type::real) {
        copy_mgr->add_column(lua_tonumber(lua_state(), -1));
    } else if (column.type() == table_column_type::hstore) {
        if (ltype == LUA_TTABLE) {
            copy_mgr->new_hash();

            lua_pushnil(lua_state());
            while (lua_next(lua_state(), -2) != 0) {
                char const *const key = lua_tostring(lua_state(), -2);
                char const *const val = lua_tostring(lua_state(), -1);
                if (key == nullptr) {
                    int const ltype_key = lua_type(lua_state(), -2);
                    throw std::runtime_error{
                        "NULL key for hstore. Possibly this is due to"
                        "an incorrect data type '{}' as key."_format(
                            lua_typename(lua_state(), ltype_key))};
                }
                if (val == nullptr) {
                    int const ltype_value = lua_type(lua_state(), -1);
                    throw std::runtime_error{
                        "NULL value for hstore. Possibly this is due to"
                        "an incorrect data type '{}' for key '{}'."_format(
                            lua_typename(lua_state(), ltype_value), key)};
                }
                copy_mgr->add_hash_elem(key, val);
                lua_pop(lua_state(), 1);
            }

            copy_mgr->finish_hash();
        } else {
            throw std::runtime_error{
                "Invalid type '{}' for hstore column"_format(
                    lua_typename(lua_state(), ltype))};
        }
    } else if (column.type() == table_column_type::direction) {
        switch (ltype) {
        case LUA_TBOOLEAN:
            copy_mgr->add_column(lua_toboolean(lua_state(), -1));
            break;
        case LUA_TNUMBER:
            copy_mgr->add_column(sgn(lua_tointeger(lua_state(), -1)));
            break;
        case LUA_TSTRING:
            copy_mgr->add_column(
                str2direction(lua_tolstring(lua_state(), -1, nullptr)));
            break;
        default:
            throw std::runtime_error{
                "Invalid type '{}' for direction column"_format(
                    lua_typename(lua_state(), ltype))};
        }
    } else {
        throw std::runtime_error{
            "Column type {} not implemented"_format(column.type())};
    }

    lua_pop(lua_state(), 1);
}

db_copy_mgr_t<db_deleter_by_type_and_id_t> *
output_flex_t::get_copy_mgr(flex_table_t *table)
{
    for (std::size_t n = 0; n < m_tables.size(); ++n) {
        if (&(m_tables[n]) == table) {
            return &m_copy_mgrs.at(n);
        }
    }
    assert(false);
}

void output_flex_t::write_row(flex_table_t *table, osmium::item_type id_type,
                              osmid_t id, std::string const &geom)
{
    assert(table);
    auto *copy_mgr = get_copy_mgr(table);
    copy_mgr->new_line(table->target());

    for (auto const &column : *table) {
        if (column.create_only()) {
            continue;
        }
        if (column.type() == table_column_type::id_type) {
            copy_mgr->add_column(type_to_char(id_type));
        } else if (column.type() == table_column_type::id_num) {
            copy_mgr->add_column(id);
        } else if (column.is_geometry_column()) {
            assert(!geom.empty());
            copy_mgr->add_hex_geom(geom);
        } else if (column.type() == table_column_type::area) {
            if (geom.empty()) {
                copy_mgr->add_null_column();
            } else {
                double const area =
                    get_options()->reproject_area
                        ? ewkb::parser_t(geom).get_area<reprojection>(
                              get_options()->projection.get())
                        : ewkb::parser_t(geom)
                              .get_area<osmium::geom::IdentityProjection>();
                copy_mgr->add_column(area);
            }
        } else {
            write_column(copy_mgr, column);
        }
    }

    copy_mgr->finish_line();
}

int output_flex_t::app_mark()
{
    char const *type_name = luaL_checkstring(lua_state(), 1);
    if (!type_name) {
        return 0;
    }

    osmium::object_id_type const id = luaL_checkinteger(lua_state(), 2);

    if (type_name[0] == 'w') {
        m_stage2_ways_tracker->mark(id);
    } else if (type_name[0] == 'r') {
        m_stage2_rels_tracker->mark(id);
    }

    return 0;
}

// Gets all way nodes from the middle the first time this is called.
std::size_t output_flex_t::get_way_nodes()
{
    assert(m_context_way);
    if (m_num_way_nodes == std::numeric_limits<std::size_t>::max()) {
        m_num_way_nodes = m_mid->nodes_get_list(&m_context_way->nodes());
    }

    return m_num_way_nodes;
}

int output_flex_t::app_get_bbox()
{
    if (lua_gettop(lua_state()) > 1) {
        throw std::runtime_error{"No parameter(s) needed for get_box()"};
    }

    if (m_context_node) {
        lua_pushnumber(lua_state(), m_context_node->location().lon());
        lua_pushnumber(lua_state(), m_context_node->location().lat());
        lua_pushnumber(lua_state(), m_context_node->location().lon());
        lua_pushnumber(lua_state(), m_context_node->location().lat());
        return 4;
    }

    if (m_context_way) {
        get_way_nodes();
        auto const bbox = m_context_way->envelope();
        if (bbox.valid()) {
            lua_pushnumber(lua_state(), bbox.bottom_left().lon());
            lua_pushnumber(lua_state(), bbox.bottom_left().lat());
            lua_pushnumber(lua_state(), bbox.top_right().lon());
            lua_pushnumber(lua_state(), bbox.top_right().lat());
            return 4;
        }
    }

    return 0;
}

static void check_name(std::string const &name, char const *in)
{
    auto const pos = name.find_first_of("\"',.;$%&/()<>{}=?^*#");

    if (pos == std::string::npos) {
        return;
    }

    throw std::runtime_error{
        "Special characters are not allowed in {} names: '{}'"_format(in,
                                                                      name)};
}

flex_table_t &output_flex_t::create_flex_table()
{
    std::string const table_name =
        luaX_get_table_string(lua_state(), "name", -1, "The table");

    check_name(table_name, "table");

    auto const it = std::find_if(m_tables.cbegin(), m_tables.cend(),
                                 [&table_name](flex_table_t const &table) {
                                     return table.name() == table_name;
                                 });
    if (it != m_tables.cend()) {
        throw std::runtime_error{
            "Table with that name already exists: '{}'"_format(table_name)};
    }

    m_tables.emplace_back(table_name, get_options()->projection->target_srs(),
                          get_options()->append);
    auto &new_table = m_tables.back();

    lua_pop(lua_state(), 1);

    // optional "schema" field
    lua_getfield(lua_state(), -1, "schema");
    if (lua_isstring(lua_state(), -1)) {
        std::string const schema = lua_tostring(lua_state(), -1);
        check_name(schema, "schame");
        new_table.set_schema(schema);
    }
    lua_pop(lua_state(), 1);

    // optional "data_tablespace" field
    lua_getfield(lua_state(), -1, "data_tablespace");
    if (lua_isstring(lua_state(), -1)) {
        std::string const schema = lua_tostring(lua_state(), -1);
        check_name(schema, "data_tablespace");
        new_table.set_data_tablespace(schema);
    }
    lua_pop(lua_state(), 1);

    // optional "index_tablespace" field
    lua_getfield(lua_state(), -1, "index_tablespace");
    if (lua_isstring(lua_state(), -1)) {
        std::string const schema = lua_tostring(lua_state(), -1);
        check_name(schema, "index_tablespace");
        new_table.set_index_tablespace(schema);
    }
    lua_pop(lua_state(), 1);

    return new_table;
}

void output_flex_t::setup_id_columns(flex_table_t *table)
{
    assert(table);
    lua_getfield(lua_state(), -1, "ids");
    if (lua_type(lua_state(), -1) != LUA_TTABLE) {
        fmt::print(stderr,
                   "WARNING! Table '{}' doesn't have an 'ids' column. Updates"
                   " and expire will not work!\n",
                   table->name());
        lua_pop(lua_state(), 1); // ids
        return;
    }

    std::string const type{
        luaX_get_table_string(lua_state(), "type", -1, "The ids field")};

    if (type == "node") {
        table->set_id_type(osmium::item_type::node);
    } else if (type == "way") {
        table->set_id_type(osmium::item_type::way);
    } else if (type == "relation") {
        table->set_id_type(osmium::item_type::relation);
    } else if (type == "area") {
        table->set_id_type(osmium::item_type::area);
    } else if (type == "any") {
        std::string type_column_name{"osm_type"};
        lua_getfield(lua_state(), -1, "type_column");
        if (lua_isstring(lua_state(), -1)) {
            type_column_name = lua_tolstring(lua_state(), -1, nullptr);
            check_name(type_column_name, "column");
        }
        lua_pop(lua_state(), 1); // type_column
        auto &column = table->add_column(type_column_name, "id_type");
        column.set_not_null();
        table->set_id_type(osmium::item_type::undefined);
    } else {
        throw std::runtime_error{"Unknown ids type: " + type};
    }

    std::string const name =
        luaX_get_table_string(lua_state(), "id_column", -2, "The ids field");
    check_name(name, "column");

    auto &column = table->add_column(name, "id_num");
    column.set_not_null();
    lua_pop(lua_state(), 3); // id_column, type, ids
}

void output_flex_t::setup_flex_table_columns(flex_table_t *table)
{
    assert(table);
    lua_getfield(lua_state(), -1, "columns");
    if (lua_type(lua_state(), -1) != LUA_TTABLE) {
        throw std::runtime_error{
            "No columns defined for table '{}'"_format(table->name())};
    }

    std::size_t num_columns = 0;
    lua_pushnil(lua_state());
    while (lua_next(lua_state(), -2) != 0) {
        if (!lua_isnumber(lua_state(), -2)) {
            throw std::runtime_error{
                "The 'columns' field must contain an array"};
        }
        if (!lua_istable(lua_state(), -1)) {
            throw std::runtime_error{
                "The entries in the 'columns' array must be tables"};
        }

        char const *const type =
            luaX_get_table_string(lua_state(), "type", -1, "Column entry");
        char const *const name =
            luaX_get_table_string(lua_state(), "column", -2, "Column entry");
        check_name(name, "column");

        auto &column = table->add_column(name, type);

        column.set_not_null(luaX_get_table_bool(lua_state(), "not_null", -3,
                                                "Entry 'not_null'", false));

        column.set_create_only(luaX_get_table_bool(
            lua_state(), "create_only", -4, "Entry 'create_only'", false));

        lua_pop(lua_state(), 5); // create_only, not_null, column, type, table
        ++num_columns;
    }

    if (num_columns == 0) {
        throw std::runtime_error{
            "No columns defined for table '{}'"_format(table->name())};
    }
}

int output_flex_t::app_define_table()
{
    luaL_checktype(lua_state(), 1, LUA_TTABLE);

    auto &new_table = create_flex_table();
    setup_id_columns(&new_table);
    setup_flex_table_columns(&new_table);

    lua_pushlightuserdata(lua_state(), (void *)(m_tables.size()));
    luaL_getmetatable(lua_state(), osm2pgsql_table_name);
    lua_setmetatable(lua_state(), -2);

    return 1;
}

// Check function parameters of all osm2pgsql.table functions and return the
// flex table this function is on.
flex_table_t &output_flex_t::table_func_params(int n)
{
    if (lua_gettop(lua_state()) != n) {
        throw std::runtime_error{"Need {} parameter(s)"_format(n)};
    }

    void *user_data = lua_touserdata(lua_state(), 1);
    if (user_data == nullptr || !lua_getmetatable(lua_state(), 1)) {
        throw std::runtime_error{
            "first parameter must be of type osm2pgsql.table"};
    }

    luaL_getmetatable(lua_state(), osm2pgsql_table_name);
    if (!lua_rawequal(lua_state(), -1, -2)) {
        throw std::runtime_error{
            "first parameter must be of type osm2pgsql.table"};
    }
    lua_pop(lua_state(), 2);

    auto &table = m_tables.at(reinterpret_cast<uintptr_t>(user_data) - 1);
    lua_remove(lua_state(), 1);
    return table;
}

int output_flex_t::table_tostring()
{
    auto const &table = table_func_params(1);

    std::string const str{"osm2pgsql.table[{}]"_format(table.name())};
    lua_pushstring(lua_state(), str.c_str());

    return 1;
}

int output_flex_t::table_add_row()
{
    auto &table = table_func_params(2);
    luaL_checktype(lua_state(), 1, LUA_TTABLE);

    if (m_context_node) {
        if (!table.matches_type(osmium::item_type::node)) {
            throw std::runtime_error{
                "Trying to add node to table '{}'"_format(table.name())};
        }
        add_row(&table, *m_context_node);
    } else if (m_context_way) {
        if (!table.matches_type(osmium::item_type::way)) {
            throw std::runtime_error{
                "Trying to add way to table '{}'"_format(table.name())};
        }
        if (m_in_stage2) {
            delete_from_table(&table, osmium::item_type::way,
                              m_context_way->id());
        }
        add_row(&table, *m_context_way);
    } else if (m_context_relation) {
        if (!table.matches_type(osmium::item_type::relation)) {
            throw std::runtime_error{
                "Trying to add relation to table '{}'"_format(table.name())};
        }
        if (m_in_stage2) {
            delete_from_table(&table, osmium::item_type::relation,
                              m_context_relation->id());
        }
        add_row(&table, *m_context_relation);
    } else {
        throw std::runtime_error{"The add_row() function can only be called "
                                 "from inside a process function"};
    }

    return 0;
}

int output_flex_t::table_columns()
{
    auto const &table = table_func_params(1);

    lua_createtable(lua_state(), (int)table.num_columns(), 0);

    int n = 0;
    for (auto const &column : table) {
        lua_pushinteger(lua_state(), ++n);
        lua_newtable(lua_state());

        luaX_add_table_str(lua_state(), "name", column.name().c_str());
        luaX_add_table_str(lua_state(), "type", column.type_name().c_str());
        luaX_add_table_str(lua_state(), "sql_type",
                           column.sql_type_name(table.srid()).c_str());
        luaX_add_table_str(lua_state(), "sql_modifiers",
                           column.sql_modifiers().c_str());
        luaX_add_table_bool(lua_state(), "not_null", column.not_null());
        luaX_add_table_bool(lua_state(), "create_only", column.create_only());

        lua_rawset(lua_state(), -3);
    }
    return 1;
}

int output_flex_t::table_name()
{
    auto const &table = table_func_params(1);
    lua_pushstring(lua_state(), table.name().c_str());
    return 1;
}

int output_flex_t::table_schema()
{
    auto const &table = table_func_params(1);
    lua_pushstring(lua_state(), table.schema().c_str());
    return 1;
}

static std::unique_ptr<geom_transform_t>
get_transform(lua_State *lua_state, flex_table_column_t const &column)
{
    assert(lua_state);
    assert(lua_gettop(lua_state) == 1);

    std::unique_ptr<geom_transform_t> transform{};

    lua_getfield(lua_state, -1, column.name().c_str());
    int const ltype = lua_type(lua_state, -1);
    if (ltype != LUA_TTABLE) {
        lua_pop(lua_state, 1); // geom field
        return transform;
    }

    lua_getfield(lua_state, -1, "create");
    char const *create_type = lua_tostring(lua_state, -1);
    if (create_type == nullptr) {
        throw std::runtime_error{
            "Missing geometry transformation for column '{}'"_format(
                column.name())};
    }

    transform = create_geom_transform(create_type);
    lua_pop(lua_state, 1); // 'create' field
    init_geom_transform(transform.get(), lua_state);
    if (!transform->is_compatible_with(column.type())) {
        throw std::runtime_error{
            "Geometry transformation is not compatible "
            "with column type '{}'"_format(column.type_name())};
    }

    lua_pop(lua_state, 1); // geom field

    return transform;
}

static geom_transform_t const *
get_default_transform(flex_table_column_t const &column,
                      osmium::item_type object_type)
{
    static geom_transform_point_t const default_transform_node_to_point{};
    static geom_transform_line_t const default_transform_way_to_line{};
    static geom_transform_area_t const default_transform_way_to_area{};

    switch (object_type) {
    case osmium::item_type::node:
        if (column.type() == table_column_type::point) {
            return &default_transform_node_to_point;
        }
        break;
    case osmium::item_type::way:
        if (column.type() == table_column_type::linestring) {
            return &default_transform_way_to_line;
        }
        if (column.type() == table_column_type::polygon) {
            return &default_transform_way_to_area;
        }
        break;
    default:
        break;
    }

    throw std::runtime_error{
        "Missing geometry transformation for column '{}'"_format(
            column.name())};
}

geom::osmium_builder_t::wkbs_t
output_flex_t::run_transform(geom_transform_t const *transform,
                             osmium::Node const &node)
{
    return transform->run(&m_builder, node);
}

geom::osmium_builder_t::wkbs_t
output_flex_t::run_transform(geom_transform_t const *transform,
                             osmium::Way const & /*way*/)
{
    if (get_way_nodes() <= 1U) {
        return {};
    }
    return transform->run(&m_builder, m_context_way);
}

geom::osmium_builder_t::wkbs_t
output_flex_t::run_transform(geom_transform_t const *transform,
                             osmium::Relation const &relation)
{
    m_buffer.clear();
    auto const num_ways =
        m_mid->rel_way_members_get(relation, nullptr, m_buffer);

    if (num_ways == 0) {
        return {};
    }

    for (auto &way : m_buffer.select<osmium::Way>()) {
        m_mid->nodes_get_list(&(way.nodes()));
    }

    return transform->run(&m_builder, relation, m_buffer);
}

template <typename OBJECT>
void output_flex_t::add_row(flex_table_t *table, OBJECT const &object)
{
    assert(table);

    osmid_t const id = table->map_id(object.type(), object.id());

    if (!table->has_geom_column()) {
        write_row(table, object.type(), id, "");
        return;
    }

    auto const geom_transform =
        get_transform(lua_state(), table->geom_column());
    assert(lua_gettop(lua_state()) == 1);

    geom_transform_t const *transform = geom_transform.get();

    if (!transform) {
        transform = get_default_transform(table->geom_column(), object.type());
    }

    auto const wkbs = run_transform(transform, object);
    for (auto const &wkb : wkbs) {
        m_expire.from_wkb(wkb.c_str(), id);
        write_row(table, object.type(), id, wkb);
    }
}

void output_flex_t::call_process_function(int index,
                                          osmium::OSMObject const &object)
{
    std::lock_guard<std::mutex> guard{lua_mutex};

    assert(lua_gettop(lua_state()) == 3);

    lua_pushvalue(lua_state(), index); // the function to call
    push_osm_object_to_lua_stack(
        lua_state(), object,
        get_options()->extra_attributes); // the single argument

    luaX_set_context(lua_state(), this);
    if (lua_pcall(lua_state(), 1, 0, 0)) {
        throw std::runtime_error{"Failed to execute lua processing function:"
                                 " {}"_format(lua_tostring(lua_state(), -1))};
    }
}

void output_flex_t::enqueue_ways(pending_queue_t &job_queue, osmid_t id,
                                 std::size_t output_id, std::size_t &added)
{
    osmid_t const prev = m_ways_pending_tracker.last_returned();
    if (id_tracker::is_valid(prev) && prev >= id) {
        if (prev > id) {
            job_queue.push(pending_job_t(id, output_id));
        }
        // already done the job
        return;
    }

    //make sure we get the one passed in
    if (!m_ways_done_tracker->is_marked(id) && id_tracker::is_valid(id)) {
        job_queue.push(pending_job_t(id, output_id));
        added++;
    }

    //grab the first one or bail if its not valid
    osmid_t popped = m_ways_pending_tracker.pop_mark();
    if (!id_tracker::is_valid(popped)) {
        return;
    }

    //get all the ones up to the id that was passed in
    while (popped < id) {
        if (!m_ways_done_tracker->is_marked(popped)) {
            job_queue.push(pending_job_t(popped, output_id));
            added++;
        }
        popped = m_ways_pending_tracker.pop_mark();
    }

    //make sure to get this one as well and move to the next
    if (popped > id) {
        if (!m_ways_done_tracker->is_marked(popped) &&
            id_tracker::is_valid(popped)) {
            job_queue.push(pending_job_t(popped, output_id));
            added++;
        }
    }
}

void output_flex_t::pending_way(osmid_t id, int exists)
{
    if (!m_has_process_way) {
        return;
    }

    m_buffer.clear();
    if (!m_mid->ways_get(id, m_buffer)) {
        return;
    }

    if (exists) {
        way_delete(id);
        auto const rel_ids = m_mid->relations_using_way(id);
        for (auto const id : rel_ids) {
            m_rels_pending_tracker.mark(id);
        }
    }

    auto &way = m_buffer.get<osmium::Way>(0);

    m_context_way = &way;
    call_process_function(2, way);
    m_context_way = nullptr;
    m_num_way_nodes = std::numeric_limits<std::size_t>::max();
    m_buffer.clear();
}

void output_flex_t::enqueue_relations(pending_queue_t &job_queue, osmid_t id,
                                      std::size_t output_id, std::size_t &added)
{
    if (!m_has_process_relation) {
        return;
    }

    osmid_t const prev = m_rels_pending_tracker.last_returned();
    if (id_tracker::is_valid(prev) && prev >= id) {
        if (prev > id) {
            job_queue.emplace(id, output_id);
        }
        // already done the job
        return;
    }

    //make sure we get the one passed in
    if (id_tracker::is_valid(id)) {
        job_queue.emplace(id, output_id);
        ++added;
    }

    //grab the first one or bail if its not valid
    osmid_t popped = m_rels_pending_tracker.pop_mark();
    if (!id_tracker::is_valid(popped)) {
        return;
    }

    //get all the ones up to the id that was passed in
    while (popped < id) {
        job_queue.emplace(popped, output_id);
        ++added;
        popped = m_rels_pending_tracker.pop_mark();
    }

    //make sure to get this one as well and move to the next
    if (popped > id) {
        if (id_tracker::is_valid(popped)) {
            job_queue.emplace(popped, output_id);
            ++added;
        }
    }
}

void output_flex_t::pending_relation(osmid_t id, int exists)
{
    if (!m_has_process_relation) {
        return;
    }

    // Try to fetch the relation from the DB
    // Note that we cannot use the global buffer here because
    // we cannot keep a reference to the relation and an autogrow buffer
    // might be relocated when more data is added.
    if (!m_mid->relations_get(id, m_rels_buffer)) {
        return;
    }

    // If the flag says this object may exist already, delete it first.
    if (exists) {
        delete_from_tables(osmium::item_type::relation, id);
    }

    auto const &relation = m_rels_buffer.get<osmium::Relation>(0);

    m_context_relation = &relation;
    call_process_function(3, relation);
    m_context_relation = nullptr;
    m_rels_buffer.clear();
}

void output_flex_t::commit()
{
    for (auto &copy_mgr : m_copy_mgrs) {
        copy_mgr.sync();
    }
}

void output_flex_t::stop(osmium::thread::Pool *pool)
{
    for (auto &copy_mgr : m_copy_mgrs) {
        copy_mgr.sync();
    }

    for (auto &table : m_tables) {
        pool->submit(
            [&]() { table.stop(m_options.slim & !m_options.droptemp); });
    }

    if (m_options.expire_tiles_zoom_min > 0) {
        m_expire.output_and_destroy(m_options.expire_tiles_filename.c_str(),
                                    m_options.expire_tiles_zoom_min);
    }
}

void output_flex_t::node_add(osmium::Node const &node)
{
    if (!m_has_process_node) {
        return;
    }

    m_context_node = &node;
    call_process_function(1, node);
    m_context_node = nullptr;
}

void output_flex_t::way_add(osmium::Way *way)
{
    assert(way);

    if (!m_has_process_way) {
        return;
    }

    m_context_way = way;
    call_process_function(2, *way);
    m_context_way = nullptr;
    m_num_way_nodes = std::numeric_limits<std::size_t>::max();
}

void output_flex_t::relation_add(osmium::Relation const &relation)
{
    if (!m_has_process_relation) {
        return;
    }

    m_context_relation = &relation;
    call_process_function(3, relation);
    m_context_relation = nullptr;
}

void output_flex_t::delete_from_table(flex_table_t *table,
                                      osmium::item_type type, osmid_t osm_id)
{
    assert(table);
    auto const id = table->map_id(type, osm_id);
    auto const result = table->get_geom_by_id(type, id);
    if (m_expire.from_result(result, id) != 0) {
        auto copy_mgr = get_copy_mgr(table);
        copy_mgr->new_line(table->target());

        // If the table id type is some specific type, we don't care about the
        // type of the individual object, because they all will be the same.
        if (table->id_type() != osmium::item_type::undefined) {
            type = osmium::item_type::undefined;
        }
        copy_mgr->delete_object(type_to_char(type)[0], id);
    }
}

void output_flex_t::delete_from_tables(osmium::item_type type, osmid_t osm_id)
{
    for (auto &table : m_tables) {
        if (table.matches_type(type)) {
            delete_from_table(&table, type, osm_id);
        }
    }
}

/* Delete is easy, just remove all traces of this object. We don't need to
 * worry about finding objects that depend on it, since the same diff must
 * contain the change for that also. */
void output_flex_t::node_delete(osmid_t osm_id)
{
    delete_from_tables(osmium::item_type::node, osm_id);
}

void output_flex_t::way_delete(osmid_t osm_id)
{
    delete_from_tables(osmium::item_type::way, osm_id);
}

void output_flex_t::relation_delete(osmid_t osm_id)
{
    delete_from_tables(osmium::item_type::relation, osm_id);
}

void output_flex_t::node_modify(osmium::Node const &node)
{
    node_delete(node.id());
    node_add(node);
}

void output_flex_t::way_modify(osmium::Way *way)
{
    way_delete(way->id());
    way_add(way);
}

void output_flex_t::relation_modify(osmium::Relation const &rel)
{
    relation_delete(rel.id());
    relation_add(rel);
}

void output_flex_t::init_clone()
{
    for (auto &table : m_tables) {
        table.connect(m_options.database_options.conninfo());
        table.prepare();
    }
}

void output_flex_t::start()
{
    for (auto &table : m_tables) {
        table.connect(m_options.database_options.conninfo());
        table.start();
    }
}

std::shared_ptr<output_t>
output_flex_t::clone(std::shared_ptr<middle_query_t> const &mid,
                     std::shared_ptr<db_copy_thread_t> const &copy_thread) const
{
    return std::shared_ptr<output_t>(new output_flex_t{
        mid, *get_options(), copy_thread, true, m_lua_state, m_has_process_node,
        m_has_process_way, m_has_process_relation, m_tables,
        m_stage2_ways_tracker, m_stage2_rels_tracker});
}

output_flex_t::output_flex_t(
    std::shared_ptr<middle_query_t> const &mid, options_t const &o,
    std::shared_ptr<db_copy_thread_t> const &copy_thread, bool is_clone,
    std::shared_ptr<lua_State> lua_state, bool has_process_node,
    bool has_process_way, bool has_process_relation,
    std::vector<flex_table_t> tables,
    std::shared_ptr<id_tracker> ways_tracker,
    std::shared_ptr<id_tracker> rels_tracker)
: output_t(mid, o), m_tables(std::move(tables)),
  m_ways_done_tracker(new id_tracker{}),
  m_stage2_ways_tracker(std::move(ways_tracker)),
  m_stage2_rels_tracker(std::move(rels_tracker)), m_copy_thread(copy_thread),
  m_lua_state(std::move(lua_state)), m_builder(o.projection),
  m_expire(o.expire_tiles_zoom, o.expire_tiles_max_bbox, o.projection),
  m_buffer(32768, osmium::memory::Buffer::auto_grow::yes),
  m_rels_buffer(1024, osmium::memory::Buffer::auto_grow::yes),
  m_has_process_node(has_process_node), m_has_process_way(has_process_way),
  m_has_process_relation(has_process_relation)
{
    assert(copy_thread);

    if (!is_clone) {
        init_lua(m_options.style);
    }

    for (auto &table : m_tables) {
        m_copy_mgrs.emplace_back(m_copy_thread);
        table.init();
    }

    if (is_clone) {
        init_clone();
    }
}

static bool prepare_process_function(lua_State *lua_state, char const *name)
{
    lua_getfield(lua_state, 1, name);

    if (lua_type(lua_state, -1) == LUA_TFUNCTION) {
        return true;
    }

    if (lua_type(lua_state, -1) == LUA_TNIL) {
        return false;
    }

    throw std::runtime_error{"osm2pgsql.{} must be a function"_format(name)};
}

void output_flex_t::init_lua(std::string const &filename)
{
    m_lua_state.reset(luaL_newstate(),
                      [](lua_State *state) { lua_close(state); });

    // Set up global lua libs
    luaL_openlibs(lua_state());

    // Set up global "osm2pgsql" object
    lua_newtable(lua_state());

    luaX_add_table_str(lua_state(), "version", get_osm2pgsql_short_version());
    luaX_add_table_int(lua_state(), "srid",
                       get_options()->projection->target_srs());
    luaX_add_table_str(lua_state(), "mode",
                       m_options.append ? "append" : "create");
    luaX_add_table_int(lua_state(), "stage", 1);

    luaX_add_table_func(lua_state(), "define_table",
                        lua_trampoline_app_define_table);
    luaX_add_table_func(lua_state(), "mark", lua_trampoline_app_mark);

    lua_setglobal(lua_state(), "osm2pgsql");

    // Define "osmpgsql.table" metatable
    if (luaL_newmetatable(lua_state(), osm2pgsql_table_name) != 1) {
        throw std::runtime_error{"Internal error: Lua newmetatable failed"};
    }
    lua_pushvalue(lua_state(), -1);
    lua_setfield(lua_state(), -2, "__index");
    luaX_add_table_func(lua_state(), "__tostring",
                        lua_trampoline_table_tostring);
    luaX_add_table_func(lua_state(), "add_row", lua_trampoline_table_add_row);
    luaX_add_table_func(lua_state(), "name", lua_trampoline_table_name);
    luaX_add_table_func(lua_state(), "schema", lua_trampoline_table_schema);
    luaX_add_table_func(lua_state(), "columns", lua_trampoline_table_columns);

    // Clean up stack
    lua_settop(lua_state(), 0);

    // Load compiled in init.lua
    if (luaL_dostring(lua_state(), lua_init())) {
        throw std::runtime_error{"Internal error in Lua setup: {}"_format(
            lua_tostring(lua_state(), -1))};
    }

    // Store the "get_bbox" in the "object_metatable".
    lua_getglobal(lua_state(), "object_metatable");
    lua_getfield(lua_state(), -1, "__index");
    luaX_add_table_func(lua_state(), "get_bbox", lua_trampoline_app_get_bbox);
    lua_settop(lua_state(), 0);

    // Store the global object "object_metatable" defined in the init.lua
    // script in the registry and then remove the global object. It will
    // later be used as metatable for OSM objects.
    lua_pushlightuserdata(lua_state(), (void *)osm2pgsql_object_metatable);
    lua_getglobal(lua_state(), "object_metatable");
    lua_settable(lua_state(), LUA_REGISTRYINDEX);
    lua_pushnil(lua_state());
    lua_setglobal(lua_state(), "object_metatable");

    // Load user config file
    luaX_set_context(lua_state(), this);
    if (luaL_dofile(lua_state(), filename.c_str())) {
        throw std::runtime_error{"Error loading lua config: {}"_format(
            lua_tostring(lua_state(), -1))};
    }

    // Check whether the process_* functions are available and store them on
    // the Lua stack for fast access later
    lua_getglobal(lua_state(), "osm2pgsql");
    m_has_process_node = prepare_process_function(lua_state(), "process_node");
    m_has_process_way = prepare_process_function(lua_state(), "process_way");
    m_has_process_relation =
        prepare_process_function(lua_state(), "process_relation");

    lua_remove(lua_state(), 1); // global "osm2pgsql"
}

std::size_t output_flex_t::pending_count() const
{
    return m_ways_pending_tracker.size() + m_rels_pending_tracker.size();
}

void output_flex_t::stage2_proc()
{
    bool const has_marked_ways = m_stage2_ways_tracker->size() > 0;
    bool const has_marked_rels = m_stage2_rels_tracker->size() > 0;

    if (!has_marked_ways && !has_marked_rels) {
        fmt::print(stderr, "Skipping stage 2 (no marked objects).\n");
        return;
    }

    fmt::print(stderr, "Entering stage 2...\n");
    m_in_stage2 = true;

    if (!m_options.append) {
        fmt::print(stderr, "Creating id indexes...\n");
        const std::time_t start_time = std::time(nullptr);

        for (auto &table : m_tables) {
            if ((has_marked_ways &&
                 table.matches_type(osmium::item_type::way)) ||
                (has_marked_rels &&
                 table.matches_type(osmium::item_type::relation))) {
                fmt::print(stderr, "  Creating id index on table '{}'...\n",
                           table.name());
                table.create_id_index();
            }
        }

        fmt::print(stderr, "  Creating id indexes took {} seconds\n"_format(
                               std::time(nullptr) - start_time));
    }

    lua_gc(lua_state(), LUA_GCCOLLECT, 0);
    fmt::print(stderr, "Lua program uses {} MBytes\n",
               lua_gc(lua_state(), LUA_GCCOUNT, 0) / 1024);

    lua_getglobal(lua_state(), "osm2pgsql");
    lua_pushinteger(lua_state(), 2);
    lua_setfield(lua_state(), -2, "stage");
    lua_pop(lua_state(), 1); // osm2pgsql

    osmid_t id;

    fmt::print(stderr, "Entering stage 2 processing of {} ways...\n"_format(
                           m_stage2_ways_tracker->size()));

    while (id_tracker::is_valid((id = m_stage2_ways_tracker->pop_mark()))) {
        m_buffer.clear();
        if (!m_mid->ways_get(id, m_buffer)) {
            continue;
        }
        auto &way = m_buffer.get<osmium::Way>(0);
        way_add(&way);
    }

    fmt::print(stderr,
               "Entering stage 2 processing of {} relations...\n"_format(
                   m_stage2_rels_tracker->size()));

    while (id_tracker::is_valid((id = m_stage2_rels_tracker->pop_mark()))) {
        m_rels_buffer.clear();
        if (!m_mid->relations_get(id, m_rels_buffer)) {
            continue;
        }
        auto const &relation = m_rels_buffer.get<osmium::Relation>(0);
        relation_add(relation);
    }
}

void output_flex_t::merge_pending_relations(output_t *other)
{
    auto *opgsql = dynamic_cast<output_flex_t *>(other);
    if (opgsql) {
        osmid_t id;
        while (id_tracker::is_valid(
            (id = opgsql->m_rels_pending_tracker.pop_mark()))) {
            m_rels_pending_tracker.mark(id);
        }
    }
}

void output_flex_t::merge_expire_trees(output_t *other)
{
    auto *opgsql = dynamic_cast<output_flex_t *>(other);
    if (opgsql) {
        m_expire.merge_and_destroy(opgsql->m_expire);
    }
}
