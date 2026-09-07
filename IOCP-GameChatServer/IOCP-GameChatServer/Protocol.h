#pragma once
#include <cstdint>
#include "Packet.h"
class Session;
struct Packet;

// 예시 패킷 ID들
enum PACKET_ID : uint16_t
{
    PKT_CS_LOGIN = 1, // 클 -> 서 : 닉네임 설정
    PKT_CS_CHAT = 2, // 클 -> 서 : 채팅 메시지
    PKT_SC_CHAT = 3, // 서 -> 클 : 채팅 수신
    PKT_CS_ENTER_ROOM = 4,

    // Bomb Arena - 대기실 구간 (protocol-v1.md)
    PKT_CS_HELLO = 10,       // 클 -> 서 : 프로토콜 버전 확인
    PKT_SC_WELCOME = 11,     // 서 -> 클 : 접속 승인, Player ID 부여
    PKT_CS_JOIN_ROOM = 20,   // 클 -> 서 : 게임 방 입장 (0 = 자동 배정)
    PKT_SC_ROOM_STATE = 21,  // 서 -> 클 : 방 참가자 · 준비 상태
    PKT_CS_READY = 22,       // 클 -> 서 : 준비 완료 토글
    PKT_SC_GAME_START = 30,  // 서 -> 클 : 매치 시작, 맵 · 상수 전달

    PKT_SC_SHUTDOWN = 1000,
    PKT_SC_KICK = 1001,
    PKT_SC_ERROR = 1002,     // 서 -> 클 : 요청 거절
};

// 프로토콜 버전 (CS_HELLO 로 확인)
constexpr uint16_t PROTOCOL_VERSION = 1;

// SC_ERROR 코드
enum PROTOCOL_ERROR : uint16_t
{
    ERR_VERSION_MISMATCH = 1,
    ERR_BAD_FORMAT       = 2,
    ERR_NOT_ALLOWED      = 3,
    ERR_NO_SUCH_ROOM     = 4,
    ERR_ROOM_FULL        = 5,
    ERR_MATCH_ENDED      = 6,
    ERR_BOMB_LIMIT       = 7,
};

bool IsKnownPacketId(uint16_t id);
void ProcessPacket(Session* session, Packet& pkt);
void OnSessionDisconnected(Session* session);