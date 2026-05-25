#pragma once
#include <Winsock2.h>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <vector>

#include "RingBuffer.h"

constexpr size_t BUFFER_SIZE = 4096;

class Session;

enum class SessionState
{
	Connected,
	Closing,
	Closed
};

enum class IoType
{
	Recv,
	Send
};

struct IocpContext
{
	OVERLAPPED overlapped;
	IoType type;

	explicit IocpContext(IoType t) : type(t)
	{
		ZeroMemory(&overlapped, sizeof(overlapped));
	}
};

struct RecvContext : IocpContext
{
	WSABUF wsabuf;
	char buffer[BUFFER_SIZE];

	RecvContext() : IocpContext(IoType::Recv)
	{
		ZeroMemory(buffer, sizeof(buffer));
		wsabuf.buf = buffer;
		wsabuf.len = BUFFER_SIZE;
	}
};

struct SendContext : IocpContext
{
	WSABUF wsabuf;
	std::vector<char> buffer;

	SendContext(const char* data, int len)
		: IocpContext(IoType::Send), buffer(data, data + len)
	{
		wsabuf.buf = buffer.data();
		wsabuf.len = static_cast<ULONG>(buffer.size());
	}
};

class Session
{
private:
	SOCKET sock;

	std::atomic<SessionState> state{ SessionState::Connected };
	std::atomic<bool> gameCleanupDone{ false };
	std::atomic<long> pendingIO{ 0 };
	std::atomic<bool> destroying{ false };
	std::atomic<bool> recvPosted{ false };
	std::atomic<bool> closeIssued{ false };

	RecvContext recvContext;
	RingBuffer recvRing;

	void PostSend(const char* data, int len);

public:
	explicit Session(SOCKET s);

	void PostRecv();

	void OnRecvComplete(DWORD bytes, bool success);
	void OnSendComplete(SendContext* context, DWORD bytes, bool success);

	void RequestClose();
	void Close();

	void SendPacket(uint16_t id, const void* data, uint16_t dataSize);

	bool TryBeginDestroy();

	SessionState GetState() const { return state.load(std::memory_order_acquire); }
	long GetPendingIO() const { return pendingIO.load(std::memory_order_acquire); }
	bool IsGamecleanupDone() const { return gameCleanupDone.load(std::memory_order_acquire); }

	void MarkGameCleanupDone() { gameCleanupDone.store(true, std::memory_order_release); }

	SOCKET& GetSocket();
};
