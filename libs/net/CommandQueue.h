#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace media {

/// The "worker submits, main thread executes" queue both applications use.
///
/// This exists because decoding, the GL context and every UI scene are
/// main-thread state. HTTP worker threads therefore never touch the player or
/// the scene: each handler packs a closure, submits it here, and blocks until
/// the frame loop's poll() has drained the queue and handed back a result.
///
/// The contract that makes it safe:
///   - one instance is owned by one main-thread loop, which calls drain()
///     once per frame and waitForWork() when it would otherwise spin;
///   - submit() is called from any number of worker threads and returns
///     nullopt (never blocks forever) once shutdown has begun;
///   - drain() runs the closures OUTSIDE the lock, so one slow command cannot
///     stall another worker's submit();
///   - a closure that throws is turned into an error Result rather than
///     killing the loop, so the API never dies with a 500-less hang.
///
/// `Result` must be default-constructible and copy/move-assignable — in both
/// applications it is nlohmann::json.
template <typename Result>
class CommandQueue {
public:
	/// Advisory default; the frame loop decides the real value.
	static constexpr int kDefaultWaitMs = 10;

	/// Submit `work` and block until the main thread produces a result.
	/// Returns nullopt only when the queue is shutting down, so a worker can
	/// never wait forever for a poll() that will not come.
	std::optional<Result> submit(std::function<Result()> work) {
		auto command = std::make_shared<Command>();
		command->run = std::move(work);

		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (shuttingDown_) {
				return std::nullopt;
			}
			queue_.push_back(command);
		}
		workCv_.notify_one();

		std::unique_lock<std::mutex> lock(mutex_);
		resultCv_.wait(lock, [&] { return command->done || shuttingDown_; });
		if (!command->done) {
			return std::nullopt;
		}
		return command->result;
	}

	/// Main thread: run everything queued, outside the lock.
	void drain(std::function<Result(const std::string&)> onError) {
		std::deque<std::shared_ptr<Command>> batch;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			batch.swap(queue_);
		}
		for (auto& command : batch) {
			Result result;
			try {
				result = command->run();
			} catch (const std::exception& e) {
				result = onError(std::string("command failed: ") + e.what());
			} catch (...) {
				result = onError(std::string("command failed: unknown exception"));
			}
			{
				std::lock_guard<std::mutex> lock(mutex_);
				command->result = std::move(result);
				command->done = true;
			}
			resultCv_.notify_all();
		}
	}

	/// Main thread: block briefly when there is nothing to do. Keeps the API
	/// responsive without busy-spinning the render loop.
	void waitForWork(int timeoutMs) {
		std::unique_lock<std::mutex> lock(mutex_);
		if (!queue_.empty() || shuttingDown_) {
			return;
		}
		workCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[&] { return !queue_.empty() || shuttingDown_; });
	}

	/// Wake every blocked worker and refuse further work. Called once, on the
	/// shutdown path, before the server thread is joined.
	void failAllWaiters() {
		std::lock_guard<std::mutex> lock(mutex_);
		shuttingDown_ = true;
		queue_.clear();
		resultCv_.notify_all();
		workCv_.notify_all();
	}

private:
	struct Command {
		std::function<Result()> run;
		Result result;
		bool done = false;
	};

	std::mutex mutex_;
	std::condition_variable workCv_;     // wakes the main thread
	std::condition_variable resultCv_;   // wakes blocked workers
	std::deque<std::shared_ptr<Command>> queue_;
	bool shuttingDown_ = false;
};

} // namespace media
