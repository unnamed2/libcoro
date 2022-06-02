#ifndef _CORO_WIN32_IOCP_H_
#define _CORO_WIN32_IOCP_H_

#ifndef WIN32
#error "This file should only be compiled for Windows"
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <Windows.h>

#include <stdint.h>

#include "scheduler.hpp"

namespace coro::win32 {
	
	using iocp_routine_callback = void(*)(void* context, DWORD error_code , size_t bytes_transferred);

	template<typename _Caller>
	void iocp_routine(void* context,DWORD error_code, size_t bytes_transferred) {
		(*reinterpret_cast<_Caller*>(context))(error_code, bytes_transferred);
	}
	
	struct iocp_overlapped {
		OVERLAPPED overlapped;
		iocp_routine_callback routine;
		void* context;
	};
	
	struct iocp_awaiter : thread_awaiter {
		HANDLE iocp_handle;
		
		iocp_awaiter() {
			iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
		}
		virtual void wait(std::vector<coroutine_handle>& handles) override {
			DWORD bytes_transferred;
			LPOVERLAPPED overlapped;
			ULONG_PTR completion_key;
			if (GetQueuedCompletionStatus(iocp_handle, &bytes_transferred, &completion_key, &overlapped, INFINITE)) {
				iocp_overlapped* overlapped2 = reinterpret_cast<iocp_overlapped*>(overlapped);
				if (overlapped2->routine != nullptr) {
					printf("call routine %p \n", overlapped2->routine);
					overlapped2->routine(overlapped2->context, 0, bytes_transferred);
					printf("called routine %p \n", overlapped2->routine);
				}
			}
			else {
				if (overlapped != nullptr) {
					iocp_overlapped* overlapped2 = reinterpret_cast<iocp_overlapped*>(overlapped);
					if (overlapped2->routine != nullptr)
						overlapped2->routine(overlapped2->context, WSAGetLastError(), bytes_transferred);
				}
			}
		} 
		
		virtual bool should_suspend() const override {
			return false;
		}

		void add_awaiter(HANDLE handle_type) {
			HANDLE handle = CreateIoCompletionPort(handle_type, iocp_handle, (ULONG_PTR)handle_type, 0);
		}
	};

	iocp_awaiter* get_iocp_awaiter();

	template<typename _Caller>
	inline void init_overlapped(iocp_overlapped* overlapped, _Caller&& cb) {
		memset(overlapped, 0, sizeof(iocp_overlapped));
		overlapped->routine = &iocp_routine<_Caller>;
		overlapped->context = std::addressof(cb);
	}
}

namespace coro::net
{
	using socket_t = SOCKET;

	inline socket_t socket(int af, int type, int protocol) {
		SOCKET sock = ::WSASocketW(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
		win32::get_iocp_awaiter()->add_awaiter((HANDLE)sock);
		return sock;
	}

	inline int bind(socket_t socket, const sockaddr* addr, int addrlen) {
		return ::bind(socket, addr, addrlen);
	}

	inline int listen(socket_t socket, int backlog) {
		return ::listen(socket, backlog);
	}

	struct accept_awaitable {
		
		win32::iocp_overlapped overlapped;
		socket_t socket;
		SOCKET accepted_socket;
		bool ok = false;

		sockaddr_storage local_address[2];
		
		sockaddr* wait_addr;
		int* wait_addrlen;
		
		accept_awaitable(socket_t sock, sockaddr* addr, int* namelen): socket(sock), wait_addr(addr), wait_addrlen(namelen) {
			
		}
		
		bool await_ready() {
			sockaddr_storage addr;
			int addrlen = sizeof(addr);
			if (getsockname(socket, (sockaddr*)&addr, &addrlen) != 0) {
				return true;
			}
			
			accepted_socket = ::WSASocketW(addr.ss_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
			if(accepted_socket == INVALID_SOCKET) {
				return true;
			}
			return false;
		}
		
		bool await_suspend(coroutine_handle handle) {
			win32::init_overlapped(&overlapped, [this, handle](DWORD err, size_t) {
				ok = err == 0;
				go(handle);
				});
			BOOL ret = AcceptEx(socket, accepted_socket, local_address, 0, sizeof(sockaddr_storage), sizeof(sockaddr_storage), nullptr, &overlapped.overlapped);
			if(ret == FALSE && WSAGetLastError() != WSA_IO_PENDING) {
				return false;
			}
			park(handle);
			return true;
		}

		socket_t await_resume() {
			if (!ok) {
				if (accepted_socket != INVALID_SOCKET) {
					closesocket(accepted_socket);
				}
				return INVALID_SOCKET;
			}
			if (wait_addr != nullptr) {
				memcpy(wait_addr, local_address + 1, min(*wait_addrlen, sizeof(sockaddr_storage)));
			}
			win32::get_iocp_awaiter()->add_awaiter((HANDLE)accepted_socket);
			return accepted_socket;
		}

	};

	[[nodiscard]] inline accept_awaitable accept(socket_t socket, sockaddr* addr, int* namelen) {
		return accept_awaitable(socket, addr, namelen);
	}

	struct recv_awaitable {

		socket_t socket;
		WSABUF buf;
		DWORD flag;
		int ret_value = -1;
		DWORD recv_len;
		win32::iocp_overlapped overlapped;
		
		recv_awaitable(socket_t sock, char* data, int len, int flag) : socket(sock), flag(flag), buf{ (ULONG)len, data } {
		}
			
		bool await_ready() {
			return false;
		}
		
		bool await_suspend(coroutine_handle handle) {
			win32::init_overlapped(&overlapped, [this, handle](DWORD errorCode , size_t trans) {
				WSASetLastError(errorCode);
				if (errorCode == 0)
					ret_value = (int)trans;
				else
					ret_value = -1;
				go(handle);
				});
			int ret = WSARecv(socket, (WSABUF*)&buf, 1, &recv_len,&flag, &overlapped.overlapped, nullptr);
			if(ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
				return false;
			}
			/* 
			WSARecv returning 0 means that the operation has completed,
			but the IOCP object will still receive a notification,
			so we suspend the coroutine in order to ensure that the overlapped is valid.
			*/
			park(handle);
			return true;
		}

		int await_resume() {
			return ret_value;
		}
	};

	[[nodiscard]] inline recv_awaitable recv(socket_t socket, char* data, int len, int flag) {
		return recv_awaitable(socket, data, len, flag);
	}

	struct send_awaiter {
		
		socket_t socket;
		WSABUF buf;
		DWORD flag;
		
		int ret_value = -1;
		DWORD send_len;
		win32::iocp_overlapped overlapped;
		
		send_awaiter(socket_t sock, const char* data, int len, int flag) : socket(sock), flag(flag), buf{ (ULONG)len, const_cast<char*>(data) } {
		}
			
		bool await_ready() {
			return false;
		}
		
		bool await_suspend(coroutine_handle handle) {
			win32::init_overlapped(&overlapped, [this, handle](DWORD errorCode, size_t trans) {
				WSASetLastError(errorCode);
				if (errorCode == 0)
					ret_value = -1;
				else
					ret_value = (int)trans;

				go(handle);
				});

			int ret = WSASend(socket, (WSABUF*)&buf, 1, &send_len, flag, &overlapped.overlapped, nullptr);
			if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
				return false;
			}
			park(handle);
			return true;
		}

		int await_resume() {
			return ret_value;
		}

	};

	[[nodiscard]] inline send_awaiter send(socket_t socket, const char* data, int len, int flag) {
		return send_awaiter(socket, data, len, flag);
	}

	inline void close_socket(socket_t socket) {
		closesocket(socket);
	}

	struct connect_awaiter {
		int connect_result;

		constexpr bool await_ready() {
			return true;
		}

		void await_suspend(coroutine_handle h) {
		}

		int await_resume() const {
			return connect_result;
		}
	};

	[[nodiscard]] inline connect_awaiter connect(socket_t sock, const sockaddr* addr, int namelen) {
		return { ::connect(sock, addr, namelen) };
	}

	constexpr inline socket_t invalid_socket = INVALID_SOCKET;
}


#endif