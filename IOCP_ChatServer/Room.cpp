#include "Room.h"
#include "Player.h"
#include "Session.h"
#include <iostream>
#include "Packet.h"

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

	const uint32_t senderId = sender->GetId();
	const std::string name = sender->GetName();

	// 바디 오버헤드: senderId(4) + nameLen(2) + name + msgLen(2)
	const size_t headerSize = sizeof(PacketHeader);
	// msg 내용을 제외한 나머지 부분의 크기 계산
	const size_t overhead = sizeof(uint32_t) + sizeof(uint16_t) + name.size() + sizeof(uint16_t);

	if(headerSize + overhead >= MAX_PACKET_SIZE)
		return;

	const size_t maxMsgLen = (size_t)MAX_PACKET_SIZE - headerSize - overhead;
	
	std::string clipped = msg;
	if (clipped.size() > maxMsgLen)
	{
		clipped.resize(maxMsgLen);
	}

	for (Player* target : players)
	{
		if (!target || target == sender) continue;
		target->SendChat(senderId, name, clipped);
	}
}


//void Room::BroadcastChat(Player* sender, const std::string& msg)
//{
//	if (!sender || sender->GetRoom() == nullptr)
//		return;
//
//	const std::string name = sender->GetName();
//
//	std::string prefix = name + ": ";
//
//	const size_t maxBody = (size_t)MAX_PACKET_SIZE - sizeof(PacketHeader);
//	if (prefix.size() >= maxBody)
//		return;
//
//	std::string clipped = msg;
//	const size_t maxMsgLen = maxBody - prefix.size();
//	if(clipped.size() > maxMsgLen)
//	{
//		clipped.resize(maxMsgLen);
//	}
//
//	const std::string fullMsg = prefix + clipped;
//
//	for (Player* target : players)
//	{
//		if (!target || target == sender) continue;
//		target->SendChat(fullMsg);
//	}
//}
