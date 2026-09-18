#pragma once

#include <functional>
#include <string>

namespace comp::runtime_update
{
	struct result
	{
		bool dxvk_ok = false;
		bool remix_ok = false;
		bool remix_skipped = false;
		std::string error;
		std::string warning;
		std::string status;
	};

	using status_fn = std::function<void(const char*)>;

	// Download vanilla DXVK 32-bit d3d9 → d3d9_dxvk.dll and the RTX Remix
	// runtime into root_path. Never overwrites the proxy d3d9.dll.
	result run(const status_fn& status);
}
