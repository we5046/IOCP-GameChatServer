#include "FSM.h"
#include "Packet.h"   // PACKET_ID
#include "Player.h"
#include "GameServer.h"
#include "Protocol.h"

static constexpr uint16_t MAX_PACKET_ID = PKT_SC_KICK;

static FSMHandler g_FSM[(int)PlayerState::Count][MAX_PACKET_ID + 1] = {};

static void FSM_Login(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnLogin(s, pkt);
    p->SetState(PlayerState::LoggedIn);
}

static void FSM_EnterRoom(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnEnterRoom(s, pkt);
    if (p->GetRoom() != nullptr)
        p->SetState(PlayerState::InRoom);
}

static void FSM_Chat(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnChat(s, pkt);
}

// ---- Bomb Arena 대기실 ----
// 상태 전이는 GameServer 안에서 처리한다. 검증에 실패하면 상태가 바뀌면 안 되기 때문이다.
static void FSM_Hello(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnHello(s, pkt);
}

static void FSM_JoinRoom(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnJoinRoom(s, pkt);
}

static void FSM_Ready(GameServer& gs, Session* s, Player* p, const Packet& pkt)
{
    gs.OnReady(s, pkt);
}

void InitFSM()
{
    // Connected(로그인 전)에서는 LOGIN만 허용
    g_FSM[(int)PlayerState::None][PKT_CS_LOGIN] = FSM_Login;

    // LoggedIn(룸 전)에서는 ENTER_ROOM만 허용 (채팅 금지)
    g_FSM[(int)PlayerState::LoggedIn][PKT_CS_ENTER_ROOM] = FSM_EnterRoom;

    // InRoom에서는 CHAT 가능 (원하면 ENTER_ROOM로 방 이동 허용도 가능)
    g_FSM[(int)PlayerState::InRoom][PKT_CS_CHAT] = FSM_Chat;

    // InRoom에서 ENTER_ROOM 허용 -> 방 이동 지원
    g_FSM[(int)PlayerState::InRoom][PKT_CS_ENTER_ROOM] = FSM_EnterRoom;

    // ---- Bomb Arena 대기실 ----
    // 접속 직후 HELLO 로 프로토콜 버전을 확인한다. 채팅용 LOGIN 과 같은 자리에 둔다.
    g_FSM[(int)PlayerState::None][PKT_CS_HELLO] = FSM_Hello;

    // WELCOME 을 받은 뒤에만 방에 들어갈 수 있다.
    g_FSM[(int)PlayerState::LoggedIn][PKT_CS_JOIN_ROOM] = FSM_JoinRoom;

    // 방 안에서만 준비 상태를 바꿀 수 있다.
    g_FSM[(int)PlayerState::InRoom][PKT_CS_READY] = FSM_Ready;

    // InGame 행은 비워 둔다. 게임 중에는 방 이동도, 준비 변경도 허용하지 않는다.
}

FSMHandler GetHandler(PlayerState st, uint16_t packetId)
{
    if (packetId > MAX_PACKET_ID) return nullptr;
    return g_FSM[(int)st][packetId];
}