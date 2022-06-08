#ifndef _CORO_SCHEDULER2_
#define _CORO_SCHEDULER2_

#include <coroutine>
#include <vector>
#include <mutex>
#include <map>
#include <memory>

namespace coro {

	enum class task_status {
		created,
		ready,
		suspend,
		done,
	};

	struct task2 {
		struct promise_type {
			promise_type() {}

			task2 get_return_object() {
				return task2{
					std::coroutine_handle<promise_type>::from_promise(*this)
				};
			}

			auto initial_suspend() noexcept {
				return std::suspend_always{};
			}
			
			struct task2_final_suspend {
				constexpr bool await_ready() const noexcept { return false; }
				
				bool await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
					handle.promise().status = task_status::done;
					return true;
				}
				
				constexpr void await_resume() const noexcept {}
			};

			auto final_suspend() noexcept {
				return task2_final_suspend{};
			}

			void unhandled_exception() {}

			void return_void() {}

			task_status status = task_status::created;

		};

		using handle_type = std::coroutine_handle<promise_type>;

		handle_type* operator->() { return &handle; }
		const handle_type* operator->() const { return &handle; }

		task2(handle_type handle = nullptr) : handle{ handle } {}
		task2(task2&& other) : handle{ other.handle } { other.handle = nullptr; }
		task2(const task2& other) = delete;
		task2& operator=(const task2& other) = delete;
		task2& operator=(task2&& other) {
			handle = other.handle;
			other.handle = nullptr;
			return *this;
		}

		operator handle_type () const { return handle; }

		auto& promise() const noexcept { return handle.promise(); }
		auto& get() const noexcept { return handle; }

	private:
		handle_type handle;
	};

	using coroutine_handle = task2::handle_type;

	struct thread_awaiter {
		virtual void wait(std::vector<coroutine_handle>& handles) = 0;
		virtual bool should_suspend() const = 0;
	};

	void park(coroutine_handle handle);

	void go(coroutine_handle handle);
	
	void start_main_coroutine(coroutine_handle main_handle);
	
}

#endif