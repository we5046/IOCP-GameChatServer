#include <string.h>
#include <algorithm>
#include "RingBuffer.h"

bool RingBuffer::Write(const void* src, int size)
{
    if ((unsigned int)size > (RING_SIZE - storedBytes))
        return false; // 공간 부족

    int firstCopy = std::min(size, RING_SIZE - writePos);
    memcpy(buffer + writePos, src, firstCopy);

    int remain = size - firstCopy;
    if (remain > 0)
        memcpy(buffer, (char*)src + firstCopy, remain);

    writePos = (writePos + size) % RING_SIZE;
    storedBytes += size;

    return true;
}

bool RingBuffer::Peek(void* dst, int size)
{
    if (storedBytes < (unsigned int)size)
        return false;

    int firstCopy = std::min(size, RING_SIZE - readPos);
    memcpy(dst, buffer + readPos, firstCopy);

    int remain = size - firstCopy;
    if (remain > 0)
        memcpy((char*)dst + firstCopy, buffer, remain);

    return true;
}

bool RingBuffer::Read(void* dst, int size)
{
    if (!Peek(dst, size))
        return false;

    readPos = (readPos + size) % RING_SIZE;
    storedBytes -= size;

    return true;
}

bool RingBuffer::Has(size_t size) const
{
    return storedBytes >= size;
}

void RingBuffer::Skip(size_t size)
{
    readPos = (readPos + size) % RING_SIZE;
    storedBytes -= size;
}