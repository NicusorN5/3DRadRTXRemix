// Live dump of 3DRad.exe object list + host tables. Compile: csc /nologo dump_group_live.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

internal static class Program
{
	const uint PROCESS_VM_READ = 0x0010;
	const uint PROCESS_QUERY_INFORMATION = 0x0400;
	const uint PROCESS_VM_OPERATION = 0x0008;
	const uint PROCESS_VM_WRITE = 0x0020;
	const uint MEM_COMMIT = 0x1000;
	const uint MEM_RESERVE = 0x2000;
	const uint MEM_RELEASE = 0x8000;
	const uint PAGE_READWRITE = 0x04;
	const uint WM_GETTEXT = 0x000D;
	const uint WM_GETTEXTLENGTH = 0x000E;
	const int LB_GETCOUNT = 0x018B;
	const int LB_GETCURSEL = 0x0188;
	const int LB_GETTEXT = 0x0189;
	const int LB_GETTEXTLEN = 0x018A;
	const int LB_GETITEMDATA = 0x0199;
	const int LB_GETSEL = 0x0187;
	const int GWL_STYLE = -16;
	const int MAX_PATH = 260;

	[DllImport("user32.dll")] static extern bool EnumWindows(EnumProc lpEnumFunc, IntPtr lParam);
	[DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr hWnd, EnumProc lpEnumFunc, IntPtr lParam);
	[DllImport("user32.dll", CharSet = CharSet.Auto)] static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
	[DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
	[DllImport("user32.dll", CharSet = CharSet.Auto)] static extern IntPtr SendMessage(IntPtr hWnd, int Msg, IntPtr wParam, IntPtr lParam);
	[DllImport("user32.dll", CharSet = CharSet.Auto)] static extern IntPtr SendMessage(IntPtr hWnd, int Msg, IntPtr wParam, StringBuilder lParam);
	[DllImport("user32.dll")] static extern int GetWindowLong(IntPtr hWnd, int nIndex);
	[DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
	[DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
	[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr hObject);
	[DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, int dwSize, out IntPtr lpNumberOfBytesRead);
	[DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, UIntPtr dwSize, uint flAllocationType, uint flProtect);
	[DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, UIntPtr dwSize, uint dwFreeType);
	[DllImport("kernel32.dll", CharSet = CharSet.Auto)] static extern uint GetModuleFileNameEx(IntPtr hProcess, IntPtr hModule, StringBuilder lpFilename, int nSize);
	[DllImport("psapi.dll", CharSet = CharSet.Auto)] static extern uint GetModuleFileNameExW(IntPtr hProcess, IntPtr hModule, StringBuilder lpFilename, int nSize);

	delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

	[StructLayout(LayoutKind.Sequential)]
	struct RECT { public int Left, Top, Right, Bottom; }

	static readonly StringBuilder _sb = new StringBuilder(512);
	static int _pid;
	static readonly List<IntPtr> _hwnds = new List<IntPtr>();

	static void Main()
	{
		var procs = Process.GetProcessesByName("3DRad");
		if (procs.Length == 0)
		{
			Console.WriteLine("3DRad.exe not running");
			return;
		}
		var proc = procs[0];
		_pid = proc.Id;
		IntPtr baseAddr = proc.MainModule.BaseAddress;
		Console.WriteLine("pid={0} base=0x{1:X} title={2}", _pid, baseAddr.ToInt64(), proc.MainWindowTitle);

		EnumWindows((h, l) =>
		{
			uint pid;
			GetWindowThreadProcessId(h, out pid);
			if ((int)pid == _pid) _hwnds.Add(h);
			return true;
		}, IntPtr.Zero);

		var kids = new List<IntPtr>();
		foreach (var top in _hwnds.ToArray())
		{
			EnumChildWindows(top, (h, l) => { kids.Add(h); return true; }, IntPtr.Zero);
		}
		foreach (var h in kids) if (!_hwnds.Contains(h)) _hwnds.Add(h);

		IntPtr list = IntPtr.Zero;
		int bestScore = -1;
		foreach (var h in _hwnds)
		{
			_sb.Length = 0;
			GetClassName(h, _sb, 512);
			string cls = _sb.ToString();
			if (!cls.Equals("ListBox", StringComparison.OrdinalIgnoreCase)) continue;
			int style = GetWindowLong(h, GWL_STYLE);
			int count = (int)SendMessage(h, LB_GETCOUNT, IntPtr.Zero, IntPtr.Zero);
			RECT r;
			GetWindowRect(h, out r);
			int score = 0;
			if ((style & 0x10) != 0) score += 40; // LBS_OWNERDRAWFIXED
			if ((style & 0x40) != 0) score += 20; // HASSTRINGS
			if (count >= 4) score += Math.Min(count, 80);
			int w = r.Right - r.Left;
			if (w >= 80 && w <= 400) score += 15;
			Console.WriteLine("ListBox hwnd=0x{0:X} style=0x{1:X8} count={2} rect={3},{4} {5}x{6} score={7}",
				h.ToInt64(), style, count, r.Left, r.Top, w, r.Bottom - r.Top, score);
			if (score > bestScore) { bestScore = score; list = h; }
		}

		if (list == IntPtr.Zero)
		{
			Console.WriteLine("no ListBox");
		}
		else
		{
			DumpList(list);
		}

		IntPtr hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, false, _pid);
		if (hp == IntPtr.Zero)
		{
			Console.WriteLine("OpenProcess failed {0}", Marshal.GetLastWin32Error());
			return;
		}
		try
		{
			DumpHost(hp, baseAddr);
		}
		finally
		{
			CloseHandle(hp);
		}
	}

	static string RemoteLbText(IntPtr list, int index)
	{
		int len = (int)SendMessage(list, LB_GETTEXTLEN, (IntPtr)index, IntPtr.Zero);
		if (len < 0) return "<err>";
		// Cross-process: allocate in target
		IntPtr hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, false, _pid);
		if (hp == IntPtr.Zero)
		{
			StringBuilder local = new StringBuilder(Math.Max(len + 2, 16));
			SendMessage(list, LB_GETTEXT, (IntPtr)index, local);
			return local.ToString();
		}
		int bytes = (len + 8) * 2;
		IntPtr remote = VirtualAllocEx(hp, IntPtr.Zero, (UIntPtr)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (remote == IntPtr.Zero)
		{
			CloseHandle(hp);
			StringBuilder local = new StringBuilder(Math.Max(len + 2, 16));
			SendMessage(list, LB_GETTEXT, (IntPtr)index, local);
			return local.ToString();
		}
		SendMessage(list, LB_GETTEXT, (IntPtr)index, remote);
		byte[] buf = new byte[bytes];
		IntPtr nread;
		ReadProcessMemory(hp, remote, buf, bytes, out nread);
		VirtualFreeEx(hp, remote, UIntPtr.Zero, MEM_RELEASE);
		CloseHandle(hp);
		// Try Unicode then ANSI
		string uni = Encoding.Unicode.GetString(buf);
		int z = uni.IndexOf('\0');
		if (z >= 0) uni = uni.Substring(0, z);
		if (uni.Length > 0 && LooksText(uni)) return uni;
		string ansi = Encoding.Default.GetString(buf);
		z = ansi.IndexOf('\0');
		if (z >= 0) ansi = ansi.Substring(0, z);
		return ansi;
	}

	static bool LooksText(string s)
	{
		int ok = 0;
		foreach (char c in s)
		{
			if (c >= 32 && c < 127) ok++;
			else if (c == 0) break;
			else return false;
		}
		return ok >= 2;
	}

	static void DumpList(IntPtr list)
	{
		int style = GetWindowLong(list, GWL_STYLE);
		int count = (int)SendMessage(list, LB_GETCOUNT, IntPtr.Zero, IntPtr.Zero);
		int cur = (int)SendMessage(list, LB_GETCURSEL, IntPtr.Zero, IntPtr.Zero);
		Console.WriteLine("--- LIST hwnd=0x{0:X} style=0x{1:X8} ownerdraw={2} hasstrings={3} count={4} cursel={5} ---",
			list.ToInt64(), style, ((style & 0x10) != 0 || (style & 0x20) != 0) ? 1 : 0,
			(style & 0x40) != 0 ? 1 : 0, count, cur);
		for (int i = 0; i < count && i < 256; i++)
		{
			string text = RemoteLbText(list, i);
			int data = (int)SendMessage(list, LB_GETITEMDATA, (IntPtr)i, IntPtr.Zero);
			int sel = (int)SendMessage(list, LB_GETSEL, (IntPtr)i, IntPtr.Zero);
			string mark = (i == cur) ? " <SEL>" : (sel > 0 ? " <sel>" : "");
			Console.WriteLine("  [{0,3}] data=0x{1:X8} ({1}) '{2}'{3}", i, data, text, mark);
		}
	}

	static bool Read(IntPtr hp, IntPtr addr, byte[] buf)
	{
		IntPtr n;
		return ReadProcessMemory(hp, addr, buf, buf.Length, out n) && n.ToInt32() == buf.Length;
	}

	static int ReadI32(IntPtr hp, IntPtr addr)
	{
		byte[] b = new byte[4];
		if (!Read(hp, addr, b)) return 0;
		return BitConverter.ToInt32(b, 0);
	}

	static uint ReadU32(IntPtr hp, IntPtr addr)
	{
		byte[] b = new byte[4];
		if (!Read(hp, addr, b)) return 0;
		return BitConverter.ToUInt32(b, 0);
	}

	static string ReadAsciiZ(IntPtr hp, IntPtr addr, int max)
	{
		if (addr == IntPtr.Zero) return "";
		byte[] b = new byte[max];
		IntPtr n;
		if (!ReadProcessMemory(hp, addr, b, b.Length, out n) || n.ToInt32() < 2) return "";
		int z = Array.IndexOf(b, (byte)0);
		if (z < 0) z = n.ToInt32();
		string s = Encoding.ASCII.GetString(b, 0, z);
		foreach (char c in s)
		{
			if (c < 32 || c > 126) return "";
		}
		return s;
	}

	static string ModulePath(IntPtr hp, uint hmod)
	{
		if (hmod == 0) return "";
		StringBuilder sb = new StringBuilder(MAX_PATH);
		uint n = GetModuleFileNameExW(hp, (IntPtr)hmod, sb, MAX_PATH);
		if (n == 0) return string.Format("hmod=0x{0:X}", hmod);
		string p = sb.ToString();
		int slash = Math.Max(p.LastIndexOf('\\'), p.LastIndexOf('/'));
		// keep last 2 folders
		string[] parts = p.Split('\\', '/');
		if (parts.Length >= 3)
			return parts[parts.Length - 3] + "\\" + parts[parts.Length - 2] + "\\" + parts[parts.Length - 1];
		return p;
	}

	static void DumpHost(IntPtr hp, IntPtr baseAddr)
	{
		const int pref = 0x00400000;
		int countVa = 0x00450460;
		int listVa = 0x00454468;
		int hmodVa = 0x0044AE58;
		long rebase = baseAddr.ToInt64() - pref;
		IntPtr countAt = (IntPtr)(countVa + rebase);
		IntPtr listAt = (IntPtr)(listVa + rebase);
		IntPtr hmodAt = (IntPtr)(hmodVa + rebase);
		int count = ReadI32(hp, countAt);
		Console.WriteLine("--- HOST count@0x{0:X}={1} list@0x{2:X} hmod@0x{3:X} ---",
			countAt.ToInt64(), count, listAt.ToInt64(), hmodAt.ToInt64());
		if (count <= 0 || count > 4096)
		{
			Console.WriteLine("count insane");
			return;
		}

		byte[] listBuf = new byte[count * 4];
		byte[] hmodBuf = new byte[count * 4];
		if (!Read(hp, listAt, listBuf) || !Read(hp, hmodAt, hmodBuf))
		{
			Console.WriteLine("failed to read host tables");
			return;
		}

		for (int i = 0; i < count; i++)
		{
			uint host = BitConverter.ToUInt32(listBuf, i * 4);
			uint hmod = BitConverter.ToUInt32(hmodBuf, i * 4);
			string path = ModulePath(hp, hmod);
			if (host == 0)
			{
				Console.WriteLine("  slot {0,3} host=NULL path={1}", i, path);
				continue;
			}
			IntPtr h = (IntPtr)host;
			uint plugin = ReadU32(hp, h);
			short type = (short)(ReadI32(hp, (IntPtr)(host + 0x291A)) & 0xFFFF);
			int nchild = ReadI32(hp, (IntPtr)(host + 0x291C));
			uint childArr = ReadU32(hp, (IntPtr)(host + 0x2920));
			uint parent = ReadU32(hp, (IntPtr)(host + 0x2924));
			uint linked = ReadU32(hp, (IntPtr)(host + 0x2928));
			int shown = 0;
			if (plugin != 0) shown = ReadI32(hp, (IntPtr)plugin + 0x04);

			string name = "";
			// host+4..+0x20 pointer names
			for (int off = 4; off <= 0x20; off += 4)
			{
				uint p = ReadU32(hp, (IntPtr)(host + off));
				string s = ReadAsciiZ(hp, (IntPtr)p, 64);
				if (s.Length >= 2) { name = s; break; }
			}
			if (name.Length == 0)
			{
				name = ReadAsciiZ(hp, (IntPtr)(host + 4), 48);
			}

			int parentSlot = -1, linkedSlot = -1;
			if (parent < (uint)count) parentSlot = (int)parent;
			else
			{
				for (int j = 0; j < count; j++)
				{
					if (BitConverter.ToUInt32(listBuf, j * 4) == parent) { parentSlot = j; break; }
					uint hj = BitConverter.ToUInt32(listBuf, j * 4);
					if (hj != 0 && ReadU32(hp, (IntPtr)hj) == parent) { parentSlot = j; break; }
				}
			}
			if (linked < (uint)count) linkedSlot = (int)linked;
			else
			{
				for (int j = 0; j < count; j++)
				{
					if (BitConverter.ToUInt32(listBuf, j * 4) == linked) { linkedSlot = j; break; }
				}
			}

			string kids = "";
			if (nchild > 0 && nchild <= 256 && childArr != 0)
			{
				byte[] arr = new byte[nchild * 8];
				if (Read(hp, (IntPtr)childArr, arr))
				{
					var parts = new List<string>();
					for (int k = 0; k < nchild; k++)
					{
						int slot = BitConverter.ToInt32(arr, k * 8);
						int bone = BitConverter.ToInt32(arr, k * 8 + 4);
						parts.Add(string.Format("{0}:{1}", slot, bone));
					}
					kids = string.Join(",", parts.ToArray());
				}
			}

			bool isGroup = path.IndexOf("Group", StringComparison.OrdinalIgnoreCase) >= 0
				|| name.IndexOf("Group", StringComparison.OrdinalIgnoreCase) >= 0;
			string mark = isGroup ? " **GROUP**" : "";
			Console.WriteLine(
				"  slot {0,3} host=0x{1:X8} plugin=0x{2:X8} type={3} shown={4} parent=0x{5:X8}(slot {6}) linked=0x{7:X8}(slot {8}) nchild={9} [{10}] name='{11}' path={12}{13}",
				i, host, plugin, type, shown, parent, parentSlot, linked, linkedSlot, nchild, kids, name, path, mark);
		}
	}
}
