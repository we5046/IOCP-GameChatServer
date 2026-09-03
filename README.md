# IOCP Game Chat Server

Windows IOCP 기반의 비동기 멀티플레이 채팅 서버입니다.
네트워크 I/O는 IOCP 워커 스레드에서 병렬로 처리하고, 게임 로직은 단일 게임 스레드에서 직렬로 처리하여
**Player·Room 자료구조에 락이 필요 없는 구조**를 목표로 했습니다.

- **언어·환경** : C++17 / Winsock2 / Windows IOCP / Visual Studio 2022 (v143)
- **지원 구성** : Debug·Release × x86·x64 (4개 구성 모두 빌드·실행 확인)

---

## 실행 방법

### 1. 빌드

```
git clone https://github.com/we5046/IOCP-GameChatServer.git
```

`IOCP-GameChatServer/IOCP-GameChatServer.sln` 을 Visual Studio 2022로 엽니다.
솔루션에는 서버와 클라이언트 두 프로젝트가 포함되어 있습니다.

솔루션 빌드(`Ctrl+Shift+B`) 하면 두 프로젝트가 함께 빌드됩니다.

### 2. 서버 실행

`IOCP-GameChatServer.exe` 를 실행합니다. `127.0.0.1:9000` 에서 접속을 대기합니다.

```
WorkerThreads Created: 16
[GameServer] FSM initialized
```

`Ctrl+C` 로 종료하면 접속 중인 클라이언트에 종료 패킷을 보낸 뒤 워커 스레드를 정리합니다.

### 3. 클라이언트 실행

`IOCP-ChatClient.exe` 를 실행합니다. 접속 대상은 인자로 바꿀 수 있습니다.

```
IOCP-ChatClient.exe [서버IP] [포트]      기본값: 127.0.0.1 9000
```

실행하면 닉네임과 방 번호를 묻습니다. 채팅 창에서 `/help` 를 입력하면 명령어를 볼 수 있습니다.
**여러 개를 동시에 실행해 같은 방 번호로 입장하면 방 단위 브로드캐스트를 확인할 수 있습니다.**

---

## 구조

```text
IOCP-GameChatServer/
├── IOCP-GameChatServer/
│   ├── IOCP-GameChatServer.sln          솔루션 (서버 + 클라이언트)
│   └── IOCP-GameChatServer/             서버 소스
│       ├── Network.cpp / .h             IOCP 생성, 워커·Accept 스레드, main()
│       ├── Session.cpp / .h             소켓 1개의 수명, WSARecv/WSASend, 수신 프레이밍
│       ├── RingBuffer.cpp / .h          TCP 스트림 → 패킷 경계 복원
│       ├── Protocol.cpp / .h            패킷 ID 정의, 네트워크 계층 검증
│       ├── GameJobQueue.cpp / .h        네트워크 스레드 → 게임 스레드 경계
│       ├── GameServer.cpp / .h          게임 스레드 루프, Player·Room 관리
│       ├── FSM.cpp / .h                 플레이어 상태별 허용 패킷 디스패치
│       ├── Player.cpp / .h              게임 레벨 플레이어
│       ├── Room.cpp / .h                방 참가·퇴장·브로드캐스트
│       └── Packet.cpp / .h              패킷 헤더 정의 및 유효성 검사
├── client/
│   ├── client.cpp                       콘솔 채팅 클라이언트
│   └── IOCP-ChatClient.vcxproj
└── docs/
    ├── architecture.md                  컴포넌트 역할과 전체 흐름
    ├── packet-flow.md                   패킷 처리 경로
    └── troubleshooting.md               개발 중 겪은 문제와 해결
```

---

## 아키텍처

```text
클라이언트 소켓
   │
   ▼  WSARecv 완료
커널 IOCP 완료 큐
   │
   ▼  GetQueuedCompletionStatus
IOCP 워커 스레드 (CPU 코어 수)        ← 여러 개, 병렬
   │  Session::OnRecvComplete
   │  RingBuffer 프레이밍 → Protocol 검증
   ▼
GameJobQueue                          ← 락으로 보호되는 유일한 경계
   │
   ▼
게임 스레드 (1개)                      ← 단일, 직렬
   Player · Room · FSM · 브로드캐스트
```

### 이 구조를 택한 이유

워커 스레드가 게임 로직을 직접 처리하면 `players`·`rooms` 맵의 모든 접근에 락이 필요하고,
락 경합과 데드락 위험이 따라옵니다. 게임 로직을 단일 스레드로 직렬화하면 그 자료구조에는
락이 아예 필요 없어집니다. 대신 네트워크 I/O만 병렬로 처리해 처리량을 확보합니다.

실제로 `GameServer.cpp` 에는 임계 영역이 하나도 없습니다.

---

## 주요 구현

### 세션 수명 관리

IOCP에서 가장 다루기 까다로운 부분입니다. `WSARecv`/`WSASend` 를 걸면 커널이 그 버퍼와
`OVERLAPPED` 주소를 기억한 채 나중에 사용하므로, 완료 통지 전에 세션을 해제하면
**커널이 해제된 메모리에 쓰게 되어 힙이 손상됩니다.**

이를 막기 위해 세 가지 조건을 모두 만족할 때만 세션을 해제합니다.

| 조건 | 무엇으로부터 지키는가 |
|---|---|
| `state != Connected` | 아직 통신 중인 세션의 오삭제 |
| `IsGamecleanupDone()` | **게임 스레드** — Player·Room에서 제거되기 전 해제 방지 |
| `pendingIO == 0` | **커널** — 진행 중인 I/O가 참조하는 메모리 해제 방지 |

여기에 더해 `TryBeginDestroy()` 가 CAS로 단 한 스레드만 통과시켜 이중 해제를 막습니다.
`pendingIO` 는 I/O를 거는 직전에 증가시키고 완료 통지에서 감소시킵니다.

### 상태 전이의 원자성

워커가 여러 개이므로 같은 세션의 수신 실패와 송신 실패가 서로 다른 스레드에서 동시에
감지될 수 있습니다. 단순 대입으로 상태를 바꾸면 종료 처리가 두 번 실행되어 Player가
두 번 삭제됩니다. 따라서 `compare_exchange_strong` 으로 **정확히 한 스레드만 통과**하도록 했습니다.

같은 패턴을 `recvPosted`(중복 수신 등록 방지), `closeIssued`(중복 소켓 종료 방지),
`destroying`(이중 해제 방지)에도 적용했습니다.

### TCP 프레이밍

TCP는 스트림이라 패킷 경계를 보존하지 않습니다. 한 번의 수신에 패킷이 여러 개 붙어 오거나,
하나가 잘려 올 수 있습니다. 링버퍼에 누적한 뒤 **완전한 패킷이 남아 있는 동안 반복해서** 꺼냅니다.

링버퍼를 직접 구현한 이유는 세 가지입니다.

1. `WSARecv` 는 연속된 메모리(`WSABUF`)를 요구하는데, 노드 기반 컨테이너로는 이를 만족할 수 없습니다.
2. 고정 배열을 재사용하므로 세션당 런타임 할당이 발생하지 않습니다. 동시 접속이 많을수록 힙 할당자 경합이 병목이 됩니다.
3. **메모리 상한이 보장됩니다.** 버퍼가 가득 차면 쓰기가 실패하고 해당 세션을 끊으므로,
   데이터를 계속 흘려보내는 클라이언트에 서버 메모리를 잠식당하지 않습니다.

### 패킷 검증과 FSM

네트워크 계층(`Protocol.cpp`)에서 패킷 크기와 알려진 ID 여부를 먼저 검사하고,
어긋나면 KICK 패킷을 보낸 뒤 연결을 끊습니다. 게임 계층에 도달하기 전에 차단합니다.

게임 계층에서는 FSM이 **플레이어 상태별로 허용된 패킷만** 핸들러로 전달합니다.
로그인 전에 채팅 패킷을 보내는 등의 순서 위반을 상태 기계 수준에서 차단합니다.

---

## 프로토콜

모든 패킷은 4바이트 헤더로 시작합니다. `#pragma pack(1)` 로 패딩 없이 고정됩니다.

```cpp
struct PacketHeader {
    uint16_t size;   // 헤더 포함 전체 길이
    uint16_t id;     // 패킷 ID
};
```

| ID | 이름 | 방향 | 내용 |
|---:|---|---|---|
| 1 | `PKT_CS_LOGIN` | 클 → 서 | 닉네임 |
| 2 | `PKT_CS_CHAT` | 클 → 서 | 채팅 메시지 |
| 3 | `PKT_SC_CHAT` | 서 → 클 | 채팅 수신 |
| 4 | `PKT_CS_ENTER_ROOM` | 클 → 서 | 방 번호(int32) |
| 1000 | `PKT_SC_SHUTDOWN` | 서 → 클 | 서버 종료 통보 |
| 1001 | `PKT_SC_KICK` | 서 → 클 | 비정상 패킷으로 인한 강제 종료 |

최대 패킷 크기는 4096바이트이며, 이 범위를 벗어난 헤더는 조작으로 간주해 연결을 끊습니다.

---

## 알려진 제한

현재 코드에서 파악된 미완성 항목입니다. 개선 대상으로 관리하고 있습니다.

1. **송신 큐 미구현** — `WSASend` 를 호출할 때마다 송신 컨텍스트를 새로 할당합니다.
   같은 세션에 여러 스레드가 동시에 송신하면 순서가 보장되지 않습니다.
2. **부분 전송 미처리** — `WSASend` 는 요청보다 적은 바이트로 완료될 수 있으나,
   현재는 전송 완료 바이트 수를 요청 길이와 비교하지 않습니다.
3. **종료 시 잔여 I/O 대기 비활성** — `GameServer::Shutdown()` 의 대기 루프가 주석 처리되어 있어,
   진행 중인 I/O가 남은 상태로 워커가 종료될 여지가 있습니다. 원인 규명 후 복원 예정입니다.
4. **`AcceptEx` 미사용** — accept 전용 블로킹 스레드를 사용하며, 종료 시 listen 소켓을 닫아 깨웁니다.
   accept 자체를 IOCP에 태우는 방식으로 개선할 수 있습니다.
5. **수신 경로에 복사 1회** — 링버퍼에 zero-copy 인터페이스가 없어 수신 버퍼에서 링버퍼로 한 번 복사합니다.
6. **로깅** — `std::cout` 직접 출력이라 멀티스레드 환경에서 로그가 섞일 수 있습니다.
7. **부하 테스트 미실시** — 동시 접속 한계와 처리량을 아직 측정하지 않았습니다.

---

## 개발 기록

초기 구현 과정에서 힙 손상이 발생했습니다. 원인은 **송신 버퍼의 수명**이었습니다.
`WSASend` 는 즉시 반환하지만 커널은 그 뒤에도 버퍼를 읽으므로, 호출 측이 보유한 버퍼가
먼저 사라지면 커널이 이미 해제된 메모리를 참조합니다.

현재는 송신 컨텍스트가 데이터를 자체 버퍼에 복사해 보관하고, 완료 통지를 받은 뒤에 해제하도록
수정했습니다(`fix/iocp-send-heap-corruption`). 이 경험이 세션 수명 관리에 `pendingIO` 참조
카운팅을 도입한 계기가 되었습니다.

---

## English Summary

An asynchronous multiplayer chat server for Windows built on I/O Completion Ports.
Network I/O runs in parallel on IOCP worker threads; game logic runs serially on a single game
thread, so player and room structures need no locks. The two sides communicate through one
lock-protected job queue. Session lifetime is guarded by a pending-I/O reference count and
atomic state transitions, which is what prevents the kernel from writing into freed memory.
Build with Visual Studio 2022; the solution contains both the server and a console test client.
