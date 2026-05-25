#pragma once
#include <cstdint>
#include "Packet.h"
class Session;
struct Packet;

// 예시 패킷 ID들
enum PACKET_ID : uint16_t
{
    PKT_CS_LOGIN = 1, // 클 -> 서 : 닉네임 설정
    PKT_CS_CHAT = 2, // 클 -> 서 : 채팅 메시지
    PKT_SC_CHAT = 3, // 서 -> 클 : 채팅 수신
    PKT_CS_ENTER_ROOM = 4,

    PKT_SC_SHUTDOWN = 1000,
    PKT_SC_KICK = 1001,
};

bool IsKnownPacketId(uint16_t id);
void ProcessPacket(Session* session, Packet& pkt);
void OnSessionDisconnected(Session* session);