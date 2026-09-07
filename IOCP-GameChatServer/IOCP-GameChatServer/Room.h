#pragma once
#include <vector>
#include <string>
#include <cstdint>
class Player;

// Room은 Player를 delete 하지 않는다.
// Room은 Player의 목록만 관리
// 네트워크, 세션에 대해선 알지 못함.
class Room
{
private:
	uint16_t roomId = 0;
	std::vector<Player*> players;

	// 0 = 인원 제한 없음(채팅 방). 게임 방은 2로 설정한다.
	uint8_t capacity = 0;
	bool gameStarted = false;

public:
	Room() = default;
	explicit Room(uint16_t id) : roomId(id){}

	uint16_t GetId() const { return roomId; }
	void SetId(uint16_t id) { roomId = id; }

	void Join(Player* p);
	void Leave(Player* p);
	void BroadcastChat(Player* sender, const std::string& msg);

	// 대기실 구간에서 필요한 접근자
	const std::vector<Player*>& GetPlayers() const { return players; }
	size_t Count() const { return players.size(); }

	uint8_t GetCapacity() const { return capacity; }
	void SetCapacity(uint8_t c) { capacity = c; }
	bool IsFull() const { return capacity != 0 && players.size() >= static_cast<size_t>(capacity); }

	bool IsGameStarted() const { return gameStarted; }
	void SetGameStarted(bool v) { gameStarted = v; }
};