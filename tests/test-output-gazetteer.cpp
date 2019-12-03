#include <catch.hpp>

#include "common-import.hpp"
#include "common-options.hpp"

static options_t options() { return testing::opt_t().gazetteer(); }

static pg::result_t require_place(pg::conn_t const &conn, char type, osmid_t id,
                                  char const *cls, char const *typ)
{
    return conn.require_row(
        "SELECT * FROM place"
        " WHERE osm_type = '{}' AND osm_id = {}"
        " AND class = '{}' AND type = '{}'"_format(type, id, cls, typ));
}

static void require_place_not(pg::conn_t const &conn, char type, osmid_t id,
                              char const *cls)
{
    REQUIRE(
        conn.get_count(
            "place", "osm_type = '{}' AND osm_id = {} AND class = '{}'"_format(
                         type, id, cls)) == 0);
}

TEST_CASE("output_gazetteer_t import")
{
    testing::db::import_t db;

    SECTION("Main tags")
    {
        REQUIRE_NOTHROW(db.run_import(
            options(), "n1 Tamenity=restaurant,name=Foobar x12.3 y3\n"
                       "n2 Thighway=bus_stop,railway=stop,name=X x56.4 y-4\n"
                       "n3 Tnatural=no x2 y5\n"));

        auto conn = db.connect();

        require_place(conn, 'N', 1, "amenity", "restaurant");
        require_place(conn, 'N', 2, "highway", "bus_stop");
        require_place(conn, 'N', 2, "railway", "stop");
        require_place_not(conn, 'N', 3, "natural");
    }

    SECTION("Main tags with name")
    {
        REQUIRE_NOTHROW(db.run_import(
            options(), "n45 Tlanduse=cemetry x0 y0\n"
                       "n54 Tlanduse=cemetry,name=There x3 y5\n"
                       "n55 Tname:de=Da,landuse=cemetry x0.0 y6.5\n"));

        auto conn = db.connect();

        require_place_not(conn, 'N', 45, "landuse");
        require_place(conn, 'N', 54, "landuse", "cemetry");
        require_place(conn, 'N', 55, "landuse", "cemetry");
    }

    SECTION("Main tags as fallback")
    {
        REQUIRE_NOTHROW(db.run_import(
            options(), "n100 Tjunction=yes,highway=bus_stop x0 y0\n"
                       "n101 Tjunction=yes,name=Bar x4 y6\n"
                       "n200 Tbuilding=yes,amenity=cafe x3 y7\n"
                       "n201 Tbuilding=yes,name=Intersting x4 y5\n"
                       "n202 Tbuilding=yes x6 y9\n"));

        auto conn = db.connect();

        require_place_not(conn, 'N', 100, "junction");
        require_place(conn, 'N', 101, "junction", "yes");
        require_place_not(conn, 'N', 200, "building");
        require_place(conn, 'N', 201, "building", "yes");
        require_place_not(conn, 'N', 202, "building");
    }
}
