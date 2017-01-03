#include "util.hpp"
#include "compiler.hpp"
#include "filesystem.hpp"
#include <unistd.h>

using namespace Granite;
using namespace std;

int main()
{
#if 0
	GLSLCompiler compiler;
	vector<uint32_t> spirv;
	compiler.set_source_from_file("/tmp/test.frag");
	compiler.set_stage(Stage::Fragment);
	compiler.compile(spirv);
#endif

	auto &fs = Filesystem::get();
	auto entries = fs.walk("/tmp/");
	for (auto &e : entries)
	{
		Filesystem::Stat s;
		uint64_t size = 0;
		if (fs.stat(e.path, s))
			size = s.size;
		LOG("File: %s (type: %d) (size: %zu)\n", e.path.c_str(), static_cast<int>(e.type), size_t(size));
	}

#if 0
	fs.install_notification("/tmp/hei", [](const Filesystem::NotifyInfo &info) {
		switch (info.type)
		{
			case Filesystem::NotifyType::FileChanged:
				LOG("File changed: %s\n", info.path.c_str());
				break;
			case Filesystem::NotifyType::FileDeleted:
				LOG("File deleted: %s\n", info.path.c_str());
				break;
			case Filesystem::NotifyType::FileCreated:
				LOG("File created: %s\n", info.path.c_str());
				break;
		}
	});
#endif

	fs.install_notification("/tmp", [](const Filesystem::NotifyInfo &info) {
		switch (info.type)
		{
			case Filesystem::NotifyType::FileChanged:
				LOG("Dir file changed: %s\n", info.path.c_str());
				break;
			case Filesystem::NotifyType::FileDeleted:
				LOG("Dir file deleted: %s\n", info.path.c_str());
				break;
			case Filesystem::NotifyType::FileCreated:
				LOG("Dir file created: %s\n", info.path.c_str());
				break;
		}
	});

	for (;;)
	{
		usleep(10000);
		fs.poll_notifications();
	}
}