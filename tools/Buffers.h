//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdexcept>
#include <array>
#include <limits>
#include <algorithm>
#include <numeric>
#include <bitset>
#include <memory>
#include <stddef.h>
#include "threading.h"
#include "utils.h"

namespace tgvoip
{
class Buffer;

class BufferInputStream
{

public:
	BufferInputStream(const unsigned char *data, size_t length);
	BufferInputStream(const Buffer &buffer);
	~BufferInputStream();
	void Seek(size_t offset);
	size_t GetLength() const;
	size_t GetOffset() const;
	size_t Remaining() const;
	unsigned char ReadByte();
	int64_t ReadInt64();
	int32_t ReadInt32();
	int16_t ReadInt16();
	uint32_t ReadTlLength();
	void ReadBytes(unsigned char *to, size_t count);
	void ReadBytes(Buffer &to);
	BufferInputStream GetPartBuffer(size_t length, bool advance);

	inline uint64_t ReadUInt64()
	{
		return static_cast<uint64_t>(ReadInt64());
	}
	inline uint32_t ReadUInt32()
	{
		return static_cast<uint32_t>(ReadInt32());
	}
	inline uint16_t ReadUInt16()
	{
		return static_cast<uint16_t>(ReadInt16());
	}

private:
	void EnsureEnoughRemaining(size_t need);
	const unsigned char *buffer;
	size_t length;
	size_t offset = 0;
};

class BufferOutputStream
{
	friend class Buffer;

public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(BufferOutputStream);
	BufferOutputStream(size_t size);
	BufferOutputStream(unsigned char *buffer, size_t size);
	~BufferOutputStream();
	void WriteByte(unsigned char byte);
	void WriteInt64(int64_t i);
	void WriteInt32(int32_t i);
	void WriteInt16(int16_t i);
	void WriteBytes(const unsigned char *bytes, size_t count);
	void WriteBytes(const Buffer &buffer);
	void WriteBytes(const Buffer &buffer, size_t offset, size_t count);
	unsigned char *GetBuffer();
	size_t GetLength();
	void Reset();
	void Rewind(size_t numBytes);

	inline void WriteUInt64(uint64_t i)
	{
		WriteInt64(static_cast<int64_t>(i));
	}
	inline void WriteUInt32(uint32_t i)
	{
		WriteInt32(static_cast<int32_t>(i));
	}
	inline void WriteUInt16(uint16_t i)
	{
		WriteInt16(static_cast<int16_t>(i));
	}

	BufferOutputStream &operator=(BufferOutputStream &&other)
	{
		if (this != &other)
		{
			if (!bufferProvided && buffer)
				free(buffer);
			buffer = other.buffer;
			offset = other.offset;
			size = other.size;
			bufferProvided = other.bufferProvided;
			other.buffer = NULL;
		}
		return *this;
	}

private:
	void ExpandBufferIfNeeded(size_t need);
	unsigned char *buffer = NULL;
	size_t size;
	size_t offset = 0;
	bool bufferProvided;
};

class Buffer
{
public:
	Buffer(size_t capacity)
	{
		if (capacity > 0)
		{
			data = (unsigned char *)malloc(capacity);
			if (!data)
				throw std::bad_alloc();
		}
		else
		{
			data = NULL;
		}
		length = capacity;
	};
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(Buffer); // use Buffer::CopyOf to copy contents explicitly
	Buffer(Buffer &&other) noexcept
	{
		data = other.data;
		length = other.length;
		freeFn = other.freeFn;
		reallocFn = other.reallocFn;
		other.data = NULL;
	};
	Buffer(BufferOutputStream &&stream)
	{
		data = stream.buffer;
		length = stream.offset;
		stream.buffer = NULL;
	}
	Buffer()
	{
		data = NULL;
		length = 0;
	}
	~Buffer()
	{
		if (data)
		{
			if (freeFn)
				freeFn(data);
			else
				free(data);
		}
		data = NULL;
		length = 0;
	};
	Buffer &operator=(Buffer &&other)
	{
		if (this != &other)
		{
			if (data)
			{
				if (freeFn)
					freeFn(data);
				else
					free(data);
			}
			data = other.data;
			length = other.length;
			freeFn = other.freeFn;
			reallocFn = other.reallocFn;
			other.data = NULL;
			other.length = 0;
		}
		return *this;
	}
	unsigned char &operator[](size_t i)
	{
		if (i >= length)
			throw std::out_of_range("");
		return data[i];
	}
	const unsigned char &operator[](size_t i) const
	{
		if (i >= length)
			throw std::out_of_range("");
		return data[i];
	}
	unsigned char *operator*()
	{
		return data;
	}
	const unsigned char *operator*() const
	{
		return data;
	}
	void CopyFrom(const Buffer &other, size_t count, size_t srcOffset = 0, size_t dstOffset = 0)
	{
		if (!other.data)
			throw std::invalid_argument("CopyFrom can't copy from NULL");
		if (other.length < srcOffset + count || length < dstOffset + count)
			throw std::out_of_range("Out of offset+count bounds of either buffer");
		memcpy(data + dstOffset, other.data + srcOffset, count);
	}
	void CopyFrom(const void *ptr, size_t dstOffset, size_t count)
	{
		if (length < dstOffset + count)
			throw std::out_of_range("Offset+count is out of bounds");
		memcpy(data + dstOffset, ptr, count);
	}
	void Resize(size_t newSize)
	{
		if (reallocFn)
			data = (unsigned char *)reallocFn(data, newSize);
		else
			data = (unsigned char *)realloc(data, newSize);
		if (!data)
			throw std::bad_alloc();
		length = newSize;
	}
	size_t Length() const
	{
		return length;
	}
	bool IsEmpty() const
	{
		return length == 0 || !data;
	}
	static Buffer CopyOf(const Buffer &other)
	{
		if (other.IsEmpty())
			return Buffer();
		Buffer buf(other.length);
		buf.CopyFrom(other, other.length);
		return buf;
	}
	static Buffer CopyOf(const Buffer &other, size_t offset, size_t length)
	{
		if (offset + length > other.Length())
			throw std::out_of_range("offset+length out of bounds");
		Buffer buf(length);
		buf.CopyFrom(other, length, offset);
		return buf;
	}
	static Buffer Wrap(unsigned char *data, size_t size, std::function<void(void *)> freeFn, std::function<void *(void *, size_t)> reallocFn)
	{
		Buffer b = Buffer();
		b.data = data;
		b.length = size;
		b.freeFn = freeFn;
		b.reallocFn = reallocFn;
		return b;
	}

private:
	unsigned char *data;
	size_t length;
	std::function<void(void *)> freeFn;
	std::function<void *(void *, size_t)> reallocFn;
};

template <typename T, size_t size, typename AVG_T = T>
class HistoricBuffer
{
public:
	HistoricBuffer()
	{
	}

	AVG_T Average() const
	{
		return std::accumulate(data.begin(), data.end(), static_cast<AVG_T>(0)) / static_cast<AVG_T>(size);
	}

	AVG_T Average(size_t firstN) const
	{
		AVG_T avg = static_cast<AVG_T>(0);
		for (size_t i = 0; i < firstN; i++) // Manual iteration required to wrap around array with specific offset
		{
			avg += (*this)[i];
		}
		return avg / static_cast<AVG_T>(firstN);
	}

	AVG_T NonZeroAverage() const
	{
		AVG_T avg = static_cast<AVG_T>(0);
		int nonZeroCount = 0;
		for (T i : data)
		{
			if (i != 0)
			{
				nonZeroCount++;
				avg += i;
			}
		}
		if (nonZeroCount == 0)
			return static_cast<AVG_T>(0);
		return avg / static_cast<AVG_T>(nonZeroCount);
	}

	void Add(T el)
	{
		data[offset] = el;
		offset = (offset + 1) % size;
	}

	T Min() const
	{
		return *std::min_element(data.begin(), data.end());
	}

	T Max() const
	{
		return *std::max_element(data.begin(), data.end());
	}

	void Reset()
	{
		std::fill(data.begin(), data.end(), static_cast<T>(0));
		offset = 0;
	}

	T operator[](size_t i) const
	{
		assert(i < size);
		// [0] should return the most recent entry, [1] the one before it, and so on
		ptrdiff_t _i = offset - i - 1;
		if (_i < 0) // Wrap around offset a-la posmod
			_i = size + _i;
		return data[_i];
	}

	T &operator[](size_t i)
	{
		assert(i < size);
		// [0] should return the most recent entry, [1] the one before it, and so on
		ptrdiff_t _i = offset - i - 1;
		if (_i < 0) // Wrap around offset a-la posmod
			_i = size + _i;
		return data[_i];
	}

	size_t Size() const
	{
		return size;
	}

private:
	std::array<T, size> data{};
	ptrdiff_t offset = 0;
};

template <size_t bufSize, size_t bufCount>
class BufferPool
{
public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(BufferPool);
	BufferPool() : bufferStart(new unsigned char[bufSize * bufCount], std::default_delete<unsigned char[]>()) {}
	~BufferPool(){};
	Buffer Get()
	{
		std::shared_ptr<unsigned char> lock = bufferStart;

		auto resizeFn = [](void *buf, size_t newSize) -> void * {
			if (newSize > bufSize)
				throw std::invalid_argument("newSize>bufferSize");
			return buf;
		};
		MutexGuard m(mutex);
		for (size_t i = 0; i < bufCount; i++)
		{
			if (!usedBuffers[offset])
			{
				usedBuffers[offset] = 1;
				size_t offsetCopy = offset;
				offset = (offset + 1) % bufCount;
				auto freeFn = [this, offsetCopy, &lock](void *_buf) {
					MutexGuard m(mutex);
					usedBuffers[offsetCopy] = 0;
					lock.reset();
				};
				return Buffer::Wrap(bufferStart.get() + (bufSize * offsetCopy), bufSize, freeFn, resizeFn);
			}
		}
		throw std::bad_alloc();
	}

private:
	std::bitset<bufCount> usedBuffers;
	size_t offset = 0;
	std::shared_ptr<unsigned char> bufferStart;
	Mutex mutex;
};
} // namespace tgvoip
