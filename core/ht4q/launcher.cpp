/*
	HydroThunder4Quest: startup screen - start the game, or change the settings first (controller
	mapping, tilt controls, textures, haptics). Drawn by Flycast's GUI (gui_display_ui) instead of
	starting the game right away; in the headset it is pointed at with a controller (the XR presenter
	feeds the pointer to the GUI as a mouse). Settings are saved to emu.cfg.
	2 players on two headsets: "Host" starts the game with a full screen per player and streams player 2's
	view (core/ht4q/twoplayer.cpp, stream.cpp); "Join" finds a host on the network and plays player 2
	from its stream (viewer.cpp) - no game needed on the joining headset.

	Shown in Quest builds; on the desktop with HT4Q_LAUNCHER=1 (mouse only).
	HT4Q_JOIN=<host>[:port]   join that host right away (tests)
*/
#include "ht4q.h"
#include "controls.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/maple/maple_cfg.h"
#include "oslib/oslib.h"
#include "imgui.h"
#include "ui/imgui_driver.h"
#include "rend/gles/ht4q_stereo.h"
#include "stream.h"
#include "network/ggpo.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <unistd.h>

namespace ht4q
{

static const char *GAME_ID = "T-9702N";

enum class Page { Main, Settings, Join, Viewing };
static Page page = Page::Main;
static bool started;

// Remapping: waiting for a controller input for (func, slot)
static struct {
	int func = -1, slot = 0;
	bool released = false;	// all inputs let go since the capture began (the click itself)
	double deadline = 0;
} capture;
static double calibrateAt;	// time to take the neutral tilt, 0: none

static double now() { return ImGui::GetTime(); }

bool launcherOpen()
{
	static const bool enabled = [] {
		const char *v = getenv("HT4Q_LAUNCHER");
#ifdef HT4Q_XR
		return v == nullptr || v[0] != '0';
#else
		return v != nullptr && v[0] == '1';
#endif
	}();
	return enabled && !started;
}

#ifdef HT4Q_XR
// The app's private files directory (tools/ht4q/install.sh puts the game in game/ under it)
static std::string privateDir()
{
	char pkg[256] {};
	if (FILE *f = fopen("/proc/self/cmdline", "r"))
	{
		fread(pkg, 1, sizeof(pkg) - 1, f);
		fclose(f);
	}
	return std::string("/data/data/") + pkg + "/files";
}

static std::string toLower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}
#endif


// The game to boot when none was given: a disc image in the private game directory, Rev 1 cue first
static std::string findGame()
{
#ifdef HT4Q_XR
	const std::string dir = privateDir() + "/game";
	std::string best;
	int bestScore = -1;
	if (DIR *d = opendir(dir.c_str()))
	{
		while (dirent *e = readdir(d))
		{
			const std::string name = e->d_name, lower = toLower(name);
			const size_t dot = lower.rfind('.');
			const std::string ext = dot == std::string::npos ? "" : lower.substr(dot);
			if (ext != ".cue" && ext != ".gdi" && ext != ".chd" && ext != ".cdi")
				continue;
			const int score = (lower.find("rev 1") != std::string::npos ? 2 : 0) + (ext == ".cue" ? 1 : 0);
			if (score > bestScore)
				best = dir + "/" + name, bestScore = score;
		}
		closedir(d);
	}
	return best;
#else
	return "";
#endif
}

static std::string gamePath()
{
	static std::string found = settings.content.path.empty() ? findGame() : settings.content.path;
	return found;
}

static bool hqTexturesInstalled()
{
#ifdef HT4Q_XR
	// install.sh puts HQ textures in the private files directory (textures/T-9702N)
	if (config::TexturePath.get().empty())
		config::TexturePath.set({ privateDir() });
#endif
	std::string path = hostfs::getTextureLoadPath(GAME_ID);
	return !path.empty() && access(path.c_str(), F_OK) == 0;
}

static constexpr int STREAM_PORT = 3266;

static void startGame()
{
	if (settings.content.path.empty())
		settings.content.path = gamePath();
	// the rumble pack goes in the controller's second slot (the first holds the VMU with the saves)
	config::MapleExpansionDevices[0][1] = MDT_PurupuruPack;
	SaveSettings();
	started = true;
	NOTICE_LOG(COMMON, "ht4q launcher: starting %s", settings.content.path.c_str());
}

static bool bigButton(const char *label, float width)
{
	return ImGui::Button(label, ImVec2(width, ImGui::GetFontSize() * 2.2f));
}

// A horizontal -1..1 meter centered on 0
static void meter(const char *label, float v)
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(ImGui::GetFontSize() * 7);
	ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetFontSize() * 12, h = ImGui::GetFontSize() * 0.8f, mid = p.x + w / 2;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(60, 60, 70, 255));
	const float x = mid + std::clamp(v, -1.f, 1.f) * w / 2;
	dl->AddRectFilled(ImVec2(std::min(mid, x), p.y), ImVec2(std::max(mid, x), p.y + h), IM_COL32(80, 170, 255, 255));
	dl->AddLine(ImVec2(mid, p.y), ImVec2(mid, p.y + h), IM_COL32(255, 255, 255, 200));
	ImGui::Dummy(ImVec2(w, h));
}

static void updateCapture()
{
	if (capture.func < 0)
		return;
	XrControllers c;
	const bool active = xrControllers(c);
	if (!active || now() > capture.deadline)
	{
		capture.func = -1;
		return;
	}
	// a button, trigger or grip; for an axis function a stick pushed along an axis
	const bool axisFunc = controls::funcIsAxis(capture.func);
	const float axes[4] { c.hand[0].stick[0], c.hand[0].stick[1], c.hand[1].stick[0], c.hand[1].stick[1] };
	int pressed = -1;
	bool any = false;
	for (int i = 0; i < XI_COUNT; i++)
	{
		any = any || c.value[i] > 0.2f;
		if (!axisFunc && c.value[i] > 0.6f && pressed < 0)
			pressed = i;
	}
	for (int a = 0; a < 4; a++)
	{
		any = any || std::fabs(axes[a]) > 0.3f;
		if (axisFunc && std::fabs(axes[a]) > 0.7f && pressed < 0)
			pressed = XA_LX + a;
	}
	if (!capture.released)
	{
		capture.released = !any;
		return;
	}
	if (pressed < 0)
		return;
	// an input does one thing: take it away from the others
	for (int f = 0; f < F_COUNT; f++)
	{
		Binding b = controls::binding(f);
		bool changed = false;
		for (int& in : b)
			if (in == pressed)
				in = -1, changed = true;
		if (changed)
			controls::setBinding(f, b);
	}
	Binding b = controls::binding(capture.func);
	b[capture.slot] = pressed;
	controls::setBinding(capture.func, b);
	capture.func = -1;
}

enum Tab { TAB_BUTTONS, TAB_STEERING, TAB_DISPLAY, TAB_RUMBLE, TAB_COUNT };
static const char *tabNames[TAB_COUNT] { "Buttons", "Steering", "Display", "Rumble" };
static int selectTab = -1;		// a tab to switch to (test hook), -1: none

static void help(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

static void buttonsTab()
{
	help("Select a slot, then press the button, trigger or grip to use for it. "
		"For the two adjust axes, push a thumbstick instead.");
	if (ImGui::BeginTable("map", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn("1", ImGuiTableColumnFlags_WidthStretch, 1.f);
		ImGui::TableSetupColumn("2", ImGuiTableColumnFlags_WidthStretch, 1.f);
		ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed);
		for (int f = 0; f < F_COUNT; f++)
		{
			ImGui::PushID(f);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(controls::funcLabel(f));
			const Binding b = controls::binding(f);
			for (int slot = 0; slot < 2; slot++)
			{
				ImGui::TableNextColumn();
				ImGui::PushID(slot);
				const bool waiting = capture.func == f && capture.slot == slot;
				std::string label = waiting ? "Press... " + std::to_string((int)std::ceil(capture.deadline - now()))
						: controls::inputLabel(b[slot]);
				ImGui::BeginDisabled(capture.func >= 0 && !waiting);
				if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)) && !waiting)
				{
					capture.func = f;
					capture.slot = slot;
					capture.released = false;
					capture.deadline = now() + 6;
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::TableNextColumn();
			ImGui::BeginDisabled(capture.func >= 0);
			if (ImGui::Button("Clear"))
				controls::setBinding(f, { -1, -1 });
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(capture.func >= 0);
	if (ImGui::Button("Reset all controls to defaults"))
		controls::resetDefaults();
	ImGui::EndDisabled();
}

static void steeringTab()
{
	const float sliderW = ImGui::GetContentRegionAvail().x * 0.45f;
	int steering = (int)controls::steering();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Steer with");
	bool changed = ImGui::RadioButton("Right stick", &steering, (int)Steering::RightStick);
	ImGui::SameLine();
	changed |= ImGui::RadioButton("Left stick", &steering, (int)Steering::LeftStick);
	ImGui::SameLine();
	changed |= ImGui::RadioButton("Tilt left/right", &steering, (int)Steering::Tilt);
	if (changed)
		controls::setSteering((Steering)steering);
	ImGui::Spacing();
	bool tiltSpeed = controls::tiltSpeed();
	if (ImGui::Checkbox("Tilt forward/back for throttle and brake", &tiltSpeed))
		controls::setTiltSpeed(tiltSpeed);
	help("The triggers still work too.");
	if (steering != (int)Steering::Tilt && !tiltSpeed)
		return;
	ImGui::SeparatorText("Tilt");
	help("Tilt either controller; the one tilted furthest counts.");
	int range = controls::tiltRange();
	ImGui::SetNextItemWidth(sliderW);
	if (ImGui::SliderInt("Tilt for full lock / full throttle", &range, 10, 60, "%d deg"))
		controls::setTiltRange(range);
	if (calibrateAt > 0)
	{
		ImGui::Text("Hold the controllers as you would with no throttle... %d", (int)std::ceil(calibrateAt - now()));
		if (now() >= calibrateAt)
		{
			controls::calibrateTilt();
			calibrateAt = 0;
		}
	}
	else if (ImGui::Button("Set the no-throttle position"))
		calibrateAt = now() + 3;
	XrControllers c;
	float steer = 0, speed = 0;
	if (xrControllers(c))
		controls::tiltAxes(c, steer, speed);
	ImGui::Spacing();
	meter("Steering", steer);
	meter("Speed", speed);
}

static void displayTab()
{
	const float sliderW = ImGui::GetContentRegionAvail().x * 0.45f;
	ImGui::SeparatorText("Textures");
	const bool hq = hqTexturesInstalled();
	int tex = config::CustomTextures && hq ? 1 : 0;
	if (ImGui::RadioButton("Original", &tex, 0))
		config::CustomTextures.set(false);
	ImGui::SameLine();
	ImGui::BeginDisabled(!hq);
	if (ImGui::RadioButton("HQ (AI-upscaled)", &tex, 1))
		config::CustomTextures.set(true);
	ImGui::EndDisabled();
	if (!hq)
		help("HQ textures are not installed: make them from your own game with tools/ht4q/hqtex.py.");

	ImGui::SeparatorText("Frame rate");
	bool fps60 = frameRate60();
	if (ImGui::Checkbox("60 fps (experimental)", &fps60))
		setFrameRate60(fps60);
	help("The game normally runs at 30 fps. At 60, motion is smoother; the speed of the boats is "
		"matched, but some effects may run faster. Switches immediately.");

	ImGui::SeparatorText("View");
	help("In a race: press the right stick to pause and adjust these with the sticks.");
	int hud = controls::hudSize(), depth = controls::depthEffect();
	ImGui::SetNextItemWidth(sliderW);
	if (ImGui::SliderInt("HUD size", &hud, 30, 120, "%d%%"))
		controls::setHudSize(hud);
	ImGui::SetNextItemWidth(sliderW);
	if (ImGui::SliderInt("Depth effect", &depth, 0, 300, "%d%%"))
		controls::setDepthEffect(depth);
	if (ImGui::Button("Reset view"))
		controls::resetView();
}

static void rumbleTab()
{
	help("The game's rumble, played on both controllers.");
	int strength = controls::hapticStrength();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
	if (ImGui::SliderInt("Rumble strength", &strength, 0, 100, strength == 0 ? "Off" : "%d%%"))
		controls::setHapticStrength(strength);
	ImGui::SameLine();
	if (ImGui::Button("Test"))
		onVibration(0, 1.f, 0.f, 400);
}

// Player 1 here, player 2 on the headset that joins: a second controller (fed by the stream viewer),
// a full screen per player, and the stream server.
static void hostGame()
{
	config::MapleMainDevices[1] = MDT_SegaController;
	config::MapleExpansionDevices[1][0] = MDT_None;
	config::MapleExpansionDevices[1][1] = MDT_PurupuruPack;	// player 2's rumble, sent to the viewer
	setTwoPlayerFull(true);
	streamStart(STREAM_PORT);
	startGame();
}

static void joinPage(float base)
{
	ImGui::PushFont(nullptr, base * 1.6f);
	ImGui::TextUnformatted("Join a 2-player game");
	ImGui::PopFont();
	ImGui::SameLine(ImGui::GetContentRegionMax().x - base * 8);
	if (ImGui::Button("Back", ImVec2(base * 8, 0)))
	{
		viewerDiscover(false);
		page = Page::Main;
		return;
	}
	viewerDiscover(true);
	ImGui::Spacing();
	help("On the other headset, choose Host 2-player game. Games on this network appear here.");
	ImGui::Spacing();
	const std::vector<StreamHost> hosts = viewerHosts();
	if (hosts.empty())
		ImGui::TextDisabled("Looking for games...");
	ImGui::PushFont(nullptr, base * 1.3f);
	for (const StreamHost& h : hosts)
	{
		const std::string label = h.name + "  (" + h.addr + ")";
		if (bigButton(label.c_str(), ImGui::GetContentRegionMax().x * 0.6f))
		{
			viewerDiscover(false);
			viewerJoin(h.addr, h.port);
			page = Page::Viewing;
		}
	}
	ImGui::PopFont();
	if (viewerState() == ViewerState::Failed)
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Lost the connection to the game.");
}

// The joined game: player 2's view from the host, with this player's controller sent back.
static void viewingPage(float base)
{
	MapleInputState pad[4] {};
	ggpo::getInput(pad);
	applyXrInput(pad);
	memserverPoll(pad);		// scripted tests
	viewerSendInput(pad[0]);

	static ViewerFrame frame;
	static ImTextureID tex;
	if (viewerFrame(frame))
	{
		tex = imguiDriver->updateTexture("ht4q_stream", frame.rgba.data(), frame.width, frame.height, false);
		// HT4Q_FRAMEDUMP=<path>: <path>.shot.req -> the received frame to <path>.png (tests)
		static const char *dump = getenv("HT4Q_FRAMEDUMP");
		if (dump != nullptr && access((std::string(dump) + ".shot.req").c_str(), F_OK) == 0)
		{
			unlink((std::string(dump) + ".shot.req").c_str());
			stbi_write_png((std::string(dump) + ".png").c_str(), frame.width, frame.height, 4, frame.rgba.data(), frame.width * 4);
		}
	}
	ImGuiIO& io = ImGui::GetIO();
	if (tex && frame.width > 0)
	{
		// one eye of a stereo frame; letterboxed in the window
		const float fw = frame.info.stereo ? frame.width / 2.f : frame.width, fh = frame.height;
		const float scale = std::min(io.DisplaySize.x / fw, io.DisplaySize.y / fh);
		const ImVec2 size(fw * scale, fh * scale);
		const ImVec2 pos((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2);
		ImGui::GetBackgroundDrawList()->AddImage(tex, pos, ImVec2(pos.x + size.x, pos.y + size.y),
				ImVec2(0, 0), ImVec2(frame.info.stereo ? 0.5f : 1.f, 1));
	}
	const ViewerState st = viewerState();
	if (st != ViewerState::Connected)
	{
		ImGui::TextUnformatted(st == ViewerState::Connecting ? "Joining..." : "Lost the connection to the game.");
		if (bigButton("Back", base * 10))
		{
			viewerLeave();
			page = Page::Join;
		}
	}
}

static void settingsPage(float base)
{
	ImGui::PushFont(nullptr, base * 1.6f);
	ImGui::TextUnformatted("Settings");
	ImGui::PopFont();
	ImGui::SameLine(ImGui::GetContentRegionMax().x - base * 8);
	ImGui::BeginDisabled(capture.func >= 0);
	const bool done = ImGui::Button("Done", ImVec2(base * 8, 0));
	ImGui::EndDisabled();
	if (done)
	{
		SaveSettings();
		capture.func = -1;
		calibrateAt = 0;
		page = Page::Main;
		return;
	}
	ImGui::PushFont(nullptr, base * 1.25f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(base * 0.9f, base * 0.35f));
	const bool tabs = ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink);
	ImGui::PopStyleVar();
	ImGui::PopFont();
	if (!tabs)
		return;
	static void (*const draw[TAB_COUNT])() { buttonsTab, steeringTab, displayTab, rumbleTab };
	for (int t = 0; t < TAB_COUNT; t++)
	{
		// while waiting for a controller input, its trigger must not also switch tabs
		ImGui::BeginDisabled(capture.func >= 0 && t != TAB_BUTTONS);
		ImGui::PushFont(nullptr, base * 1.25f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(base * 0.9f, base * 0.35f));
		const bool open = ImGui::BeginTabItem(tabNames[t], nullptr, selectTab == t ? ImGuiTabItemFlags_SetSelected : 0);
		ImGui::PopStyleVar();
		ImGui::PopFont();
		ImGui::EndDisabled();
		if (!open)
			continue;
		ImGui::Spacing();
		ImGui::BeginChild("tab", ImVec2(0, 0));
		draw[t]();
		ImGui::EndChild();
		ImGui::EndTabItem();
	}
	selectTab = -1;
	ImGui::EndTabBar();
}

// Makes the black around the logo's oval transparent: near-black pixels reachable from the edges
static void clearBackground(u8 *rgba, int w, int h)
{
	std::vector<int> todo;
	std::vector<bool> seen(w * h);
	for (int x = 0; x < w; x++)
		todo.push_back(x), todo.push_back((h - 1) * w + x);
	for (int y = 0; y < h; y++)
		todo.push_back(y * w), todo.push_back(y * w + w - 1);
	while (!todo.empty())
	{
		const int i = todo.back();
		todo.pop_back();
		u8 *p = &rgba[i * 4];
		if (seen[i] || std::max({ p[0], p[1], p[2] }) > 40)
			continue;
		seen[i] = true;
		p[3] = 0;
		const int x = i % w, y = i / w;
		if (x > 0) todo.push_back(i - 1);
		if (x < w - 1) todo.push_back(i + 1);
		if (y > 0) todo.push_back(i - w);
		if (y < h - 1) todo.push_back(i + w);
	}
}

// The Hydro Thunder logo from the title screen, out of the player's own texture pack or dump
// (game artwork, so it is never shipped with the app). Null if there is none.
static ImTextureID logo(float& aspect)
{
	static ImTextureID id;
	static float ratio;
	static bool tried;
	if (!tried)
	{
		tried = true;
		hqTexturesInstalled();	// sets the texture path
		const std::string file = "5069ed0e.png";	// its texture hash
		for (const std::string& path : { hostfs::getTextureLoadPath(GAME_ID) + file,
				hostfs::getTextureDumpPath() + GAME_ID + "/" + file })
		{
			int w, h, n;
			if (u8 *data = stbi_load(path.c_str(), &w, &h, &n, 4))
			{
				clearBackground(data, w, h);
				id = imguiDriver->updateTexture("ht4q_logo", data, w, h, false);
				ratio = (float)w / h;
				stbi_image_free(data);
				break;
			}
		}
	}
	aspect = ratio;
	return id;
}

static void mainPage(float base)
{
	ImGuiIO& io = ImGui::GetIO();
	const float width = ImGui::GetContentRegionMax().x;
	ImGui::Dummy(ImVec2(0, io.DisplaySize.y * 0.08f));
	float aspect;
	if (ImTextureID tex = logo(aspect))
	{
		const float lw = width * 0.5f;
		ImGui::SetCursorPosX((width - lw) / 2);
		ImGui::Image(tex, ImVec2(lw, lw / aspect));
	}
	else
	{
		ImGui::Dummy(ImVec2(0, io.DisplaySize.y * 0.1f));
		ImGui::PushFont(nullptr, base * 3.f);
		const char *title = "HYDRO THUNDER";
		ImGui::SetCursorPosX((width - ImGui::CalcTextSize(title).x) / 2);
		ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.f, 1.f), "%s", title);
		ImGui::PopFont();
	}
	const char *sub = "for Meta Quest";
	ImGui::SetCursorPosX((width - ImGui::CalcTextSize(sub).x) / 2);
	ImGui::TextDisabled("%s", sub);
	ImGui::Dummy(ImVec2(0, base * 1.5f));
	ImGui::PushFont(nullptr, base * 1.4f);
	const float bw = width * 0.3f;
	const bool haveGame = !gamePath().empty();
	ImGui::SetCursorPosX((width - bw) / 2);
	ImGui::BeginDisabled(!haveGame);
	if (bigButton("Start game", bw))
		startGame();
	ImGui::EndDisabled();
	ImGui::SetCursorPosX((width - bw) / 2);
	ImGui::BeginDisabled(!haveGame);
	if (bigButton("Host 2-player game", bw))
		hostGame();
	ImGui::EndDisabled();
	ImGui::SetCursorPosX((width - bw) / 2);
	if (bigButton("Join 2-player game", bw))
		page = Page::Join;
	ImGui::SetCursorPosX((width - bw) / 2);
	if (bigButton("Settings", bw))
		page = Page::Settings;
	ImGui::SetCursorPosX((width - bw) / 2);
	if (bigButton("Quit", bw))
	{
		SaveSettings();
		dc_exit();
	}
	ImGui::PopFont();
	if (!haveGame)
	{
		const char *msg = "Game not found. Install your Hydro Thunder disc image with tools/ht4q/install.sh.";
		ImGui::SetCursorPosX((width - ImGui::CalcTextSize(msg).x) / 2);
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%s", msg);
	}
	ImGui::SetCursorPosY(io.DisplaySize.y - base * 2.5f);
	const char *hint = "Point with a controller and pull its trigger to select";
	ImGui::SetCursorPosX((width - ImGui::CalcTextSize(hint).x) / 2);
	ImGui::TextDisabled("%s", hint);
}

// Test hook: <data dir>/../ht4q_ui.req holding "main" or a settings tab number switches the page,
// "start" starts the game, "adjust" toggles the view adjustment in a race (with quest.sh shot, to check layouts without the controllers)
static void uiRequest()
{
#ifdef HT4Q_XR
	static const std::string req = get_writable_data_path("../ht4q_ui.req");
	if (access(req.c_str(), F_OK) != 0)
		return;
	char buf[16] {};
	if (FILE *f = fopen(req.c_str(), "r"))
	{
		fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
	}
	unlink(req.c_str());
	if (strncmp(buf, "main", 4) == 0)
		page = Page::Main;
	else if (strncmp(buf, "adjust", 6) == 0)
		toggleAdjust();
	else if (strncmp(buf, "start", 5) == 0)
		startGame();
	else
	{
		page = Page::Settings;
		selectTab = std::clamp(atoi(buf), 0, TAB_COUNT - 1);
	}
#endif
}

void drawAdjustPanel()
{
	uiRequest();
	if (!adjustingView())
		return;
	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 pos(io.DisplaySize.x * ADJUST_PANEL[0], io.DisplaySize.y * ADJUST_PANEL[1]);
	const ImVec2 size(io.DisplaySize.x * ADJUST_PANEL[2], io.DisplaySize.y * ADJUST_PANEL[3]);
	const float base = io.DisplaySize.y / 30.f;
	const ImVec4 hintColor(0.82f, 0.86f, 0.92f, 1.f);	// readable over a bright scene
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(base, base * 0.6f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::Begin("##ht4q_adjust", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	ImDrawList *panel = ImGui::GetWindowDrawList();
#ifdef HT4Q_XR
	// into a framebuffer of its own, with alpha: the headset shows it over the scene (ht4q_xr.cpp)
	panel->AddCallback([](const ImDrawList *, const ImDrawCmd *) { beginPanelTarget(); }, nullptr);
#endif
	// a light veil behind the text, so the scene shows through
	panel->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(8, 16, 30, 110), base * 0.5f);
	ImGui::PushFont(nullptr, base);
	ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.f, 1.f), "Paused: adjusting the view");
	ImGui::Spacing();
	const auto value = [&](const char *name, int pct, int lo, int hi, int axisFunc) {
		ImGui::TextUnformatted(name);
		ImGui::SameLine(base * 7);
		ImGui::PushFont(nullptr, base * 1.3f);
		ImGui::Text("%d%%", pct);
		ImGui::PopFont();
		ImGui::SameLine(base * 10.5f);
		// a bar for the range
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = size.x - base * 11.5f, h = base * 0.5f, y = p.y + base * 0.45f;
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), IM_COL32(40, 55, 80, 255), h / 2);
		dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * (pct - lo) / (hi - lo), y + h), IM_COL32(80, 170, 255, 255), h / 2);
		ImGui::Dummy(ImVec2(w, base * 1.3f));
		ImGui::PushStyleColor(ImGuiCol_Text, hintColor);
		ImGui::Text("    %s", controls::inputLabel(controls::binding(axisFunc)[0]));
		ImGui::PopStyleColor();
	};
	value("HUD size", controls::hudSize(), 30, 120, F_HUD_AXIS);
	value("Depth effect", controls::depthEffect(), 0, 300, F_DEPTH_AXIS);
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, hintColor);
	ImGui::Text("%s: reset to defaults", controls::inputLabel(controls::binding(F_RESET_VIEW)[0]));
	ImGui::Text("%s: resume the race", controls::inputLabel(controls::binding(F_ADJUST)[0]));
	ImGui::PopStyleColor();
	ImGui::PopFont();
#ifdef HT4Q_XR
	panel->AddCallback([](const ImDrawList *, const ImDrawCmd *) { endPanelTarget(); }, nullptr);
#endif
	ImGui::End();
	ImGui::PopStyleVar(2);
}

void drawLauncher()
{
	ImGuiIO& io = ImGui::GetIO();
	uiRequest();
	updateCapture();
	static bool autoJoin = [] {
		if (const char *v = getenv("HT4Q_JOIN"))
		{
			std::string host = v;
			const size_t colon = host.find(':');
			const int port = colon == std::string::npos ? STREAM_PORT : atoi(host.c_str() + colon + 1);
			viewerJoin(host.substr(0, colon), port);
			page = Page::Viewing;
		}
		return true;
	}();
	(void)autoJoin;
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, page == Page::Viewing ? IM_COL32(0, 0, 0, 0) : IM_COL32(12, 22, 40, 255));
	ImGui::Begin("##ht4q_launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoSavedSettings);
	// sized for the headset's 1440x1080 screen, 2.4 m wide at 2 m
	const float base = io.DisplaySize.y / 36.f;
	ImGui::PushFont(nullptr, base);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(base * 0.5f, base * 0.3f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(base * 0.5f, base * 0.4f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(base, base * 0.8f));
	if (page == Page::Main)
		mainPage(base);
	else if (page == Page::Join)
		joinPage(base);
	else if (page == Page::Viewing)
		viewingPage(base);
	else
		settingsPage(base);
	ImGui::PopStyleVar(3);
	ImGui::PopFont();
	ImGui::End();
	ImGui::PopStyleColor();

	// a large pointer, visible in the headset
	if (ImGui::IsMousePosValid())
	{
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		dl->AddCircleFilled(io.MousePos, base * 0.35f, IM_COL32(255, 255, 255, 230));
		dl->AddCircle(io.MousePos, base * 0.35f, IM_COL32(0, 0, 0, 255), 0, 2.f);
	}
}

}	// namespace ht4q
