#include "Session.h"
#include <string>
#include <iostream>
#include "Packet.h"
#include "Protocol.h"
#include "GameServer.h"


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

	if (ret == SOCKET_ERROR && GetLastError() != WSA_IO_PENDING)
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
	if(GetState() != SessionState::Connected)
		return;

	bool expected = false;
	if(!sendPosted.compare_exchange_strong(expected, true))
	{
		// 이미 등록된 상태
		return;
	}

	std::vector<char>* pkt = nullptr;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (sendQueue.empty()) {
			// 보낼 게 없는데 sendPosted만 true가 됐으면 롤백
			sendPosted.store(false);
			isSending = false;
			return;
		}
		pkt = &sendQueue.front();
	}
	// 메시지를 sendBuffer에 복사

	ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));

	sendWsabuf.buf = pkt->data();
	sendWsabuf.len = (ULONG)pkt->size();

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

	if (ret == SOCKET_ERROR && GetLastError() != WSA_IO_PENDING)
	{
		std::cout << "WSASEND 실패" << "\n";
		sendPosted.store(false);
		pendingIO.fetch_sub(1, std::memory_order_relaxed);
		RequestClose();

		std::lock_guard<std::mutex> lock(sendMutex);
		isSending = false;
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
		pkt.body = recvRing.GetReadPtr();
		ProcessPacket(this, pkt);

		recvRing.Skip(pkt.header.size - sizeof(PacketHeader));
	}
	// 다시 recv 등록
	PostRecv();
}

void Session::OnSendComplete(DWORD bytes)
{
	sendPosted.store(false);
	pendingIO.fetch_sub(1);

	// 에러 / FIN
	if (bytes == 0)
	{
		RequestClose();
		return;
	}

	bool hasMore = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (!sendQueue.empty()) 
			sendQueue.pop_front();
		hasMore = !sendQueue.empty();
		if (!hasMore) 
			isSending = false;
	}

	if(hasMore)
		PostSend();
}

void Session::RequestClose()
{
	// 이미 종료중이라면 아무 것도 안 한다.
	SessionState expected = SessionState::Connected;
	if (!state.compare_exchange_strong(expected, SessionState::Closing))
		return;

	// 여기서 하는 일은 딱 1개: GameServer에게 "논리적 disconnect" 알림
	GameServer::Instance().EnqueueDisconnectJob(this);

	// shutdown/closesocket 금지 (물리 종료는 WorkerThread에서만)
}

WSAOVERLAPPED* Session::GetRecvOverlapped() { return &recvOverlapped; }
WSAOVERLAPPED* Session::GetSendOverlapped() { return &sendOverlapped; }

void Session::SendPacket(uint16_t id, const void* data, uint16_t dataSize)
{
	if (GetState() == SessionState::Closing)
		return;

	uint16_t packetSize = sizeof(PacketHeader) + dataSize;

	// 1) 패킷 하나를 통째로 담을 buffer 생성
	std::vector<char> packet(packetSize);

	// 2) 헤더 채우기
	PacketHeader header = { packetSize, id };

	// 3) [헤더][바디] 복사
	memcpy(packet.data(), &header, sizeof(header));
	memcpy(packet.data() + sizeof(header), data, dataSize);

	// 4) 큐에 push
	bool needKick = false;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		sendQueue.push_back(std::move(packet));
		if(!isSending)
		{
			isSending = true;
			needKick = true;
		}
	}

	// 5) 지금 아무 것도 안 보내는 중이면 전송 시작
	if (needKick)
	{
		PostSend();
	}
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
