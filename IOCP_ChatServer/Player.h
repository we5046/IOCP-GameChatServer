#pragma once
#include <string>
class Session;
class Room;

// Player는 Session을 알지만, 소유하지는 않는다.
// Player는 "게임 의도"만 표현한다. (SendChat)
// 네트워크 세부사항은 알지 못한다.
class Player
{
private:
	Session* session;
	Room* room;
	std::string name;

public:
	explicit Player(Session* s);

	Session* GetSession() const;
	Room* GetRoom() const;
	std::string GetName() const;
	void SetRoom(Room* r);
	void SetName(std::string& msg);
	void SendChat(const std::string& msg);
	bool IsLoggedIn() const;
};