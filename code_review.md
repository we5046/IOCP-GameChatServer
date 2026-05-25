# IOCP ChatServer Code Review

## Summary

현재 `IOCP_ChatServer`는 IOCP 기반 채팅 서버의 기본 구조를 갖춘 복원본입니다.

- `Network`: IOCP 생성, 워커 스레드, accept, completion 처리
- `Session`: WSARecv/WSASend 등록, 수신 버퍼링, 송신 큐 관리
- `Protocol`: 패킷 유효성 검증 후 게임 큐로 전달
- `GameServer`: 단일 게임 스레드에서 접속, 패킷, disconnect 처리
- `Room`/`Player`: 방 입장, 퇴장, 채팅 브로드캐스트

포트폴리오 코드로 쓰기 위해서는 아래 항목을 우선 수정하는 것이 좋습니다.

## P0 - Must Fix

### 1. RingBuffer wrap-around 시 packet body가 깨질 수 있음

- 위치: `Session.cpp`
- 관련 코드: `recvRing.Skip(sizeof(PacketHeader));`, `pkt.body = recvRing.GetReadPtr();`

현재 패킷 헤더를 읽은 뒤 body 포인터를 링버퍼 내부 주소로 직접 넘깁니다. 하지만 body가 링버퍼 끝과 처음에 걸쳐 있으면 메모리가 연속되어 있지 않습니다. 이 상태에서 `GameServer::EnqueuePacketJob()`이 `memcpy()`를 하면 잘못된 데이터를 읽을 수 있습니다.

권장 수정:

- `RingBuffer::Read()` 또는 새 helper로 body를 `std::vector<char>`에 복사한다.
- `Packet`이 raw pointer 대신 복사된 body를 참조하도록 처리한다.
- 또는 `EnqueuePacketJob()`이 RingBuffer를 직접 모르도록, `Session`에서 완성된 `GameJob` 데이터를 만들어 넘긴다.

### 2. `players[s]` 사용으로 null 삽입 및 크래시 가능

- 위치: `GameServer.cpp`
- 관련 함수: `HandleEnterRoom()`, `HandleLogin()`, `HandleChat()`

`unordered_map::operator[]`는 키가 없으면 새 항목을 삽입합니다. 현재는 등록되지 않았거나 이미 disconnect 처리된 `Session*`가 들어오면 `nullptr` Player가 삽입될 수 있고, `p->IsLoggedIn()`에서 크래시가 날 수 있습니다.

권장 수정:

- `players.find(s)`로 조회한다.
- 없으면 즉시 return한다.
- `Player*`가 null인지 확인한다.

### 3. Session 수명 관리가 안전하지 않음

- 위치: `GameServer.cpp`, `Network.cpp`, `Session.cpp`
- 관련 흐름: Packet Job, Disconnect Job, WorkerThread delete

현재 게임 큐에는 `Session*` raw pointer가 들어갑니다. disconnect 이후 `WorkerThread`가 `delete session`을 수행한 뒤에도 같은 세션의 Packet Job이 큐에 남아 있으면 use-after-free 위험이 있습니다.

권장 수정:

- 최소 수정: `Session`에 generation id 또는 closed flag를 두고 stale job을 무시한다.
- 구조 개선: `shared_ptr<Session>` 기반으로 수명을 관리하거나, IOCP pending count와 game job count를 함께 추적한다.
- disconnect 이후 해당 세션의 Packet Job은 처리하지 않는 규칙을 명확히 둔다.

## P1 - Should Fix

### 4. partial send 처리가 없음

- 위치: `Session.cpp`
- 관련 함수: `OnSendComplete()`

`WSASend` 완료 바이트가 요청한 패킷 크기보다 작을 수 있습니다. 현재는 `bytes > 0`이면 패킷 하나를 통째로 pop하므로 일부 데이터가 유실될 수 있습니다.

권장 수정:

- 현재 전송 중인 패킷의 offset을 저장한다.
- `bytes < packet.size()`이면 남은 구간을 다시 `WSASend`한다.
- 완료된 경우에만 `sendQueue.pop_front()`를 수행한다.

### 5. IOCP cleanup 이벤트가 FIN 처리로 들어갈 수 있음

- 위치: `Network.cpp`
- 관련 함수: `WorkerThread()`

`overlapped == nullptr`인 cleanup check 이벤트도 `bytesTransferred == 0`입니다. 현재는 cleanup check에서도 disconnect detected 로그와 `RequestClose()`가 다시 호출될 수 있습니다.

권장 수정:

- `overlapped == nullptr`인 경우에는 I/O 완료가 아니므로 에러/FIN 처리 분기에서 제외한다.
- recv 완료에서 0 byte일 때만 FIN으로 해석한다.

### 6. 초기화 및 소켓 API 에러 체크 부족

- 위치: `Network.cpp`
- 관련 API: `WSAStartup`, `socket`, `bind`, `listen`, `CreateIoCompletionPort`, `PostRecv`

실패 시 원인을 알기 어렵고, 일부 실패 후에도 서버가 계속 진행될 수 있습니다.

권장 수정:

- 각 API 반환값을 검사한다.
- 실패 시 `WSAGetLastError()` 또는 `GetLastError()`를 로그로 남긴다.
- 이미 생성된 리소스를 닫고 종료한다.

## P2 - Improve

### 7. `Room`의 roomId 타입 불일치

- 위치: `Room.h`, `GameServer.cpp`, `Packet.h`

패킷과 `GameServer`는 `int32_t roomId`를 사용하지만 `Room`은 `uint16_t`를 사용합니다. 큰 값이나 음수 값이 들어오면 잘릴 수 있습니다.

권장 수정:

- `Room::roomId`를 `int32_t` 또는 `uint32_t`로 통일한다.
- 유효한 방 번호 범위를 검증한다.

### 8. `Network.cpp`에 `main()`이 포함되어 있음

- 위치: `Network.cpp`

네트워크 모듈과 프로그램 진입점이 한 파일에 있어 역할이 섞여 있습니다.

권장 수정:

- `main.cpp`를 만들고 서버 시작 로직을 옮긴다.
- `Network`는 IOCP 초기화, accept 처리, 세션 등록 같은 네트워크 책임만 갖게 한다.

### 9. raw pointer 소유권이 불명확함

- 위치: `GameServer`, `Network`, `Room`, `Player`

`Player`는 `GameServer`가 소유하고 `Room`은 참조만 하는 구조로 보입니다. 의도는 좋지만 코드만 보면 소유권이 명확하게 강제되지 않습니다.

권장 수정:

- `GameServer::players`를 `std::unique_ptr<Player>`로 바꾼다.
- `Room`은 non-owning pointer를 보관한다는 주석을 유지한다.
- `Session` 수명 정책은 별도로 명확히 정한다.

## Suggested Fix Order

1. `players[s]`를 `find()` 기반 조회로 교체한다.
2. RingBuffer body 복사 문제를 수정한다.
3. `overlapped == nullptr` cleanup 이벤트가 FIN 처리로 들어가지 않게 분리한다.
4. Session stale job/use-after-free 방지 정책을 추가한다.
5. partial send offset 처리를 추가한다.
6. 소켓 API 에러 체크와 로그를 보강한다.
7. `Room` roomId 타입을 통일한다.
8. `main.cpp` 분리 및 소유권 정리를 진행한다.

## Portfolio Notes

현재 코드는 "IOCP 기반 채팅 서버를 직접 구현했다"는 증거로 사용할 수 있는 뼈대가 있습니다. 다만 포트폴리오에서는 단순히 돌아가는 것보다 다음 포인트를 설명할 수 있어야 합니다.

- IOCP worker thread와 game thread를 분리한 이유
- TCP stream에서 packet framing을 처리하는 방식
- send queue가 필요한 이유
- disconnect와 pending I/O를 함께 관리해야 하는 이유
- `Room`과 `Player`를 네트워크 코드와 분리한 이유

위 P0 항목을 고친 뒤 README에 구조 설명과 실행 방법을 추가하면 포트폴리오 설득력이 크게 올라갑니다.
