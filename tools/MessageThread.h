//
// Created by Grishka on 17.06.2018.
//

#ifndef LIBTGVOIP_MESSAGETHREAD_H
#define LIBTGVOIP_MESSAGETHREAD_H

#include "tools/threading.h"
#include "tools/utils.h"
#include <vector>
#include <functional>
#include <algorithm>
#include <atomic>

namespace tgvoip
{
class MessageThread : public Thread
{
public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(MessageThread);
	MessageThread();
	virtual ~MessageThread();
	uint32_t Post(std::function<void()> func, double delay = 0, double interval = 0);
	void Cancel(uint32_t id);
	void CancelSelf();
	void Stop();

	enum
	{
		INVALID_ID = 0
	};

private:
	struct Message
	{
		uint32_t id;
		double deliverAt;
		double interval;
		std::function<void()> func;
	};


	void Run();
	void InsertMessageInternal(Message &m);

	std::atomic<bool> running;
	std::vector<Message> queue;
	Mutex loopMutex;
	Mutex queueAccessMutex;
	uint32_t lastMessageID = 1;
	bool cancelCurrent = false;

#ifdef _WIN32
	HANDLE event;
#else
	pthread_cond_t cond;
#endif
};
} // namespace tgvoip

#endif //LIBTGVOIP_MESSAGETHREAD_H