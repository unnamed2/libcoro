#include <coroutine>
#include <atomic>
#include <queue>
#include <scheduler.hpp>
#include <thread>
#include <functional>

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
					park(h);
					m.waiters.push(h);
					return true;

				}
				// If we successfully acquire the lock, 
				// we don't have to do anything
				return false;
			}

			void await_resume() {
			}
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

	struct condition_variable {
		spin_lock slock;

		struct waiter_info {
			coroutine_handle handle;
			mutex& mtx;
		};
		std::queue<waiter_info> waiters;

		struct condition_variable_waiter {
			condition_variable& cv;
			mutex& mtx;
			condition_variable_waiter(condition_variable& cv, mutex& mtx) : cv(cv), mtx(mtx) {}

			bool await_ready() {
				return false;
			}

			bool await_suspend(coroutine_handle h) {
				std::lock_guard<spin_lock> lg(cv.slock);

				cv.waiters.push({ h, mtx });
				park(h);
				mtx.unlock();
				return true;
			}

			void await_resume() { }
		};


		condition_variable_waiter wait(mutex& mtx) {
			return condition_variable_waiter(*this, mtx);
		}

		void notify_one() {
			std::lock_guard<spin_lock> lg(slock);
			if (!waiters.empty()) {
				auto h = waiters.front();
				waiters.pop();
				wakeup(h);
			}
		}

		void notify_all() {
			std::lock_guard<spin_lock> lg(slock);
			while (!waiters.empty()) {
				auto h = waiters.front();
				waiters.pop();
				wakeup(h);
			}
		}

		void wakeup(waiter_info& w) {
			auto mutex_awaiter = w.mtx.lock();
			if (!mutex_awaiter.await_suspend(w.handle)) {
				go(w.handle);
			}
		}
	};

	struct wait_group {
		std::atomic<int> expect_count;

		spin_lock lock;
		std::queue<coroutine_handle> waiters;

		wait_group(int n = 0) : expect_count(n) {}

		void add(int count) {
			expect_count += count;
		}

		void done() {
			int old_count = expect_count.fetch_sub(1);
			if (old_count == 1) {
				std::lock_guard<spin_lock> lg(lock);
				while (!waiters.empty()) {
					auto h = waiters.front();
					waiters.pop();
					go(h);
				}
			}
		}

		struct wg_awaiter {
			wait_group& wg;

			wg_awaiter(wait_group& wg) : wg(wg) {}

			bool await_ready() {
				return wg.expect_count == 0;
			}

			void await_suspend(coroutine_handle h) {
				std::lock_guard<spin_lock> lg(wg.lock);
				wg.waiters.push(h);
				park(h);
			}

			void await_resume() {}
		};

		wg_awaiter wait() {
			return wg_awaiter(*this);
		}
	};
}