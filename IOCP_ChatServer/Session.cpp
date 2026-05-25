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

	sendWsabuf.buf = nullptr;		// Set to sendQueue.front().data() right before sending.
	sendWsabuf.len = 0;				// Set to sendQueue.front().size() right before sending.

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
		// A recv operation is already posted.
		return;
	}

	// One I/O operation is being posted.
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
		std::cout << "WSARecv post failed\n";
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
		// A send operation is already posted.
		return;
	}

	std::vector<char>* pkt = nullptr;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (sendQueue.empty()) {
			sendPosted.store(false);
			isSending = false;
			return;
		}
		pkt = &sendQueue.front();
	}

	// Send the first packet in the queue.
	ZeroMemory(&sendOverlapped, sizeof(sendOverlapped));

	sendWsabuf.buf = pkt->data();
	sendWsabuf.len = (ULONG)pkt->size();

	DWORD bytes = 0;
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
		std::cout << "WSASend failed\n";
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
		// The peer closed the connection gracefully.
		RequestClose();
		return;
	}

	if (GetState() == SessionState::Closing)
		return;

	// Append received bytes to the stream buffer.
	if (!recvRing.Write(recvBuffer, bytes))
	{
		std::cout << "[Session] RecvRing overflow, closing session\n";
		RequestClose();
		return;
	}

	// Extract complete packets from the stream buffer.
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

	PostRecv();
}

void Session::OnSendComplete(DWORD bytes)
{
	sendPosted.store(false);
	pendingIO.fetch_sub(1);

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
	SessionState expected = SessionState::Connected;
	if (!state.compare_exchange_strong(expected, SessionState::Closing))
		return;

	GameServer::Instance().EnqueueDisconnectJob(this);
}

WSAOVERLAPPED* Session::GetRecvOverlapped() { return &recvOverlapped; }
WSAOVERLAPPED* Session::GetSendOverlapped() { return &sendOverlapped; }

void Session::SendPacket(uint16_t id, const void* data, uint16_t dataSize)
{
	if (GetState() == SessionState::Closing)
		return;

	uint16_t packetSize = sizeof(PacketHeader) + dataSize;

	std::vector<char> packet(packetSize);

	PacketHeader header = { packetSize, id };

	memcpy(packet.data(), &header, sizeof(header));
	memcpy(packet.data() + sizeof(header), data, dataSize);

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
