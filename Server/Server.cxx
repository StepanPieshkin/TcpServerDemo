#include <thread>
#include <span>
#include <iostream>
#include <format>

#include "boost/asio.hpp"

#include <readerwriterqueue/readerwriterqueue.h>

namespace demo
{
	template<typename T>
	concept dispatcher =
		requires(T d) {
			{ d.create_handler() };
	};

	template<typename Executor>
	class asio_async_auto_reset_event
	{
	public:
		explicit asio_async_auto_reset_event(const Executor & executor)
			: _timer(executor)
		{
			// Setting expiration to infinity will cause handlers to wait on the timer until cancelled.
			_timer.expires_at(boost::posix_time::pos_infin);
		}

		auto async_wait() { return _timer.async_wait(boost::asio::use_awaitable); }

		void notify_one() { _timer.cancel_one(); }

		void notify_all() { _timer.cancel(); }

	private:
		boost::asio::deadline_timer _timer;
	};

	class spsc_stream
	{
		static constexpr size_t _blockSize = 1024 * 1024;	// 1 MB

		struct data_block
		{
			std::byte _data[_blockSize];
			std::atomic<size_t> _written;
			size_t _read;
		};

	public:
		void write(std::span<std::byte> data)
		{
			size_t dataLeft = std::size(data);

			while (dataLeft)
			{
				size_t written = _lastBlock ? _lastBlock->_written.load(std::memory_order_relaxed) : _blockSize;
 				size_t remaining = _blockSize - written;

				if (!remaining)
				{
					// Add next block
 					std::unique_ptr<data_block> newBlock = std::make_unique_for_overwrite<data_block>();
 
 					_lastBlock = newBlock.get();
 					_blocks.emplace(std::move(newBlock));

					remaining = _blockSize;
					written = 0;
				}

				size_t chunkSize = std::min(remaining, dataLeft);

				std::copy(std::begin(data), std::begin(data) + chunkSize, _lastBlock->_data);
				dataLeft -= chunkSize;

				_lastBlock->_written.store(written + chunkSize, std::memory_order::memory_order_release);

				if (_lastBlock->_written.load(std::memory_order_relaxed) == _blockSize)
					_lastBlock = nullptr;
			}
		}
		
		size_t read(std::span<std::byte> data)
		{
			auto dataIt = std::begin(data);

			while (dataIt != std::end(data))
			{
				data_block * firstBlock = _blocks.peek()->get();
				if (!firstBlock)
					return std::distance(std::begin(data), dataIt);

				size_t read = firstBlock->_read;
				size_t written = firstBlock->_written.load(std::memory_order_acquire);
				size_t remaining = written - read;

				if (!remaining)
					return std::distance(std::begin(data), dataIt);

				size_t chunkSize = std::min(remaining, (size_t)std::distance(dataIt, std::end(data)));
				std::copy(firstBlock->_data + read, firstBlock->_data + read + chunkSize, dataIt);
				dataIt += chunkSize;
				firstBlock->_read += chunkSize;

				if (firstBlock->_read == _blockSize)
					_blocks.pop();
			}
		}

	private:
		data_block * _lastBlock = nullptr;
		moodycamel::ReaderWriterQueue<std::unique_ptr<data_block>> _blocks;		
	};

	class speedometer
	{
	public:
		void add_bytes(size_t n) noexcept
		{
			if (!_bytes.fetch_add(n, std::memory_order_relaxed))
				_startTime = std::chrono::high_resolution_clock::now();
		}

		void reset() noexcept
		{
			_bytes.store(0, std::memory_order_relaxed);
		}

		void dump()
		{
			constexpr size_t MB = 1024 * 1024;
			//constexpr size_t GB = 1024 * 1024 * 1024;

			size_t bytes = _bytes.load(std::memory_order_relaxed);
			auto duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - _startTime);

			std::cout << std::format("Transfered: {:.2f} MB, average speed {:.2f} MB/s\n", (double)bytes / MB, (double)bytes * 1000000.0 / duration.count() / MB );
		}

	private:
		std::chrono::high_resolution_clock::time_point _startTime;
		std::atomic<size_t> _bytes = 0;
	};

	template<typename Dispatcher>
	class string_parser
	{
	public:
		string_parser(string_parser &) = delete;

		string_parser(string_parser &&) = default;

		string_parser(Dispatcher & dispatcher) noexcept :
			_dispatcher(dispatcher) {}

		void push_data(std::span<std::byte> data)
		{
			auto it = std::begin(data);

			while (it != std::end(data))
			{
				// Read message size first
				if (_sizeWriteIt != std::end(_currentMessageSize._bytes))
				{
					while (_sizeWriteIt != std::end(_currentMessageSize._bytes) && it != std::end(data))
						*_sizeWriteIt++ = *it++;

					// If we got size - allocate the message
					if (_sizeWriteIt == std::end(_currentMessageSize._bytes))
						_currentMessage.reserve(_currentMessageSize._size);
				}

				// If we have size - read message body
				if (_sizeWriteIt == std::end(_currentMessageSize._bytes))
				{
					while (it != std::end(data) && _currentMessageSize._size > sizeof(_currentMessageSize))
						_currentMessage.push_back(static_cast<char>(*it++)), _currentMessageSize._size--;

					// Check if message if complete
					if (_currentMessageSize._size == sizeof(_currentMessageSize))
					{
						_sizeWriteIt = std::begin(_currentMessageSize._bytes);

						_dispatcher.dispatch(std::move(_currentMessage));
					}
				}
			}
		}

	private:
		std::string _currentMessage;
		union
		{
			std::array<std::byte, sizeof(uint32_t)> _bytes;
			uint32_t _size;
		} _currentMessageSize;
		std::array<std::byte, sizeof(uint32_t)>::iterator _sizeWriteIt = std::begin(_currentMessageSize._bytes);
		Dispatcher & _dispatcher;
	};

	template<typename Executor>
	class demo_dispatcher
	{
	public:
		demo_dispatcher(demo_dispatcher &) = delete;

		demo_dispatcher(demo_dispatcher &&) = default;

		demo_dispatcher(Executor executor) noexcept :
			_executor(executor) {}

		auto create_handler() // FIXME noexcept
		{
			return string_parser(*this);
		}

		void dispatch(std::string && message)
		{
			std::cout << std::format("Message received: length {}\n", message.length());

			// Enqueue message for further processing
			boost::asio::execution::execute(_executor, [m = std::forward<std::string>(message)]()
			{
				std::string m1 = m;
				std::ranges::sort(m1);

				std::cout << std::format("Message processed\n");
			});
		}

	private:
		Executor _executor;
	};

	template<dispatcher Dispatcher>
	class tcp_server
	{
	public:
		tcp_server(tcp_server &) = delete;

		tcp_server(tcp_server &&) = default;

		~tcp_server()
		{
			_ioContext.stop();

			for (auto & thread : _ioThreads)
				thread.join();

			std::cout << std::format("Server stopped\n");

			_speedometer.dump();
		}

		tcp_server(Dispatcher && dispatcher, boost::asio::ip::tcp::endpoint && endpoint, size_t threadCount = 1) :
			_dispatcher(std::forward<Dispatcher>(dispatcher)), _ioContext((int)threadCount), _signals(_ioContext, SIGINT, SIGTERM), _endpoint(std::forward<boost::asio::ip::tcp::endpoint>(endpoint))
		{
			std::cout << std::format("Starting server: io thread count {}\n", threadCount);

			_signals.async_wait([&](const boost::system::error_code & error, int signal_number) { std::cout << std::format("IO error: {}\n", error.message()); });

			_ioThreads.reserve(threadCount);
			for (size_t i = 0; i < threadCount; ++i)
				_ioThreads.emplace_back([this] { _ioContext.run(); });

			for (size_t i = 0; i < threadCount; ++i)
				boost::asio::co_spawn(_ioContext, listen(), boost::asio::detached);
		}

	private:
		boost::asio::awaitable<void> listen()
		{
			try
			{
				std::cout << std::format("Starting listening: address {}, port {}\n", _endpoint.address().to_string(), (size_t)_endpoint.port());

				auto executor = co_await boost::asio::this_coro::executor;
				
				boost::asio::ip::tcp::acceptor acceptor(executor, _endpoint);
				for (;;)
				{
					boost::asio::ip::tcp::socket socket = co_await acceptor.async_accept(boost::asio::use_awaitable);
					boost::asio::co_spawn(executor, read(std::move(socket)), boost::asio::detached);
				}
			}
			catch (std::exception & e)
			{
				std::cout << std::format("Listener exception: {}\n", e.what());
			}
		}

		boost::asio::awaitable<void> read(boost::asio::ip::tcp::socket socket)
		{
			std::cout << std::format("New connection: remote address {}, port {}, handle {}\n", socket.remote_endpoint().address().to_string(), socket.remote_endpoint().port(), (size_t)socket.native_handle());

			try
			{
				//std::array<std::byte, 4096 * 128> data;	// 4 KB
				std::vector<std::byte> data(1024 * 1024);
				auto handler = _dispatcher.create_handler();

				for (;;)
				{
					size_t n = co_await socket.async_read_some(boost::asio::buffer(data), boost::asio::use_awaitable);

					//std::cout << std::format("Received {} bytes from connection {}\n", n, (size_t)socket.native_handle());

					//handler.push_data(std::ranges::subrange(std::begin(data), std::begin(data) + n));

					_speedometer.add_bytes(n);
				}
			}
			catch (std::exception & e)
			{
				std::cout << std::format("Reader exception: {}\n", e.what());
			}
		}

		Dispatcher _dispatcher;
		std::vector<std::thread> _ioThreads;
		boost::asio::io_context _ioContext;		
		boost::asio::signal_set _signals;
		boost::asio::ip::tcp::endpoint _endpoint;

		speedometer _speedometer;
	};
}

int main(int argc, char * argv[])
{
	// By default server will use 4 IO threads for network operations
	size_t ioThreadCount = 4;

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
			}
		}
	}

	// Thread pool with default thread count (equal to number of CPU cores)
	boost::asio::thread_pool threadPool;

	// Start TCP server
	auto demoServer = demo::tcp_server(
		demo::demo_dispatcher(threadPool.get_executor()),
		{ boost::asio::ip::make_address_v4(address), static_cast<boost::asio::ip::port_type>(port) },
		ioThreadCount);

	getchar();

	return 0;
}