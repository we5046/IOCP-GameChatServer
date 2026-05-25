#pragma once
static const int RING_SIZE = 8192;
struct RingBuffer
{
	char buffer[RING_SIZE];
	int readPos = 0;		// 읽기 위치
	int writePos = 0;		// 쓰기 위치
	size_t storedBytes = 0;	// 현재 들어있는 데이터량

	bool Write(const void* src, int size);
	bool Peek(void* dst, int size);
	bool Read(void* dst, int size);
	bool Has(size_t size) const;
	void Skip(size_t size);
};