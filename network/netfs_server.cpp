#include "network.hpp"
#include "util.hpp"
#include "netfs.hpp"
#include <arpa/inet.h>
#include "filesystem.hpp"
#include "event.hpp"

using namespace Granite;
using namespace std;

struct FilesystemHandler : LooperHandler
{
	FilesystemHandler(unique_ptr<Socket> socket)
		: LooperHandler(move(socket))
	{
	}

	bool handle(Looper &, EventFlags flags) override
	{
		if (flags & EVENT_IN)
			Filesystem::get().poll_notifications();

		return true;
	}
};

struct NotificationSystem : EventHandler
{
	NotificationSystem(Looper &looper)
		: looper(looper)
	{
		EventManager::get_global().register_handler(FilesystemProtocolEvent::type_id, &NotificationSystem::on_filesystem, this);
		for (auto &proto : Filesystem::get().get_protocols())
		{
			auto &fs = proto.second;
			if (fs->get_notification_fd() >= 0)
			{
				auto socket = unique_ptr<Socket>(new Socket(fs->get_notification_fd(), false));
				auto handler = unique_ptr<FilesystemHandler>(new FilesystemHandler(move(socket)));
				auto *ptr = handler.get();
				looper.register_handler(EVENT_IN, move(handler));
				protocols[proto.first] = ptr;
			}
		}
	}

	bool on_filesystem(const Event &e)
	{
		auto &fs = e.as<FilesystemProtocolEvent>();
		if (fs.get_backend().get_notification_fd() >= 0)
		{
			auto socket = unique_ptr<Socket>(new Socket(fs.get_backend().get_notification_fd(), false));
			auto handler = unique_ptr<FilesystemHandler>(new FilesystemHandler(move(socket)));
			auto *ptr = handler.get();
			looper.register_handler(EVENT_IN, move(handler));
			protocols[fs.get_protocol()] = ptr;
		}
		return true;
	}

	Looper &looper;
	std::unordered_map<std::string, FilesystemHandler *> protocols;
};

struct FSHandler : LooperHandler
{
	FSHandler(NotificationSystem &notify_system, unique_ptr<Socket> socket)
		: LooperHandler(move(socket)), notify_system(notify_system)
	{
		reply_builder.begin(4);
		command_reader.start(reply_builder.get_buffer());
		state = ReadCommand;
	}

	bool parse_command(Looper &)
	{
		command_id = reply_builder.read_u32();
		switch (command_id)
		{
		case NETFS_WALK:
		case NETFS_LIST:
		case NETFS_READ_FILE:
		case NETFS_WRITE_FILE:
		case NETFS_STAT:
			state = ReadChunkSize;
			reply_builder.begin(3 * sizeof(uint32_t));
			command_reader.start(reply_builder.get_buffer());
			return true;

		default:
			return false;
		}
	}

	bool read_chunk_size(Looper &)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			if (reply_builder.read_u32() != NETFS_BEGIN_CHUNK_REQUEST)
				return false;

			uint64_t chunk_size = reply_builder.read_u64();
			if (!chunk_size)
				return false;

			reply_builder.begin(chunk_size);
			command_reader.start(reply_builder.get_buffer());
			state = ReadChunkData;
			return true;
		}

		return ret == Socket::ErrorWouldBlock;
	}

	bool read_chunk_data2(Looper &looper)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			reply_builder.begin();
			reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
			reply_builder.add_u32(NETFS_ERROR_OK);
			reply_builder.add_u64(file->get_size());
			command_writer.start(reply_builder.get_buffer());
			state = WriteReplyChunk;
			looper.modify_handler(EVENT_OUT, *this);
			return true;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool read_chunk_size2(Looper &looper)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			if (reply_builder.read_u32() != NETFS_BEGIN_CHUNK_REQUEST)
				return false;

			uint64_t chunk_size = reply_builder.read_u64();
			if (!chunk_size)
				return false;

			mapped = file->map_write(chunk_size);
			if (!mapped)
			{
				reply_builder.begin();
				reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
				reply_builder.add_u32(NETFS_ERROR_IO);
				reply_builder.add_u64(0);
				command_writer.start(reply_builder.get_buffer());
				state = WriteReplyChunk;
				looper.modify_handler(EVENT_OUT, *this);
			}
			else
			{
				reply_builder.begin(chunk_size);
				command_reader.start(mapped, chunk_size);
				state = ReadChunkData2;
			}
			return true;
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool begin_write_file(Looper &looper, const string &arg)
	{
		file = Filesystem::get().open(arg, FileMode::WriteOnly);
		if (!file)
		{
			reply_builder.begin();
			reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
			reply_builder.add_u32(NETFS_ERROR_IO);
			reply_builder.add_u64(0);
			command_writer.start(reply_builder.get_buffer());
			state = WriteReplyChunk;
			looper.modify_handler(EVENT_OUT, *this);
		}
		else
		{
			reply_builder.begin(3 * sizeof(uint32_t));
			command_reader.start(reply_builder.get_buffer());
			state = ReadChunkSize2;
		}
		return true;
	}

	bool begin_read_file(const string &arg)
	{
		file = Filesystem::get().open(arg);
		mapped = nullptr;
		if (file)
			mapped = file->map();

		reply_builder.begin();
		if (mapped)
		{
			reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
			reply_builder.add_u32(NETFS_ERROR_OK);
			reply_builder.add_u64(file->get_size());
		}
		else
		{
			reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
			reply_builder.add_u32(NETFS_ERROR_IO);
			reply_builder.add_u64(0);
		}
		command_writer.start(reply_builder.get_buffer());
		return true;
	}

	void write_string_list(const vector<ListEntry> &list)
	{
		reply_builder.begin();
		reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
		reply_builder.add_u32(NETFS_ERROR_OK);
		auto offset = reply_builder.add_u64(0);
		reply_builder.add_u32(list.size());
		for (auto &l : list)
		{
			reply_builder.add_string(l.path);
			switch (l.type)
			{
			case PathType::File:
				reply_builder.add_u32(NETFS_FILE_TYPE_PLAIN);
				break;
			case PathType::Directory:
				reply_builder.add_u32(NETFS_FILE_TYPE_DIRECTORY);
				break;
			case PathType::Special:
				reply_builder.add_u32(NETFS_FILE_TYPE_SPECIAL);
				break;
			}
		}
		reply_builder.poke_u64(offset, reply_builder.get_buffer().size() - (offset + 8));
		command_writer.start(reply_builder.get_buffer());
	}

	bool begin_stat(const string &arg)
	{
		FileStat s;
		reply_builder.begin();
		reply_builder.add_u32(NETFS_BEGIN_CHUNK_REPLY);
		if (Filesystem::get().stat(arg, s))
		{
			reply_builder.add_u32(NETFS_ERROR_OK);
			reply_builder.add_u64(8 + 4);
			reply_builder.add_u64(s.size);
			switch (s.type)
			{
			case PathType::File:
				reply_builder.add_u32(NETFS_FILE_TYPE_PLAIN);
				break;
			case PathType::Directory:
				reply_builder.add_u32(NETFS_FILE_TYPE_DIRECTORY);
				break;
			case PathType::Special:
				reply_builder.add_u32(NETFS_FILE_TYPE_SPECIAL);
				break;
			}
		}
		else
		{
			reply_builder.add_u32(NETFS_ERROR_IO);
			reply_builder.add_u64(0);
		}
		command_writer.start(reply_builder.get_buffer());
		return true;
	}

	bool begin_list(const string &arg)
	{
		auto list = Filesystem::get().list(arg);
		write_string_list(list);
		return true;
	}

	bool begin_walk(const string &arg)
	{
		auto list = Filesystem::get().walk(arg);
		write_string_list(list);
		return true;
	}

	bool read_chunk_data(Looper &looper)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
		{
			auto str = reply_builder.read_string_implicit_count();

			switch (command_id)
			{
			case NETFS_READ_FILE:
				looper.modify_handler(EVENT_OUT, *this);
				state = WriteReplyChunk;
				begin_read_file(str);
				break;

			case NETFS_WRITE_FILE:
				begin_write_file(looper, str);
				break;

			case NETFS_STAT:
				looper.modify_handler(EVENT_OUT, *this);
				state = WriteReplyChunk;
				begin_stat(str);
				break;

			case NETFS_LIST:
				looper.modify_handler(EVENT_OUT, *this);
				state = WriteReplyChunk;
				begin_list(str);
				break;

			case NETFS_WALK:
				looper.modify_handler(EVENT_OUT, *this);
				state = WriteReplyChunk;
				begin_walk(str);
				break;

			default:
				return false;
			}

			return true;
		}

		return ret == Socket::ErrorWouldBlock;
	}

	bool read_command(Looper &looper)
	{
		auto ret = command_reader.process(*socket);
		if (command_reader.complete())
			return parse_command(looper);

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool write_reply_chunk(Looper &)
	{
		auto ret = command_writer.process(*socket);
		if (command_writer.complete())
		{
			switch (command_id)
			{
			case NETFS_READ_FILE:
				if (mapped)
				{
					command_writer.start(mapped, file->get_size());
					state = WriteReplyData;
					return true;
				}
				else
					return false;

			case NETFS_WRITE_FILE:
				if (file && mapped)
					file->unmap();
				return false;

			default:
				return false;
			}
		}

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool write_reply_data(Looper &)
	{
		auto ret = command_writer.process(*socket);
		if (command_writer.complete())
			return false;

		return (ret > 0) || (ret == Socket::ErrorWouldBlock);
	}

	bool handle(Looper &looper, EventFlags) override
	{
		if (state == ReadCommand)
			return read_command(looper);
		else if (state == ReadChunkSize)
			return read_chunk_size(looper);
		else if (state == ReadChunkData)
			return read_chunk_data(looper);
		else if (state == ReadChunkSize2)
			return read_chunk_size2(looper);
		else if (state == ReadChunkData2)
			return read_chunk_data2(looper);
		else if (state == WriteReplyChunk)
			return write_reply_chunk(looper);
		else if (state == WriteReplyData)
			return write_reply_data(looper);
		else
			return false;
	}

	enum State
	{
		ReadCommand,
		ReadChunkSize,
		ReadChunkData,
		ReadChunkSize2,
		ReadChunkData2,
		WriteReplyChunk,
		WriteReplyData
	};

	NotificationSystem &notify_system;
	State state = ReadCommand;
	SocketReader command_reader;
	SocketWriter command_writer;
	ReplyBuilder reply_builder;
	uint32_t command_id = 0;

	unique_ptr<File> file;
	void *mapped = nullptr;
};

struct ListenerHandler : TCPListener
{
	ListenerHandler(NotificationSystem &notify_system, uint16_t port)
		: TCPListener(port), notify_system(notify_system)
	{
	}

	bool handle(Looper &looper, EventFlags) override
	{
		auto client = accept();
		if (client)
			looper.register_handler(EVENT_IN, unique_ptr<FSHandler>(new FSHandler(notify_system, move(client))));
		return true;
	}

	NotificationSystem &notify_system;
};


int main()
{
	Looper looper;
	auto notify = unique_ptr<NotificationSystem>(new NotificationSystem(looper));
	auto listener = unique_ptr<LooperHandler>(new ListenerHandler(*notify, 7070));

	looper.register_handler(EVENT_IN, move(listener));
	while (looper.wait(-1) >= 0);
}