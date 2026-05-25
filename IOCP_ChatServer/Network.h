#pragma once
#include <map>
class Session;

class Network
{
private:
	HANDLE m_hIOCP = INVALID_HANDLE_VALUE;
	CRITICAL_SECTION m_lock;	// m_lock 초기화 필요
	std::vector<HANDLE> m_WorkerThreads;
	std::map<SOCKET, Session*> m_sessions;

	SOCKET m_listenSocket = INVALID_SOCKET;
	HANDLE m_acceptThread = nullptr;


public:
	std::atomic<bool> m_running = true;
    static Network& Instance();
	~Network();
	bool Init();
	HANDLE GetIocpHandle() const { return m_hIOCP; }
	bool StartWorkerThreads(HANDLE& gIOCP);

	void RemoveSession(Session* s);
	void AddSession(Session* s);

	void RequestShutdownAllSessions();
	void StopWorkerThreads();

	bool AllSessionsIOCompleted();

	bool StartAcceptThread(SOCKET listenSock);
	void SignalStop();
	void JoinAcceptThread();
};	