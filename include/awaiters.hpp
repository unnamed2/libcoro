#include <coroutine>
#include <atomic>
#include <queue>
#include <scheduler.hpp>

namespace coro {
    struct spin_lock {
		std::atomic_flag flag = ATOMIC_FLAG_INIT;
		
		spin_lock() {
			flag.clear();
		}
		void lock() {
			while (flag.test_and_set(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
		}
		
		void unlock() {
			flag.clear(std::memory_order_release);
		}
    };

	struct mutex {
	private:
		spin_lock waiters_lock;
		std::atomic_flag flag;
		std::queue<std::coroutine_handle<task2::promise_type>> waiters;
	public:
		
		mutex() {
			flag.clear();
		}
		struct mutex_lock_awaiter {
			mutex& m;
			
			mutex_lock_awaiter(mutex& m) : m(m) {}
			
			bool await_ready() {
				//If the flag is not set, then we can try to steal this flag to get the lock
				return !m.flag.test_and_set(std::memory_order_acquire);
			}
			
			bool await_suspend(std::coroutine_handle<task2::promise_type> h) {
				std::lock_guard<spin_lock> lg(m.waiters_lock);
				// we need to check again 
				if (m.flag.test_and_set(std::memory_order_acquire)) {
					// If we still don't acquire the lock, 
					// then add the current coroutine to the wait queue
					h.promise().status = task_status::suspend;
					m.waiters.push(h);
					return true;
					
				}
				// If we successfully acquire the lock, 
				// we don't have to do anything, 
				// just wait for the scheduler to schedule the current coroutine again
				return false;
			}
			
			void await_resume() {}
		};

		mutex_lock_awaiter lock() {
			// co_await mutex_lock_awaiter(m);
			return mutex_lock_awaiter(*this);
		}

		void unlock() {
			waiters_lock.lock();
			if (!waiters.empty()) {
				// one of the suspended coroutines can acquire this lock
				auto h = waiters.front();
				waiters.pop();
				//let h re-enter the scheduler for scheduling
				go(h);

				//flag should not be cleared because h has been woken up and h is set to the owner of the lock 
			}
			else {
				flag.clear(std::memory_order::release);
			}
			waiters_lock.unlock();
		}
	};

	constexpr std::suspend_always yield() { return {}; }
	
}