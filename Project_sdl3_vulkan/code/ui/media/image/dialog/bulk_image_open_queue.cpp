#include "bulk_image_open_queue.hpp"
#include "core/thread/thread_overwatch.hpp"


BulkImageOpenQueue::BulkImageOpenQueue()
    : m_worker{}
    , m_mutex{}
    , m_ready_paths{}
    , m_active_paths{}
    , m_worker_watch_id{0}
{
}

void BulkImageOpenQueue::enqueue_batch(const std::vector<std::string>& paths)
{
    stop_worker_thread(true);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ready_paths.clear();
        m_active_paths = paths;
    }

    start_worker_thread(paths);
}

bool BulkImageOpenQueue::try_pop_ready(std::string *out_path)
{
    if (!out_path)
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ready_paths.empty())
        return false;

    *out_path = m_ready_paths.front();
    m_ready_paths.pop_front();
    return true;
}

void BulkImageOpenQueue::shutdown()
{
    stop_worker_thread(true);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready_paths.clear();
    m_active_paths.clear();
}

void BulkImageOpenQueue::start_worker_thread(const std::vector<std::string> &paths)
{
    if (m_worker.joinable())
        return;

    m_worker = std::jthread([this, paths](const std::stop_token& stoken) {
        const uint64_t watch_id = m_worker_watch_id.load(std::memory_order_acquire);
        ThreadOverwatch::instance().heartbeat(watch_id);

        std::deque<std::string> validated;
        for (const auto &path : paths) {
            if (stoken.stop_requested())
                break;

            if (std::filesystem::exists(path))
                validated.push_back(path);

            ThreadOverwatch::instance().heartbeat(watch_id);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto &path : validated)
                m_ready_paths.push_back(path);
            m_active_paths.clear();
        }

        ThreadOverwatch::instance().heartbeat(watch_id);

        ThreadOverwatch::instance().unwatch(watch_id);
        m_worker_watch_id.store(0, std::memory_order_release);
    });

    if (m_worker_watch_id.load(std::memory_order_acquire) == 0) {
        const auto watch_id = ThreadOverwatch::instance().watch(
            "BulkImageOpenQueue::worker",
            std::chrono::milliseconds(5000),
            [this]() { stop_worker_thread(false); },
            [this]() {
                std::vector<std::string> paths;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    paths = m_active_paths;
                }
                if (!paths.empty())
                    start_worker_thread(paths);
            });
        m_worker_watch_id.store(watch_id, std::memory_order_release);
    }

    ThreadOverwatch::instance().heartbeat(
        m_worker_watch_id.load(std::memory_order_acquire));
}

void BulkImageOpenQueue::stop_worker_thread(bool unregister_watch)
{
    if (unregister_watch) {
        const uint64_t watch_id = m_worker_watch_id.exchange(0, std::memory_order_acq_rel);
        ThreadOverwatch::instance().unwatch(watch_id);
    }

    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_worker.join();
    }
}
