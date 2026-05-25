# Packet Flow

## Client To Server Flow

```text
Client
  -> TCP send
  -> Server socket
  -> IOCP recv completion
  -> WorkerThread
  -> Session::OnRecvComplete
  -> RingBuffer
  -> PacketHeader validation
  -> Protocol::ProcessPacket
  -> GameServer::EnqueuePacketJob
  -> GameServer::GameThreadLoop
  -> FSM handler
```

The server treats TCP as a byte stream. A single recv may contain a partial packet, exactly one packet, or multiple packets. `RingBuffer` stores incoming bytes until a full packet is available.

## Server To Client Flow

```text
Game logic
  -> Player / Room
  -> Session::SendPacket
  -> sendQueue
  -> Session::PostSend
  -> WSASend
  -> IOCP send completion
  -> Session::OnSendComplete
```

Outgoing packets are copied into a session-local send queue. The session posts one send operation and continues with the next packet when the current send completes.

## Chat Message Flow

```text
Client sends chat packet
  -> Session receives bytes
  -> RingBuffer extracts packet
  -> Protocol validates packet id and size
  -> GameServer receives packet job
  -> FSM checks player state
  -> GameServer handles chat
  -> Room broadcasts message
```

Chat packets are accepted only when the player state allows chat. The FSM layer blocks invalid state transitions such as chatting before login or before entering a room.

## Room Broadcast Flow

```text
Sender Player
  -> current Room
  -> Room::BroadcastChat
  -> each target Player in the room
  -> target Session::SendPacket
```

`Room` does not own players. It stores non-owning player pointers and uses each target player's session to send chat messages.

## Korean Summary

- 클라이언트 패킷은 IOCP 완료, Session, RingBuffer, Protocol, GameServer 순서로 처리됩니다.
- 서버 응답은 Player/Room 로직에서 Session의 send queue를 통해 전송됩니다.
- 채팅은 Room 단위로 브로드캐스트되며 FSM이 허용된 상태의 패킷만 통과시킵니다.
