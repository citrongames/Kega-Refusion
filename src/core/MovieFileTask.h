#pragma once

#include <QThreadPool>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

// Unlike std::async, this future never joins a thread from get() or destruction.
// The session owns the single-worker pool and drains it only during shutdown.
// Jobs must own their file data; they must not access the live emulation host.
template<typename Work>
auto submitMovieFileTask(QThreadPool& pool, Work work) {
    using Result = std::invoke_result_t<Work>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::move(work));
    auto result = task->get_future();
    pool.start([task = std::move(task)] { (*task)(); });
    return result;
}
