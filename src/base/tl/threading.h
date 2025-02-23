#pragma once

#include "../system.h"

/*
	atomic_inc - should return the value after increment
	atomic_dec - should return the value after decrement
	atomic_compswap - should return the value before the eventual swap
	sync_barrier - creates a full hardware fence
*/

#if defined(__GNUC__)

	inline unsigned atomic_inc(volatile unsigned *pValue)
	{
		return __sync_add_and_fetch(pValue, 1);
	}

	inline unsigned atomic_dec(volatile unsigned *pValue)
	{
		return __sync_add_and_fetch(pValue, -1);
	}

	inline unsigned atomic_compswap(volatile unsigned *pValue, unsigned comperand, unsigned value)
	{
		return __sync_val_compare_and_swap(pValue, comperand, value);
	}

	inline void sync_barrier()
	{
		__sync_synchronize();
	}

#elif defined(_MSC_VER)
	#include <intrin.h>

	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>

	inline unsigned atomic_inc(volatile unsigned *pValue)
	{
		return _InterlockedIncrement((volatile long *)pValue);
	}
	
	inline unsigned atomic_dec(volatile unsigned *pValue)
	{
		return _InterlockedDecrement((volatile long *)pValue);
	}

	inline unsigned atomic_compswap(volatile unsigned *pValue, unsigned comperand, unsigned value)
	{
		return _InterlockedCompareExchange((volatile long *)pValue, (long)value, (long)comperand);
	}

	inline void sync_barrier()
	{
		MemoryBarrier();
	}
#else
	#error missing atomic implementation for this compiler
#endif

#if defined(CONF_PLATFORM_MACOSX)
	/*
		use semaphore provided by SDL on macosx
	*/
#else
	class semaphore
	{
		SEMAPHORE sem;
	public:
		semaphore() { semaphore_init(&sem); }
		~semaphore() { semaphore_destroy(&sem); }
		void wait() { semaphore_wait(&sem); }
		void signal() { semaphore_signal(&sem); }
	};
#endif

class lock
{
	friend class scope_lock;

	LOCK var;

	void take() { lock_wait(var); }
	void release() { lock_unlock(var); }

public:
	lock()
	{
		var = lock_create();
	}

	~lock()
	{
		lock_destroy(var);
	}
};

class scope_lock
{
	lock *var;
public:
	scope_lock(lock *l)
	{
		var = l;
		var->take();
	}

	~scope_lock()
	{
		var->release();
	}
};

class CSemaphore
{
	SEMAPHORE m_Sem;
	std::atomic_int m_Count{0};

public:
	CSemaphore() { semaphore_init(&m_Sem); }
	~CSemaphore() { semaphore_destroy(&m_Sem); }
	CSemaphore(const CSemaphore &) = delete;
	int GetApproximateValue() { return m_Count.load(); }
	void Wait()
	{
		semaphore_wait(&m_Sem);
		m_Count.fetch_sub(1);
	}
	void Signal()
	{
		m_Count.fetch_add(1);
		semaphore_signal(&m_Sem);
	}
};