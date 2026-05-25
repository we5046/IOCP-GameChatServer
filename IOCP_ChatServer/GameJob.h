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

	// Used only for Packet jobs.
	PacketHeader header;
	std::vector<char> body;		// body.size() == header.size - sizeof(PacketHeader)
};
