#pragma once

namespace comp::launch
{
	// Editor-only (3DRad.exe). From DllMain: patch the EXE entry so the launch
	// dialog + Remix LoadLibrary run after the loader lock, but still before
	// WinMain creates windows (old DllMain chain timing). No-op for compiler /
	// compiled players.
	void start_from_dllmain();

	// Direct3DCreate9 / Direct3DCreate9Ex: wait until the chosen d3d9 chain is
	// loaded. Does not pump messages. No-op if the entry hijack already loaded it.
	// If the user cancelled the launch dialog, this ExitProcess's 3DRad.exe
	// (and never returns). Callers still fail Create9 if it ever does return.
	void ensure_ready();

	// True after Cancel / window X on the editor launch dialog.
	bool aborted();
}
