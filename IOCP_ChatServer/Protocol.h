#pragma once
#include <cstdint>
class Session;
struct Packet;

constexpr uint16_t MAX_PACKET_SIZE = 4096;
bool IsKnownPacketId(uint16_t id);
void ProcessPacket(Session* session, Packet& pkt);

void OnSessionDisconnected(Session* session);