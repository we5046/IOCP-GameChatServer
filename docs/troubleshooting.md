# Troubleshooting

## Incomplete Type Errors

An incomplete type error usually means a file only sees a forward declaration but needs the full class definition.

Example:

```cpp
class Session;
```

This is enough for `Session*`, but not enough to call member functions like:

```cpp
session->PostRecv();
session->SendPacket(...);
```

Fix:

- keep forward declarations in headers when only pointers or references are needed
- include the real header in `.cpp` files when calling methods
- include the real header in a header only if the full type is required there

## Why `Session.h` Is Often Needed

`Session` is used across the network and game layers.

You need `Session.h` when code calls:

- `GetSocket()`
- `PostRecv()`
- `SendPacket()`
- `RequestClose()`
- `GetRecvOverlapped()`
- `GetSendOverlapped()`
- `GetPendingIO()`

If a file only stores `Session*` without calling methods, a forward declaration may be enough.

## Sparse Checkout Notes

When using Git sparse-checkout, make sure repository metadata files are included.

Recommended paths:

```text
/.github/
/IOCP_ChatServer/
/docs/
/README.md
/.gitignore
```

If `.github` or docs are missing after checkout, check the sparse-checkout patterns.

## Git Ignore And Already Tracked Files

`.gitignore` prevents new files from being tracked. It does not automatically untrack files that are already committed or staged.

To keep a local-only file such as `code_review.md` while removing it from Git tracking:

```bash
git rm --cached code_review.md
```

To remove already tracked build artifacts from Git while keeping local files:

```bash
git rm -r --cached Binary Libraries
git rm -r --cached "**/Debug" "**/Release" "**/x64"
```

Then verify:

```bash
git status --short
```

## Korean Summary

- incomplete type 오류는 전방 선언만 있고 실제 헤더가 없을 때 자주 발생합니다.
- 이미 Git이 추적 중인 파일은 `.gitignore`만으로 추적이 해제되지 않습니다.
- sparse-checkout을 사용할 때는 `.github`, `docs`, `README.md`가 포함되는지 확인해야 합니다.
