#include <scheduler.hpp>
#include <awaiters.hpp>
#include <win32_iocp.hpp>

using namespace std::literals;

size_t count = 0;

coro::task2 read_and_send(coro::net::socket_t sock_object) {
	char buff[1024];
	while (true) {
		int recv_result = co_await coro::net::recv(sock_object, buff, 1023, 0);
		if (recv_result <= 0) {
			coro::net::close_socket(sock_object);
			co_return;
		}

		buff[recv_result] = 0;
		printf("recv :: %s\n", buff);

		int send_result = co_await coro::net::send(sock_object, buff, recv_result, 0);
		if (send_result <= 0) {
			coro::net::close_socket(sock_object);
			co_return;
		}
	}
}

coro::task2 service(const char* ip_addr, uint16_t port) {
	coro::net::socket_t sock = coro::net::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, ip_addr, &addr.sin_addr);
	if (coro::net::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
		printf("failed to bind to %s:%d\n", ip_addr, port);
		co_return;
	}

	if (coro::net::listen(sock, 5) != 0) {
		printf("failed to listen\n");
		co_return;
	}

	while (true) {
		sockaddr_in client_addr = {};
		socklen_t client_addr_len = sizeof(client_addr);
		coro::net::socket_t client_sock = co_await coro::net::accept(sock, (sockaddr*)&client_addr, (socklen_t*)&client_addr_len);
		if (client_sock == INVALID_SOCKET) {
			printf("failed to accept\n");
			co_return;
		}

		printf("accepted connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		go(read_and_send(std::move(client_sock)));
	}
}

int main()
{
	coro::start_main_coroutine(service("0.0.0.0", 5432));
	return 0;
}