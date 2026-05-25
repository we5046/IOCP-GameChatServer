#pragma once
#include <cstdint>

// 예시 패킷 ID들
enum PACKET_ID : uint16_t
{
	PKT_CS_LOGIN = 1, // 클 -> 서 : 닉네임 설정
	PKT_CS_CHAT = 2, // 클 -> 서 : 채팅 메시지
	PKT_SC_CHAT = 3, // 서 -> 클 : 채팅 수신
	PKT_CS_ENTER_ROOM = 4
};

#pragma pack(push, 1)
struct PktCSEnterRoom
{
	int32_t roomId;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct PacketHeader
{
	uint16_t size;	// 전체 패킷 길이 (header + body)
	uint16_t id;	// 패킷 ID
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Packet
{
	PacketHeader header;
	const char* body;	// 실제 payload, Packet은 데이터를 소유하지 않음. 참조만 한다.

	bool IsValid() const;		// NETWORK DEFENSE

};
#pragma pack(pop)