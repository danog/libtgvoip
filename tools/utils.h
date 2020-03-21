//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#pragma once
#include <numeric>
#include <algorithm>
#include <assert.h>

#define TGVOIP_DISALLOW_COPY_AND_ASSIGN(TypeName) \
TypeName(const TypeName&) = delete;   \
void operator=(TypeName&) = delete

#define TGVOIP_MOVE_ONLY(TypeName) \
TGVOIP_DISALLOW_COPY_AND_ASSIGN(TypeName); \
TypeName(TypeName&&) = default; \
TypeName& operator=(TypeName&&) = default

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
		for (const T &i : data)
		{
			if (i)
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

	constexpr size_t Size() const
	{
		return size;
	}

private:
	std::array<T, size> data{};
	ptrdiff_t offset = 0;
};
