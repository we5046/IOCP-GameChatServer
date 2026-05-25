#pragma once
#include <Winsock2.h>
#include <deque>
#include <vector>

#include "RingBuffer.h"
#include <string>
#include <atomic>
#include <mutex>

constexpr size_t BUFFER_SIZE = 4096;

enum class SessionState
{
	Connected,
	Closing,
	Closed
};

class Session
{
private:
	SOCKET sock;		// USER SOCKET

	std::atomic<SessionState> state{ SessionState::Connected };
	std::atomic<bool> gameCleanupDone{ false }; // GameServer가 Player/Room 정리 끝냈다는 의미;
	std::atomic<long> pendingIO;
	std::mutex sendMutex;
	std::atomic<bool> destroying{ false };

	// "등록 여부"를 보장하는 플래그 (중요)
	std::atomic<bool> recvPosted{ false };
	std::atomic<bool> sendPosted{ false };

	// ---- RECV ----
	WSAOVERLAPPED recvOverlapped;	// recvI/O용 OVERLAPPED
	WSABUF recvWsabuf;				// .buf: recvBuffer 포인터, .len: 길이
	char recvBuffer[BUFFER_SIZE];	// 커널 -> 유저 공간으로 1차로 받아 놓는 임시 버퍼
	RingBuffer recvRing;			// TCP 스트림을 쌓아두고 패킷 단위로 잘라내는 링버퍼

	// ---- SEND ----
	WSAOVERLAPPED sendOverlapped;	// sendI/O용 OVERLAPPED
	WSABUF sendWsabuf;				// 현재 보내는 패킷의 data, len 임시 담는 용도, 초기값 세팅 필요 X -> 보내기 직전에 PostSend 함수에서 설정
	std::deque<std::vector<char>> sendQueue;		// 보낼 메시지를 쌓아두는 FIFO, 실제 WSASend의 대상이 되는 버퍼들

	size_t sendOffset = 0; // sendQueue.front()의 부분 전송 오프셋(sendMutex로 보호)


public:

	Session(SOCKET s);

	//IO 등록
	void PostRecv();
	void PostSend();

	// IO 완료 콜백 (To WorkerThread)
	void OnRecvComplete(DWORD bytes);
	void OnSendComplete(DWORD bytes);
	

	
	// 논리 종료 요청 (수정1)
	void RequestClose(); // isClosing = true 등

	// 실제 소켓 종료(물리적인 close)
	void Close();

	// 패킷 전송
	void SendPacket(uint16_t id, const void* data, uint16_t dataSize);

	bool TryBeginDestroy();

	// 상태/정보 조회
	SessionState GetState() const { return state.load(std::memory_order_acquire); }
	long GetPendingIO() const { return pendingIO.load(std::memory_order_acquire); }
	bool IsGamecleanupDone() const { return gameCleanupDone.load(); }

	void MarkGameCleanupDone() { gameCleanupDone.store(true); }

	SOCKET& GetSocket();

	// IOCP 구분용
	WSAOVERLAPPED* GetRecvOverlapped();
	WSAOVERLAPPED* GetSendOverlapped(); 
};