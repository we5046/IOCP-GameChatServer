#include "Room.h"
#include "Player.h"
#include "Session.h"
#include <iostream>

void Room::Join(Player* p)
{
	if (!p)
		return;

	// 중복 추가 방지
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

	const std::string name = sender->GetName();

	std::string fullMsg = name + ": " + msg;

	for (Player* target : players)
	{
		if (!target)
			continue;

		if (target == sender)
			continue;

		if (target->GetRoom()->GetId() != sender->GetRoom()->GetId())
			continue;

		target->SendChat(fullMsg);
	}
}
