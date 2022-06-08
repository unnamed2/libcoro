#include <scheduler.hpp>

#include <thread>
#include <mutex>
#include <condition_variable>
#ifdef min
#define min_undefined
#undef min
#endif

#ifdef max
#define max_undefined
#undef max
#endif

namespace coro {

	namespace details {
		template<typename arg_pool>
		struct thread_worker {
		private:

			arg_pool args;

			std::mutex mtx;
			std::condition_variable cv;
			size_t free_threads = 0;
			size_t max_threads = 0;
			std::vector<std::thread> worker_threads;

			void worker_thread_main() {
				std::unique_lock<std::mutex> ul(mtx);
				while (true) {
					free_threads++;
					cv.wait(ul, [this]() {return args.size(); });
					free_threads--;
					auto _fx_arg = args.acquire_one();
					ul.unlock();

					bool result = args.invoke(_fx_arg);

					ul.lock();
					if (result) {
						args.add(std::move(_fx_arg));
					}
				}
			}
		public:
			template<typename... Args>
			thread_worker(size_t max_threads = std::numeric_limits<size_t>::max(), Args&&... args) : max_threads(max_threads), args(std::forward<Args>(args)...) {
			}


			template<typename T>
			void push_arg(T&& arg_type) {
				std::lock_guard<std::mutex> lg(mtx);
				if (free_threads == 0 && worker_threads.size() < max_threads) {
					worker_threads.emplace_back(&thread_worker<arg_pool>::worker_thread_main, this);
				}

				args.add(std::forward<T>(arg_type));

				cv.notify_one();
			}

			template<typename T>
			void push_arg(T* values, size_t count) {
				std::lock_guard<std::mutex> lg(mtx);
				for (size_t i = 0; i < count; i++) {
					if (free_threads < i + 1 && worker_threads.size() < max_threads) {
						worker_threads.emplace_back(&thread_worker<arg_pool>::worker_thread_main, this);
					}
					args.add(std::move(values[i]));
				}
				cv.notify_all();
			}
		};
	}

	struct awaiter_pool {
		std::vector<thread_awaiter*> awaiters;
		size_t size() const { return awaiters.size(); }
		void add(thread_awaiter* waiter) {
			awaiters.push_back(waiter);
		}

		auto acquire_one() {
			auto value = awaiters.back();
			awaiters.pop_back();
			return value;
		}

		bool invoke(thread_awaiter* awaiter);
	};

	struct thread_scheduler {
		details::thread_worker<awaiter_pool> workers;
	public:
		void schedule(thread_awaiter* awaiter) {
			workers.push_arg(awaiter);
		}
	} *__thread_scheduler = new thread_scheduler();

	struct coroutine_scheduler {
	private:
		std::mutex mtx, mtx_main;

		std::stop_source stop_;
		std::vector<coroutine_handle> coroutines;
		std::condition_variable cv_schedule, cv_main_done;
		std::vector<std::thread> worker_threads;
		std::atomic<size_t> free_threads = 0;
		coroutine_handle main_handle;
	public:

		coroutine_scheduler(coroutine_handle main_handle) : main_handle(main_handle) {
			coroutines.push_back(main_handle);
			buy(1);
		}

		void schedule(coroutine_handle handle) {
			std::lock_guard<std::mutex> lg(mtx);
			buy(1);
			coroutines.emplace_back(handle);
			cv_schedule.notify_one();
		}

		void schedule(std::vector<coroutine_handle>& handles) {
			std::lock_guard<std::mutex> lg(mtx);
			buy(handles.size());
			for (auto v : handles) {
				coroutines.emplace_back(v);
			}
			cv_schedule.notify_all();
		}

		void stop_schedule() {
			stop_.request_stop();
			cv_schedule.notify_all();
			for (auto& v : worker_threads) {
				v.join();
			}
			worker_threads.clear();
			free_threads = 0;
		}

		void wait_for_main() {
			std::unique_lock<std::mutex> ul(mtx_main);
			cv_main_done.wait(ul, [this]() {return main_handle == nullptr; });
		}
	private:

		void buy(size_t count) {
			static size_t max_count = (size_t)std::thread::hardware_concurrency();
			size_t f = free_threads.load();
			auto upto = std::min(max_count, worker_threads.size() + count - f);

			for (size_t i = worker_threads.size(); i < upto; i++) {
				worker_threads.emplace_back(&coroutine_scheduler::worker_thread_main, this, stop_.get_token());
			}
		}

		size_t random() {
			static uint64_t s[2] = { (uint64_t)rand(),  (uint64_t)rand() + 1 };
			uint64_t a = s[0];
			uint64_t b = s[1];

			s[0] = b;
			a ^= a << 23;
			a ^= a >> 18;
			a ^= b;
			a ^= b >> 5;
			s[1] = a;

			return a + b;
		}

		auto coro_select() {
			size_t select_id = random() % coroutines.size();
			auto handle = coroutines[select_id];
			if (select_id != coroutines.size() - 1) {
				coroutines[select_id] = coroutines.back();
			}
			coroutines.pop_back();
			return handle;
		}

		void worker_thread_main(std::stop_token token) {
			std::unique_lock<std::mutex> ul(mtx);
			while (true) {
				free_threads++;
				cv_schedule.wait(ul, [this, token]() {return coroutines.size() > 0 || token.stop_requested(); });
				free_threads--;

				if (token.stop_requested())
					return;

				auto handle = coro_select();
				if (handle.done()) {
					handle.destroy();
					continue;
				}
				ul.unlock();

				handle.resume();

				ul.lock();
				if (handle.done()) {
					if (handle == main_handle) {
						std::scoped_lock<std::mutex> lg(mtx_main);
						main_handle = nullptr;
						cv_main_done.notify_all();
					}
					handle.destroy();
				}
				else if (handle.promise().status == task_status::ready) {
					coroutines.push_back(handle);
				}
			}
		}
	} *__coroutine_scheduler;


	inline bool awaiter_pool::invoke(thread_awaiter* awaiter) {
		std::vector<coroutine_handle> handles;
		awaiter->wait(handles);
		__coroutine_scheduler->schedule(handles);
		return !awaiter->should_suspend();
	}

	void start_main_coroutine(coroutine_handle main_handle) {
		if (__coroutine_scheduler != nullptr) {
			return;
		}

		__coroutine_scheduler = new coroutine_scheduler(main_handle);
		__coroutine_scheduler->wait_for_main();
		__coroutine_scheduler->stop_schedule();
	}

	void park(coroutine_handle handle) {
		auto& status_ref = handle.promise().status;
		if (status_ref == task_status::ready || status_ref == task_status::created)
			status_ref = task_status::suspend;
	}

	void go(coroutine_handle handle) {
		auto& status_ref = handle.promise().status;
		if (status_ref == task_status::created || status_ref == task_status::suspend) {
			status_ref = task_status::ready;
			__coroutine_scheduler->schedule(handle);
		}
	}
}


#ifdef WIN32
#include <win32_iocp.hpp>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#endif
namespace coro::win32 {
	iocp_awaiter* get_iocp_awaiter() {
		static iocp_awaiter iocp_awaiter;
		static std::once_flag flag;
		std::call_once(flag, []() {
			__thread_scheduler->schedule(&iocp_awaiter);
			});
		return &iocp_awaiter;
	}

	struct win32_wsa_initializer {
		win32_wsa_initializer() {
			WSADATA wsaData;
			WSAStartup(MAKEWORD(2, 2), &wsaData);
		}
		~win32_wsa_initializer() {
			WSACleanup();
		}
	} __initializer;
}
#endif

#ifdef __linux__
#include <linux_epoll.hpp>

namespace coro::linux_epoll {
	coro::linux_epoll::epoll_awaiter* get_epoll_awaiter() {
		static coro::linux_epoll::epoll_awaiter* instance = nullptr;
		static std::once_flag flag;
		std::call_once(flag, []() {
			instance = new coro::linux_epoll::epoll_awaiter();
			__thread_scheduler->schedule(instance);
			});
		return instance;
	}
}
#endif