#include "Player.h"
#include "Session.h"
#include "Protocol.h"

Player::Player(Session* s) : session(s), room(nullptr), name(""){}

Session* Player::GetSession() const
{
	return session;
}

Room* Player::GetRoom() const
{
	return room;
}

std::string Player::GetName() const
{
	return name;
}

void Player::SetRoom(Room* r)
{
	room = r;
}

void Player::SetName(const std::string& msg)
{
	name = msg;
}

void Player::SendChat(uint32_t senderId, const std::string& senderName, const std::string& msg)
{
	if (!session)
		return;

	const uint16_t nameLen = (uint16_t)senderName.size();
	const uint16_t msgLen = (uint16_t)msg.size();

	// body 구성 : [senderId(4)][nameLen(2)][name][msgLen(2)][msg]
	std::vector<char> body;
	body.resize(sizeof(uint32_t) + sizeof(uint16_t) + nameLen + sizeof(uint16_t) + msgLen);

	size_t offset = 0;
	// 일단은 memcpy를 활용해 순차적으로 vector<char> body에 senderId, senderName, nameLen, name, msgLen, msg를 복사
	std::memcpy(body.data() + offset, &senderId, sizeof(senderId));
	offset += sizeof(senderId);
	std::memcpy(body.data() + offset, &senderName, sizeof(senderName));
	offset += sizeof(senderName);
	if (nameLen > 0)
		std::memcpy(body.data() + offset, &nameLen, nameLen);
	offset += sizeof(nameLen);
	std::memcpy(body.data() + offset, &msg, sizeof(msg));
	offset += sizeof(msg);
	if (msgLen > 0)
		std::memcpy(body.data() + offset, &msgLen, msgLen);

	session->SendPacket(PKT_SC_CHAT, body.data(), static_cast<uint16_t>(body.size()));
}

//과거의 채팅 전송 방식
//void Player::SendChat(const std::string& msg)
//{
//	if (!session)
//		return;
//
//	// SERVER -> CLIENT
//	session->SendPacket(PKT_SC_CHAT, msg.data(), (uint16_t)msg.size());
//}

bool Player::IsLoggedIn() const
{
	return !name.empty();
}