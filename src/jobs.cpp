#include "jobs.hpp"

#include <cctype>
#include <chrono>

const char* to_string(JobState s) {
    switch (s) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
    }
    return "unknown";
}

void count_text(const std::string& text, std::size_t& lines, std::size_t& words, std::size_t& bytes) {
    lines = words = 0;
    bytes = text.size();
    bool in_word = false;
    for (char c : text) {
        if (c == '\n') ++lines;
        if (std::isspace(static_cast<unsigned char>(c))) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
        }
    }
}

JobQueue::JobQueue(std::size_t workers, std::size_t max_pending, std::size_t max_retained)
    : max_pending_(max_pending), max_retained_(max_retained) {
    if (workers == 0) workers = 1;
    for (std::size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { worker_loop(); });
}

JobQueue::~JobQueue() { shutdown(); }

std::optional<std::uint64_t> JobQueue::submit(std::string input, unsigned delay_ms) {
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_ || pending_.size() >= max_pending_) return std::nullopt;
        id = next_id_++;
        Job job;
        job.id = id;
        job.delay_ms = delay_ms > kMaxDelayMs ? kMaxDelayMs : delay_ms;
        job.input = std::move(input);
        jobs_.emplace(id, std::move(job));
        pending_.push_back(id);
    }
    work_cv_.notify_one();
    return id;
}

std::optional<Job> JobQueue::get(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = jobs_.find(id);
    if (it == jobs_.end()) return std::nullopt;
    Job copy = it->second;
    copy.input.clear();  // callers only need status and results
    return copy;
}

std::size_t JobQueue::pending() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

void JobQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    work_cv_.notify_all();
    stop_cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
}

void JobQueue::worker_loop() {
    for (;;) {
        std::uint64_t id;
        std::string input;
        unsigned delay_ms;
        {
            std::unique_lock<std::mutex> lock(mu_);
            work_cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;
            id = pending_.front();
            pending_.pop_front();
            Job& job = jobs_.at(id);
            job.state = JobState::Running;
            input = std::move(job.input);
            job.input.clear();
            delay_ms = job.delay_ms;

            if (delay_ms > 0) {
                const bool interrupted = stop_cv_.wait_for(
                    lock, std::chrono::milliseconds(delay_ms), [this] { return stopping_; });
                if (interrupted) {
                    jobs_.at(id).state = JobState::Failed;
                    jobs_.at(id).error = "server shutting down";
                    return;
                }
            }
        }

        std::size_t lines, words, bytes;
        count_text(input, lines, words, bytes);

        std::lock_guard<std::mutex> lock(mu_);
        Job& job = jobs_.at(id);  // running jobs are never evicted
        job.lines = lines;
        job.words = words;
        job.bytes = bytes;
        job.state = JobState::Done;
        finished_.push_back(id);
        while (finished_.size() > max_retained_) {
            jobs_.erase(finished_.front());
            finished_.pop_front();
        }
    }
}
