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

	// Player ID 발급기. 게임 스레드에서만 접근하므로 atomic 불필요
	uint32_t nextPlayerId = 1;

	// Bomb Arena 대기실. 채팅 방 번호와 겹치지 않도록 100부터 발급한다.
	int32_t nextGameRoomId = 100;
	uint32_t nextMatchId = 1;

	void HandleLogin(Session* s, const Packet& pkt);
	void HandleChat(Player* p, const Packet& pkt);
	void HandleEnterRoom(Session* s, const Packet& pkt);

	// Bomb Arena 대기실 처리
	void HandleHello(Session* s, const Packet& pkt);
	void HandleJoinRoom(Session* s, const Packet& pkt);
	void HandleReady(Session* s, const Packet& pkt);

	void SendError(Session* s, uint16_t rejectedId, uint16_t code);
	void SendRoomState(Session* s, Room* room);
	void BroadcastRoomState(Room* room);
	void TryStartGame(Room* room);
	Room* AcquireGameRoom(int32_t requested);

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

	// Bomb Arena 대기실 진입점 (FSM이 호출)
	void OnHello(Session* s, const Packet& pkt);
	void OnJoinRoom(Session* s, const Packet& pkt);
	void OnReady(Session* s, const Packet& pkt);
};