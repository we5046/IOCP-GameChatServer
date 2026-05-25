#pragma once
#include <map>
class Session;

class Network
{
private:
	HANDLE m_hIOCP = INVALID_HANDLE_VALUE;
	CRITICAL_SECTION g_lock;
	std::vector<HANDLE> gWorkerThreads;
	std::map<SOCKET, Session*> g_sessions;
public:
    static Network& Instance();
	~Network();
	bool Init();
	HANDLE GetIocpHandle() const { return m_hIOCP; }
	bool StartWorkerThreads(HANDLE& gIOCP);

	void RemoveSession(Session* s);
	void AddSession(Session* s);

};	