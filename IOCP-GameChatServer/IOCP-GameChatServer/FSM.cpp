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

void InitFSM()
{
    // Connected(로그인 전)에서는 LOGIN만 허용
    g_FSM[(int)PlayerState::None][PKT_CS_LOGIN] = FSM_Login;

    // LoggedIn(룸 전)에서는 ENTER_ROOM만 허용 (채팅 금지)
    g_FSM[(int)PlayerState::LoggedIn][PKT_CS_ENTER_ROOM] = FSM_EnterRoom;

    // InRoom에서는 CHAT 가능 (원하면 ENTER_ROOM로 방 이동 허용도 가능)
    g_FSM[(int)PlayerState::InRoom][PKT_CS_CHAT] = FSM_Chat;

    // (선택) InRoom에서 ENTER_ROOM 허용해서 방 이동 지원
    // g_FSM[(int)PlayerState::InRoom][PKT_CS_ENTER_ROOM] = FSM_EnterRoom;
}

FSMHandler GetHandler(PlayerState st, uint16_t packetId)
{
    if (packetId > MAX_PACKET_ID) return nullptr;
    return g_FSM[(int)st][packetId];
}