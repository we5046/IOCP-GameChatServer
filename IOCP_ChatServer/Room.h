#pragma once
#include <vector>
#include <string>
class Player;

// Room은 Player를 delete 하지 않는다.
// Room은 Player의 목록만 관리
// 네트워크, 세션에 대해선 알지 못함.
class Room
{
private:
	uint16_t roomId = 0;
	std::vector<Player*> players;

public:
	Room() = default;
	explicit Room(uint16_t id) : roomId(id){}

	uint16_t GetId() const { return roomId; }
	void SetId(uint16_t id) { roomId = id; }

	void Join(Player* p);
	void Leave(Player* p);
	void BroadcastChat(Player* sender, const std::string& msg);
};