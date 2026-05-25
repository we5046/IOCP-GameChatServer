#include "Session.h"
#include <string>
#include <iostream>
#include "Packet.h"
#include "Protocol.h"
#include "GameServer.h"

#include <Windows.h>

Session::Session(SOCKET s)
{
	sock = s;
	ZeroMemory(&recvOverlapped, sizeof(WSAOVERLAPPED));
	ZeroMemory(&sendOverlapped, sizeof(WSAOVERLAPPED));

	recvWsabuf.buf = recvBuffer;
	recvWsabuf.len = BUFFER_SIZE;

	sendWsabuf.buf = nullptr;		// 실제 전송할때 sendQueue.front().data()로 설정함.
	sendWsabuf.len = 0;				// 실제 전송할때 sendQueue.front().size()로 설정함. 

	ZeroMemory(recvBuffer, BUFFER_SIZE);

	pendingIO = 0;
}

void Session::PostRecv()
{
	if(GetState() != SessionState::Connected)
		return;

	bool expected = false;
	if(!recvPosted.compare_exchange_strong(expected, true))
	{
		// 이미 등록된 상태
		return;
	}

	// I/O 하나 등록 → pendingIO 증가
	pendingIO.fetch_add(1);

	ZeroMemory(&recvOverlapped, sizeof(recvOverlapped));

	DWORD flags = 0;
	DWORD bytes = 0;

	int ret = WSARecv(
		sock,
		&recvWsabuf,
		1,
		&bytes,
		&flags,
		&recvOverlapped,
		nullptr);

	if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		std::cout << "WSARECV 등록 실패 " << "\n";
		// 등록 실패 → 롤백
		recvPosted.store(false);
		pendingIO.fetch_sub(1);
		RequestClose();
	}
}

void Session::PostSend()
{
	if(GetState() == SessionState::Closed)
		return;

	bool expected = false;
	if(!sendPosted.compare_exchange_strong(expected, true))
	{
		// 이미 등록된 상태
		return;
	}

	char* buf = nullptr;
	ULONG len = 0;
	size_t queueSizeSnapshot = 0;	// 디버깅용
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (sendQueue.empty()) {
			// 보낼 게 없는데 sendPosted만 true가 됐으면 롤백
			sendPosted.store(false);
			sendOffset = 0;
			return;
		}
		queueSizeSnapshot = sendQueue.size(); // 디버깅용
		auto& front = sendQueue.front();

		if (sendOffset > front.size())
			sendOffset = 0; // 안전장치

		buf = front.data() + sendOffset;
		len = (ULONG)(front.size() - sendOffset);
	}
	// 메시지를 sendBuffer에 복사

	ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));

	sendWsabuf.buf = buf;
	sendWsabuf.len = len;

	#if defined(_DEBUG)
		std::cout
			<< "[IOCP][PostSend] session=" << this
			<< " sock=" << (int)sock
			<< " reqLen=" << sendWsabuf.len
			<< " queueSize=" << queueSizeSnapshot
			<< " pendingIO=" << pendingIO.load()
			<< " tid=" << GetCurrentThreadId()
			<< "\n";
	#endif

	DWORD bytes = 0;
	// I/O 등록 → pendingIO 증가
	pendingIO.fetch_add(1, std::memory_order_relaxed);

	int ret = WSASend(
		sock,
		&sendWsabuf,
		1,
		&bytes,
		0,
		&sendOverlapped,
		nullptr);

	if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING)
	{
		#if defined(_DEBUG)
			std::cout
			 << "[IOCP][PostSend][FAIL] session=" << this
			 << " sock=" << (int)sock
			 << " reqLen=" << sendWsabuf.len
			 << " WSAErr=" << WSAGetLastError()
			 << " tid=" << GetCurrentThreadId()
			 << "\n";
		#else
			 std::cout << "WSASEND 실패" << "\n";
		#endif // 디버깅용

		sendPosted.store(false);
		pendingIO.fetch_sub(1, std::memory_order_relaxed);
		RequestClose();
	}
}

void Session::OnRecvComplete(DWORD bytes)
{
	recvPosted.store(false);
	pendingIO.fetch_sub(1);
	
	if (bytes == 0)
	{
		// 상대방이 정상 종료한 경우
		RequestClose();
		return;
	}

	// 이미 Closing 상태면 데이터 무시
	if (GetState() == SessionState::Closing)
		return;

	// 이번에 Recv된 데이터를 RingBuffer에 Write
	if (!recvRing.Write(recvBuffer, bytes))
	{
		// overflow → 서버 정책에 따라 세션 끊기
		std::cout << "[Session] RecvRing overflow, closing session\n";
		RequestClose();
		return;
	}

	// 2) RingBuffer에서 패킷 단위로 뽑아서 ProcessPacket 호출
	while (true)
	{
		if (!recvRing.Has(sizeof(PacketHeader)))
			break;

		Packet pkt;
		recvRing.Peek(&pkt.header, sizeof(PacketHeader));

		if (pkt.header.size < sizeof(PacketHeader) ||
			pkt.header.size > MAX_PACKET_SIZE)
		{
			RequestClose();
			return;
		}

		if (!recvRing.Has(pkt.header.size))
			break;
		recvRing.Skip(sizeof(PacketHeader));
		int bodySize = pkt.header.size - sizeof(PacketHeader);

		std::vector<char> body(bodySize);
		recvRing.Peek(body.data(), bodySize);

		Packet safePkt;
		safePkt.header = pkt.header;
		safePkt.body = body.data();
		
		ProcessPacket(this, safePkt);

		recvRing.Skip(bodySize);
	}
	// 다시 recv 등록
	PostRecv();
}

void Session::OnSendComplete(DWORD bytes)
{
	pendingIO.fetch_sub(1);

	// 에러 / FIN
	if (bytes == 0)
	{
		#if defined(_DEBUG)
			std::cout
			 << "[IOCP][SendComplete][FIN/ERR] session=" << this
			 << " sock=" << (int)sock
			 << " transferred=0"
			 << " tid=" << GetCurrentThreadId()
			 << "\n";
		#endif  // 디버깅용

		sendPosted.store(false);
		RequestClose();
		return;
	}

	 #if defined(_DEBUG)
			// 핵심: 부분 전송(Partial Send) 탐지
	const ULONG intended = sendWsabuf.len;
	if (bytes < intended)
		 {
		std::cout
			 << "[IOCP][SendComplete][PARTIAL] session=" << this
			 << " sock=" << (int)sock
			 << " transferred=" << bytes
			 << " intended=" << intended
			 << " tid=" << GetCurrentThreadId()
			 << "\n";
		}
	#endif	// 디버깅용
		

	bool needMore = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex);

		if (sendQueue.empty())
		{
			sendOffset = 0;
			needMore = false;
		}
		else
		{
			auto& front = sendQueue.front();
			const size_t remaining = (sendOffset <= front.size()) ? (front.size() - sendOffset) : front.size();
			const size_t sent = (size_t)bytes;

			if (sent < remaining)
			{
				// 부분 전송: offset만 증가시키고 같은 패킷 이어서 전송
				sendOffset += sent;
				needMore = true;
			}
			else
			{
				// 해당 패킷 전송 완료
				sendQueue.pop_front();
				sendOffset = 0;
				needMore = !sendQueue.empty();
			}
		}
	}

	sendPosted.store(false);

	if(needMore)
		PostSend();
}

void Session::RequestClose()
{
	// 이미 종료중이라면 아무 것도 안 한다.
	SessionState expected = SessionState::Connected;
	if (!state.compare_exchange_strong(expected, SessionState::Closing))
		return;

	// 여기서 하는 일은 딱 1개: GameServer에게 "논리적 disconnect" 알림
	OnSessionDisconnected(this);	// GameServer의 Instance를 활용하여 바로 보낼 수 있지만, Protocol이 수행하도록 하는게 더 바람직 한지?
	//GameServer::Instance().EnqueueDisconnectJob(this);

	// shutdown/closesocket 금지 (물리 종료는 WorkerThread에서만)
}

WSAOVERLAPPED* Session::GetRecvOverlapped() { return &recvOverlapped; }
WSAOVERLAPPED* Session::GetSendOverlapped() { return &sendOverlapped; }

void Session::SendPacket(uint16_t id, const void* data, uint16_t dataSize)
{
	if (id != PKT_SC_SHUTDOWN)
	{
		if (GetState() == SessionState::Closing)
			return;

		if (GameServer::Instance().IsShuttingDown())
			return;
	}


	uint16_t packetSize = sizeof(PacketHeader) + dataSize;

	if (packetSize > MAX_PACKET_SIZE)
	{
		std::cout << "[SendPacket] 현재 패킷 크기 초과로 전송 불가 id=" << id << " size=" << packetSize << "\n";
		return;	// 여기서 RequestClose()를 하는게 현명할까?
	}

	// 1) 패킷 하나를 통째로 담을 buffer 생성
	std::vector<char> packet(packetSize);

	// 2) 헤더 채우기
	PacketHeader header = { packetSize, id };

	// 3) [헤더][바디] 복사
	memcpy(packet.data(), &header, sizeof(header));
	if(dataSize > 0)
		memcpy(packet.data() + sizeof(header), data, dataSize);

	// 4) 큐에 push
	bool needKick = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		sendQueue.push_back(std::move(packet));
		//if(sendPosted == false)
		//{
		//	sendPosted.store(true);
		//	needKick = true;
		//}
	}

	PostSend();
}

bool Session::TryBeginDestroy()
{
	bool expected = false;
	return destroying.compare_exchange_strong(expected, true);
}

void Session::Close()
{
	if (sock != INVALID_SOCKET)
	{
		shutdown(sock, SD_BOTH);
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
}

SOCKET& Session::GetSocket()
{
	return sock;
}
