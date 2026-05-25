#pragma once
#include <cstdint>

class GameServer;
class Player;
class Session;
struct Packet;

enum class PlayerState : uint8_t;

/* 
GameServer 扁瓷 格废
Room 立辟
Player 包府
Broadcast
Job 积己 
*/
using FSMHandler = void(*)(GameServer&, Session*, Player*, const Packet&);

void InitFSM();
FSMHandler GetHandler(PlayerState st, uint16_t packetId);