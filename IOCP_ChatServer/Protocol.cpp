#include "Protocol.h"
#include "Packet.h"
#include "Session.h"
#include "GameServer.h"

bool IsKnownPacketId(uint16_t id)
{
	switch (id)
	{
	case PKT_CS_LOGIN:
	case PKT_CS_CHAT:
	case PKT_SC_CHAT:
	case PKT_CS_ENTER_ROOM:
		return true;
	default:
		return false;
	}
}


// IOCP 네트워크와 게임서버의 어댑터 역할, 네트워크 디펜스 레이어, 게임 로직을 보호하기 위한 함수
void ProcessPacket(Session* session, Packet& pkt)
{
	/*
		- 패킷 ID 유효한가?
		- body size 합리적인가?
		- session이 closing 상태인가?
		- 너무 자주 오는가? (rate limit)
	*/

	// 1) 최소한의 네트워크 검증
	if (session->GetState() == SessionState::Closing)
		return;

	if (!pkt.IsValid())
	{
		session->RequestClose();
		return;
	}

	if (!IsKnownPacketId(pkt.header.id))
	{
		session->RequestClose();
		return;
	}

	// 3) 여기서부터 바로 게임 로직을 부르지 않고 GameServer의 JobQueue에 넣기
	GameServer::Instance().EnqueuePacketJob(session, pkt);
}

void OnSessionDisconnected(Session* session)
{
	// GameServer에게 전달
	GameServer::Instance().EnqueueDisconnectJob(session);
}
