#pragma once
#include <cstdint>

class GameServer;
class Player;
class Session;
struct Packet;

enum class PlayerState : uint8_t;

/* 
GameServer 기능 목록
Room 접근
Player 관리
Broadcast
Job 생성 
*/
using FSMHandler = void(*)(GameServer&, Session*, Player*, const Packet&);

void InitFSM();
FSMHandler GetHandler(PlayerState st, uint16_t packetId);