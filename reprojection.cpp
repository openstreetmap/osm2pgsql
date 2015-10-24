/* reprojection.c
 *
 * Convert OSM coordinates to another coordinate system for
 * the database (usually convert lat/lon to Spherical Mercator
 * so Mapnik doesn't have to).
 */

#include "config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <proj_api.h>

#include "reprojection.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** must match expire.tiles.c */
#define EARTH_CIRCUMFERENCE              40075016.68

Projection_Info::Projection_Info(const char *descr_, const char *proj4text_, int srs_, const char *option_)
    : descr(descr_), proj4text(proj4text_), srs(srs_), option(option_) {
}

namespace {

const struct Projection_Info Projection_Infos[] = {
    /*PROJ_LATLONG*/ Projection_Info(
        /*descr    */ "Latlong",
        /*proj4text*/ "+init=epsg:4326",
        /*srs      */ 4326,
        /*option   */ "-l" ),
    /*PROJ_SPHERE_MERC*/ Projection_Info(
        /*descr    */ "Spherical Mercator",
        /*proj4text*/ "+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 +x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext  +no_defs",
        /*srs      */ 900913,
        /*option   */ "-m" )
};

} // anonymous namespace

/* Positive numbers refer the to the table above, negative numbers are
   assumed to refer to EPSG codes and it uses the proj4 to find those. */
reprojection::reprojection(int proj)
    : Proj(proj), pj_source(nullptr), pj_target(nullptr), pj_tile(nullptr),
      custom_projection(nullptr)
{
    char buffer[32];

    /* hard-code the source projection to be lat/lon, since OSM XML always
     * has coordinates in degrees. */
    pj_source = pj_init_plus("+proj=longlat +ellps=WGS84 +datum=WGS84 +no_defs");

    /* hard-code the tile projection to be spherical mercator always.
     * theoretically this could be made selectable but not all projections
     * lend themselves well to making tiles; non-spherical mercator tiles
     * are uncharted waters in OSM. */
    pj_tile = pj_init_plus(Projection_Infos[PROJ_SPHERE_MERC].proj4text);

    /* now set the target projection - the only one which is really variable */
    if (proj >= 0 && proj < PROJ_COUNT)
    {
        pj_target = pj_init_plus(Projection_Infos[proj].proj4text);
    }
    else if (proj < 0)
    {
        if (snprintf(buffer, sizeof(buffer), "+init=epsg:%d", -proj ) >= (int)sizeof(buffer))
        {
            fprintf(stderr, "Buffer overflow computing proj4 initialisation string\n");
            exit(1);
        }
        pj_target = pj_init_plus(buffer);
        if (!pj_target)
        {
            fprintf (stderr, "Couldn't read EPSG definition (do you have /usr/share/proj/epsg?)\n");
            exit(1);
        }
    }

    if (!pj_source || !pj_target || !pj_tile)
    {
        fprintf(stderr, "Projection code failed to initialise\n");
        exit(1);
    }

    if (proj >= 0)
        return;

    if (snprintf(buffer, sizeof(buffer), "EPSG:%d", -proj) >= (int)sizeof(buffer))
    {
        fprintf(stderr, "Buffer overflow computing projection description\n");
        exit(1);
    }
    custom_projection = new Projection_Info(
        strdup(buffer),
        pj_get_def(pj_target, 0),
        -proj, "-E");
}

reprojection::~reprojection()
{
    pj_free(pj_source);
    pj_source = nullptr;
    pj_free(pj_target);
    pj_target = nullptr;
    pj_free(pj_tile);
    pj_tile = nullptr;

    if (custom_projection != nullptr) {
        delete custom_projection;
    }
}

struct Projection_Info const *reprojection::project_getprojinfo(void)
{
  if( Proj >= 0 )
    return &Projection_Infos[Proj];
  else
    return custom_projection;
}

void reprojection::reproject(double *lat, double *lon) const
{
    double x[1], y[1], z[1];

    /** Caution: This section is only correct if the source projection is lat/lon;
     *  so even if it looks like pj_source was just a variable, things break if
     *  pj_source is something else than lat/lon. */

    if (Proj == PROJ_LATLONG)
        return;

    if (Proj == PROJ_SPHERE_MERC)
    {
      /* The latitude co-ordinate is clipped at slightly larger than the 900913 'world'
       * extent of +-85.0511 degrees to ensure that the points appear just outside the
       * edge of the map. */

        if (*lat > 85.07)
            *lat = 85.07;
        if (*lat < -85.07)
            *lat = -85.07;

        *lat = log(tan(M_PI/4.0 + (*lat) * DEG_TO_RAD / 2.0)) * EARTH_CIRCUMFERENCE/(M_PI*2);
        *lon = (*lon) * EARTH_CIRCUMFERENCE / 360.0;
        return;
    }

    x[0] = *lon * DEG_TO_RAD;
    y[0] = *lat * DEG_TO_RAD;
    z[0] = 0;

    /** end of "caution" section. */

    pj_transform(pj_source, pj_target, 1, 1, x, y, z);

    *lat = y[0];
    *lon = x[0];
}

/**
 * Converts from (target) coordinates to coordinates in the tile projection (EPSG:3857)
 *
 * Do not confuse with coords_to_tile which does *not* calculate coordinates in the
 * tile projection, but tile coordinates.
 */
void reprojection::target_to_tile(double *lat, double *lon) const
{
    double x[1], y[1], z[1];

    if (Proj == PROJ_SPHERE_MERC)
        return;

    if (Proj == PROJ_LATLONG)
    {
        if (*lat > 85.07)
            *lat = 85.07;
        if (*lat < -85.07)
            *lat = -85.07;

        *lon = (*lon) * EARTH_CIRCUMFERENCE / 360.0;
        *lat = log(tan(M_PI/4.0 + (*lat) * DEG_TO_RAD / 2.0)) * EARTH_CIRCUMFERENCE/(M_PI*2);
        return;
    }

    x[0] = *lon;
    y[0] = *lat;
    z[0] = 0;

    /** end of "caution" section. */

    pj_transform(pj_target, pj_tile, 1, 1, x, y, z);

    *lat = y[0];
    *lon = x[0];
}

/**
 * Converts from (target) coordinates to tile coordinates.
 *
 * The zoom level for the coordinates is explicitly given in the
 * variable map_width.
 */
void reprojection::coords_to_tile(double *tilex, double *tiley, double lon, double lat,
                                  int map_width)
{
    double x[1], y[1], z[1];

    x[0] = lon;
    y[0] = lat;
    z[0] = 0;

    if (Proj == PROJ_LATLONG)
    {
        x[0] *= DEG_TO_RAD;
        y[0] *= DEG_TO_RAD;
    }

    /* since pj_tile is always spherical merc, don't bother doing anything if
     *  destination proj is the same. */

    if (Proj != PROJ_SPHERE_MERC)
    {
        pj_transform(pj_target, pj_tile, 1, 1, x, y, z);
        /** FIXME: pj_transform could fail if coordinates are outside +/- 85 degrees latitude */
    }

    /* if ever pj_tile were allowed to be PROJ_LATLONG then results would have to
     *  be divided by DEG_TO_RAD here. */

    *tilex = map_width * (0.5 + x[0] / EARTH_CIRCUMFERENCE);
    *tiley = map_width * (0.5 - y[0] / EARTH_CIRCUMFERENCE);
}

int reprojection::get_proj_id() const
{
	return Proj;
}
