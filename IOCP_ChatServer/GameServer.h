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

enum class ServerState
{
	Running,
	ShuttingDown,
	Stopped
};

class GameServer
{
private:
	std::atomic<ServerState> state;

	std::unordered_map<Session*, Player*> players;
	std::unordered_map<int32_t, std::unique_ptr<Room>> rooms;

	GameJobQueue jobQueue;
	bool running = true;

	void HandleLogin(Session* s, const Packet& pkt);
	void HandleChat(Player* p, const Packet& pkt);
	void HandleEnterRoom(Session* s, const Packet& pkt);

	// 방 관련 헬퍼 함수
	Room* FindRoom(int32_t roomId);
	Room* GetOrCreateRoom(int32_t roomId);
	void EnterRoom(Player* p, int32_t roomId);
	void LeaveRoom(Player* p);

public:
	static GameServer& Instance();
	void OnPacket(Session* s, const Packet& pkt);
	void OnSessionDisconnected(Session* s);
	void OnSessionConnected(Session* s);
	//new
	void EnqueuePacketJob(Session* s, const Packet& pkt);
	void EnqueueDisconnectJob(Session* s);
	void EnqueueConnectJob(Session* s);
	void EnqueueShutdownJob();

	// gameThread
	static DWORD WINAPI GameThreadEntry(LPVOID lpParam);
	void GameThreadLoop();

	// 생성자에서 기본 로비 같은 것도 만들어 줄 수 있음
	GameServer();
	void Init();

	//GameServer State
	void Shutdown();
	Player* GetPlayer(Session* s);
	bool IsShuttingDown() const { return state.load() == ServerState::ShuttingDown; }


	// 고민중인 함수들
	void OnLogin(Session* s, const Packet& pkt);
	void OnEnterRoom(Session* s, const Packet& pkt);
	void OnChat(Session* s, const Packet& pkt);
};