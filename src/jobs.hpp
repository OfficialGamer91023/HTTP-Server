// In-process job queue: requests enqueue work and return at once; background
// workers process it; clients poll for the result.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class JobState { Queued, Running, Done, Failed };
const char* to_string(JobState s);

struct Job {
    std::uint64_t id = 0;
    JobState state = JobState::Queued;
    unsigned delay_ms = 0;  // simulated slow work, so the async flow is visible
    std::string input;      // released once the job starts
    std::size_t lines = 0, words = 0, bytes = 0;
    std::string error;
};

// Counts lines, words and bytes the way `wc` does.
void count_text(const std::string& text, std::size_t& lines, std::size_t& words, std::size_t& bytes);

class JobQueue {
public:
    static constexpr unsigned kMaxDelayMs = 10000;

    JobQueue(std::size_t workers = 2, std::size_t max_pending = 1000, std::size_t max_retained = 10000);
    ~JobQueue();

    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Returns the new job id, or nullopt if the queue is full or stopping.
    std::optional<std::uint64_t> submit(std::string input, unsigned delay_ms = 0);

    // Returns a snapshot of the job, or nullopt if unknown (or evicted).
    std::optional<Job> get(std::uint64_t id) const;

    std::size_t pending() const;

    void shutdown();

private:
    void worker_loop();

    mutable std::mutex mu_;
    std::condition_variable work_cv_;  // wakes idle workers
    std::condition_variable stop_cv_;  // interrupts simulated delays on shutdown
    std::deque<std::uint64_t> pending_;
    std::unordered_map<std::uint64_t, Job> jobs_;
    std::deque<std::uint64_t> finished_;  // oldest first, for eviction
    std::vector<std::thread> workers_;
    std::uint64_t next_id_ = 1;
    std::size_t max_pending_;
    std::size_t max_retained_;
    bool stopping_ = false;
};
