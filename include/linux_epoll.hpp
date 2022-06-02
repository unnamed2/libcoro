#ifndef _LINUX_EPOLL_H_
#define _LINUX_EPOLL_H_

#ifndef __linux__
#error "This header is only for use on Linux systems."
#endif

#include "scheduler.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>

namespace coro::linux_epoll {

    using epoll_routine = std::function<void(uint32_t, int)>;

    struct epoll_callback_info {
        epoll_routine routine;
        std::string routine_name;
    };

    template<typename _Obj>
    void epoll_routine_t(uint32_t event, int err, void* context) {
        (*reinterpret_cast<_Obj*>(context))(event, err);
    }

    template<typename _Cb>
    void init_epoll_cb(epoll_callback_info* ep, const char* name, _Cb&& cb) {
        ep->routine = cb;
        ep->routine_name = name;
    }
    
    struct epoll_awaiter : coro::thread_awaiter {
        int fd_epoll = 0;
        
        epoll_awaiter() {
            fd_epoll = epoll_create(8);
        }

        virtual void wait(std::vector<coroutine_handle>& handles) {
            struct epoll_event events[10];
            int ret = epoll_wait(fd_epoll, events, 10, -1);
            if(ret <= 0) {
                printf("epoll_wait failed! %d\r\n", errno);
                fflush(stdout);
                return;
            }

            for(int i = 0 ; i < ret; i++) {
                epoll_callback_info* c = (epoll_callback_info*)events[i].data.ptr;
                printf("epoll calling %s\n", c->routine_name.c_str());
                if(c->routine != nullptr) {
                    c->routine(events[i].events, errno);
                }
            }
        }

		virtual bool should_suspend() const override {
            return false;
        }

        virtual bool add_fd(int fd, uint32_t event, epoll_callback_info* ci) {
            epoll_event ev;
            ev.events = event;
            ev.data.ptr = ci;
            return epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd, &ev) == 0;
        }

        virtual bool remove_fd(int fd) {
            return epoll_ctl(fd_epoll, EPOLL_CTL_DEL, fd, nullptr) == 0;
        }
    };

    epoll_awaiter* get_epoll_awaiter();
}

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

namespace coro::net 
{
    using socket_t = int;

    inline socket_t socket(int af, int type, int protocol) {
		socket_t s = ::socket(af, type, protocol);
        int flag = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flag | O_NONBLOCK);
        return s;
	}

	inline int bind(socket_t socket, const sockaddr* addr, int addrlen) {
        int val = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (const void*)&val, sizeof(int));
		return ::bind(socket, addr, addrlen);
	}

	inline int listen(socket_t socket, int backlog) {
		return ::listen(socket, backlog);
	}

    struct epoll_accept_awaiter {
        socket_t fd;
        sockaddr* sock;
        socklen_t* namelen;

        socket_t result;

        linux_epoll::epoll_callback_info cb_info;

        epoll_accept_awaiter(socket_t fd, sockaddr* addr, socklen_t* namelen) : fd(fd), sock(addr), namelen(namelen){

        }

        bool await_ready() {
            result = ::accept(fd, sock, namelen);
            if(result > 0)
                return true;
            bool res = errno != EAGAIN;
            printf("await_ready return %s\n", res?"true":"false");
            return res;
        }

        bool await_suspend(coroutine_handle handle) {
            linux_epoll::init_epoll_cb(&cb_info, "accept_callback", [this, handle](uint32_t event, int error){
                fflush(stdout);
                if(!linux_epoll::get_epoll_awaiter()->remove_fd(fd))
                    printf("remove fd %d failed!\n", fd);
                printf("accept await_suspend : event = %d, errno = %d\n", event, errno);
                if(event & EPOLLIN) {
                    result = ::accept(fd, sock, namelen);
                    printf("await_suspend callback: result = %d, errno = %d\n", result, errno);
                } else {
                    // something went wrong
                    result = -1;
                    printf("await_suspend callback: result = %d\n", result);
                }
                go(handle);
                return;
            });

            if(linux_epoll::get_epoll_awaiter()->add_fd(fd, EPOLLIN|EPOLLERR|EPOLLONESHOT, &cb_info)) {
                park(handle);
                return true;
            }
            return false;
        }

        socket_t await_resume() {
            printf("accept return %d\n", result);
            return result;
        }
    };

    inline epoll_accept_awaiter accept(socket_t fd, sockaddr* addr, socklen_t* namelen) {
        return {fd, addr, namelen};
    }

    struct epoll_recv_awaiter {
        socket_t fd;
        char* buffer;
        size_t bufflen;
        int flag;

        size_t already_readed = 0;
        int result;

        linux_epoll::epoll_callback_info cb_info;

        epoll_recv_awaiter(socket_t fd, char* buffer, size_t len, int flag) : fd(fd), buffer(buffer), bufflen(len), flag(flag){

        }

        bool await_ready() {
            return false;
        }

        bool await_suspend(coroutine_handle handle) {
            already_readed = 0;
            linux_epoll::init_epoll_cb(&cb_info, "recv_callback", [this, handle](uint32_t event, int error){
                if(event & EPOLLIN) {
                    result = ::recv(fd, buffer + already_readed, bufflen - already_readed, flag);
                    if(result > 0) {
                        already_readed += result;
                        if(flag & MSG_WAITALL && already_readed != bufflen) {
                            return;
                        } else {
                            result += already_readed;
                        }
                    }
                } else {
                    result = -1;
                }
                linux_epoll::get_epoll_awaiter()->remove_fd(fd);
                go(handle);
                return;
            });

            if(linux_epoll::get_epoll_awaiter()->add_fd(fd, EPOLLIN|EPOLLERR, &cb_info)) {
                park(handle);
                return true;
            }
            return false; 
        }

        int await_resume() {
            return result;
        }
    };

    inline epoll_recv_awaiter recv(socket_t fd, char* buff, size_t len, int flag) {
        return { fd, buff, len, flag };
    }

    struct epoll_send_awaiter  {
        socket_t fd;
        const char* buffer;
        size_t bufflen;
        int flag;

        int result;
        linux_epoll::epoll_callback_info cb_info;
        
        epoll_send_awaiter(socket_t fd, const char* buff, size_t len, int flag)
            :fd(fd), buffer(buff), bufflen(len), flag(flag), result(-1) {}

        constexpr bool await_ready() const { return false; }

        bool await_suspend(coroutine_handle handle) {
            linux_epoll::init_epoll_cb(&cb_info, "send_callback", [this, handle](uint32_t event, int err){
                linux_epoll::get_epoll_awaiter()->remove_fd(fd);
                if(event & EPOLLOUT) {
                    result = ::send(fd, buffer, bufflen, flag);
                } else {
                    result = -1;
                }
                go(handle);
            });

            if(!linux_epoll::get_epoll_awaiter()->add_fd(fd, EPOLLOUT|EPOLLONESHOT|EPOLLERR, &cb_info)) {
                result = -1;
                return false;
            }

            park(handle);
            return true;
        }

        int await_resume() const {
            return result;
        }
    };

    inline epoll_send_awaiter send(socket_t fd, const char* buffer, size_t len, int flag) {
        return { fd, buffer, len, flag };
    }

    inline void close_socket(socket_t socket) {
		close(socket);
	}

    constexpr inline socket_t invalid_socket = -1;
}

#endif