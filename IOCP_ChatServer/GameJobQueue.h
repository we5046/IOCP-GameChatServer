#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include "GameJob.h"

class GameJobQueue
{
private:
	std::queue<GameJob> queue_;
	std::mutex mutex_;
	std::condition_variable cv_;
public:
	void Push(const GameJob& job)
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push(job);
		}
		cv_.notify_one();

	}

	// blocking pop
	GameJob Pop()
	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait(lock, [this]() {return !queue_.empty(); });
		GameJob job = queue_.front();
		queue_.pop();
		return job;
	}
};