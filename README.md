# IOCP Chat Server

## Overview

IOCP Chat Server is a Windows C++ networking portfolio project built around I/O Completion Ports.

The project focuses on the core architecture of an asynchronous multiplayer chat server:

- accepting TCP clients
- registering sockets with IOCP
- processing completed recv/send operations on worker threads
- forwarding validated packets to a game thread
- managing players and rooms
- broadcasting chat messages by room

The implementation is intentionally kept in the `IOCP_ChatServer` folder to avoid breaking existing include paths while the project is being cleaned up.

## Tech Stack

- C++
- Windows Winsock2
- Windows IOCP
- Multi-threading
- Packet-based TCP communication
- Visual Studio / MSVC target environment

## Core Features

- IOCP worker thread model
- Accept thread for client connections
- Session lifecycle management
- Packet validation and dispatch
- Game job queue
- FSM-based player state handling
- Room-based chat broadcast
- Ring buffer for TCP stream framing
- Send queue with partial-send handling
- Shutdown flow groundwork

## Current Structure

```text
IOCP
├── .github
│   ├── ISSUE_TEMPLATE
│   │   └── feature_request.md
│   └── pull_request_template.md
├── IOCP_ChatServer
│   ├── FSM.cpp
│   ├── FSM.h
│   ├── GameJob.h
│   ├── GameJobQueue.cpp
│   ├── GameJobQueue.h
│   ├── GameServer.cpp
│   ├── GameServer.h
│   ├── Network.cpp
│   ├── Network.h
│   ├── Packet.cpp
│   ├── Packet.h
│   ├── Player.cpp
│   ├── Player.h
│   ├── Protocol.cpp
│   ├── Protocol.h
│   ├── RingBuffer.cpp
│   ├── RingBuffer.h
│   ├── Room.cpp
│   ├── Room.h
│   ├── Session.cpp
│   └── Session.h
├── docs
│   ├── architecture.md
│   ├── packet-flow.md
│   └── troubleshooting.md
├── README.md
└── .gitignore
```

## How To Run

TODO.

The server code currently targets Windows C++/Winsock2/IOCP. A Visual Studio solution or project setup may be added or restored later.

## Roadmap

- Stabilize header includes and build settings
- Fix packet serialization format for chat messages
- Clarify shutdown order and session cleanup guarantees
- Separate `main.cpp` from `Network.cpp`
- Add a small test client or scripted packet sender
- Add build instructions for Visual Studio
- Add diagrams for IOCP flow and game-thread flow

## Korean Summary

- Windows C++ IOCP 기반 채팅 서버 포트폴리오 프로젝트입니다.
- 네트워크 스레드와 게임 스레드를 분리하고, 패킷을 JobQueue로 전달하는 구조입니다.
- 현재는 구조 정리와 안정화가 진행 중이며 실행 방법은 TODO 상태입니다.
