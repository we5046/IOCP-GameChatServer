#pragma once
#include <cstdint>

// Packet ID list.
enum PACKET_ID : uint16_t
{
	PKT_CS_LOGIN = 1, // client -> server: set nickname
	PKT_CS_CHAT = 2, // client -> server: chat message
	PKT_SC_CHAT = 3, // server -> client: chat message
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
	uint16_t size;	// Total packet size: header + body.
	uint16_t id;	// Packet ID.
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Packet
{
	PacketHeader header;
	const char* body;	// Payload view. Packet does not own the data.

	bool IsValid() const;
};
#pragma pack(pop)
