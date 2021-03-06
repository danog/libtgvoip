//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_BLOCKINGQUEUE_H
#define LIBTGVOIP_BLOCKINGQUEUE_H

#include <stdlib.h>
#include <list>
#include "tools/threading.h"
#include "tools/utils.h"

using namespace std;

namespace tgvoip
{

template <typename T>
class BlockingQueue
{
public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(BlockingQueue);
	BlockingQueue(size_t capacity) : semaphore(capacity, 0)
	{
		this->capacity = capacity;
		//overflowCallback = NULL;
	};

	~BlockingQueue()
	{
		semaphore.Release();
	}

	void Put(T thing)
	{
		MutexGuard sync(mutex);
		queue.push_back(std::move(thing));
		if (queue.size() > capacity) {
			abort(); // I still don't like this
		}
		semaphore.Release();
	}

	T GetBlocking()
	{
		semaphore.Acquire();
		MutexGuard sync(mutex);
		return GetInternal();
	}

	T Get()
	{
		MutexGuard sync(mutex);
		if (queue.size() > 0)
			semaphore.Acquire();
		return GetInternal();
	}

	size_t Size()
	{
		return queue.size();
	}

	void PrepareDealloc()
	{
	}
private:
	T GetInternal()
	{
		//if(queue.size()==0)
		//	return NULL;
		T r = std::move(queue.front());
		queue.pop_front();
		return r;
	}

	list<T> queue;
	size_t capacity;
	//tgvoip_lock_t lock;
	Semaphore semaphore;
	Mutex mutex;
	//void (*overflowCallback)(T);
};
} // namespace tgvoip

#endif //LIBTGVOIP_BLOCKINGQUEUE_H
