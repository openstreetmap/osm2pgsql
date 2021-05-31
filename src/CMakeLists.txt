
set(osm2pgsql_lib_SOURCES
  db-check.cpp
  db-copy.cpp
  dependency-manager.cpp
  expire-tiles.cpp
  gazetteer-style.cpp
  geom.cpp
  input.cpp
  logging.cpp
  middle.cpp
  middle-db.cpp
  middle-pgsql.cpp
  middle-ram.cpp
  node-locations.cpp
  node-persistent-cache.cpp
  options.cpp
  ordered-index.cpp
  osmdata.cpp
  osmium-builder.cpp
  output-gazetteer.cpp
  output-null.cpp
  output-pgsql.cpp
  output.cpp
  pgsql.cpp
  pgsql-helper.cpp
  progress-display.cpp
  reprojection.cpp
  table.cpp
  taginfo.cpp
  tagtransform-c.cpp
  tagtransform.cpp
  thread-pool.cpp
  template-repository.cpp
  util.cpp
  wildcmp.cpp
)

if (LUA_FOUND OR LUAJIT_FOUND)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/init.lua")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/init.lua" LUA_INIT_CODE)
    configure_file(lua-init.cpp.in lua-init.cpp @ONLY)
    list(APPEND osm2pgsql_lib_SOURCES
        flex-table.cpp
        flex-table-column.cpp
        geom-transform.cpp
        lua-utils.cpp
        output-flex.cpp
        tagtransform-lua.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/lua-init.cpp)
endif()

list(APPEND osm2pgsql_lib_SOURCES ${PROJECT_BINARY_DIR}/src/version.cpp)

if (HAVE_PROJ4)
    list(APPEND osm2pgsql_lib_SOURCES reprojection-generic-proj4.cpp)
    if (NOT MSVC)
        set_source_files_properties(reprojection-generic-proj4.cpp
            PROPERTIES COMPILE_FLAGS -Wno-deprecated-declarations)
    endif()
elseif(HAVE_PROJ6)
    list(APPEND osm2pgsql_lib_SOURCES reprojection-generic-proj6.cpp)
else()
    list(APPEND osm2pgsql_lib_SOURCES reprojection-generic-none.cpp)
endif()

add_library(osm2pgsql_lib STATIC ${osm2pgsql_lib_SOURCES})
set_target_properties(osm2pgsql_lib PROPERTIES OUTPUT_NAME osm2pgsql)
target_link_libraries(osm2pgsql_lib ${LIBS})

