#pragma once
#include <string>
class Session;
class Room;

enum class PlayerState : uint8_t
{
	None,        // 아직 Player 생성 전 (Session만 있음)
	LoggedIn,    // 로그인 완료
	InRoom,       // 로비
	InGame,      // 게임 중
	Closing,     // 종료 처리 중
	Count
};
// Player는 Session을 알지만, 소유하지는 않는다.
// Player는 "게임 의도"만 표현한다. (SendChat)
// 네트워크 세부사항은 알지 못한다.
class Player
{
private:
	Session* session;
	Room* room;
	std::string name;
	PlayerState state = PlayerState::None;

	uint32_t id = 0;
public:
	explicit Player(Session* s);

	uint32_t GetId() const { return id; }

	Session* GetSession() const;
	Room* GetRoom() const;
	std::string GetName() const;
	void SetRoom(Room* r);
	void SetName(const std::string& msg);

	// void SendChat(const std::string& msg);
	// 이제부터는 MSG만 보내지않고, Player의 ID와 이름과 같이 msg를 보내게 해서 Client쪽에서 해당 ID, 이름을 알게해 적용.
	void SendChat(uint32_t senderId, const std::string& senderName, const std::string& msg);
	bool IsLoggedIn() const;

	PlayerState GetState() const { return state; }
	void SetState(PlayerState s) { state = s; }
};