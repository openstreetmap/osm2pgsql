#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cassert>
#include <list>

#include "osmtypes.hpp"
#include "keyvals.hpp"
#include "tests/middle-tests.hpp"


int test_node_set(middle_t *mid)
{
  osmid_t id = 1234;
  double lat = 12.3456789;
  double lon = 98.7654321;
  struct keyval tags;
  struct osmNode node;
  int status = 0;


  // set the node
  status = mid->nodes_set(id, lat, lon, &tags);
  if (status != 0) { std::cerr << "ERROR: Unable to set node.\n"; return 1; }

  // get it back
  int count = mid->nodes_get_list(&node, &id, 1);
  if (count != 1) { std::cerr << "ERROR: Unable to get node list.\n"; return 1; }

  // check that it's the same
  if (node.lon != lon) {
    std::cerr << "ERROR: Node should have lon=" << lon << ", but got back "
              << node.lon << " from middle.\n";
    return 1;
  }
  if (node.lat != lat) {
    std::cerr << "ERROR: Node should have lat=" << lat << ", but got back "
              << node.lat << " from middle.\n";
    return 1;
  }

  // clean up for next test
  if (dynamic_cast<slim_middle_t *>(mid)) {
    dynamic_cast<slim_middle_t *>(mid)->nodes_delete(id);
  }

  tags.resetList();

  return 0;
}

struct test_pending_processor : public middle_t::pending_processor {
    test_pending_processor(): pending_ways(), pending_rels() {}
    virtual ~test_pending_processor() {}
    virtual void enqueue_ways(osmid_t id) {
        pending_ways.push_back(id);
    }
    virtual void process_ways() {
        pending_ways.clear();
    }
    virtual void enqueue_relations(osmid_t id) {
        pending_rels.push_back(id);
    }
    virtual void process_relations() {
        pending_rels.clear();
    }
    virtual int thread_count() {
        return 0;
    }
    virtual int size() {
        return pending_ways.size() + pending_rels.size();
    }
    std::list<osmid_t> pending_ways;
    std::list<osmid_t> pending_rels;
};

int test_way_set(middle_t *mid)
{
  osmid_t way_id = 1;
  double lat = 12.3456789;
  double lon = 98.7654321;
  struct keyval tags;
  struct osmNode *node_ptr = NULL;
  osmid_t way_ids_ptr;
  int node_count = 0;
  int status = 0;
  osmid_t nds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  const int nd_count = ((sizeof nds) / (sizeof nds[0]));
  const char *test_tags[] = { "highway", "secondary",
                              "name", "Гмыри",
                              "ref", "",
                              "xxx\\", "yy\"yy",
                              "test", "\n\t\r" };
  // XXX test for \n\t\r currently disabled because it fails with middle-pgsql
  int num_test_tags = 4;

  // set the nodes
  for (int i = 0; i < nd_count; ++i) {
    status = mid->nodes_set(nds[i], lat, lon, &tags);
    if (status != 0) { std::cerr << "ERROR: Unable to set node " << nds[i] << ".\n"; return 1; }
  }

  // set some tags
  for (unsigned i = 0; i < num_test_tags; ++i) {
    tags.addItem(test_tags[i*2], test_tags[i*2+1], false);
  }

  // set the way
  status = mid->ways_set(way_id, nds, nd_count, &tags);
  if (status != 0) { std::cerr << "ERROR: Unable to set way.\n"; return 1; }

  // commit the setup data
  mid->commit();

  tags.resetList();

  // get it back
  int way_count = mid->ways_get_list(&way_id, 1, &way_ids_ptr, &tags, &node_ptr, &node_count);
  if (way_count != 1) { std::cerr << "ERROR: Unable to get way list.\n"; return 1; }

  // check that it's the same
  if (node_count != nd_count) {
    std::cerr << "ERROR: Way should have " << nd_count << " nodes, but got back "
              << node_count << " from middle.\n";
    return 1;
  }
  if (way_ids_ptr != way_id) {
    std::cerr << "ERROR: Way should have id=" << way_id << ", but got back "
              << way_ids_ptr << " from middle.\n";
    return 1;
  }
  for (int i = 0; i < nd_count; ++i) {
    if (node_ptr[i].lon != lon) {
      std::cerr << "ERROR: Way node should have lon=" << lon << ", but got back "
                << node_ptr[i].lon << " from middle.\n";
      return 1;
    }
    if (node_ptr[i].lat != lat) {
      std::cerr << "ERROR: Way node should have lat=" << lat << ", but got back "
                << node_ptr[i].lat << " from middle.\n";
      return 1;
    }
  }

  if (tags.countList() != num_test_tags) {
    std::cerr << "ERROR: Tag count should be 3 but was " << tags.countList() << ".\n";
    return 1;
  }

  for (unsigned i = 0; i < num_test_tags; ++i) {
    keyval * ret = tags.getTag(test_tags[i*2]);
    if (ret == NULL || ret->value != test_tags[i*2+1]) {
        std::cerr << "ERROR: Wrong tag returned, expected " << test_tags[i*2]
                  << "/" << test_tags[i*2+1] << ", got " << ret->key << "/"
                  << ret->value << ".\n";
        return 1;
    }
  }

  // the way we just inserted should not be pending
  test_pending_processor tpp;
  mid->iterate_ways(tpp);
  if (mid->pending_count() != 0) {
    std::cerr << "ERROR: Was expecting no pending ways, but got "
              << mid->pending_count() << " from middle.\n";
    return 1;
  }

  // some middles don't support changing the nodes - they
  // don't have diff update ability. here, we will just
  // skip the test for that.
  if (dynamic_cast<slim_middle_t *>(mid)) {
      slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid);

      // finally, try touching a node on a non-pending way. that should
      // make it become pending. we just checked that the way is not
      // pending, so any change must be due to the node changing.
      status = slim->node_changed(nds[0]);
      if (status != 0) { std::cerr << "ERROR: Unable to reset node.\n"; return 1; }
      slim->iterate_ways(tpp);
      if (slim->pending_count() != 1) {
          std::cerr << "ERROR: Was expecting a single pending way from node update, but got "
                    << slim->pending_count() << " from middle.\n";
          return 1;
      }
  }

  tags.resetList();
  free(node_ptr);

  // clean up for next test
  if (dynamic_cast<slim_middle_t *>(mid)) {
      slim_middle_t *slim = dynamic_cast<slim_middle_t *>(mid);

      for (int i = 0; i < nd_count; ++i) {
          slim->nodes_delete(nds[i]);
      }
      slim->ways_delete(way_id);
  }

  // commit the torn-down data
  mid->commit();

  return 0;
}
