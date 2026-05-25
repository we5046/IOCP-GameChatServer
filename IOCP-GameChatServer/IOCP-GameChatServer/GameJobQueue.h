#pragma once

#include <deque>
#include <mutex>
#include "GameJob.h"


class GameJobQueue
{
private:
	std::deque<GameJob> m_queue;
	std::mutex m_lock;
	std::condition_variable m_cv;
public:
	void Push(GameJob job);
	bool Pop(GameJob& outJob);
};