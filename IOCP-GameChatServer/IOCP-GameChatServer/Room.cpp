#include "Room.h"

#include <iostream>
#include <vector>

#include "Packet.h"
#include "Player.h"
#include "Session.h"

void Room::Join(Player* p)
{
	if (!p)
		return;

	for (Player* current : players)
	{
		if (current == p)
			return;
	}

	players.push_back(p);
	p->SetRoom(this);

	std::cout << "[Room " << roomId << "] Join: " << p->GetName() << "\n";
}

void Room::Leave(Player* p)
{
	if (!p)
		return;

	for (auto it = players.begin(); it != players.end(); ++it)
	{
		if (*it == p)
		{
			players.erase(it);
			p->SetRoom(nullptr);

			std::cout << "[Room " << roomId << "] Leave: " << p->GetName() << "\n";
			return;
		}
	}
}

void Room::BroadcastChat(Player* sender, const std::string& msg)
{
	if (!sender || sender->GetRoom() == nullptr)
		return;

	const uint32_t senderId = sender->GetId();
	const std::string name = sender->GetName();

	const size_t headerSize = sizeof(PacketHeader);
	const size_t overhead = sizeof(uint32_t) + sizeof(uint16_t) + name.size() + sizeof(uint16_t);

	if (headerSize + overhead >= MAX_PACKET_SIZE)
		return;

	const size_t maxMsgLen = static_cast<size_t>(MAX_PACKET_SIZE) - headerSize - overhead;

	std::string clipped = msg;
	if (clipped.size() > maxMsgLen)
		clipped.resize(maxMsgLen);

	std::vector<Player*> targets = players;
	for (Player* target : targets)
	{
		if (!target || target == sender)
			continue;

		target->SendChat(senderId, name, clipped);
	}
}
