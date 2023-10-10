#include <iostream>
#include <random>
#include <format>

#include "asio.hpp"

asio::awaitable<void> connect(asio::ip::tcp::endpoint && endpoint)
{
	auto executor = co_await asio::this_coro::executor;

	// Used to generate random data
	std::random_device randomDevice;
	std::mt19937 randomEngine(randomDevice());
	std::uniform_int_distribution<int> distribution('a', 'z');

	for(;;)
	{
		try
		{
			std::cout << std::format("Establishing new connection: address {}, port {}\n", endpoint.address().to_string(), (size_t)endpoint.port());

			// Connect
			asio::ip::tcp::socket socket(executor);
			co_await socket.async_connect(endpoint, asio::use_awaitable);

			// Generate random string of fixed size
			std::vector<char> randomData(1024 * 1024);
			*(uint16_t *)randomData.data() = (uint16_t)std::size(randomData);

			std::ranges::generate(randomData, [&]() { return static_cast<char>(distribution(randomEngine)); });

			for (;;)
			{
				// Send random string
				auto n = co_await socket.async_write_some(asio::buffer(randomData), asio::use_awaitable);

				//std::cout << std::format("Sent {} bytes to connection {}\n", n, (size_t)socket.native_handle());
			}
		}
		catch (std::exception & e)
		{
			std::printf("Connection exception: %s\n", e.what());
		}

		// Try to reconnect in 5 seconds
		asio::steady_timer timer(executor, std::chrono::steady_clock::now() + std::chrono::seconds(5));
		co_await timer.async_wait(asio::use_awaitable);
	}	
}

int main(int argc, char * argv[])
{
	// By default server will use 4 IO threads for network operations
	size_t ioThreadCount = 4;

	// Default number of simultaneous connections
	size_t connectionCount = 16;

	// Default binding port
	size_t port = 54321;

	// Default binding address
	const char * address = "127.0.0.1";

	// Get command line arguments if any
	if (argc > 1)	// 1st argument
	{
		// Address
		address = argv[1];

		if (argc > 2)	// 2nd argument
		{
			// Port
			size_t value;
			if (std::from_chars(argv[2], argv[2] + strlen(argv[2]), value).ec == std::errc())
				port = value;

			if (argc > 3)	// 3rd argument
			{
				// IO thread count
				if (std::from_chars(argv[3], argv[3] + strlen(argv[3]), value).ec == std::errc())
					ioThreadCount = value;

				if (argc > 4)	// 4th argument
				{
					// Connections count
					if (std::from_chars(argv[4], argv[4] + strlen(argv[4]), value).ec == std::errc())
						connectionCount = value;
				}
			}
		}
	}

	// Allocate IO context	
	asio::io_context _ioContext((int)ioThreadCount);

	// Attach signal to context
	asio::signal_set signals(_ioContext, SIGINT, SIGTERM);
	signals.async_wait([&](const asio::error_code & error, int signal_number) { std::cout << std::format("IO error: {}\n", error.message()); });

	// Start worker threads
	std::vector<std::thread> ioThreads;
	ioThreads.reserve(ioThreadCount);
	for (size_t i = 0; i < ioThreadCount; ++i)
		ioThreads.emplace_back([&] { _ioContext.run(); });

	// Initiate connections
	for (size_t i = 0; i < connectionCount; ++i)
		asio::co_spawn(_ioContext, connect({ asio::ip::make_address_v4(address), static_cast<asio::ip::port_type>(port) }), asio::detached);

	getchar();

	// Terminate IO context, connections and wait for threads to exit
	_ioContext.stop();

	for (auto & thread : ioThreads)
		thread.join();

	return 0;
}
