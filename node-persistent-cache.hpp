#ifndef NODE_PERSISTENT_CACHE_H
#define NODE_PERSISTENT_CACHE_H

#include "node-ram-cache.hpp"
#include <boost/shared_ptr.hpp>

#include <vector>

#define MAXIMUM_INITIAL_ID 2600000000

#define READ_NODE_CACHE_SIZE 10000
#define READ_NODE_BLOCK_SHIFT 10l
#define READ_NODE_BLOCK_SIZE (1l << READ_NODE_BLOCK_SHIFT)
#define READ_NODE_BLOCK_MASK 0x03FFl

#define WRITE_NODE_BLOCK_SHIFT 20l
#define WRITE_NODE_BLOCK_SIZE (1l << WRITE_NODE_BLOCK_SHIFT)
#define WRITE_NODE_BLOCK_MASK 0x0FFFFFl

#define PERSISTENT_CACHE_FORMAT_VERSION 1

struct persistentCacheHeader {
	int format_version;
	int id_size;
    osmid_t max_initialised_id;
};

struct cache_index_entry {
    osmid_t key;
    int value;

    cache_index_entry(osmid_t k, int v) : key(k), value(v) {}
    cache_index_entry() {}
};

inline bool operator<(cache_index_entry const &a, cache_index_entry const &b)
{
    return a.key < b.key;
}

inline bool operator<(cache_index_entry const &a, osmid_t b)
{
    return a.key < b;
}

inline bool operator<(osmid_t a, cache_index_entry const &b)
{
    return a < b.key;
}

struct node_persistent_cache : public boost::noncopyable
{
    node_persistent_cache(const struct options_t *options, const int append,
                          boost::shared_ptr<node_ram_cache> ptr);
    ~node_persistent_cache();

    int set(osmid_t id, double lat, double lon);
    int get(struct osmNode *out, osmid_t id);
    int get_list(struct osmNode *nodes, const osmid_t *ndids, int nd_count);

private:

    int set_append(osmid_t id, double lat, double lon);
    int set_create(osmid_t id, double lat, double lon);

    void writeout_dirty_nodes(osmid_t id);
    int replace_block();
    int find_block(osmid_t block_offset);
    void expand_cache(osmid_t block_offset);
    void nodes_prefetch_async(osmid_t id);
    int load_block(osmid_t block_offset);
    void nodes_set_create_writeout_block();

    void remove_from_cache_idx(osmid_t block_offset);
    void add_to_cache_idx(cache_index_entry const &entry);

    int node_cache_fd;
    const char * node_cache_fname;
    int append_mode;

    struct persistentCacheHeader cacheHeader;
    struct ramNodeBlock writeNodeBlock; /* larger node block for more efficient initial sequential writing of node cache */
    struct ramNodeBlock * readNodeBlockCache;

    typedef std::vector<cache_index_entry> cache_index;
    cache_index readNodeBlockCacheIdx;

    int scale_;
    int cache_already_written;

    boost::shared_ptr<node_ram_cache> ram_cache;
};

#endif
