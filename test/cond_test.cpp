#include <scheduler.hpp>
#include <awaiters.hpp>
#include <iostream>

coro::task2 consumer(int cid, coro::condition_variable& cv, coro::mutex& mtx, std::queue<int>& q, coro::wait_group& wg, volatile bool& fin) {
	co_await mtx.lock();
	while (true) {
		while (q.empty() && !fin)
			co_await cv.wait(mtx);
		if (fin) break;
		int v = q.front();
		q.pop();
		std::cout << "consumer " << cid << ": " << v << std::endl;
	}

	printf("CONSUMER - %d\n", cid);
	mtx.unlock();

	wg.done();
}


coro::task2 producer(int pid, coro::condition_variable& cv, coro::mutex& mtx, std::queue<int>& q, coro::wait_group& wg) {
	for (int i = 0; i < 10; ++i) {
		//std::this_thread::sleep_for(std::chrono::milliseconds(40 + pid * 10));
		co_await mtx.lock();
		q.push(i + pid * 1000);
		std::cout << "producer " << pid << ": " << i << std::endl;
		cv.notify_one();
		mtx.unlock();
	}
	printf("PRODUCER - %d\n", pid);
	wg.done();
}

coro::task2 coro_main() {

	constexpr size_t consumer_count = 70;
	constexpr size_t producer_count = 100;

	std::queue<int> q;
	coro::condition_variable cv;
	coro::mutex mtx;
	bool fin = false;
	coro::wait_group wg(producer_count);
	for (int i = 0; i < consumer_count; i++) {
		go(consumer(i, cv, mtx, q, wg, fin));
	}
	for (int i = 0; i < producer_count; i++) {
		go(producer(i, cv, mtx, q, wg));
	}
	co_await wg.wait();
	wg.add(consumer_count);
	fin = true;
	cv.notify_all();
	co_await wg.wait();
	co_return;
}

int main() {
	coro::start_main_coroutine(coro_main());
	return 0;
}