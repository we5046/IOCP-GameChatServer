#pragma once
#include <vector>
#include "Packet.h"

class Session;

enum class GameJobType
{
	Packet,
	Disconnect,
	Connect,
	SessionCleanupDone,
};

struct GameJob
{
	GameJobType type;
	Session* session;

	// Packet일 때만 사용
	PacketHeader header;
	std::vector<char> body;		// body.size() == header.size() + sizeof(PACKETHEADER);
};