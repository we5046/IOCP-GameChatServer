#include "GameServer.h"
#include "Packet.h"
#include "Session.h"
#include "Player.h"
#include "Network.h"
#include <iostream>
#include "FSM.h"
#include "Protocol.h"

// Singleton Instance
GameServer& GameServer::Instance()
{
	static GameServer instance;
	return instance;
}

void GameServer::Init()
{
	state = ServerState::Running;
	InitFSM();
	std::cout << "[GameServer] FSM initialized\n";
}

void GameServer::Shutdown()
{
	ServerState expected = ServerState::Running;
	if(!state.compare_exchange_strong(expected, ServerState::ShuttingDown))
	{
		// 이미 종료중이거나 종료된 상태
		return;
	}

	std::cout << "[GameServer] Shutdown Start\n";

	// 모든 세션에 종료 신호 보내기
	Network::Instance().RequestShutdownAllSessions();

	// GameThread에게 종료 Job 전달
	EnqueueShutdownJob();

	//// WSASend/WSARecv completion 처리 끝날 때까지 대기
	//while (!Network::Instance().AllSessionsIOCompleted())
	//{
	//	Sleep(1);
	//}

	// ★ Network에도 종료 신호 보내기
	Network::Instance().StopWorkerThreads();
}

// Helper: Session으로부터 Player 찾기 함수
Player* GameServer::GetPlayer(Session* s)
{
	auto it = players.find(s);
	if (it == players.end() || it->second == nullptr)
		return nullptr;

	return it->second;
}
// 공용 진입점: Protocol이 여기로 호출함
void GameServer::OnPacket(Session* s, const Packet& pkt)
{
	Player* p = GetPlayer(s);
	if(p == nullptr)
		return;

	FSMHandler handler = GetHandler(p->GetState(), pkt.header.id);
	if (!handler)
	{
		std::cout << "[FSM] blocked: state=" << (int)p->GetState()
			<< " pkt=" << pkt.header.id << "\n";
		return;
	}

	handler(*this, s, p, pkt);
}

void GameServer::HandleEnterRoom(Session* s, const Packet& pkt)
{
	Player* p = GetPlayer(s);
	if (p == nullptr)
		return;

	if (!p->IsLoggedIn())
	{
		std::cout << "[Warning] Login 없이 EnterRoom 패킷 도착! session=" << s << "\n";
		return;
	}

	if (pkt.header.size < sizeof(PacketHeader) + sizeof(int32_t))
	{
		std::cout << "PKT_CS_ENTER_ROOM 패킷이 도착하지 않고 알수없는 패킷 도착\n";
		return;
	}

	int32_t roomId;
	memcpy(&roomId, pkt.body, sizeof(int32_t));

	EnterRoom(p, roomId);

	std::cout << "[GameServer] Player " << p->GetName() << "님이 " << roomId << "에 입장하셨습니다.\n";
}

// 로그인 처리 : SetName 패킷
void GameServer::HandleLogin(Session* s, const Packet& pkt)
{
	const int bodySize = pkt.header.size - sizeof(PacketHeader);
	if (bodySize <= 0)
		return;

	std::string name(pkt.body, pkt.body + bodySize);

	Player* p = GetPlayer(s);
	if (p == nullptr)
		return;

	p->SetName(name);
	
	std::cout << "[GameServer] Login: " << name << "\n";
}

// 채팅 처리
void GameServer::HandleChat(Player* s, const Packet& pkt)
{
	Player* p = s;
	if (!p->IsLoggedIn())
		return;

	Room* room = p->GetRoom();
	if (room == nullptr)
		return;

	const int bodySize = pkt.header.size - sizeof(PacketHeader);
	if (bodySize <= 0)
		return;

	std::string msg(pkt.body, pkt.body + bodySize);

	room->BroadcastChat(p, msg);
}

Room* GameServer::FindRoom(int32_t roomId)
{
	auto it = rooms.find(roomId);
	if (it != rooms.end())
	{
		return it->second.get();
	}
	
	return nullptr;
}

Room* GameServer::GetOrCreateRoom(int32_t roomId)
{
	if (Room* r = FindRoom(roomId))
		return r;

	auto newRoom = std::make_unique<Room>(roomId);
	Room* ptr = newRoom.get();
	rooms.emplace(roomId, std::move(newRoom));
	return ptr;
}

void GameServer::EnterRoom(Player* p, int32_t roomId)
{
	if (!p)
		return;

	// 기존 방에서 빼고
	if (Room* old = p->GetRoom())
	{
		old->Leave(p);
	}

	Room* room = GetOrCreateRoom(roomId);
	room->Join(p);
}

void GameServer::LeaveRoom(Player* p)
{
	if (!p)
		return;

	if (Room* room = p->GetRoom())
	{
		room->Leave(p);
	}
}

// 세션 끊김 처리: Disconnect 이벤트가 들어왔을 때 호출
void GameServer::OnSessionDisconnected(Session* s)
{
	auto it = players.find(s);
	if (it != players.end())
	{
		Player* p = it->second;

		// 방을 먼저 기억해 둔다. LeaveRoom 이 끝나면 p->GetRoom() 은 nullptr 이 된다.
		Room* room = p->GetRoom();

		LeaveRoom(p);
		players.erase(it);
		delete p;

		// 대기실에 남은 사람에게 바뀐 참가자 목록을 알린다.
		if (room != nullptr && room->GetCapacity() != 0)
			BroadcastRoomState(room);

		
		std::cout << "[GameServer] Session disconnected, Player removed\n";
	}
	else
	{
		std::cout << "[GameServer] Session disconnected, but Player not founded\n";
	}

	// 여기서 이제 다시 packet 0을 보내서 종료신호. 혹은 SYN-ACK 패킷을 보내는 거임 서버로
	s->MarkGameCleanupDone();

	// WorkerThread 깨우기
	PostQueuedCompletionStatus(
		Network::Instance().GetIocpHandle(),
		0,
		reinterpret_cast<ULONG_PTR>(s),
		nullptr   // overlapped == nullptr → “cleanup check”
	);
}

void GameServer::OnSessionConnected(Session* s)
{
	Player* p = new Player(s);
	p->SetId(nextPlayerId++);
	players.emplace(s, p);

	std::cout << "[GameServer] Player connected (id = " << p->GetId()
		<< ", session = " << s << ")\n";
}

void GameServer::EnqueuePacketJob(Session* s, const Packet& pkt)
{
	if (IsShuttingDown())
	{
		// 서버 종료 중에는 패킷 무시
		std::cout << "Session[" << s << "] tried to send packet during shutdown. Ignored.\n";
		return;
	}

	GameJob job;
	job.type = GameJobType::Packet;
	job.session = s;
	job.header = pkt.header;

	const int bodySize = pkt.header.size - sizeof(PacketHeader);
	if (bodySize > 0)
	{
		job.body.resize(bodySize);
		memcpy(job.body.data(), pkt.body, bodySize);
	}

	jobQueue.Push(job);
}

void GameServer::EnqueueDisconnectJob(Session* s)
{
	GameJob job;
	job.type = GameJobType::Disconnect;
	job.session = s;
	jobQueue.Push(job);
}

void GameServer::EnqueueConnectJob(Session* s)
{
	if (IsShuttingDown())
	{
		// 서버 종료 중에는 패킷 무시
		std::cout << "Session[" << s << "] tried to send packet during shutdown. Ignored.\n";
		return;
	}

	GameJob job;
	job.type = GameJobType::Connect;
	job.session = s;

	jobQueue.Push(job);
}

void GameServer::EnqueueShutdownJob()
{
	GameJob job;
	job.type = GameJobType::Shutdown;
	job.session = nullptr;
	jobQueue.Push(job);
}

DWORD WINAPI GameServer::GameThreadEntry(LPVOID lpParam)
{
	GameServer::Instance().GameThreadLoop();
	return 0;
}

void GameServer::GameThreadLoop()
{
	while (running)
	{
		GameJob job;
		if(jobQueue.Pop(job) == false)
		{
			// 큐가 비어있음, 잠시 대기
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		switch (job.type)
		{
		case GameJobType::Packet:
		{
			// Packet 구조체 하나를 임시로 만들어서 OnPacket에 넘긴다.
			Packet pkt;
			pkt.header = job.header;
			pkt.body = job.body.data();

			OnPacket(job.session, pkt);
			break;
		}
		case GameJobType::Disconnect:
			OnSessionDisconnected(job.session);
			break;
		
		case GameJobType::Connect:
			OnSessionConnected(job.session);
			break;
		case GameJobType::Shutdown:
		{
			std::cout << "[GameThread] Shutdown job received\n";
			running = false;
			break;
		}
		}
	}
}

GameServer::GameServer() : running(true)
{
	// 1번방을 로비로 사용
	// C++14 문법
	auto lobby = std::make_unique<Room>(1);
	rooms.emplace(1, std::move(lobby));
}

void GameServer::OnLogin(Session* s, const Packet& pkt)
{
	HandleLogin(s, pkt);
}

void GameServer::OnEnterRoom(Session* s, const Packet& pkt)
{
	HandleEnterRoom(s, pkt);
}

void GameServer::OnChat(Session* s, const Packet& pkt)
{
	auto it = players.find(s);
	if (it == players.end() || it->second == nullptr)
		return;

	HandleChat(it->second, pkt);
}

// ============================================================================
//  Bomb Arena - 대기실 구간 (protocol-v1.md 의 10 / 11 / 20 / 21 / 22 / 30 / 1002)
//
//  게임 진행 패킷(31~34)은 아직 구현하지 않았다. 여기까지는 클라이언트가
//  "접속 -> 승인 -> 방 입장 -> 준비 -> 게임 시작 알림" 을 끝낼 수 있다.
// ============================================================================

namespace
{
	// 게임 상수. protocol-v1.md 의 값을 그대로 쓴다.
	constexpr uint16_t TICK_RATE   = 20;
	constexpr uint16_t TILE_UNIT   = 1000;
	constexpr uint8_t  MAP_WIDTH   = 15;
	constexpr uint8_t  MAP_HEIGHT  = 13;
	constexpr uint8_t  GAME_ROOM_CAPACITY = 2;

	// 규약이 정한 직렬화. 구조체 패딩에 기대지 않고 필드를 하나씩 쓴다.
	// 모든 정수는 little-endian 이다.
	struct Writer
	{
		std::vector<char> buf;

		void U8(uint8_t v) { buf.push_back(static_cast<char>(v)); }

		void U16(uint16_t v)
		{
			buf.push_back(static_cast<char>(v & 0xFF));
			buf.push_back(static_cast<char>((v >> 8) & 0xFF));
		}

		void U32(uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
				buf.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
		}

		void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }

		void Str(const std::string& v)
		{
			U16(static_cast<uint16_t>(v.size()));
			buf.insert(buf.end(), v.begin(), v.end());
		}

		uint16_t Size() const { return static_cast<uint16_t>(buf.size()); }
		const char* Data() const { return buf.data(); }
	};

	// 외곽 한 줄은 전부 벽, 내부에서 x 와 y 가 모두 짝수인 타일이 벽이다.
	// 시작 위치 (1,1) 과 (13,11) 은 둘 다 홀수/홀수라 빈칸이 된다.
	bool IsWallTile(int x, int y)
	{
		if (x == 0 || y == 0 || x == MAP_WIDTH - 1 || y == MAP_HEIGHT - 1)
			return true;
		return (x % 2 == 0) && (y % 2 == 0);
	}

	// 타일 (tx,ty) 의 중심 좌표. 타일 한 칸이 TILE_UNIT 이다.
	int32_t TileCenter(int t) { return t * TILE_UNIT + TILE_UNIT / 2; }

	// 20Hz 기준 서버 틱. 클라이언트가 시간 기준을 맞추는 용도로만 쓴다.
	uint32_t CurrentTick()
	{
		return static_cast<uint32_t>(GetTickCount64() / (1000 / TICK_RATE));
	}
}

void GameServer::SendError(Session* s, uint16_t rejectedId, uint16_t code)
{
	if (s == nullptr)
		return;

	Writer w;
	w.U16(rejectedId);
	w.U16(code);
	s->SendPacket(PKT_SC_ERROR, w.Data(), w.Size());

	std::cout << "[GameServer] SC_ERROR rejected=" << rejectedId
		<< " code=" << code << " session=" << s << "\n";
}

// ---------------------------------------------------------------- CS_HELLO
void GameServer::HandleHello(Session* s, const Packet& pkt)
{
	Player* p = GetPlayer(s);
	if (p == nullptr)
		return;

	const int bodySize = pkt.header.size - static_cast<int>(sizeof(PacketHeader));
	if (bodySize != static_cast<int>(sizeof(uint16_t)))
	{
		SendError(s, PKT_CS_HELLO, ERR_BAD_FORMAT);
		s->RequestClose();
		return;
	}

	uint16_t version = 0;
	memcpy(&version, pkt.body, sizeof(version));

	if (version != PROTOCOL_VERSION)
	{
		SendError(s, PKT_CS_HELLO, ERR_VERSION_MISMATCH);
		s->RequestClose();
		std::cout << "[GameServer] HELLO 버전 불일치: " << version
			<< " (서버 " << PROTOCOL_VERSION << ")\n";
		return;
	}

	// 게임 클라이언트에는 닉네임 입력이 없다. ID로 표시 이름을 만든다.
	if (p->GetName().empty())
		p->SetName("Player " + std::to_string(p->GetId()));

	Writer w;
	w.U8(0);                 // result : 0 = 성공
	w.U32(p->GetId());       // playerId
	w.U32(CurrentTick());    // serverTick
	s->SendPacket(PKT_SC_WELCOME, w.Data(), w.Size());

	// 검증을 통과한 뒤에만 상태를 옮긴다.
	p->SetState(PlayerState::LoggedIn);

	std::cout << "[GameServer] WELCOME id=" << p->GetId()
		<< " name=" << p->GetName() << "\n";
}

// ------------------------------------------------------------ 게임 방 확보
// requested 가 0 이면 자리가 남은 방을 찾고, 없으면 새로 만든다.
Room* GameServer::AcquireGameRoom(int32_t requested)
{
	if (requested == 0)
	{
		for (auto& kv : rooms)
		{
			Room* r = kv.second.get();
			if (r == nullptr)
				continue;
			// 게임 방이면서, 시작 전이고, 자리가 남은 방
			if (r->GetCapacity() != 0 && !r->IsGameStarted() && !r->IsFull())
				return r;
		}

		const int32_t id = nextGameRoomId++;
		auto created = std::make_unique<Room>(static_cast<uint16_t>(id));
		created->SetCapacity(GAME_ROOM_CAPACITY);
		Room* ptr = created.get();
		rooms.emplace(id, std::move(created));
		return ptr;
	}

	Room* r = FindRoom(requested);
	if (r == nullptr)
	{
		auto created = std::make_unique<Room>(static_cast<uint16_t>(requested));
		created->SetCapacity(GAME_ROOM_CAPACITY);
		Room* ptr = created.get();
		rooms.emplace(requested, std::move(created));
		return ptr;
	}

	// 이미 채팅으로 쓰이는 방이면 게임 방으로 바꾸지 않는다.
	// 채팅 중인 사람들의 방에 정원 2를 씌우면 그쪽이 망가진다.
	if (r->GetCapacity() == 0)
	{
		if (r->Count() > 0)
			return nullptr;
		r->SetCapacity(GAME_ROOM_CAPACITY);
	}

	return r;
}

// ----------------------------------------------------------- CS_JOIN_ROOM
void GameServer::HandleJoinRoom(Session* s, const Packet& pkt)
{
	Player* p = GetPlayer(s);
	if (p == nullptr)
		return;

	const int bodySize = pkt.header.size - static_cast<int>(sizeof(PacketHeader));
	if (bodySize != static_cast<int>(sizeof(int32_t)))
	{
		SendError(s, PKT_CS_JOIN_ROOM, ERR_BAD_FORMAT);
		return;
	}

	int32_t requested = 0;
	memcpy(&requested, pkt.body, sizeof(requested));

	if (requested < 0)
	{
		SendError(s, PKT_CS_JOIN_ROOM, ERR_NO_SUCH_ROOM);
		return;
	}

	Room* room = AcquireGameRoom(requested);
	if (room == nullptr)
	{
		SendError(s, PKT_CS_JOIN_ROOM, ERR_NO_SUCH_ROOM);
		return;
	}

	if (room->IsGameStarted())
	{
		SendError(s, PKT_CS_JOIN_ROOM, ERR_MATCH_ENDED);
		return;
	}

	if (room->IsFull())
	{
		SendError(s, PKT_CS_JOIN_ROOM, ERR_ROOM_FULL);
		return;
	}

	// 이전 방이 있으면 먼저 나간다.
	if (Room* old = p->GetRoom())
		old->Leave(p);

	p->SetReady(false);
	room->Join(p);
	p->SetState(PlayerState::InRoom);

	// 규약: roomId = 0 으로 요청해도 응답에는 실제 배정된 번호를 담는다.
	BroadcastRoomState(room);
}

// -------------------------------------------------------------- CS_READY
void GameServer::HandleReady(Session* s, const Packet& pkt)
{
	Player* p = GetPlayer(s);
	if (p == nullptr)
		return;

	Room* room = p->GetRoom();
	if (room == nullptr)
	{
		SendError(s, PKT_CS_READY, ERR_NOT_ALLOWED);
		return;
	}

	if (room->IsGameStarted())
	{
		SendError(s, PKT_CS_READY, ERR_MATCH_ENDED);
		return;
	}

	const int bodySize = pkt.header.size - static_cast<int>(sizeof(PacketHeader));
	if (bodySize != static_cast<int>(sizeof(uint8_t)))
	{
		SendError(s, PKT_CS_READY, ERR_BAD_FORMAT);
		return;
	}

	uint8_t flag = 0;
	memcpy(&flag, pkt.body, sizeof(flag));
	p->SetReady(flag != 0);

	std::cout << "[GameServer] READY id=" << p->GetId()
		<< " ready=" << (p->IsReady() ? "true" : "false") << "\n";

	// 준비 상태는 서버 응답으로만 확정된다. 먼저 알리고, 그다음 시작 조건을 본다.
	BroadcastRoomState(room);
	TryStartGame(room);
}

// --------------------------------------------------------- SC_ROOM_STATE
void GameServer::SendRoomState(Session* s, Room* room)
{
	if (s == nullptr || room == nullptr)
		return;

	const std::vector<Player*>& list = room->GetPlayers();

	Writer w;
	w.I32(static_cast<int32_t>(room->GetId()));
	w.U8(room->GetCapacity());
	w.U16(static_cast<uint16_t>(list.size()));

	for (Player* member : list)
	{
		if (member == nullptr)
			continue;
		w.U32(member->GetId());
		w.U8(member->IsReady() ? 1 : 0);
		w.Str(member->GetName());
	}

	s->SendPacket(PKT_SC_ROOM_STATE, w.Data(), w.Size());
}

void GameServer::BroadcastRoomState(Room* room)
{
	if (room == nullptr)
		return;

	// Send 도중 목록이 바뀔 일은 없지만(게임 스레드 단독), 의도를 분명히 하려고 복사한다.
	std::vector<Player*> targets = room->GetPlayers();
	for (Player* member : targets)
	{
		if (member == nullptr)
			continue;
		SendRoomState(member->GetSession(), room);
	}
}

// -------------------------------------------------------- SC_GAME_START
void GameServer::TryStartGame(Room* room)
{
	if (room == nullptr || room->IsGameStarted())
		return;

	if (room->GetCapacity() == 0)
		return;                                  // 채팅 방은 시작하지 않는다

	const std::vector<Player*>& list = room->GetPlayers();
	if (list.size() != static_cast<size_t>(room->GetCapacity()))
		return;                                  // 정원이 차야 시작한다

	for (Player* member : list)
	{
		if (member == nullptr || !member->IsReady())
			return;                              // 전원이 준비해야 시작한다
	}

	const uint32_t matchId = nextMatchId++;

	Writer w;
	w.U32(matchId);
	w.U16(TICK_RATE);
	w.U16(TILE_UNIT);
	w.U8(MAP_WIDTH);
	w.U8(MAP_HEIGHT);

	// tiles : 행 우선. 인덱스 y * MAP_WIDTH + x
	for (int y = 0; y < MAP_HEIGHT; ++y)
		for (int x = 0; x < MAP_WIDTH; ++x)
			w.U8(IsWallTile(x, y) ? 1 : 0);

	// spawns : 방 참가자 전원을 정확히 한 번씩 담는다.
	// 정원 2인 기준으로 (1,1) 과 (13,11) 을 대각으로 배치한다.
	static const int spawnTile[2][2] = { { 1, 1 }, { 13, 11 } };

	w.U16(static_cast<uint16_t>(list.size()));
	for (size_t i = 0; i < list.size(); ++i)
	{
		Player* member = list[i];
		const int slot = static_cast<int>(i % 2);
		w.U32(member->GetId());
		w.I32(TileCenter(spawnTile[slot][0]));
		w.I32(TileCenter(spawnTile[slot][1]));
	}

	room->SetGameStarted(true);

	for (Player* member : list)
	{
		if (member == nullptr)
			continue;
		member->SetState(PlayerState::InGame);
		if (Session* s = member->GetSession())
			s->SendPacket(PKT_SC_GAME_START, w.Data(), w.Size());
	}

	std::cout << "[GameServer] GAME_START match=" << matchId
		<< " room=" << room->GetId()
		<< " players=" << list.size() << "\n";
}

// -------------------------------------------------------------- 진입점
void GameServer::OnHello(Session* s, const Packet& pkt)    { HandleHello(s, pkt); }
void GameServer::OnJoinRoom(Session* s, const Packet& pkt) { HandleJoinRoom(s, pkt); }
void GameServer::OnReady(Session* s, const Packet& pkt)    { HandleReady(s, pkt); }
