#include "Packet.h"
#include "Protocol.h"

bool Packet::IsValid() const
{
    if (header.size < sizeof(PacketHeader))
        return false;

    if (header.size > MAX_PACKET_SIZE)
        return false;

    return true;
}