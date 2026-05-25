#include "Session.h"

#include <Windows.h>
#include <cstring>
#include <iostream>

#include "GameServer.h"
#include "Packet.h"
#include "Protocol.h"

Session::Session(SOCKET s) : sock(s)
{
}

void Session::PostRecv()
{
	if (GetState() != SessionState::Connected)
		return;

	bool expected = false;
	if (!recvPosted.compare_exchange_strong(expected, true))
		return;

	ZeroMemory(&recvContext.overlapped, sizeof(recvContext.overlapped));
	recvContext.wsabuf.buf = recvContext.buffer;
	recvContext.wsabuf.len = BUFFER_SIZE;

	DWORD flags = 0;
	DWORD bytes = 0;
	pendingIO.fetch_add(1, std::memory_order_acq_rel);

	int ret = WSARecv(
		sock,
		&recvContext.wsabuf,
		1,
		&bytes,
		&flags,
		&recvContext.overlapped,
		nullptr);

	if (ret == SOCKET_ERROR)
	{
		const int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			std::cout << "[IOCP][PostRecv][FAIL] session=" << this
				<< " sock=" << (int)sock
				<< " err=" << err
				<< " pendingIO=" << pendingIO.load()
				<< " ctx=" << &recvContext
				<< "\n";

			recvPosted.store(false, std::memory_order_release);
			pendingIO.fetch_sub(1, std::memory_order_acq_rel);
			RequestClose();
		}
	}
}

void Session::PostSend(const char* data, int len)
{
	if (GetState() == SessionState::Closed)
		return;

	if (data == nullptr || len <= 0 || len > MAX_PACKET_SIZE)
		return;

	SendContext* context = new SendContext(data, len);

	#if defined(_DEBUG)
	std::cout << "[IOCP][PostSend] session=" << this
		<< " sock=" << (int)sock
		<< " len=" << context->wsabuf.len
		<< " pendingIO=" << pendingIO.load()
		<< " ctx=" << context
		<< " tid=" << GetCurrentThreadId()
		<< "\n";
	#endif

	DWORD bytes = 0;
	pendingIO.fetch_add(1, std::memory_order_acq_rel);

	int ret = WSASend(
		sock,
		&context->wsabuf,
		1,
		&bytes,
		0,
		&context->overlapped,
		nullptr);

	if (ret == SOCKET_ERROR)
	{
		const int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			std::cout << "[IOCP][PostSend][FAIL] session=" << this
				<< " sock=" << (int)sock
				<< " len=" << context->wsabuf.len
				<< " err=" << err
				<< " pendingIO=" << pendingIO.load()
				<< " ctx=" << context
				<< "\n";

			pendingIO.fetch_sub(1, std::memory_order_acq_rel);
			delete context;
			RequestClose();
		}
	}
}

void Session::OnRecvComplete(DWORD bytes, bool success)
{
	recvPosted.store(false, std::memory_order_release);
	pendingIO.fetch_sub(1, std::memory_order_acq_rel);

	#if defined(_DEBUG)
	std::cout << "[IOCP][RecvComplete] session=" << this
		<< " sock=" << (int)sock
		<< " bytes=" << bytes
		<< " success=" << success
		<< " pendingIO=" << pendingIO.load()
		<< " ctx=" << &recvContext
		<< " tid=" << GetCurrentThreadId()
		<< "\n";
	#endif

	if (!success || bytes == 0)
	{
		RequestClose();
		return;
	}

	if (GetState() != SessionState::Connected)
		return;

	if (!recvRing.Write(recvContext.buffer, static_cast<int>(bytes)))
	{
		std::cout << "[Session] RecvRing overflow, closing session\n";
		RequestClose();
		return;
	}

	while (true)
	{
		if (!recvRing.Has(sizeof(PacketHeader)))
			break;

		PacketHeader header{};
		if (!recvRing.Peek(&header, sizeof(header)))
			break;

		if (header.size < sizeof(PacketHeader) || header.size > MAX_PACKET_SIZE)
		{
			std::cout << "[Session] Invalid packet size=" << header.size
				<< " session=" << this << "\n";
			RequestClose();
			return;
		}

		if (!recvRing.Has(header.size))
			break;

		recvRing.Skip(sizeof(PacketHeader));
		const int bodySize = static_cast<int>(header.size - sizeof(PacketHeader));

		std::vector<char> body(bodySize);
		if (bodySize > 0 && !recvRing.Peek(body.data(), bodySize))
		{
			RequestClose();
			return;
		}

		Packet pkt{};
		pkt.header = header;
		pkt.body = bodySize > 0 ? body.data() : nullptr;

		ProcessPacket(this, pkt);
		recvRing.Skip(bodySize);
	}

	PostRecv();
}

void Session::OnSendComplete(SendContext* context, DWORD bytes, bool success)
{
	pendingIO.fetch_sub(1, std::memory_order_acq_rel);

	#if defined(_DEBUG)
	const ULONG intended = context ? context->wsabuf.len : 0;
	std::cout << "[IOCP][SendComplete] session=" << this
		<< " sock=" << (int)sock
		<< " bytes=" << bytes
		<< " intended=" << intended
		<< " success=" << success
		<< " pendingIO=" << pendingIO.load()
		<< " ctx=" << context
		<< " tid=" << GetCurrentThreadId()
		<< "\n";
	#endif

	delete context;

	if (!success || bytes == 0)
		RequestClose();
}

void Session::RequestClose()
{
	SessionState expected = SessionState::Connected;
	if (!state.compare_exchange_strong(expected, SessionState::Closing, std::memory_order_acq_rel))
		return;

	std::cout << "[IOCP][Disconnect] session=" << this
		<< " sock=" << (int)sock
		<< " pendingIO=" << pendingIO.load()
		<< " state=Closing\n";

	Close();
	OnSessionDisconnected(this);
}

void Session::SendPacket(uint16_t id, const void* data, uint16_t dataSize)
{
	if (id != PKT_SC_SHUTDOWN)
	{
		if (GetState() != SessionState::Connected)
			return;

		if (GameServer::Instance().IsShuttingDown())
			return;
	}

	const uint32_t packetSize32 = static_cast<uint32_t>(sizeof(PacketHeader)) + dataSize;
	if (packetSize32 < sizeof(PacketHeader) || packetSize32 > MAX_PACKET_SIZE)
	{
		std::cout << "[SendPacket] invalid packet size id=" << id
			<< " size=" << packetSize32 << "\n";
		return;
	}

	if (dataSize > 0 && data == nullptr)
	{
		std::cout << "[SendPacket] null body id=" << id
			<< " size=" << dataSize << "\n";
		return;
	}

	const uint16_t packetSize = static_cast<uint16_t>(packetSize32);
	std::vector<char> packet(packetSize);

	PacketHeader header = { packetSize, id };
	memcpy(packet.data(), &header, sizeof(header));
	if (dataSize > 0)
		memcpy(packet.data() + sizeof(header), data, dataSize);

	PostSend(packet.data(), static_cast<int>(packet.size()));
}

bool Session::TryBeginDestroy()
{
	bool expected = false;
	return destroying.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}

void Session::Close()
{
	bool expected = false;
	if (!closeIssued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		return;

	SOCKET oldSock = sock;
	if (oldSock != INVALID_SOCKET)
	{
		shutdown(oldSock, SD_BOTH);
		closesocket(oldSock);
		sock = INVALID_SOCKET;
	}

	state.store(SessionState::Closed, std::memory_order_release);

	std::cout << "[IOCP][Close] session=" << this
		<< " sock=" << (int)oldSock
		<< " pendingIO=" << pendingIO.load()
		<< " state=Closed\n";
}

SOCKET& Session::GetSocket()
{
	return sock;
}
