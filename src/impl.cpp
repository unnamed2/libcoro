#include <scheduler.hpp>

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
	} * __thread_scheduler = new thread_scheduler();

	struct coroutine_pool {
		std::vector<coroutine_handle> handles;

		coroutine_handle main_handle;
		std::condition_variable& cv;
		coroutine_pool(coroutine_handle main, std::condition_variable& cv) : main_handle(main), cv(cv) {
		}

		size_t size() const { return handles.size(); }

		void add(coroutine_handle handle) {
			handles.emplace_back(handle);
		}

		auto acquire_one() {
			int select_id = rand() % handles.size();
			auto handle = handles[select_id];
			handles[select_id] = handles.back();
			handles.pop_back();
			return handle;
		}

		bool invoke(coroutine_handle handle) {
			handle.resume();

			if (handle.done()) {
				if (handle == main_handle) {
					cv.notify_all();
				}
				return false;
			}

			return handle.promise().status == task_status::ready;
		}
	};

	struct coroutine_scheduler {
		details::thread_worker<coroutine_pool> workers;
		std::mutex mtx;
		coroutine_handle main_coroutine;

		std::condition_variable cv;
	public:
		coroutine_scheduler(coroutine_handle main_handle)
			: workers(std::thread::hardware_concurrency() - 1, main_handle, cv), main_coroutine(main_handle) {

		}

		void schedule(coroutine_handle handle) {
			workers.push_arg(handle);
		}

		void schedule(const std::vector<coroutine_handle>& handles) {
			workers.push_arg(handles.data(), handles.size());
		}

		void wait_for_main_coroutine() {
			std::unique_lock ul(mtx);
			cv.wait(ul, [this]() {
				return main_coroutine.done();
			});
		}
	} * __coroutine_scheduler = nullptr;
	
	
	inline bool awaiter_pool::invoke(thread_awaiter* awaiter) {
		std::vector<coroutine_handle> handles;
		awaiter->wait(handles);
		__coroutine_scheduler->schedule(handles);
		return !awaiter->should_suspend();
	}
	
	void start_main_coroutine(coroutine_handle main_handle) {
		if(__coroutine_scheduler != nullptr) {
			return;
		}
		
		__coroutine_scheduler = new coroutine_scheduler(main_handle);
		go(main_handle);
		__coroutine_scheduler->wait_for_main_coroutine();
	}

	void park(coroutine_handle handle) {
		handle.promise().status = task_status::suspend;
	}

	void go(coroutine_handle handle) {
		handle.promise().status = task_status::ready;
		__coroutine_scheduler->schedule(handle);
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