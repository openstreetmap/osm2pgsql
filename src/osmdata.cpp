#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <osmium/thread/pool.hpp>

#include "db-copy.hpp"
#include "format.hpp"
#include "middle.hpp"
#include "middle-pgsql.hpp"
#include "osmdata.hpp"
#include "output.hpp"
#include "util.hpp"

osmdata_t::osmdata_t(std::shared_ptr<middle_t> mid,
                     std::vector<std::shared_ptr<output_t>> const &outs)
: m_mid(mid), m_outs(outs)
{
    assert(m_mid);

    if (m_outs.empty()) {
        throw std::runtime_error{"Must have at least one output, "
                                 "but none have been configured."};
    }

    // Get the "extra_attributes" option from the first output. We expect
    // all others to be the same.
    m_with_extra_attrs = m_outs[0]->get_options()->extra_attributes;
}

/**
 * For modify and delete member functions a middle_t is not enough, an object
 * of the derived class slim_middle_t is needed. This function does the
 * conversion. It should always succeed, because the modify and delete
 * functions are never called for non-slim middles.
 */
slim_middle_t &osmdata_t::slim_middle() const noexcept
{
    auto *slim = dynamic_cast<slim_middle_t *>(m_mid.get());
    assert(slim);
    return *slim;
}

void osmdata_t::node_add(osmium::Node const &node) const
{
    m_mid->node_set(node);

    if (m_with_extra_attrs || !node.tags().empty()) {
        for (auto &out : m_outs) {
            out->node_add(node);
        }
    }
}

void osmdata_t::way_add(osmium::Way *way) const
{
    m_mid->way_set(*way);

    if (m_with_extra_attrs || !way->tags().empty()) {
        for (auto &out : m_outs) {
            out->way_add(way);
        }
    }
}

void osmdata_t::relation_add(osmium::Relation const &rel) const
{
    m_mid->relation_set(rel);

    if (m_with_extra_attrs || !rel.tags().empty()) {
        for (auto &out : m_outs) {
            out->relation_add(rel);
        }
    }
}

void osmdata_t::node_modify(osmium::Node const &node) const
{
    auto &slim = slim_middle();

    slim.node_delete(node.id());
    slim.node_set(node);

    for (auto &out : m_outs) {
        out->node_modify(node);
    }

    slim.node_changed(node.id());
}

void osmdata_t::way_modify(osmium::Way *way) const
{
    auto &slim = slim_middle();

    slim.way_delete(way->id());
    slim.way_set(*way);

    for (auto &out : m_outs) {
        out->way_modify(way);
    }

    slim.way_changed(way->id());
}

void osmdata_t::relation_modify(osmium::Relation const &rel) const
{
    auto &slim = slim_middle();

    slim.relation_delete(rel.id());
    slim.relation_set(rel);

    for (auto &out : m_outs) {
        out->relation_modify(rel);
    }

    slim.relation_changed(rel.id());
}

void osmdata_t::node_delete(osmid_t id) const
{
    for (auto &out : m_outs) {
        out->node_delete(id);
    }

    slim_middle().node_delete(id);
}

void osmdata_t::way_delete(osmid_t id) const
{
    for (auto &out : m_outs) {
        out->way_delete(id);
    }

    slim_middle().way_delete(id);
}

void osmdata_t::relation_delete(osmid_t id) const
{
    for (auto &out : m_outs) {
        out->relation_delete(id);
    }

    slim_middle().relation_delete(id);
}

void osmdata_t::start() const
{
    for (auto &out : m_outs) {
        out->start();
    }
}

void osmdata_t::flush() const
{
    m_mid->flush();
}

namespace {

//TODO: have the main thread using the main middle to query the middle for batches of ways (configurable number)
//and stuffing those into the work queue, so we have a single producer multi consumer threaded queue
//since the fetching from middle should be faster than the processing in each backend.

struct pending_threaded_processor : public middle_t::pending_processor
{
    using output_vec_t = std::vector<std::shared_ptr<output_t>>;

    static void do_jobs(output_vec_t const &outputs, pending_queue_t &queue,
                        size_t &ids_done, std::mutex &mutex, int append,
                        bool ways)
    {
        while (true) {
            //get the job off the queue synchronously
            pending_job_t job;
            mutex.lock();
            if (queue.empty()) {
                mutex.unlock();
                break;
            }
            job = queue.top();
            queue.pop();
            mutex.unlock();

            //process it
            if (ways) {
                outputs.at(job.output_id)->pending_way(job.osm_id, append);
            } else {
                outputs.at(job.output_id)->pending_relation(job.osm_id, append);
            }

            mutex.lock();
            ++ids_done;
            mutex.unlock();
        }
    }

    static void print_stats(pending_queue_t &queue, std::mutex &mutex)
    {
        size_t queue_size;
        do {
            mutex.lock();
            queue_size = queue.size();
            mutex.unlock();

            fmt::print(stderr, "\rLeft to process: {}...", queue_size);

            std::this_thread::sleep_for(std::chrono::seconds(1));
        } while (queue_size > 0);
    }

    //starts up count threads and works on the queue
    pending_threaded_processor(std::shared_ptr<middle_t> mid,
                               output_vec_t const &outs, size_t thread_count,
                               int append)
    //note that we cant hint to the stack how large it should be ahead of time
    //we could use a different datastructure like a deque or vector but then
    //the outputs the enqueue jobs would need the version check for the push(_back) method
    : outs(outs), ids_queued(0), append(append), queue(), ids_done(0)
    {

        //clone all the things we need
        clones.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            auto const midq = mid->get_query_instance();
            auto copy_thread = std::make_shared<db_copy_thread_t>(
                outs[0]->get_options()->database_options.conninfo());

            //clone the outs
            output_vec_t out_clones;
            for (auto const &out : outs) {
                out_clones.push_back(out->clone(midq, copy_thread));
            }

            //keep the clones for a specific thread to use
            clones.push_back(out_clones);
        }
    }

    void enqueue_ways(osmid_t id) override
    {
        for (size_t i = 0; i < outs.size(); ++i) {
            outs[i]->enqueue_ways(queue, id, i, ids_queued);
        }
    }

    //waits for the completion of all outstanding jobs
    void process_ways() override
    {
        //reset the number we've done
        ids_done = 0;

        fmt::print(stderr, "\nGoing over pending ways...\n");
        fmt::print(stderr, "\t{} ways are pending\n", ids_queued);
        fmt::print(stderr, "\nUsing {} helper-processes\n", clones.size());
        util::timer_t timer;

        //make the threads and start them
        std::vector<std::future<void>> workers;
        for (auto const &clone : clones) {
            workers.push_back(std::async(
                std::launch::async, do_jobs, std::cref(clone), std::ref(queue),
                std::ref(ids_done), std::ref(mutex), append, true));
        }
        workers.push_back(std::async(std::launch::async, print_stats,
                                     std::ref(queue), std::ref(mutex)));

        for (auto &w : workers) {
            try {
                w.get();
            } catch (...) {
                // drain the queue, so that the other workers finish
                mutex.lock();
                while (!queue.empty()) {
                    queue.pop();
                }
                mutex.unlock();
                throw;
            }
        }

        timer.stop();
        fmt::print(stderr, "\rFinished processing {} ways in {} s\n\n",
                   ids_queued, timer.elapsed());
        if (timer.elapsed() > 0) {
            fmt::print(
                stderr, "{} Pending ways took {}s at a rate of {:.2f}/s\n",
                ids_queued, timer.elapsed(), timer.per_second(ids_queued));
        }
        ids_queued = 0;
        ids_done = 0;

        //collect all the new rels that became pending from each
        //output in each thread back to their respective main outputs
        for (auto const &clone : clones) {
            //for each clone/original output
            for (output_vec_t::const_iterator original_output = outs.begin(),
                                              clone_output = clone.begin();
                 original_output != outs.end() && clone_output != clone.end();
                 ++original_output, ++clone_output) {
                //done copying ways for now
                clone_output->get()->commit();
                //merge the pending from this threads copy of output back
                original_output->get()->merge_pending_relations(
                    clone_output->get());
            }
        }
    }

    void enqueue_relations(osmid_t id) override
    {
        for (size_t i = 0; i < outs.size(); ++i) {
            outs[i]->enqueue_relations(queue, id, i, ids_queued);
        }
    }

    void process_relations() override
    {
        //reset the number we've done
        ids_done = 0;

        fmt::print(stderr, "\nGoing over pending relations...\n");
        fmt::print(stderr, "\t{} relations are pending\n", ids_queued);
        fmt::print(stderr, "\nUsing {} helper-processes\n", clones.size());
        util::timer_t timer;

        //make the threads and start them
        std::vector<std::future<void>> workers;
        for (auto const &clone : clones) {
            workers.push_back(std::async(
                std::launch::async, do_jobs, std::cref(clone), std::ref(queue),
                std::ref(ids_done), std::ref(mutex), append, false));
        }
        workers.push_back(std::async(std::launch::async, print_stats,
                                     std::ref(queue), std::ref(mutex)));

        for (auto &w : workers) {
            try {
                w.get();
            } catch (...) {
                // drain the queue, so the other worker finish immediately
                mutex.lock();
                while (!queue.empty()) {
                    queue.pop();
                }
                mutex.unlock();
                throw;
            }
        }

        timer.stop();
        fmt::print(stderr, "\rFinished processing {} relations in {} s\n\n",
                   ids_queued, timer.elapsed());
        if (timer.elapsed() > 0) {
            fmt::print(
                stderr, "{} Pending relations took {}s at a rate of {:.2f}/s\n",
                ids_queued, timer.elapsed(), timer.per_second(ids_queued));
        }
        ids_queued = 0;
        ids_done = 0;

        //collect all expiry tree informations together into one
        for (auto const &clone : clones) {
            //for each clone/original output
            for (output_vec_t::const_iterator original_output = outs.begin(),
                                              clone_output = clone.begin();
                 original_output != outs.end() && clone_output != clone.end();
                 ++original_output, ++clone_output) {
                //done copying rels for now
                clone_output->get()->commit();
                //merge the expire tree from this threads copy of output back
                original_output->get()->merge_expire_trees(clone_output->get());
            }
        }
    }

private:
    // output copies, one vector per thread
    std::vector<output_vec_t> clones;
    output_vec_t
        outs; //would like to move ownership of outs to osmdata_t and middle passed to output_t instead of owned by it
    //how many jobs do we have in the queue to start with
    size_t ids_queued;
    //appending to output that is already there (diff processing)
    bool append;
    //job queue
    pending_queue_t queue;

    //how many ids within the job have been processed
    size_t ids_done;
    //so the threads can manage some of the shared state
    std::mutex mutex;
};

} // anonymous namespace

/**
 * Is there any pending work in the middle or one of the outputs?
 */
bool osmdata_t::has_pending() const noexcept
{
    if (m_mid->has_pending()) {
        return true;
    }

    return std::any_of(m_outs.cbegin(), m_outs.cend(),
                       [](std::shared_ptr<output_t> const &out) {
                           return out->has_pending();
                       });
}

void osmdata_t::stop() const
{
    /* Commit the transactions, so that multiple processes can
     * access the data simultaneously to process the rest in parallel
     * as well as see the newly created tables.
     */
    m_mid->commit();
    for (auto &out : m_outs) {
        //TODO: each of the outs can be in parallel
        out->commit();
    }

    // should be the same for all outputs
    auto const *opts = m_outs[0]->get_options();

    // are there any objects left pending?
    if (has_pending()) {
        fmt::print(stderr, "Entering stage 1b...\n");

        //threaded pending processing
        pending_threaded_processor ptp(m_mid, m_outs, opts->num_procs,
                                       opts->append);

        if (!m_outs.empty()) {
            //This stage takes ways which were processed earlier, but might be
            //involved in a multipolygon relation. They could also be ways that
            //were modified in diff processing.
            m_mid->iterate_ways(ptp);

            //This is like pending ways, except there aren't pending relations
            //on import, only on update.
            //TODO: Can we skip this on import?
            m_mid->iterate_relations(ptp);
        }
    } else {
        fmt::print(stderr, "Skipping stage 1b.\n");
    }

    if (opts->append) {
        fmt::print(stderr, "Entering stage 1c...\n");

        for (auto &out : m_outs) {
            if (out->has_stage1c_pending()) {
                auto new_mid = std::make_shared<middle_pgsql_t>(opts);
                new_mid->start();

                out->stage1c_proc(new_mid.get());

                if (new_mid->has_pending()) {
                    pending_threaded_processor ptp(
                        new_mid, m_outs, opts->num_procs, opts->append);
                    new_mid->iterate_relations(ptp);
                }
            }
        }
    }

    for (auto &out : m_outs) {
        out->stage2_proc();
    }

    // Clustering, index creation, and cleanup.
    // All the intensive parts of this are long-running PostgreSQL commands
    {
        osmium::thread::Pool pool{opts->parallel_indexing ? opts->num_procs : 1,
                                  512};

        if (opts->droptemp) {
            // When dropping middle tables, make sure they are gone before
            // indexing starts.
            m_mid->stop(pool);
        }

        for (auto &out : m_outs) {
            out->stop(&pool);
        }

        if (!opts->droptemp) {
            // When keeping middle tables, there is quite a large index created
            // which is better done after the output tables have been copied.
            // Note that --disable-parallel-indexing needs to be used to really
            // force the order.
            m_mid->stop(pool);
        }

        // Waiting here for pool to execute all tasks.
        // XXX If one of them has an error, all other will finish first,
        //     which may take a long time.
    }
}
