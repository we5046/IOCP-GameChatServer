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

static void TryDestroySession(Session* session)
{
	if (!session) return;

	if (session->GetState() == SessionState::Connected) return;
	if (!session->IsGamecleanupDone()) return;
	if (session->GetPendingIO() != 0) return;

	// 논리 종료, 게임 정리, pending I/O 완료 후에만 세션을 제거한다.
	if (!session->TryBeginDestroy()) return;

	Network::Instance().RemoveSession(session);
	delete session;
}

bool Network::Init()
{
	InitializeCriticalSection(&m_lock);
	m_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

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
	DeleteCriticalSection(&m_lock);
}

unsigned int GetWorkerThreadCount()
{
	unsigned int n = std::thread::hardware_concurrency();
	return (n == 0) ? 4 : n;
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

		// overlapped == nullptr이면 워커 종료 또는 cleanup-only completion이다.
		if (overlapped == nullptr)
		{
			if (session == nullptr)
			{
				std::cout << "[WorkerThread] exit\n";
				break;
			}

			TryDestroySession(session);
			continue;
		}

		if (session == nullptr)
		{
			// completion key가 비어 있으면 처리할 세션이 없으므로 로그만 남긴다.
			std::cout << "[WorkerThread] completionKey is null (unexpected)\n";
			continue;
		}

		IocpContext* context = reinterpret_cast<IocpContext*>(overlapped);
		const bool success = ret == TRUE;
		const DWORD err = success ? 0 : GetLastError();

		if (!success)
		{
			std::cout << "[Worker] IO failed. err=" << err
				<< " session=" << session
				<< " ctx=" << context
				<< " type=" << static_cast<int>(context->type)
				<< "\n";
		}

		if (context->type == IoType::Recv)
		{
			session->OnRecvComplete(bytesTransferred, success);
			TryDestroySession(session);
			continue;
		}

		if (context->type == IoType::Send)
		{
			session->OnSendComplete(static_cast<SendContext*>(context), bytesTransferred, success);
			TryDestroySession(session);
			continue;
		}

		std::cout << "[Worker] Unknown IO context. session=" << session
			<< " ctx=" << context << "\n";
		session->RequestClose();
		TryDestroySession(session);
	}
	return 0;
}

bool Network::StartWorkerThreads(HANDLE& gIOCP)
{
	unsigned int count = GetWorkerThreadCount();
	m_WorkerThreads.reserve(count);

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

		m_WorkerThreads.push_back(hThread);
	}
	std::cout << "WorkerThreads Created: " << count << "\n";
	return true;
}

void Network::RemoveSession(Session* s)
{
	EnterCriticalSection(&m_lock);
	for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
	{
		if (it->second == s)
		{
			m_sessions.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&m_lock);
}

void Network::AddSession(Session* s)
{
	EnterCriticalSection(&m_lock);
	m_sessions.emplace(s->GetSocket(), s);
	LeaveCriticalSection(&m_lock);
}

void Network::RequestShutdownAllSessions()
{
	EnterCriticalSection(&m_lock);
	for (auto& [sock, session] : m_sessions)
	{
		// 종료 패킷을 먼저 보내고 논리 종료를 요청한다.
		session->SendPacket(PKT_SC_SHUTDOWN, nullptr, 0);
		session->RequestClose();
	}
	LeaveCriticalSection(&m_lock);
}

void Network::StopWorkerThreads()
{
	if (m_WorkerThreads.empty())
		return;

	// 각 워커 스레드에 종료 신호(overlapped == nullptr, completionKey == 0)를 보낸다.
	for (unsigned int i = 0; i < m_WorkerThreads.size(); ++i)
	{
		PostQueuedCompletionStatus(m_hIOCP, 0, 0, nullptr);
	}

	size_t index = 0;
	while (index < m_WorkerThreads.size())
	{
		size_t remaining = m_WorkerThreads.size() - index;
		DWORD chunk = (DWORD)((remaining > MAXIMUM_WAIT_OBJECTS) ? MAXIMUM_WAIT_OBJECTS : remaining);

		DWORD ret = WaitForMultipleObjects(
			chunk,
			&m_WorkerThreads[index],
			TRUE,
			INFINITE);
		if (ret == WAIT_FAILED)
		{
			std::cout << "WaitForMultipleObjects failed: " << GetLastError() << "\n";
			break;
		}

		index += chunk;
	}
}

bool Network::AllSessionsIOCompleted()
{
	EnterCriticalSection(&m_lock);

	for (auto& [sock, session] : m_sessions)
	{
		if (session->GetPendingIO() != 0)
		{
			LeaveCriticalSection(&m_lock);
			return false;
		}
	}

	LeaveCriticalSection(&m_lock);
	return true;
}

DWORD WINAPI AcceptThread(LPVOID lpParam);

bool Network::StartAcceptThread(SOCKET listenSock)
{
	m_listenSocket = listenSock;
	m_acceptThread = CreateThread(nullptr, 0, AcceptThread, (LPVOID)listenSock, 0, nullptr);

	return m_acceptThread != nullptr;
}

void Network::SignalStop()
{
	m_running = false;

	// accept()를 깨우기 위해 listen socket을 닫는다.
	SOCKET s = m_listenSocket;
	m_listenSocket = INVALID_SOCKET;
	if (s != INVALID_SOCKET)
		closesocket(s);
}

void Network::JoinAcceptThread()
{
	if (m_acceptThread)
	{
		WaitForSingleObject(m_acceptThread, INFINITE);
		CloseHandle(m_acceptThread);
		m_acceptThread = nullptr;
	}
}

DWORD WINAPI AcceptThread(LPVOID lpParam)
{
	SOCKET listenSock = (SOCKET)lpParam;

	while (Network::Instance().m_running)
	{
		SOCKET client = accept(listenSock, nullptr, nullptr);
		if (client == INVALID_SOCKET)
		{
			int err = WSAGetLastError();

			// SignalStop에서 listen socket을 닫은 경우는 정상 종료다.
			if (!Network::Instance().m_running)
				break;

			std::cout << "[AcceptThread] accept failed: " << err << "\n";
			continue;
		}

		// Session 생성, IOCP 등록, PostRecv, GameServer Connect Job 등록.
		Session* session = new Session(client);

		Network::Instance().AddSession(session);
		GameServer::Instance().EnqueueConnectJob(session);

		// IOCP completion key는 Session*로 사용한다.
		CreateIoCompletionPort((HANDLE)client, Network::Instance().GetIocpHandle(), (ULONG_PTR)session, 0);

		session->PostRecv();
	}
	return 0;
}

BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		Network::Instance().SignalStop();
		GameServer::Instance().Shutdown();
		return TRUE;
	default:
		return FALSE;
	}
}

int main()
{
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	WSAData wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	GameServer::Instance().Init();

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
		CloseHandle(hGameThread);
	}

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
	SOCKADDR_IN listenAddr;
	listenAddr.sin_family = AF_INET;
	listenAddr.sin_port = htons(9000);
	InetPton(AF_INET, L"127.0.0.1", &listenAddr.sin_addr);

	bind(listenSock, (SOCKADDR*)&listenAddr, sizeof(listenAddr));
	listen(listenSock, SOMAXCONN);

	if (!Network::Instance().Init())
	{
		std::cout << "Network Init 실패\n";
		return -1;
	}

	if (Network::Instance().StartAcceptThread(listenSock))
	{
		listenSock = INVALID_SOCKET;
	}
	else
	{
		closesocket(listenSock);
		std::cerr << "AcceptThread 시작 실패\n";
		return -1;
	}

	while (Network::Instance().m_running)
	{
		Sleep(100);
	}

	Network::Instance().JoinAcceptThread();
	WSACleanup();
	return 0;
}
