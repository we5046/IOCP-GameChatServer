#include "GameJobQueue.h"

void GameJobQueue::Push(GameJob job)
{
	std::lock_guard<std::mutex> lock(m_lock);
	m_queue.push_back(job);

	m_cv.notify_one();

}

bool GameJobQueue::Pop(GameJob& outJob)
{
	std::lock_guard<std::mutex> lock(m_lock);

	if (m_queue.empty())
		return false;

	outJob = m_queue.front();
	m_queue.pop_front();
	return true;
}