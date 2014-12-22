#define _LARGEFILE64_SOURCE     /* See feature_test_macrors(7) */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>

#include "osmtypes.hpp"
#include "output.hpp"
#include "options.hpp"
#include "node-persistent-cache.hpp"
#include "util.hpp"

#include <stdexcept>
#include <algorithm>

#ifdef _WIN32
 #include "win_fsync.h"
 #define lseek64 _lseeki64
 #ifndef S_IRUSR
  #define S_IRUSR S_IREAD
 #endif
 #ifndef S_IWUSR
  #define S_IWUSR S_IWRITE
 #endif
#else
 #ifdef __APPLE__
 #define lseek64 lseek
 #else
  #ifndef HAVE_LSEEK64
   #if SIZEOF_OFF_T == 8
    #define lseek64 lseek
   #else
    #error Flat nodes cache requires a 64 bit capable seek
   #endif
  #endif
 #endif
#endif

void node_persistent_cache::writeout_dirty_nodes(osmid_t id)
{
    int i;

    if (writeNodeBlock.dirty > 0)
    {
        if (lseek64(node_cache_fd,
                (writeNodeBlock.block_offset << WRITE_NODE_BLOCK_SHIFT)
                        * sizeof(struct ramNode)
                    + sizeof(struct persistentCacheHeader), SEEK_SET) < 0) {
            fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();

        };
        if (write(node_cache_fd, writeNodeBlock.nodes,
                WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode))
                < WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode))
        {
            fprintf(stderr, "Failed to write out node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
        cacheHeader.max_initialised_id = ((writeNodeBlock.block_offset + 1)
                << WRITE_NODE_BLOCK_SHIFT) - 1;
        writeNodeBlock.used = 0;
        writeNodeBlock.dirty = 0;
        if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        };
        if (write(node_cache_fd, &cacheHeader,
                sizeof(struct persistentCacheHeader))
                != sizeof(struct persistentCacheHeader))
        {
            fprintf(stderr, "Failed to update persistent cache header: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
        if (fsync(node_cache_fd) < 0) {
            fprintf(stderr, "Info: Node cache could not be guaranteeded to be made durable. fsync failed: %s\n",
                    strerror(errno));
        };
    }
    if (id < 0)
    {
        for (i = 0; i < READ_NODE_CACHE_SIZE; i++)
        {
            if (readNodeBlockCache[i].dirty)
            {
                if (lseek64(node_cache_fd,
                        (readNodeBlockCache[i].block_offset
                                << READ_NODE_BLOCK_SHIFT)
                                * sizeof(struct ramNode)
                                + sizeof(struct persistentCacheHeader),
                            SEEK_SET) < 0) {
                    fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                            strerror(errno));
                    util::exit_nicely();
                };
                if (write(node_cache_fd, readNodeBlockCache[i].nodes,
                        READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
                        < READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
                {
                    fprintf(stderr, "Failed to write out node cache: %s\n",
                            strerror(errno));
                    util::exit_nicely();
                }
            }
            readNodeBlockCache[i].dirty = 0;
        }
    }

}

static void ramNodes_clear(struct ramNode * nodes, int size)
{
    int i;
    for (i = 0; i < size; i++)
    {
#ifdef FIXED_POINT
        nodes[i].lon = INT_MIN;
        nodes[i].lat = INT_MIN;
#else
        nodes[i].lon = NAN;
        nodes[i].lat = NAN;
#endif
    }
}

/**
 * Find the cache block with the lowest usage count for replacement
 */
int node_persistent_cache::replace_block()
{
    int min_used = INT_MAX;
    int block_id = -1;
    int i;

    for (i = 0; i < READ_NODE_CACHE_SIZE; i++)
    {
        if (readNodeBlockCache[i].used < min_used)
        {
            min_used = readNodeBlockCache[i].used;
            block_id = i;
        }
    }
    if (min_used > 0)
    {
        for (i = 0; i < READ_NODE_CACHE_SIZE; i++)
        {
            if (readNodeBlockCache[i].used > 1)
            {
                readNodeBlockCache[i].used--;
            }
        }
    }
    return block_id;
}

/**
 * Find cache block number by block_offset
 */
int node_persistent_cache::find_block(osmid_t block_offset)
{
    cache_index::iterator it = std::lower_bound(readNodeBlockCacheIdx.begin(),
                                                readNodeBlockCacheIdx.end(),
                                                block_offset);
    if (it != readNodeBlockCacheIdx.end() && it->key == block_offset)
        return it->value;

    return -1;
}

void node_persistent_cache::remove_from_cache_idx(osmid_t block_offset)
{
    cache_index::iterator it = std::lower_bound(readNodeBlockCacheIdx.begin(),
                                                readNodeBlockCacheIdx.end(),
                                                block_offset);

    if (it == readNodeBlockCacheIdx.end() || it->key != block_offset)
        return;

    readNodeBlockCacheIdx.erase(it);
}

void node_persistent_cache::add_to_cache_idx(cache_index_entry const &entry)
{
    cache_index::iterator it = std::lower_bound(readNodeBlockCacheIdx.begin(),
                                                readNodeBlockCacheIdx.end(),
                                                entry);
    readNodeBlockCacheIdx.insert(it, entry);
}

/**
 * Initialise the persistent cache with NaN values to identify which IDs are valid or not
 */
void node_persistent_cache::expand_cache(osmid_t block_offset)
{
    osmid_t i;
    struct ramNode * dummyNodes = (struct ramNode *)malloc(
            READ_NODE_BLOCK_SIZE * sizeof(struct ramNode));
    if (!dummyNodes) {
        fprintf(stderr, "Out of memory: Could not allocate node structure during cache expansion\n");
        util::exit_nicely();
    }
    ramNodes_clear(dummyNodes, READ_NODE_BLOCK_SIZE);
    /* Need to expand the persistent node cache */
    if (lseek64(node_cache_fd,
            cacheHeader.max_initialised_id * sizeof(struct ramNode)
                + sizeof(struct persistentCacheHeader), SEEK_SET) < 0) {
        fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    };
    for (i = cacheHeader.max_initialised_id >> READ_NODE_BLOCK_SHIFT;
            i <= block_offset; i++)
    {
        if (write(node_cache_fd, dummyNodes,
                READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
                < READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
        {
            fprintf(stderr, "Failed to expand persistent node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
    }
    cacheHeader.max_initialised_id = ((block_offset + 1)
            << READ_NODE_BLOCK_SHIFT) - 1;
    if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    };
    if (write(node_cache_fd, &cacheHeader, sizeof(struct persistentCacheHeader))
            != sizeof(struct persistentCacheHeader))
    {
        fprintf(stderr, "Failed to update persistent cache header: %s\n",
                strerror(errno));
        util::exit_nicely();
    }
    free(dummyNodes);
    fsync(node_cache_fd);
}


void node_persistent_cache::nodes_prefetch_async(osmid_t id)
{
#ifdef HAVE_POSIX_FADVISE
    osmid_t block_offset = id >> READ_NODE_BLOCK_SHIFT;

    int block_id = find_block(block_offset);

    if (block_id < 0)
        {   /* The needed block isn't in cache already, so initiate loading */
        writeout_dirty_nodes(id);

        /* Make sure the node cache is correctly initialised for the block that will be read */
        if (cacheHeader.max_initialised_id
                < ((block_offset + 1) << READ_NODE_BLOCK_SHIFT))
            expand_cache(block_offset);

        if (posix_fadvise(node_cache_fd, (block_offset << READ_NODE_BLOCK_SHIFT) * sizeof(struct ramNode)
                      + sizeof(struct persistentCacheHeader), READ_NODE_BLOCK_SIZE * sizeof(struct ramNode),
                          POSIX_FADV_WILLNEED | POSIX_FADV_RANDOM) != 0) {
            fprintf(stderr, "Info: async prefetch of node cache failed. This might reduce performance\n");
        };
    }
#endif
}


/**
 * Load block offset in a synchronous way.
 */
int node_persistent_cache::load_block(osmid_t block_offset)
{

    int block_id = replace_block();

    if (readNodeBlockCache[block_id].dirty)
    {
        if (lseek64(node_cache_fd,
                (readNodeBlockCache[block_id].block_offset
                        << READ_NODE_BLOCK_SHIFT) * sizeof(struct ramNode)
                    + sizeof(struct persistentCacheHeader), SEEK_SET) < 0) {
            fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        };
        if (write(node_cache_fd, readNodeBlockCache[block_id].nodes,
                READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
                < READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
        {
            fprintf(stderr, "Failed to write out node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
        readNodeBlockCache[block_id].dirty = 0;
    }

    remove_from_cache_idx(readNodeBlockCache[block_id].block_offset);
    ramNodes_clear(readNodeBlockCache[block_id].nodes, READ_NODE_BLOCK_SIZE);
    readNodeBlockCache[block_id].block_offset = block_offset;
    readNodeBlockCache[block_id].used = READ_NODE_CACHE_SIZE;

    /* Make sure the node cache is correctly initialised for the block that will be read */
    if (cacheHeader.max_initialised_id
            < ((block_offset + 1) << READ_NODE_BLOCK_SHIFT))
    {
        expand_cache(block_offset);
    }

    /* Read the block into cache */
    if (lseek64(node_cache_fd,
            (block_offset << READ_NODE_BLOCK_SHIFT) * sizeof(struct ramNode)
                + sizeof(struct persistentCacheHeader), SEEK_SET) < 0) {
        fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    };
    if (read(node_cache_fd, readNodeBlockCache[block_id].nodes,
            READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
            != READ_NODE_BLOCK_SIZE * sizeof(struct ramNode))
    {
        fprintf(stderr, "Failed to read from node cache: %s\n",
                strerror(errno));
        exit(1);
    }
    add_to_cache_idx(cache_index_entry(readNodeBlockCache[block_id].block_offset,
                                   block_id));

    return block_id;
}

void node_persistent_cache::nodes_set_create_writeout_block()
{
    if (write(node_cache_fd, writeNodeBlock.nodes,
              WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode))
        < WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode))
    {
        fprintf(stderr, "Failed to write out node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    }
#ifdef HAVE_SYNC_FILE_RANGE
    /* writing out large files can cause trouble on some operating systems.
     * For one, if to much dirty data is in RAM, the whole OS can stall until
     * enough dirty data is written out which can take a while. It can also interfere
     * with outher disk caching operations and might push things out to swap. By forcing the OS to
     * immediately write out the data and blocking after a while, we ensure that no more
     * than a couple of 10s of MB are dirty in RAM at a time.
     * Secondly, the nodes are stored in an additional ram cache during import. Keeping the
     * node cache file in buffer cache therefore duplicates the data wasting 16GB of ram.
     * Therefore tell the OS not to cache the node-persistent-cache during initial import.
     * */
    if (sync_file_range(node_cache_fd, writeNodeBlock.block_offset*WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode) +
                        sizeof(struct persistentCacheHeader), WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode),
                        SYNC_FILE_RANGE_WRITE) < 0) {
        fprintf(stderr, "Info: Sync_file_range writeout has an issue. This shouldn't be anything to worry about.: %s\n",
                strerror(errno));
    };

    if (writeNodeBlock.block_offset > 16) {
        if(sync_file_range(node_cache_fd, (writeNodeBlock.block_offset - 16)*WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode) +
                           sizeof(struct persistentCacheHeader), WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode),
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) < 0) {
            fprintf(stderr, "Info: Sync_file_range block has an issue. This shouldn't be anything to worry about.: %s\n",
                strerror(errno));

        }
#ifdef HAVE_POSIX_FADVISE
        if (posix_fadvise(node_cache_fd, (writeNodeBlock.block_offset - 16)*WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode) +
                          sizeof(struct persistentCacheHeader), WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode), POSIX_FADV_DONTNEED) !=0 ) {
            fprintf(stderr, "Info: Posix_fadvise failed. This shouldn't be anything to worry about.: %s\n",
                strerror(errno));
        };
#endif
    }
#endif
}

int node_persistent_cache::set_create(osmid_t id, double lat, double lon)
{
    osmid_t block_offset = id >> WRITE_NODE_BLOCK_SHIFT;
    int i;

    if (cache_already_written)
        return 0;

    if (writeNodeBlock.block_offset != block_offset)
    {
        if (writeNodeBlock.dirty)
        {
            nodes_set_create_writeout_block();
            writeNodeBlock.used = 0;
            writeNodeBlock.dirty = 0;
            /* After writing out the node block, the file pointer is at the next block level */
            writeNodeBlock.block_offset++;
            cacheHeader.max_initialised_id = (writeNodeBlock.block_offset
                    << WRITE_NODE_BLOCK_SHIFT) - 1;
        }
        if (writeNodeBlock.block_offset > block_offset)
        {
            fprintf(stderr,
                    "ERROR: Block_offset not in sequential order: %" PRIdOSMID "%" PRIdOSMID "\n",
                    writeNodeBlock.block_offset, block_offset);
            util::exit_nicely();
        }

        /* We need to fill the intermediate node cache with node nodes to identify which nodes are valid */
        for (i = writeNodeBlock.block_offset; i < block_offset; i++)
        {
            ramNodes_clear(writeNodeBlock.nodes, WRITE_NODE_BLOCK_SIZE);
            nodes_set_create_writeout_block();
        }

        ramNodes_clear(writeNodeBlock.nodes, WRITE_NODE_BLOCK_SIZE);
        writeNodeBlock.used = 0;
        writeNodeBlock.block_offset = block_offset;
    }
#ifdef FIXED_POINT
    writeNodeBlock.nodes[id & WRITE_NODE_BLOCK_MASK].lat = util::double_to_fix(lat, scale_);
    writeNodeBlock.nodes[id & WRITE_NODE_BLOCK_MASK].lon = util::double_to_fix(lon, scale_);
#else
    writeNodeBlock.nodes[id & WRITE_NODE_BLOCK_MASK].lat = lat;
    writeNodeBlock.nodes[id & WRITE_NODE_BLOCK_MASK].lon = lon;
#endif
    writeNodeBlock.used++;
    writeNodeBlock.dirty = 1;

    return 0;
}

int node_persistent_cache::set_append(osmid_t id, double lat, double lon)
{
    osmid_t block_offset = id >> READ_NODE_BLOCK_SHIFT;

    int block_id = find_block(block_offset);

    if (block_id < 0)
        block_id = load_block(block_offset);

#ifdef FIXED_POINT
    if (isnan(lat) && isnan(lon))
    {
        readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat =
                INT_MIN;
        readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon =
                INT_MIN;
    }
    else
    {
        readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat =
                util::double_to_fix(lat, scale_);
        readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon =
                util::double_to_fix(lon, scale_);
    }
#else
    readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat = lat;
    readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon = lon;
#endif
    readNodeBlockCache[block_id].used++;
    readNodeBlockCache[block_id].dirty = 1;

    return 1;
}

int node_persistent_cache::set(osmid_t id, double lat, double lon)
{
    return append_mode ?
        set_append(id, lat, lon) :
        set_create(id, lat, lon);
}

int node_persistent_cache::get(struct osmNode *out, osmid_t id)
{
    osmid_t block_offset = id >> READ_NODE_BLOCK_SHIFT;

    int block_id = find_block(block_offset);

    if (block_id < 0)
    {
        writeout_dirty_nodes(id);
        block_id = load_block(block_offset);
    }

    readNodeBlockCache[block_id].used++;

#ifdef FIXED_POINT
    if ((readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat
            == INT_MIN)
            && (readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon
                    == INT_MIN))
    {
        return 1;
    }
    else
    {
        out->lat =
                util::fix_to_double(readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat, scale_);
        out->lon =
                util::fix_to_double(readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon, scale_);
        return 0;
    }
#else
    if ((isnan(readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat)) &&
            (isnan(readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon)))
    {
        return 1;
    }
    else
    {
        out->lat = readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lat;
        out->lon = readNodeBlockCache[block_id].nodes[id & READ_NODE_BLOCK_MASK].lon;
        return 0;
    }
#endif

    return 0;
}

int node_persistent_cache::get_list(struct osmNode *nodes, const osmid_t *ndids,
        int nd_count)
{
    int count = 0;
    int i;
    for (i = 0; i < nd_count; i++)
    {
        /* Check cache first */
        if (ram_cache && (ram_cache->get(&nodes[i], ndids[i]) == 0))
        {
            count++;
        }
        else
        {
            nodes[i].lat = NAN;
            nodes[i].lon = NAN;
        }
    }
    if (count == nd_count)
        return count;

    for (i = 0; i < nd_count; i++)
    {
        /* In order to have a higher OS level I/O queue depth
           issue posix_fadvise(WILLNEED) requests for all I/O */
        if (isnan(nodes[i].lat) && isnan(nodes[i].lon))
            nodes_prefetch_async(ndids[i]);
    }
    for (i = 0; i < nd_count; i++)
    {
        if ((isnan(nodes[i].lat) && isnan(nodes[i].lon))
                && (get(&(nodes[i]), ndids[i]) == 0))
            count++;
    }

    if (count < nd_count)
    {
        int j = 0;
        for (i = 0; i < nd_count; i++)
        {
            if (!isnan(nodes[i].lat))
            {
                nodes[j].lat = nodes[i].lat;
                nodes[j].lon = nodes[i].lon;
                j++;
            }
        }
        for (i = count; i < nd_count; i++)
        {
            nodes[i].lat = NAN;
            nodes[i].lon = NAN;
        }
    }

    return count;
}

node_persistent_cache::node_persistent_cache(const options_t *options, int append,
                                             boost::shared_ptr<node_ram_cache> ptr)
    : node_cache_fd(0), node_cache_fname(NULL), append_mode(0), cacheHeader(),
      writeNodeBlock(), readNodeBlockCache(NULL),
      scale_(0), cache_already_written(0), ram_cache(ptr)
{
    int i, err;
    scale_ = options->scale;
    append_mode = append;
    if (options->flat_node_file) {
        node_cache_fname = options->flat_node_file->c_str();
    } else {
        throw std::runtime_error("Unable to set up persistent cache: the name "
                                 "of the flat node file was not set.");
    }
    fprintf(stderr, "Mid: loading persistent node cache from %s\n",
            node_cache_fname);

    readNodeBlockCacheIdx.reserve(READ_NODE_CACHE_SIZE);

    /* Setup the file for the node position cache */
    if (append_mode)
    {
        node_cache_fd = open(node_cache_fname, O_RDWR, S_IRUSR | S_IWUSR);
        if (node_cache_fd < 0)
        {
            fprintf(stderr, "Failed to open node cache file: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
    }
    else
    {
        if (cache_already_written)
        {
            node_cache_fd = open(node_cache_fname, O_RDWR, S_IRUSR | S_IWUSR);
        }
        else
        {
            node_cache_fd = open(node_cache_fname, O_RDWR | O_CREAT | O_TRUNC,
                    S_IRUSR | S_IWUSR);
        }

        if (node_cache_fd < 0)
        {
            fprintf(stderr, "Failed to create node cache file: %s\n",
                    strerror(errno));
            util::exit_nicely();
        }
        if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                    strerror(errno));
            util::exit_nicely();
        };
        if (cache_already_written == 0)
        {

            #ifdef HAVE_POSIX_FALLOCATE
            if ((err = posix_fallocate(node_cache_fd, 0,
                    sizeof(struct ramNode) * MAXIMUM_INITIAL_ID)) != 0)
            {
                if (err == ENOSPC) {
                    fprintf(stderr, "Failed to allocate space for node cache file: No space on disk\n");
                } else if (err == EFBIG) {
                    fprintf(stderr, "Failed to allocate space for node cache file: File is too big\n");
                } else {
                    fprintf(stderr, "Failed to allocate space for node cache file: Internal error %i\n", err);
                }

                close(node_cache_fd);
                util::exit_nicely();
            }
            fprintf(stderr, "Allocated space for persistent node cache file\n");
            #endif
            writeNodeBlock.nodes = (struct ramNode *)malloc(
                    WRITE_NODE_BLOCK_SIZE * sizeof(struct ramNode));
            if (!writeNodeBlock.nodes) {
                fprintf(stderr, "Out of memory: Failed to allocate node writeout buffer\n");
                util::exit_nicely();
            }
            ramNodes_clear(writeNodeBlock.nodes, WRITE_NODE_BLOCK_SIZE);
            writeNodeBlock.block_offset = 0;
            writeNodeBlock.used = 0;
            writeNodeBlock.dirty = 0;
            cacheHeader.format_version = PERSISTENT_CACHE_FORMAT_VERSION;
            cacheHeader.id_size = sizeof(osmid_t);
            cacheHeader.max_initialised_id = 0;
            if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
                fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                        strerror(errno));
                util::exit_nicely();
            };
            if (write(node_cache_fd, &cacheHeader,
                    sizeof(struct persistentCacheHeader))
                    != sizeof(struct persistentCacheHeader))
            {
                fprintf(stderr, "Failed to write persistent cache header: %s\n",
                        strerror(errno));
                util::exit_nicely();
            }
        }

    }
    if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    };
    if (read(node_cache_fd, &cacheHeader, sizeof(struct persistentCacheHeader))
            != sizeof(struct persistentCacheHeader))
    {
        fprintf(stderr, "Failed to read persistent cache header: %s\n",
                strerror(errno));
        util::exit_nicely();
    }
    if (cacheHeader.format_version != PERSISTENT_CACHE_FORMAT_VERSION)
    {
        fprintf(stderr, "Persistent cache header is wrong version\n");
        util::exit_nicely();
    }

    if (cacheHeader.id_size != sizeof(osmid_t))
    {
        fprintf(stderr, "Persistent cache header is wrong id type\n");
        util::exit_nicely();
    }

    fprintf(stderr,"Maximum node in persistent node cache: %" PRIdOSMID "\n", cacheHeader.max_initialised_id);

    readNodeBlockCache = (struct ramNodeBlock *)malloc(
            READ_NODE_CACHE_SIZE * sizeof(struct ramNodeBlock));
    if (!readNodeBlockCache) {
        fprintf(stderr, "Out of memory: Failed to allocate node read cache\n");
        util::exit_nicely();
    }
    for (i = 0; i < READ_NODE_CACHE_SIZE; i++)
    {
        readNodeBlockCache[i].nodes = (struct ramNode *)malloc(
                READ_NODE_BLOCK_SIZE * sizeof(struct ramNode));
        if (!readNodeBlockCache[i].nodes) {
            fprintf(stderr, "Out of memory: Failed to allocate node read cache\n");
            util::exit_nicely();
        }
        readNodeBlockCache[i].block_offset = -1;
        readNodeBlockCache[i].used = 0;
        readNodeBlockCache[i].dirty = 0;
    }
}

node_persistent_cache::~node_persistent_cache()
{
    int i;
    writeout_dirty_nodes(-1);

    if (lseek64(node_cache_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "Failed to seek to correct position in node cache: %s\n",
                strerror(errno));
        util::exit_nicely();
    };
    if (write(node_cache_fd, &cacheHeader, sizeof(struct persistentCacheHeader))
            != sizeof(struct persistentCacheHeader))
    {
        fprintf(stderr, "Failed to update persistent cache header: %s\n",
                strerror(errno));
        util::exit_nicely();
    }
    fprintf(stderr,"Maximum node in persistent node cache: %" PRIdOSMID "\n", cacheHeader.max_initialised_id);

    fsync(node_cache_fd);

    if (close(node_cache_fd) != 0)
    {
        fprintf(stderr, "Failed to close node cache file: %s\n",
                strerror(errno));
    }

    for (i = 0; i < READ_NODE_CACHE_SIZE; i++)
    {
        free(readNodeBlockCache[i].nodes);
    }
    free(readNodeBlockCache);
    readNodeBlockCache = NULL;
}
