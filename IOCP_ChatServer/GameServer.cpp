#include "GameServer.h"
#include "Packet.h"
#include "Session.h"
#include "Player.h"
#include "Network.h"
#include <iostream>

// Singleton Instance
GameServer& GameServer::Instance()
{
	static GameServer instance;
	return instance;
}

// 공용 진입점: Protocol이 여기로 호출함
void GameServer::OnPacket(Session* s, const Packet& pkt)
{
	switch (pkt.header.id)
	{
	case PKT_CS_LOGIN:
		HandleLogin(s, pkt);
		break;

	case PKT_CS_CHAT:
		HandleChat(s, pkt);
		break;

	case PKT_CS_ENTER_ROOM:
		HandleEnterRoom(s, pkt);
		break;

	default:
		break;
	}
}

void GameServer::HandleEnterRoom(Session* s, const Packet& pkt)
{
	Player* p = players[s];
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

	Player* p = players[s];
	if (p == nullptr)
		return;

	p->SetName(name);
	
	std::cout << "[GameServer] Login: " << name << "\n";
}

// 채팅 처리
void GameServer::HandleChat(Session* s, const Packet& pkt)
{
	Player* p = players[s];
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

		LeaveRoom(p);
		players.erase(it);
		delete p;

		
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
	players.emplace(s, p);

	std::cout << "[GameServer] Player connected (session = " << s << ")\n";
}

void GameServer::EnqueuePacketJob(Session* s, const Packet& pkt)
{
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
	GameJob job;
	job.type = GameJobType::Connect;
	job.session = s;
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
		GameJob job = jobQueue.Pop();

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
