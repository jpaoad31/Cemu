#include "input/HotkeySettings.h"
#include "interface/WindowSystem.h"

#include "helpers/wxHelpers.h"

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif

#if BOOST_OS_MACOS
#include <Carbon/Carbon.h>
#endif

#include "wxgui/wxgui.h"
#include "wxgui/CemuApp.h"
#include "wxgui/MainWindow.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/CafeSystem.h"

WindowSystem::WindowInfo g_window_info{};

std::shared_mutex g_mutex;
MainWindow* g_mainFrame = nullptr;

#if BOOST_OS_WINDOWS
void _wxLaunch()
{
	SetThreadName("MainThread_UI");
	wxEntry();
}
#endif

void WindowSystem::Create()
{
	SetThreadName("cemu");
#if BOOST_OS_WINDOWS
	// on Windows wxWidgets there is a bug where wxDirDialog->ShowModal will deadlock in Windows internals somehow
	// moving the UI thread off the main thread fixes this
	std::thread t = std::thread(_wxLaunch);
	t.join();
#else
	int argc = 0;
	char* argv[1]{};
	wxEntry(argc, argv);
#endif
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory> /*errorId*/)
{
	wxString caption;
	if (title.empty())
		caption = wxASCII_STR(wxMessageBoxCaptionStr);
	else
		caption = wxString::FromUTF8(title);
	wxMessageBox(wxString::FromUTF8(message), caption, wxOK | wxCENTRE | wxICON_ERROR);
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_window_info;
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps)
{
	std::string windowText;
	windowText = BUILD_VERSION_WITH_NAME_STRING;

	if (isIdle)
	{
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(windowText);
		return;
	}
	if (isLoading)
	{
		windowText.append(" - Loading...");
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(windowText);
		return;
	}

	const char* renderer = "";
	if (g_renderer)
	{
		switch (g_renderer->GetType())
		{
		case RendererAPI::OpenGL:
			renderer = "[OpenGL]";
			break;
		case RendererAPI::Vulkan:
			renderer = "[Vulkan]";
			break;
		case RendererAPI::Metal:
			renderer = "[Metal]";
			break;
		default: break;
		}
	}

	// get GPU vendor/mode
	const char* graphicMode = "[Generic]";
	if (LatteGPUState.glVendor == GLVENDOR_AMD)
		graphicMode = "[AMD GPU]";
	else if (LatteGPUState.glVendor == GLVENDOR_INTEL)
		graphicMode = "[Intel GPU]";
	else if (LatteGPUState.glVendor == GLVENDOR_NVIDIA)
		graphicMode = "[NVIDIA GPU]";
	else if (LatteGPUState.glVendor == GLVENDOR_APPLE)
		graphicMode = "[Apple GPU]";

	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	windowText.append(fmt::format(" - FPS: {:.2f} {} {} [TitleId: {:08x}-{:08x}]", (double)fps, renderer, graphicMode, (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF)));

	if (ActiveSettings::IsOnlineEnabled())
	{
		if (ActiveSettings::GetNetworkService() == NetworkService::Nintendo)
			windowText.append(" [Online]");
		else if (ActiveSettings::GetNetworkService() == NetworkService::Pretendo)
			windowText.append(" [Online-Pretendo]");
		else if (ActiveSettings::GetNetworkService() == NetworkService::Custom)
			windowText.append(" [Online-" + GetNetworkConfig().networkname.GetValue() + "]");
	}
	windowText.append(" ");
	windowText.append(CafeSystem::GetForegroundTitleName());
	// append region
	CafeConsoleRegion region = CafeSystem::GetForegroundTitleRegion();
	uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();
	if (region == CafeConsoleRegion::JPN)
		windowText.append(fmt::format(" [JP v{}]", titleVersion));
	else if (region == CafeConsoleRegion::USA)
		windowText.append(fmt::format(" [US v{}]", titleVersion));
	else if (region == CafeConsoleRegion::EUR)
		windowText.append(fmt::format(" [EU v{}]", titleVersion));
	else
		windowText.append(fmt::format(" [v{}]", titleVersion));

	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->AsyncSetTitle(windowText);
		auto* pad = g_mainFrame->GetPadView();
		if (pad)
			pad->AsyncSetTitle(fmt::format("{} - FPS: {:.02f}", _("GamePad View").utf8_string(), fps));
	}
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_window_info.width;
	h = g_window_info.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.pad_width;
		h = g_window_info.pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_width;
	h = g_window_info.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	if (g_window_info.pad_open)
	{
		w = g_window_info.phys_pad_width;
		h = g_window_info.phys_pad_height;
	}
	else
	{
		w = 0;
		h = 0;
	}
}

double WindowSystem::GetWindowDPIScale()
{
	return g_window_info.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return g_window_info.pad_open ? g_window_info.pad_dpi_scale.load() : 1.0;
}

bool WindowSystem::IsPadWindowOpen()
{
	return g_window_info.pad_open;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_window_info.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	uint32 key = 0;

	switch (platformKey)
	{
#if BOOST_OS_WINDOWS
	case PlatformKeyCodes::LCONTROL:
		key = VK_LCONTROL;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = VK_RCONTROL;
		break;
	case PlatformKeyCodes::TAB:
		key = VK_TAB;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = VK_ESCAPE;
		break;
	case PlatformKeyCodes::RETURN:
		key = VK_RETURN;
		break;
	case PlatformKeyCodes::SPACE:
		key = VK_SPACE;
		break;
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	case PlatformKeyCodes::LCONTROL:
		key = GDK_KEY_Control_L;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = GDK_KEY_Control_R;
		break;
	case PlatformKeyCodes::TAB:
		key = GDK_KEY_Tab;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = GDK_KEY_Escape;
		break;
	case PlatformKeyCodes::RETURN:
		key = GDK_KEY_Return;
		break;
	case PlatformKeyCodes::SPACE:
		key = GDK_KEY_space;
		break;
#elif BOOST_OS_MACOS
	case PlatformKeyCodes::LCONTROL:
		key = kVK_Control;
		break;
	case PlatformKeyCodes::RCONTROL:
		key = kVK_RightControl;
		break;
	case PlatformKeyCodes::TAB:
		key = kVK_Tab;
		break;
	case PlatformKeyCodes::ESCAPE:
		key = kVK_Escape;
		break;
	case PlatformKeyCodes::RETURN:
		key = kVK_Return;
		break;
	case PlatformKeyCodes::SPACE:
		key = kVK_Space;
		break;
#endif
	default:
		return false;
	}

	return WindowSystem::IsKeyDown(key);
}

std::string WindowSystem::GetKeyCodeName(uint32 button)
{
#if BOOST_OS_WINDOWS
	LONG scan_code = MapVirtualKeyA((UINT)button, MAPVK_VK_TO_VSC_EX);
	if (HIBYTE(scan_code))
		scan_code |= 0x100;

	// because MapVirtualKey strips the extended bit for some keys
	switch (button)
	{
	case VK_LEFT:
	case VK_UP:
	case VK_RIGHT:
	case VK_DOWN: // arrow keys
	case VK_PRIOR:
	case VK_NEXT: // page up and page down
	case VK_END:
	case VK_HOME:
	case VK_INSERT:
	case VK_DELETE:
	case VK_DIVIDE: // numpad slash
	case VK_NUMLOCK:
	{
		scan_code |= 0x100; // set extended bit
		break;
	}
	}

	scan_code <<= 16;

	char key_name[128];
	if (GetKeyNameTextA(scan_code, key_name, std::size(key_name)) != 0)
		return key_name;
	else
		return fmt::format("key_{}", button);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	return gdk_keyval_name(button);
#elif BOOST_OS_MACOS
	// macOS virtual key codes are physical positions; name them by the US layout (stable, so the
	// names can be saved in binding expressions). Was "key_<code>" for every key.
	static const std::unordered_map<uint32, const char*> kNames = {
		{kVK_ANSI_A, "A"}, {kVK_ANSI_B, "B"}, {kVK_ANSI_C, "C"}, {kVK_ANSI_D, "D"}, {kVK_ANSI_E, "E"}, {kVK_ANSI_F, "F"},
		{kVK_ANSI_G, "G"}, {kVK_ANSI_H, "H"}, {kVK_ANSI_I, "I"}, {kVK_ANSI_J, "J"}, {kVK_ANSI_K, "K"}, {kVK_ANSI_L, "L"},
		{kVK_ANSI_M, "M"}, {kVK_ANSI_N, "N"}, {kVK_ANSI_O, "O"}, {kVK_ANSI_P, "P"}, {kVK_ANSI_Q, "Q"}, {kVK_ANSI_R, "R"},
		{kVK_ANSI_S, "S"}, {kVK_ANSI_T, "T"}, {kVK_ANSI_U, "U"}, {kVK_ANSI_V, "V"}, {kVK_ANSI_W, "W"}, {kVK_ANSI_X, "X"},
		{kVK_ANSI_Y, "Y"}, {kVK_ANSI_Z, "Z"},
		{kVK_ANSI_0, "0"}, {kVK_ANSI_1, "1"}, {kVK_ANSI_2, "2"}, {kVK_ANSI_3, "3"}, {kVK_ANSI_4, "4"},
		{kVK_ANSI_5, "5"}, {kVK_ANSI_6, "6"}, {kVK_ANSI_7, "7"}, {kVK_ANSI_8, "8"}, {kVK_ANSI_9, "9"},
		{kVK_ANSI_Minus, "-"}, {kVK_ANSI_Equal, "="}, {kVK_ANSI_LeftBracket, "["}, {kVK_ANSI_RightBracket, "]"},
		{kVK_ANSI_Backslash, "\\"}, {kVK_ANSI_Semicolon, ";"}, {kVK_ANSI_Quote, "'"}, {kVK_ANSI_Comma, ","},
		{kVK_ANSI_Period, "."}, {kVK_ANSI_Slash, "/"}, {kVK_ANSI_Grave, "`"}, {kVK_ISO_Section, "Section"},
		{kVK_Return, "Return"}, {kVK_Tab, "Tab"}, {kVK_Space, "Space"}, {kVK_Delete, "Backspace"},
		{kVK_ForwardDelete, "Delete"}, {kVK_Escape, "Escape"}, {kVK_Command, "Command"},
		{kVK_RightCommand, "Right Command"}, {kVK_Shift, "Shift"}, {kVK_RightShift, "Right Shift"},
		{kVK_CapsLock, "Caps Lock"}, {kVK_Option, "Option"}, {kVK_RightOption, "Right Option"},
		{kVK_Control, "Control"}, {kVK_RightControl, "Right Control"}, {kVK_Function, "Fn"},
		{kVK_Home, "Home"}, {kVK_End, "End"}, {kVK_PageUp, "Page Up"}, {kVK_PageDown, "Page Down"}, {kVK_Help, "Help"},
		{kVK_LeftArrow, "Left"}, {kVK_RightArrow, "Right"}, {kVK_UpArrow, "Up"}, {kVK_DownArrow, "Down"},
		{kVK_F1, "F1"}, {kVK_F2, "F2"}, {kVK_F3, "F3"}, {kVK_F4, "F4"}, {kVK_F5, "F5"}, {kVK_F6, "F6"},
		{kVK_F7, "F7"}, {kVK_F8, "F8"}, {kVK_F9, "F9"}, {kVK_F10, "F10"}, {kVK_F11, "F11"}, {kVK_F12, "F12"},
		{kVK_F13, "F13"}, {kVK_F14, "F14"}, {kVK_F15, "F15"}, {kVK_F16, "F16"}, {kVK_F17, "F17"}, {kVK_F18, "F18"},
		{kVK_F19, "F19"}, {kVK_F20, "F20"},
		{kVK_ANSI_Keypad0, "Keypad 0"}, {kVK_ANSI_Keypad1, "Keypad 1"}, {kVK_ANSI_Keypad2, "Keypad 2"},
		{kVK_ANSI_Keypad3, "Keypad 3"}, {kVK_ANSI_Keypad4, "Keypad 4"}, {kVK_ANSI_Keypad5, "Keypad 5"},
		{kVK_ANSI_Keypad6, "Keypad 6"}, {kVK_ANSI_Keypad7, "Keypad 7"}, {kVK_ANSI_Keypad8, "Keypad 8"},
		{kVK_ANSI_Keypad9, "Keypad 9"}, {kVK_ANSI_KeypadDecimal, "Keypad ."}, {kVK_ANSI_KeypadMultiply, "Keypad *"},
		{kVK_ANSI_KeypadPlus, "Keypad +"}, {kVK_ANSI_KeypadClear, "Keypad Clear"}, {kVK_ANSI_KeypadDivide, "Keypad /"},
		{kVK_ANSI_KeypadEnter, "Keypad Enter"}, {kVK_ANSI_KeypadMinus, "Keypad -"}, {kVK_ANSI_KeypadEquals, "Keypad ="},
	};
	if (const auto it = kNames.find(button); it != kNames.end())
		return it->second;
	return fmt::format("key_{}", button);
#else
	return fmt::format("key_{}", button);
#endif
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return g_inputConfigWindowHasFocus;
}

void WindowSystem::NotifyGameLoaded()
{
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->OnGameLoaded();
		g_mainFrame->UpdateSettingsAfterGameLaunch();
	}
}

void WindowSystem::NotifyGameExited()
{
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
		g_mainFrame->RestoreSettingsAfterGameExited();
}

void WindowSystem::RefreshGameList()
{
	std::shared_lock lock(g_mutex);
	if (g_mainFrame)
	{
		g_mainFrame->RequestGameListRefresh();
	}
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	HotkeySettings::CaptureInput(currentState, lastState);
}

bool WindowSystem::IsFullScreen()
{
	return g_window_info.is_fullscreen;
}
