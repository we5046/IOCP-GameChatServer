#pragma once
#include "Room.h"
#include "GameJobQueue.h"
#include <WinSock2.h>
#include <string>
#include <unordered_map>

class Session;
class Player;
class Room;
struct Packet;

// Owns Player objects and coordinates Room commands.
class GameServer
{
private:
	std::unordered_map<Session*, Player*> players;
	std::unordered_map<int32_t, std::unique_ptr<Room>> rooms;

	GameJobQueue jobQueue;
	bool running = true;

	void HandleLogin(Session* s, const Packet& pkt);
	void HandleChat(Session* p, const Packet& pkt);
	void HandleEnterRoom(Session* s, const Packet& pkt);

	// Room helpers.
	Room* FindRoom(int32_t roomId);
	Room* GetOrCreateRoom(int32_t roomId);
	void EnterRoom(Player* p, int32_t roomId);
	void LeaveRoom(Player* p);

public:
	static GameServer& Instance();
	void OnPacket(Session* s, const Packet& pkt);
	void OnSessionDisconnected(Session* s);
	void OnSessionConnected(Session* s);

	// Enqueue jobs from network threads to the game thread.
	void EnqueuePacketJob(Session* s, const Packet& pkt);
	void EnqueueDisconnectJob(Session* s);
	void EnqueueConnectJob(Session* s);

	// GameThread entry point.
	static DWORD WINAPI GameThreadEntry(LPVOID lpParam);
	void GameThreadLoop();

	GameServer();
};
