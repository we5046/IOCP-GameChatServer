#pragma once
#include <cstdint>

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

// Maximum allowed packet size (header + body)
constexpr uint16_t MAX_PACKET_SIZE = 4096;