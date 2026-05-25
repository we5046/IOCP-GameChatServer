# Architecture

## Overall Structure

The server is organized around two major flows:

1. Network I/O is handled by IOCP worker threads.
2. Game logic is handled by a game thread through a job queue.

This keeps socket completion handling separate from player, room, and packet gameplay logic.

```text
Client Socket
  -> IOCP Worker Thread
  -> Session
  -> Protocol Validation
  -> GameJobQueue
  -> GameServer
  -> Player / Room
```

## Component Roles

### GameServer

`GameServer` owns the high-level game flow.

- manages connected players
- manages rooms
- receives packet jobs from the network layer
- dispatches packets through FSM handlers
- handles login, room entry, chat, disconnect, and shutdown jobs

### Network

`Network` owns the IOCP and socket-facing infrastructure.

- creates the IOCP handle
- starts worker threads
- starts the accept thread
- registers accepted sockets with IOCP
- keeps a session registry
- posts worker shutdown signals

### Session

`Session` represents one connected client socket.

- owns recv/send overlapped structures
- posts `WSARecv`
- posts `WSASend`
- stores received stream data in `RingBuffer`
- owns the send queue
- tracks pending I/O count
- transitions between connected and closing states

### Player

`Player` represents the game-level identity attached to a session.

- stores name and player state
- references the current room
- sends chat packets through the session

### Room

`Room` groups players and broadcasts room-local messages.

- joins players
- removes players
- broadcasts chat messages to players in the same room

### Packet

`Packet` defines the basic packet header and packet view.

- packet size
- packet id
- body pointer
- validation helper

### RingBuffer

`RingBuffer` handles TCP stream framing.

- stores bytes from `WSARecv`
- peeks packet headers
- checks whether a full packet has arrived
- skips consumed bytes

## IOCP Server Flow

```text
1. Server starts Winsock.
2. Network creates an IOCP handle.
3. Worker threads wait on GetQueuedCompletionStatus.
4. Accept thread accepts client sockets.
5. Each client socket is registered to IOCP with Session* as completion key.
6. Session posts WSARecv.
7. Worker receives completion and calls Session::OnRecvComplete.
8. Session extracts full packets and forwards them to Protocol.
9. Protocol validates packets and pushes GameJob to GameServer.
10. GameServer processes jobs on the game thread.
```

## Korean Summary

- 서버는 IOCP 네트워크 처리와 게임 로직 처리를 분리합니다.
- Session은 소켓 I/O를 담당하고, GameServer는 Player와 Room 로직을 담당합니다.
- 패킷은 Protocol 검증 후 GameJobQueue를 통해 게임 스레드에서 처리됩니다.
