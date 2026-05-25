#include "GameServer.h"
#include "Packet.h"
#include "Session.h"
#include "Player.h"
#include "Network.h"
#include <iostream>

// Singleton instance.
GameServer& GameServer::Instance()
{
	static GameServer instance;
	return instance;
}

// Game logic entry point for packets validated by Protocol.
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
		std::cout << "[Warning] EnterRoom packet arrived before login. session=" << s << "\n";
		return;
	}

	if (pkt.header.size < sizeof(PacketHeader) + sizeof(int32_t))
	{
		std::cout << "Invalid PKT_CS_ENTER_ROOM packet\n";
		return;
	}

	int32_t roomId;
	memcpy(&roomId, pkt.body, sizeof(int32_t));

	EnterRoom(p, roomId);

	std::cout << "[GameServer] Player " << p->GetName() << " entered room " << roomId << ".\n";
}

// Login: store the nickname sent by the client.
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

// Chat: broadcast the message to the player's current room.
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

	// Leave the previous room first.
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

// Disconnect handling on the game thread.
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
		std::cout << "[GameServer] Session disconnected, but Player not found\n";
	}

	// Mark that game-side cleanup is complete.
	s->MarkGameCleanupDone();

	// Wake a worker so it can check pending I/O and delete the session if safe.
	PostQueuedCompletionStatus(
		Network::Instance().GetIocpHandle(),
		0,
		reinterpret_cast<ULONG_PTR>(s),
		nullptr   // overlapped == nullptr: cleanup check
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
			// Rebuild a temporary Packet view from copied GameJob data.
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
	// Room 1 is the default lobby.
	auto lobby = std::make_unique<Room>(1);
	rooms.emplace(1, std::move(lobby));
}
