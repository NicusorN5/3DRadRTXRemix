#pragma once

namespace comp::compiler_inject
{
	// Compiler-only: watch stand-alone compiles and copy the Remix proxy runtime
	// into the output folder. Never loads DXVK / d3d9_remix into this process.
	void start();
}
