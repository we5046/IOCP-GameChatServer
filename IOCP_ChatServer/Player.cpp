#include "Player.h"
#include "Session.h"
#include "Packet.h"

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

void Player::SetName(std::string& msg)
{
	name = msg;
}

void Player::SendChat(const std::string& msg)
{
	if (!session)
		return;

	// SERVER -> CLIENT
	session->SendPacket(PKT_SC_CHAT, msg.data(), (uint16_t)msg.size());
}

bool Player::IsLoggedIn() const
{
	return !name.empty();
}