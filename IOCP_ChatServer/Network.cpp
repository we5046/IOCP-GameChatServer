#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <iostream>
#include <deque>
#include <vector>
#include <map>
#include <thread>
#include "RingBuffer.h"
#include "GameServer.h"
#include "Protocol.h"
#include "Session.h"
#include "Network.h"

#pragma comment(lib,"ws2_32.lib")

#define BUFFER_SIZE 1024

unsigned int GetWorkerThreadCount();
bool StartWorkerThreads();

bool IsKnownPacketId(uint16_t id);

DWORD WINAPI WorkerThread(LPVOID lpParam);

bool Network::Init()
{
	InitializeCriticalSection(&g_lock);
	m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,	0, 0);

	if (m_hIOCP == nullptr)
		return false;

	StartWorkerThreads(m_hIOCP);
	return true;
}

Network& Network::Instance()
{
	static Network instance;
	return instance;
}

Network::~Network()
{
	DeleteCriticalSection(&g_lock);
}

// Choose worker count from hardware concurrency.
unsigned int GetWorkerThreadCount()
{
	unsigned int n = std::thread::hardware_concurrency();
	return (n == 0) ? 4 : n;
}

bool Network::StartWorkerThreads(HANDLE& gIOCP)
{
	unsigned int count = GetWorkerThreadCount();
	gWorkerThreads.reserve(count);

	for (unsigned int i = 0; i < count; ++i)
	{
		HANDLE hThread = CreateThread(
			nullptr,
			0,
			WorkerThread,
			gIOCP,
			0,
			nullptr
		);

		if (hThread == nullptr)
		{
			std::cout << "CreateThread failed: " << GetLastError();
			return false;
		}

		gWorkerThreads.push_back(hThread);
	}
	std::cout << "WorkerThreads Created: " << count << "\n";
	return true;
}

void Network::RemoveSession(Session* s)
{
	EnterCriticalSection(&g_lock);
	g_sessions.erase(s->GetSocket());
	LeaveCriticalSection(&g_lock);
}

void Network::AddSession(Session* s)
{
	EnterCriticalSection(&g_lock);
	g_sessions.emplace(s->GetSocket(), s);
	LeaveCriticalSection(&g_lock);
}

DWORD WINAPI WorkerThread(LPVOID lpParam)
{
	HANDLE hIocp = lpParam;

	while (true)
	{
		DWORD bytesTransferred = 0;
		Session* session = nullptr;
		LPOVERLAPPED overlapped = nullptr;

		bool ret = GetQueuedCompletionStatus(
			hIocp,
			&bytesTransferred,
			(PULONG_PTR)&session,
			&overlapped,
			INFINITE);

		if (session == nullptr)
			continue;

		// 1. Handle completed I/O.
		if (overlapped == session->GetRecvOverlapped())
		{
			session->OnRecvComplete(bytesTransferred);
			printf("[Thread %d] RecvComplete\n", GetCurrentThreadId());
		}
		else if (overlapped == session->GetSendOverlapped())
		{
			session->OnSendComplete(bytesTransferred);
			printf("[Thread %d] SendComplete\n", GetCurrentThreadId());
		}
		else if(overlapped == nullptr)
		{
			// Cleanup check event posted by GameServer.
		}
		else
		{
			// Unexpected overlapped. Production code should log this.
		}

		// 2. Handle error or FIN.
		if (!ret || bytesTransferred == 0)
		{
			std::cout << "[Worker] Disconnect detected. err="
				<< GetLastError() << " session=" << session << "\n";

			session->RequestClose();
		}

		// 3. Delete the session only after game cleanup and all I/O completes.
		if (session->GetState() == SessionState::Closing &&
			session->IsGamecleanupDone() &&
			session->GetPendingIO() == 0)
		{
			Network::Instance().RemoveSession(session);
			session->Close();
			delete session;
		}
	}
	return 0;
}

int main()
{
	WSAData wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// Start the game thread first.
	HANDLE hGameThread = CreateThread(
		NULL,
		0,
		GameServer::GameThreadEntry,
		NULL,
		0,
		NULL
	);
	if (hGameThread == NULL)
	{
		printf("Failed to create GameThread. Error: %d\n", GetLastError());
	}
	else
	{
		// Close only the thread handle. The thread keeps running.
		CloseHandle(hGameThread);
	}
	//std::thread gameThread([]() {
	//	GameServer::Instance().GameThreadLoop();
	//	});
	//gameThread.detach();

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	SOCKADDR_IN listenAddr;
	listenAddr.sin_family = AF_INET;
	listenAddr.sin_port = htons(9000);
	InetPton(AF_INET, L"127.0.0.1", &listenAddr.sin_addr);

	bind(listenSock, (SOCKADDR*)&listenAddr, sizeof(listenAddr));
	listen(listenSock, SOMAXCONN);

	// Initialize IOCP and worker threads.
	if (!Network::Instance().Init())
	{
		std::cout << "Network Init failed\n";
		return -1;
	}
	//gIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

	//StartWorkerThreads();

	while (true)
	{
		SOCKET client = accept(listenSock, nullptr, nullptr);
		if (client == INVALID_SOCKET)
			continue;

		Session* session = new Session(client);

		std::cout << "Client " << client << " connected\n";

		Network::Instance().AddSession(session);
		GameServer::Instance().EnqueueConnectJob(session);

		// Register the client socket to IOCP and use Session* as the completion key.
		CreateIoCompletionPort((HANDLE)client, Network::Instance().GetIocpHandle(), (ULONG_PTR)session, 0);

		session->PostRecv();
	}

	WSACleanup();
	return 0;
}
