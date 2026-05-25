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

	if (session->GetState() != SessionState::Closing) return;
	if (!session->IsGamecleanupDone()) return;
	if (session->GetPendingIO() != 0) return;

	// 여기까지 왔으면 "지금 당장 삭제 가능" 상태
	if (!session->TryBeginDestroy()) return;

	Network::Instance().RemoveSession(session);
	session->Close();
	delete session;
}

// Network.cpp
bool Network::Init()
{
	InitializeCriticalSection(&m_lock);
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
	DeleteCriticalSection(&m_lock);
}


// 아마도 하드웨어 스레드 개수 구하는 것 같은데 
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

		// 1) shutdown signal 먼저 처리
		if (overlapped == nullptr)
		{
			if (session == nullptr)
			{
				std::cout << "[WorkerThread] exit\n";
				break; // (a) 워커 종료
			}
			// (b) cleanup-only completion
			TryDestroySession(session);
			continue;
		}

		if (session == nullptr)
		{
			// completion key가 nullptr인 비정상 케이스(로그만 남기고 continue 권장)
			std::cout << "[WorkerThread] completionKey is null (unexpected)\n";
			continue;
		}

		if (overlapped == session->GetRecvOverlapped())
		{
			session->OnRecvComplete(bytesTransferred);
			TryDestroySession(session);
			printf("[RECV COMPLETE] bytes=%u session=%p\n", bytesTransferred, session);
			//std::cout << "[Thread " << GetCurrentThreadId() << "] RecvComplete\n";
			continue;
		}
		else if (overlapped == session->GetSendOverlapped())
		{
			session->OnSendComplete(bytesTransferred);
			TryDestroySession(session);
			printf("[SEND COMPLETE] bytes=%u session=%p\n", bytesTransferred, session);
			//std::cout << "[Thread " << GetCurrentThreadId() << "] SendComplete\n";
			continue;
		}
		else
		{
			DWORD err = ret ? 0 : GetLastError();
			std::cout << "[Worker] Disconnect. err=" << err << " session=" << session << "\n";
			session->RequestClose();
			TryDestroySession(session);
		}
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
	m_sessions.erase(s->GetSocket());
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
		// 수정(1)
		session->SendPacket(PKT_SC_SHUTDOWN, nullptr, 0);
		session->RequestClose();
	}
	LeaveCriticalSection(&m_lock);
}

void Network::StopWorkerThreads()
{
	// 워커 스레드가 없으면 종료할 것 없음
	if (m_WorkerThreads.empty())
		return;

	// 각 워커 스레드에 종료 신호(overlapped==nullptr, completionKey==0)를 보낸다.
	for(unsigned int i = 0; i < m_WorkerThreads.size(); ++i)
	{
		PostQueuedCompletionStatus(m_hIOCP, 0, 0, nullptr);
	}

	size_t index = 0;
	while(index < m_WorkerThreads.size())
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

// AcceptThread 함수 선언필요해서 작성
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

	// accept() 깨우기
	SOCKET s = m_listenSocket;
	m_listenSocket = INVALID_SOCKET;
	if( s != INVALID_SOCKET )
		closesocket(s);
}

void Network::JoinAcceptThread()
{
	if(m_acceptThread)
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

			// SignalStop()에서 closesocket(listenSock)을 했으면 이 if문에서 걸림 -> 정상 종료
			if (!Network::Instance().m_running)
				break;

			// 그 외의 에러
			std::cout << "[AcceptThread] accept failed: " << err << "\n";
			continue;
		}

		// 정상 accept 처리
		// Session 생성 + IOCP 등록 + PostRecv + GameServer Connect Job 등록
		Session* session = new Session(client);

		Network::Instance().AddSession(session);
		GameServer::Instance().EnqueueConnectJob(session);

		// 서버에 접속한 클라이언트를 IOCP로 감시해주세요. 다만 KEY는 이제부터 Session*
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
		return TRUE; // OS에게 "우리가 처리한다"라고 알림
	default:
		return FALSE;
	}
}

int main()
{
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	WSAData wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	// ★ FSM 초기화 (아주 중요)
	GameServer::Instance().Init();
	// 이 안에서 InitFSM() 호출

	// GameThread 먼저 시작
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
		// gameThread.detach(); 와 같은 행위
		CloseHandle(hGameThread); // 핸들을 닫아도 스레드는 계속 돈다 (Detach와 동일)
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

	// 인자 넣는거 못외움
	if (!Network::Instance().Init())
	{
		std::cout << "Network Init 실패\n";
		return -1;
	}

	if (Network::Instance().StartAcceptThread(listenSock))
	{
		listenSock = INVALID_SOCKET; // AcceptThread가 소켓 관리를 맡음
	}
	else
	{
		closesocket(listenSock); // 실패했으면 main이 닫고 종료
		std::cerr << "AcceptThread 시작 실패\n";
		return -1;
	}

	while(Network::Instance().m_running)
	{
		Sleep(100);
	}

	// AcceptThread 사용 종료
	Network::Instance().JoinAcceptThread();
	WSACleanup();
	return 0;

}