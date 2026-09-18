#pragma once

namespace comp::game::particles
{
	void open_remix_pane(HWND owner, std::uint64_t identity);
	bool is_remix_pane_hwnd(HWND hwnd);
	bool filter_remix_pane_message(MSG* msg);
}
