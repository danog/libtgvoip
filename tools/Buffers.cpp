//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "tools/Buffers.h"
#include <assert.h>
#include <string.h>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include "tools/logging.h"
#include "controller/net/NetworkSocket.h"
#include "controller/protocol/protocol/Interface.h"

using namespace tgvoip;

#pragma mark - BufferInputStream

BufferInputStream::BufferInputStream(const unsigned char *data, const size_t _length) : buffer(data), length(_length)
{
}

BufferInputStream::BufferInputStream(const Buffer &buffer) : buffer(*buffer), length(buffer.Length())
{
}

void BufferInputStream::Seek(size_t offset) const
{
	if (offset > length)
	{
		throw std::out_of_range("Not enough bytes in buffer");
	}
	this->offset = offset;
}

size_t BufferInputStream::GetLength() const
{
	return length;
}

size_t BufferInputStream::GetOffset() const
{
	return offset;
}

size_t BufferInputStream::Remaining() const
{
	return length - offset;
}

unsigned char BufferInputStream::ReadByte() const
{
	EnsureEnoughRemaining(1);
	return (unsigned char)buffer[offset++];
}

int32_t BufferInputStream::ReadInt32() const
{
	EnsureEnoughRemaining(4);
	int32_t res = ((int32_t)buffer[offset] & 0xFF) |
				  (((int32_t)buffer[offset + 1] & 0xFF) << 8) |
				  (((int32_t)buffer[offset + 2] & 0xFF) << 16) |
				  (((int32_t)buffer[offset + 3] & 0xFF) << 24);
	offset += 4;
	return res;
}

int64_t BufferInputStream::ReadInt64() const
{
	EnsureEnoughRemaining(8);
	int64_t res = ((int64_t)buffer[offset] & 0xFF) |
				  (((int64_t)buffer[offset + 1] & 0xFF) << 8) |
				  (((int64_t)buffer[offset + 2] & 0xFF) << 16) |
				  (((int64_t)buffer[offset + 3] & 0xFF) << 24) |
				  (((int64_t)buffer[offset + 4] & 0xFF) << 32) |
				  (((int64_t)buffer[offset + 5] & 0xFF) << 40) |
				  (((int64_t)buffer[offset + 6] & 0xFF) << 48) |
				  (((int64_t)buffer[offset + 7] & 0xFF) << 56);
	offset += 8;
	return res;
}

int16_t BufferInputStream::ReadInt16() const
{
	EnsureEnoughRemaining(2);
	int16_t res = (uint16_t)buffer[offset] | ((uint16_t)buffer[offset + 1] << 8);
	offset += 2;
	return res;
}

uint32_t BufferInputStream::ReadTlLength() const
{
	unsigned char l = ReadByte();
	if (l < 254)
		return l;
	assert(length - offset >= 3);
	EnsureEnoughRemaining(3);
	uint32_t res = ((uint32_t)buffer[offset] & 0xFF) |
				   (((uint32_t)buffer[offset + 1] & 0xFF) << 8) |
				   (((uint32_t)buffer[offset + 2] & 0xFF) << 16);
	offset += 3;
	return res;
}

void BufferInputStream::ReadBytes(unsigned char *to, size_t count) const
{
	EnsureEnoughRemaining(count);
	memcpy(to, buffer + offset, count);
	offset += count;
}

void BufferInputStream::ReadBytes(Buffer &to) const
{
	ReadBytes(*to, to.Length());
}

BufferInputStream BufferInputStream::GetPartBuffer(size_t length, bool advance) const
{
	EnsureEnoughRemaining(length);
	BufferInputStream s = BufferInputStream(buffer + offset, length);
	if (advance)
		offset += length;
	return s;
}
const unsigned char *BufferInputStream::GetRawBuffer() const
{
	return buffer + offset;
}

void BufferInputStream::EnsureEnoughRemaining(size_t need) const
{
	if (length - offset < need)
	{
		throw std::out_of_range("Not enough bytes in buffer");
	}
}

bool BufferInputStream::TryRead(uint8_t &data) const
{
	if (offset + 1 > length)
		return false;
	data = buffer[offset++];
	return true;
}
bool BufferInputStream::TryRead(uint16_t &data) const
{
	if (offset + 2 > length)
		return false;
	data = (uint16_t)buffer[offset] | ((uint16_t)buffer[offset + 1] << 8);
	offset += 2;
	return true;
}
bool BufferInputStream::TryRead(uint32_t &data) const
{
	if (offset + 4 > length)
		return false;
	data = ((uint32_t)buffer[offset] & 0xFF) |
		   (((uint32_t)buffer[offset + 1] & 0xFF) << 8) |
		   (((uint32_t)buffer[offset + 2] & 0xFF) << 16) |
		   (((uint32_t)buffer[offset + 3] & 0xFF) << 24);
	offset += 4;
	return true;
}
bool BufferInputStream::TryRead(uint64_t &data) const
{
	if (offset + 8 > length)
		return false;
	data = ((uint64_t)buffer[offset] & 0xFF) |
		   (((uint64_t)buffer[offset + 1] & 0xFF) << 8) |
		   (((uint64_t)buffer[offset + 2] & 0xFF) << 16) |
		   (((uint64_t)buffer[offset + 3] & 0xFF) << 24) |
		   (((uint64_t)buffer[offset + 4] & 0xFF) << 32) |
		   (((uint64_t)buffer[offset + 5] & 0xFF) << 40) |
		   (((uint64_t)buffer[offset + 6] & 0xFF) << 48) |
		   (((uint64_t)buffer[offset + 7] & 0xFF) << 56);
	offset += 8;
	return true;
}
bool BufferInputStream::TryRead(int16_t &data) const
{
	if (offset + 2 > length)
		return false;
	data = (int16_t)buffer[offset] | ((int16_t)buffer[offset + 1] << 8);
	offset += 2;
	return true;
}
bool BufferInputStream::TryRead(int32_t &data) const
{
	if (offset + 4 > length)
		return false;
	data = ((int32_t)buffer[offset] & 0xFF) |
		   (((int32_t)buffer[offset + 1] & 0xFF) << 8) |
		   (((int32_t)buffer[offset + 2] & 0xFF) << 16) |
		   (((int32_t)buffer[offset + 3] & 0xFF) << 24);
	offset += 4;
	return true;
}
bool BufferInputStream::TryRead(int64_t &data) const
{
	if (offset + 8 > length)
		return false;
	data = ((int64_t)buffer[offset] & 0xFF) |
		   (((int64_t)buffer[offset + 1] & 0xFF) << 8) |
		   (((int64_t)buffer[offset + 2] & 0xFF) << 16) |
		   (((int64_t)buffer[offset + 3] & 0xFF) << 24) |
		   (((int64_t)buffer[offset + 4] & 0xFF) << 32) |
		   (((int64_t)buffer[offset + 5] & 0xFF) << 40) |
		   (((int64_t)buffer[offset + 6] & 0xFF) << 48) |
		   (((int64_t)buffer[offset + 7] & 0xFF) << 56);
	offset += 8;
	return true;
}
bool BufferInputStream::TryRead(uint8_t *to, size_t len) const
{
	if (offset + len > length)
		return false;
	memcpy(to, buffer + offset, len);
	offset += len;
	return true;
}
bool BufferInputStream::TryRead(Buffer &to) const
{
	return TryRead(*to, to.Length());
}

bool BufferInputStream::TryRead(NetworkAddress &to, bool ipv6) const
{
	to.isIPv6 = ipv6;
	return ipv6 ? TryRead(to.addr.ipv6, 16) : TryRead(to.addr.ipv4);
}
bool BufferInputStream::TryRead(Serializable &to, const VersionInfo &ver) const
{
	return to.parse(*this, ver);
}

template <typename X, typename Y>
bool BufferInputStream::TryReadCompat(Y &data) const
{
	X temp;
	if (!TryRead(temp))
		return false;
	data = static_cast<Y>(temp);
	return true;
}

bool BufferInputStream::Has(size_t len) const
{
	return offset + len > length;
}

bool BufferInputStream::TryReadTlLength(uint32_t &data) const
{
	uint8_t byte;
	if (!TryRead(byte))
	{
		return false;
	}
	if (byte < 254)
	{
		data = byte;
		return true;
	}
	if (length - offset < 3)
	{
		return false;
	}

	data = ((uint32_t)buffer[offset] & 0xFF) |
		   (((uint32_t)buffer[offset + 1] & 0xFF) << 8) |
		   (((uint32_t)buffer[offset + 2] & 0xFF) << 16);
	offset += 3;

	return true;
}
#pragma mark - BufferOutputStream

BufferOutputStream::BufferOutputStream(size_t size_)
	: size(size_),
	  bufferProvided(false)

{
	buffer = reinterpret_cast<unsigned char *>(std::malloc(size_));
	if (!buffer)
		throw std::bad_alloc();
	bufferProvided = false;
}

BufferOutputStream::BufferOutputStream(unsigned char *buffer_, size_t size_)
	: buffer(buffer_),
	  size(size_),
	  bufferProvided(true)
{
}

BufferOutputStream::~BufferOutputStream()
{
	if (!bufferProvided && buffer)
		free(buffer);
}

void BufferOutputStream::WriteByte(unsigned char byte)
{
	this->ExpandBufferIfNeeded(1);
	buffer[offset++] = byte;
}

void BufferOutputStream::WriteInt32(int32_t i)
{
	this->ExpandBufferIfNeeded(4);
	buffer[offset + 3] = (unsigned char)((i >> 24) & 0xFF);
	buffer[offset + 2] = (unsigned char)((i >> 16) & 0xFF);
	buffer[offset + 1] = (unsigned char)((i >> 8) & 0xFF);
	buffer[offset] = (unsigned char)(i & 0xFF);
	offset += 4;
}

void BufferOutputStream::WriteInt64(int64_t i)
{
	this->ExpandBufferIfNeeded(8);
	buffer[offset + 7] = (unsigned char)((i >> 56) & 0xFF);
	buffer[offset + 6] = (unsigned char)((i >> 48) & 0xFF);
	buffer[offset + 5] = (unsigned char)((i >> 40) & 0xFF);
	buffer[offset + 4] = (unsigned char)((i >> 32) & 0xFF);
	buffer[offset + 3] = (unsigned char)((i >> 24) & 0xFF);
	buffer[offset + 2] = (unsigned char)((i >> 16) & 0xFF);
	buffer[offset + 1] = (unsigned char)((i >> 8) & 0xFF);
	buffer[offset] = (unsigned char)(i & 0xFF);
	offset += 8;
}

void BufferOutputStream::WriteInt16(int16_t i)
{
	this->ExpandBufferIfNeeded(2);
	buffer[offset + 1] = (unsigned char)((i >> 8) & 0xFF);
	buffer[offset] = (unsigned char)(i & 0xFF);
	offset += 2;
}

void BufferOutputStream::WriteBytes(const unsigned char *bytes, size_t count)
{
	this->ExpandBufferIfNeeded(count);
	memcpy(buffer + offset, bytes, count);
	offset += count;
}

void BufferOutputStream::WriteBytes(const Buffer &buffer)
{
	WriteBytes(*buffer, buffer.Length());
}

void BufferOutputStream::WriteBytes(const Buffer &buffer, size_t offset, size_t count)
{
	if (offset + count > buffer.Length())
		throw std::out_of_range("offset out of buffer bounds");
	WriteBytes(*buffer + offset, count);
}

void BufferOutputStream::Write(const Serializable &data, const VersionInfo &ver)
{
	data.serialize(*this, ver);
}

unsigned char *BufferOutputStream::GetBuffer()
{
	return buffer;
}

size_t BufferOutputStream::GetOffset()
{
	return offset;
}

size_t BufferOutputStream::GetLength()
{
	return offset;
}

void BufferOutputStream::ExpandBufferIfNeeded(size_t need)
{
	if (offset + need > size)
	{
		if (bufferProvided)
		{
			throw std::out_of_range("buffer overflow");
		}
		size += std::max(need, size_t{1024});
		unsigned char *newBuffer = reinterpret_cast<unsigned char *>(std::realloc(buffer, size));
		if (!newBuffer)
		{
			std::free(buffer);
			buffer = nullptr;
			size = 0;
			throw std::bad_alloc();
		}
		buffer = newBuffer;
	}
}

void BufferOutputStream::Reset()
{
	offset = 0;
}

void BufferOutputStream::Rewind(size_t numBytes)
{
	if (numBytes > offset)
		throw std::out_of_range("buffer underflow");
	offset -= numBytes;
}
void BufferOutputStream::Advance(size_t numBytes)
{
	ExpandBufferIfNeeded(numBytes);
	offset += numBytes;
}
