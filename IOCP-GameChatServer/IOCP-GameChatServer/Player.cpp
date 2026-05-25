#include "Player.h"

#include <cstring>
#include <vector>

#include "Packet.h"
#include "Protocol.h"
#include "Session.h"

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

	const size_t bodySize = sizeof(uint32_t) + sizeof(uint16_t) + senderName.size() + sizeof(uint16_t) + msg.size();
	if (bodySize + sizeof(PacketHeader) > MAX_PACKET_SIZE)
		return;

	const uint16_t nameLen = static_cast<uint16_t>(senderName.size());
	const uint16_t msgLen = static_cast<uint16_t>(msg.size());

	std::vector<char> body(bodySize);
	size_t offset = 0;

	std::memcpy(body.data() + offset, &senderId, sizeof(senderId));
	offset += sizeof(senderId);

	std::memcpy(body.data() + offset, &nameLen, sizeof(nameLen));
	offset += sizeof(nameLen);

	if (nameLen > 0)
	{
		std::memcpy(body.data() + offset, senderName.data(), nameLen);
		offset += nameLen;
	}

	std::memcpy(body.data() + offset, &msgLen, sizeof(msgLen));
	offset += sizeof(msgLen);

	if (msgLen > 0)
		std::memcpy(body.data() + offset, msg.data(), msgLen);

	session->SendPacket(PKT_SC_CHAT, body.data(), static_cast<uint16_t>(body.size()));
}

bool Player::IsLoggedIn() const
{
	return !name.empty();
}
