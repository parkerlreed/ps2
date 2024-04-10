/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include "VMManager.h"

#include <atomic>
#include <sstream>
#include <mutex>

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/SettingsWrapper.h"
#include "common/Timer.h"
#include "common/Threading.h"
#include "fmt/core.h"

#include "Counters.h"
#include "CDVD/CDVD.h"
#include "DEV9/DEV9.h"
#include "Elfheader.h"
#include "FW.h"
#include "GameDatabase.h"
#include "GS.h"
#include "Host.h"
#include "HostSettings.h"
#include "IopBios.h"
#include "MTVU.h"
#include "MemoryCardFile.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "SaveState.h"
#include "R5900.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "PAD/PAD.h"
#include "Sio.h"
#include "ps2/BiosTools.h"

#include "DebugTools/MIPSAnalyst.h"

#ifdef _M_X86
#include "common/emitter/x86_intrin.h"
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <objbase.h>
#include <timeapi.h>
#endif

namespace VMManager
{
	static void ApplyGameFixes();
	static bool UpdateGameSettingsLayer();
	static void CheckForConfigChanges(const Pcsx2Config& old_config);
	static void CheckForCPUConfigChanges(const Pcsx2Config& old_config);
	static void CheckForGSConfigChanges(const Pcsx2Config& old_config);
	static void CheckForFramerateConfigChanges(const Pcsx2Config& old_config);
	static void CheckForPatchConfigChanges(const Pcsx2Config& old_config);
	static void CheckForDEV9ConfigChanges(const Pcsx2Config& old_config);
	static void CheckForMemoryCardConfigChanges(const Pcsx2Config& old_config);

	static bool AutoDetectSource(const std::string& filename);
	static bool ApplyBootParameters(VMBootParameters params, std::string* state_to_load);
	static void LoadPatches(const std::string& serial, u32 crc,
		bool show_messages, bool show_messages_when_disabled);
	static void UpdateRunningGame(bool resetting, bool game_starting);

	static void SetTimerResolutionIncreased(bool enabled);
	static void SetHardwareDependentDefaultSettings(SettingsInterface& si);
	static void EnsureCPUInfoInitialized();
	static void SetEmuThreadAffinities();
} // namespace VMManager

static std::unique_ptr<SysMainMemory> s_vm_memory;
static std::unique_ptr<SysCpuProviderPack> s_cpu_provider_pack;

static std::atomic<VMState> s_state{VMState::Shutdown};
static bool s_cpu_implementation_changed = false;
static Threading::ThreadHandle s_vm_thread_handle;

static std::deque<std::thread> s_save_state_threads;
static std::mutex s_save_state_threads_mutex;

static std::recursive_mutex s_info_mutex;
static std::string s_disc_path;
static u32 s_game_crc;
static u32 s_patches_crc;
static std::string s_game_serial;
static std::string s_game_name;
static std::string s_elf_override;
static u32 s_active_game_fixes = 0;
static std::vector<u8> s_widescreen_cheats_data;
static bool s_widescreen_cheats_loaded = false;
static std::vector<u8> s_no_interlacing_cheats_data;
static bool s_no_interlacing_cheats_loaded = false;
static s32 s_active_widescreen_patches = 0;
static u32 s_active_no_interlacing_patches = 0;
static u32 s_mxcsr_saved;
static bool s_gs_open_on_initialize = false;

VMState VMManager::GetState()
{
	return s_state.load(std::memory_order_acquire);
}

void VMManager::SetState(VMState state)
{
	// Some state transitions aren't valid.
	const VMState old_state = s_state.load(std::memory_order_acquire);
	pxAssert(state != VMState::Initializing && state != VMState::Shutdown);
	SetTimerResolutionIncreased(state == VMState::Running);
	s_state.store(state, std::memory_order_release);

	if (state != VMState::Stopping && (state == VMState::Paused || old_state == VMState::Paused))
	{
		const bool paused = (state == VMState::Paused);
		if (!paused)
			PerformanceMetrics::Reset();

		if (state == VMState::Paused)
			Host::OnVMPaused();
		else
			Host::OnVMResumed();
	}
	// If stopping, break execution as soon as possible.
	else if (state == VMState::Stopping && old_state == VMState::Running)
		Cpu->ExitExecution();
}

bool VMManager::HasValidVM()
{
	const VMState state = s_state.load(std::memory_order_acquire);
	return (state >= VMState::Running && state <= VMState::Resetting);
}

u32 VMManager::GetGameCRC()
{
	std::unique_lock lock(s_info_mutex);
	return s_game_crc;
}

std::string VMManager::GetGameSerial()
{
	std::unique_lock lock(s_info_mutex);
	return s_game_serial;
}

bool VMManager::Internal::InitializeGlobals()
{
	x86caps.Identify();
	x86caps.CountCores();
	x86caps.SIMD_EstablishMXCSRmask();
	x86caps.CalculateMHz();

	if (GSinit() != 0)
		return false;

	if (!SPU2::Initialize())
		return false;

	if (USBinit() != 0)
		return false;

	return true;
}

void VMManager::Internal::ReleaseGlobals()
{
	USBshutdown();
	SPU2::Shutdown();
	GSshutdown();
}

bool VMManager::Internal::InitializeMemory()
{
	pxAssert(!s_vm_memory && !s_cpu_provider_pack);

	s_vm_memory = std::make_unique<SysMainMemory>();
	s_cpu_provider_pack = std::make_unique<SysCpuProviderPack>();

	return s_vm_memory->Allocate();
}

void VMManager::Internal::ReleaseMemory()
{
	std::vector<u8>().swap(s_widescreen_cheats_data);
	s_widescreen_cheats_loaded = false;
	std::vector<u8>().swap(s_no_interlacing_cheats_data);
	s_no_interlacing_cheats_loaded = false;

	s_cpu_provider_pack.reset();
	s_vm_memory.reset();
}

SysMainMemory& GetVmMemory()
{
	return *s_vm_memory;
}

SysCpuProviderPack& GetCpuProviders()
{
	return *s_cpu_provider_pack;
}

void VMManager::LoadSettings()
{
	std::unique_lock<std::mutex> lock = Host::GetSettingsLock();
	SettingsInterface* si = Host::GetSettingsInterface();
	SettingsLoadWrapper slw(*si);
	EmuConfig.LoadSave(slw);
	PAD::LoadConfig(*si);
	Host::LoadSettings(*si, lock);

	// Remove any user-specified hacks in the config (we don't want stale/conflicting values when it's globally disabled).
	EmuConfig.GS.MaskUserHacks();
	EmuConfig.GS.MaskUpscalingHacks();

	// Disable interlacing if we have no-interlacing patches active.
	if (s_active_no_interlacing_patches > 0 && EmuConfig.GS.InterlaceMode == GSInterlaceMode::Automatic)
		EmuConfig.GS.InterlaceMode = GSInterlaceMode::Off;

	// Switch to 16:9 if widescreen patches are enabled, and AR is auto.
	if (s_active_widescreen_patches > 0 && EmuConfig.GS.AspectRatio == AspectRatioType::RAuto4_3_3_2)
	{
		// Don't change when reloading settings in the middle of a FMV with switch.
		if (EmuConfig.CurrentAspectRatio == EmuConfig.GS.AspectRatio)
			EmuConfig.CurrentAspectRatio = AspectRatioType::R16_9;

		EmuConfig.GS.AspectRatio = AspectRatioType::R16_9;
	}

	if (HasValidVM())
		ApplyGameFixes();
}

void VMManager::ApplyGameFixes()
{
	s_active_game_fixes = 0;

	const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial);
	if (!game)
		return;

	s_active_game_fixes += game->applyGameFixes(EmuConfig, EmuConfig.EnableGameFixes);
	s_active_game_fixes += game->applyGSHardwareFixes(EmuConfig.GS);
}

bool VMManager::UpdateGameSettingsLayer()
{
	return true;
}

void VMManager::LoadPatches(const std::string& serial, u32 crc, bool show_messages, bool show_messages_when_disabled)
{
	const std::string crc_string(fmt::format("{:08X}", crc));
	s_patches_crc = crc;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;
	ForgetLoadedPatches();

	std::string message;

	int patch_count = 0;
	if (EmuConfig.EnablePatches)
	{
		const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(serial);
		if (game)
		{
			const std::string* patches = game->findPatch(crc);
			if (patches && (patch_count = LoadPatchesFromString(*patches)) > 0)
			{
				PatchesCon->WriteLn(Color_Green, "(GameDB) Patches Loaded: %d", patch_count);
				fmt::format_to(std::back_inserter(message), "{} game patches", patch_count);
			}

			LoadDynamicPatches(game->dynaPatches);
		}
	}

	// regular cheat patches
	int cheat_count = 0;
	if (EmuConfig.EnableCheats)
	{
		cheat_count = LoadPatchesFromDir(crc_string, EmuFolders::Cheats, "Cheats", true);
		if (cheat_count > 0)
		{
			PatchesCon->WriteLn(Color_Green, "Cheats Loaded: %d", cheat_count);
			fmt::format_to(std::back_inserter(message), "{}{} cheat patches", (patch_count > 0) ? " and " : "", cheat_count);
		}
	}

	// wide screen patches
	if (EmuConfig.EnableWideScreenPatches && crc != 0)
	{
		if ((s_active_widescreen_patches = LoadPatchesFromDir(crc_string, EmuFolders::CheatsWS, "Widescreen hacks", false)) > 0)
			Console.WriteLn(Color_Gray, "Found widescreen patches in the cheats_ws folder --> skipping cheats_ws.zip");
		else
		{
			// No ws cheat files found at the cheats_ws folder, try the ws cheats zip file.
			if (!s_widescreen_cheats_loaded)
			{
				s_widescreen_cheats_loaded = true;

				std::optional<std::vector<u8>> data = Host::ReadResourceFile("cheats_ws.zip");
				if (data.has_value())
					s_widescreen_cheats_data = std::move(data.value());
			}

			if (!s_widescreen_cheats_data.empty())
			{
				s_active_widescreen_patches = LoadPatchesFromZip(crc_string, s_widescreen_cheats_data.data(), s_widescreen_cheats_data.size());
				PatchesCon->WriteLn(Color_Green, "(Wide Screen Cheats DB) Patches Loaded: %d", s_active_widescreen_patches);
			}
		}

		if (s_active_widescreen_patches > 0)
		{
			fmt::format_to(std::back_inserter(message), "{}{} widescreen patches", (patch_count > 0 || cheat_count > 0) ? " and " : "", s_active_widescreen_patches);

			// Switch to 16:9 if widescreen patches are enabled, and AR is auto.
			if (EmuConfig.GS.AspectRatio == AspectRatioType::RAuto4_3_3_2)
			{
				// Don't change when reloading settings in the middle of a FMV with switch.
				if (EmuConfig.CurrentAspectRatio == EmuConfig.GS.AspectRatio)
					EmuConfig.CurrentAspectRatio = AspectRatioType::R16_9;

				EmuConfig.GS.AspectRatio = AspectRatioType::R16_9;
			}
		}
	}

	// no-interlacing patches
	if (EmuConfig.EnableNoInterlacingPatches && crc != 0)
	{
		if ((s_active_no_interlacing_patches = LoadPatchesFromDir(crc_string, EmuFolders::CheatsNI, "No-interlacing patches", false)) > 0)
		{
			Console.WriteLn(Color_Gray, "Found no-interlacing patches in the cheats_ni folder --> skipping cheats_ni.zip");
		}
		else
		{
			// No ws cheat files found at the cheats_ws folder, try the ws cheats zip file.
			if (!s_no_interlacing_cheats_loaded)
			{
				s_no_interlacing_cheats_loaded = true;

				std::optional<std::vector<u8>> data = Host::ReadResourceFile("cheats_ni.zip");
				if (data.has_value())
					s_no_interlacing_cheats_data = std::move(data.value());
			}

			if (!s_no_interlacing_cheats_data.empty())
			{
				s_active_no_interlacing_patches = LoadPatchesFromZip(crc_string, s_no_interlacing_cheats_data.data(), s_no_interlacing_cheats_data.size());
				PatchesCon->WriteLn(Color_Green, "(No-Interlacing Cheats DB) Patches Loaded: %u", s_active_no_interlacing_patches);
			}
		}

		if (s_active_no_interlacing_patches > 0)
		{
			fmt::format_to(std::back_inserter(message), "{}{} no-interlacing patches", (patch_count > 0 || cheat_count > 0 || s_active_widescreen_patches > 0) ? " and " : "", s_active_no_interlacing_patches);

			// Disable interlacing in GS if active.
			if (EmuConfig.GS.InterlaceMode == GSInterlaceMode::Automatic)
			{
				EmuConfig.GS.InterlaceMode = GSInterlaceMode::Off;
				GetMTGS().ApplySettings();
			}
		}
	}
	else
	{
		s_active_no_interlacing_patches = 0;
	}

	if (show_messages)
	{
		if (cheat_count > 0 || s_active_widescreen_patches > 0 || s_active_no_interlacing_patches > 0)
		{
			message += " are active.";
			//Host::AddIconOSDMessage("LoadPatches", ICON_FA_FILE_CODE, message, Host::OSD_INFO_DURATION);
		}
		else if (show_messages_when_disabled)
		{
			//Host::AddIconOSDMessage("LoadPatches", ICON_FA_FILE_CODE, "No cheats or patches (widescreen, compatibility or others) are found / enabled.", Host::OSD_INFO_DURATION);
		}
	}
}

void VMManager::UpdateRunningGame(bool resetting, bool game_starting)
{
	// The CRC can be known before the game actually starts (at the bios), so when
	// we have the CRC but we're still at the bios and the settings are changed
	// (e.g. the user presses TAB to speed up emulation), we don't want to apply the
	// settings as if the game is already running (title, loadeding patches, etc).
	const bool ingame      = (ElfCRC && (g_GameLoading || g_GameStarted));
	u32 new_crc            = ingame ? ElfCRC : 0;
	std::string new_serial = ingame ? SysGetDiscID() : SysGetBiosDiscID();

	if (!resetting && s_game_crc == new_crc && s_game_serial == new_serial)
		return;

	{
		std::unique_lock lock(s_info_mutex);
		s_game_serial = std::move(new_serial);
		s_game_crc    = new_crc;
		s_game_name.clear();

		std::string memcardFilters;

		if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial))
		{
			if (!s_elf_override.empty())
				s_game_name = Path::GetFileTitle(s_elf_override);
			else
				s_game_name = game->name;

			memcardFilters = game->memcardFiltersAsString();
		}
		else
		{
			if (s_game_serial.empty() && s_game_crc == 0)
				s_game_name = "Booting PS2 BIOS...";
		}

		sioSetGameSerial(memcardFilters.empty() ? s_game_serial : memcardFilters);

		// If we don't reset the timer here, when using folder memcards the reindex will cause an eject,
		// which a bunch of games don't like since they access the memory card on boot.
		if (game_starting || resetting)
			AutoEject::ClearAll();
	}

	Console.WriteLn(Color_StrongGreen, "Game Changed:");
	Console.WriteLn(Color_StrongGreen, fmt::format("  Name: {}", s_game_name));
	Console.WriteLn(Color_StrongGreen, fmt::format("  Serial: {}", s_game_serial));
	Console.WriteLn(Color_StrongGreen, fmt::format("  CRC: {:08X}", s_game_crc));

	UpdateGameSettingsLayer();
	ApplySettings();

	// Clear the memory card eject notification again when booting for the first time, or starting.
	// Otherwise, games think the card was removed on boot.
	if (game_starting || resetting)
		AutoEject::ClearAll();

	// Check this here, for two cases: dynarec on, and when enable cheats is set per-game.
	if (s_patches_crc != s_game_crc)
		ReloadPatches(game_starting, false);

	GetMTGS().SendGameCRC(new_crc);

	Host::OnGameChanged(s_disc_path, s_elf_override, s_game_serial, s_game_name, s_game_crc);

	MIPSAnalyst::ScanForFunctions(R5900SymbolMap, ElfTextRange.first, ElfTextRange.first + ElfTextRange.second, true);
	R5900SymbolMap.UpdateActiveSymbols();
	R3000SymbolMap.UpdateActiveSymbols();
}

void VMManager::ReloadPatches(bool verbose, bool show_messages_when_disabled)
{
	LoadPatches(s_game_serial, s_game_crc, verbose, show_messages_when_disabled);
}

bool VMManager::AutoDetectSource(const std::string& filename)
{
	if (!filename.empty())
	{
		if (!FileSystem::FileExists(filename.c_str()))
		{
			Host::ReportErrorAsync("Error", fmt::format("Requested filename '{}' does not exist.", filename));
			return false;
		}

		// TODO: Maybe we should check if it's a valid iso here...
		CDVDsys_SetFile(CDVD_SourceType::Iso, filename);
		CDVDsys_ChangeSource(CDVD_SourceType::Iso);
		s_disc_path = filename;
	}
	else
	{
		// make sure we're not fast booting when we have no filename
		CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
		EmuConfig.UseBOOT2Injection = false;
	}
	return true;
}

bool VMManager::ApplyBootParameters(VMBootParameters params, std::string* state_to_load)
{
	const bool default_fast_boot = Host::GetBoolSettingValue("EmuCore", "EnableFastBoot", true);
	EmuConfig.UseBOOT2Injection = params.fast_boot.value_or(default_fast_boot);

	s_elf_override = std::move(params.elf_override);
	s_disc_path.clear();

	// resolve source type
	if (params.source_type.has_value())
	{
		if (params.source_type.value() == CDVD_SourceType::Iso && !FileSystem::FileExists(params.filename.c_str()))
		{
			Host::ReportErrorAsync("Error", fmt::format("Requested filename '{}' does not exist.", params.filename));
			return false;
		}

		// Use specified source type.
		s_disc_path = std::move(params.filename);
		CDVDsys_SetFile(params.source_type.value(), s_disc_path);
		CDVDsys_ChangeSource(params.source_type.value());
	}
	else
	{
		// Automatic type detection of boot parameter based on filename.
		if (!AutoDetectSource(params.filename))
			return false;
	}

	if (!s_elf_override.empty())
	{
		if (!FileSystem::FileExists(s_elf_override.c_str()))
		{
			Host::ReportErrorAsync("Error", fmt::format("Requested boot ELF '{}' does not exist.", s_elf_override));
			return false;
		}

		Hle_SetElfPath(s_elf_override.c_str());
		EmuConfig.UseBOOT2Injection = true;
	}
	else
	{
		Hle_ClearElfPath();
	}

	return true;
}

bool VMManager::Initialize(VMBootParameters boot_params)
{
	pxAssertRel(s_state.load(std::memory_order_acquire) == VMState::Shutdown, "VM is shutdown");

	s_state.store(VMState::Initializing, std::memory_order_release);
	s_vm_thread_handle = Threading::ThreadHandle::GetForCallingThread();
	Host::OnVMStarting();

	ScopedGuard close_state = [] {
		s_vm_thread_handle = {};
		s_state.store(VMState::Shutdown, std::memory_order_release);
		Host::OnVMDestroyed();
	};

	std::string state_to_load;
	if (!ApplyBootParameters(std::move(boot_params), &state_to_load))
		return false;

	// early out if we don't have a bios
	if (!IsBIOSAvailable(EmuConfig.FullpathToBios()))
		return false;

	if (!DoCDVDopen())
		return false;
	ScopedGuard close_cdvd = [] { DoCDVDclose(); };

	s_gs_open_on_initialize = GetMTGS().IsOpen();
	if (!s_gs_open_on_initialize && !GetMTGS().WaitForOpen())
		// we assume GS is going to report its own error
		return false;

	ScopedGuard close_gs = []() {
		if (!s_gs_open_on_initialize)
			GetMTGS().WaitForClose();
	};

	if (!SPU2::Open())
		return false;
	ScopedGuard close_spu2(&SPU2::Close);

	if (PADinit() != 0 || PADopen() != 0)
		return false;
	ScopedGuard close_pad = []() {
		PADclose();
		PADshutdown();
	};

	if (DEV9init() != 0 || DEV9open() != 0)
		return false;
	ScopedGuard close_dev9 = []() {
		DEV9close();
		DEV9shutdown();
	};

	if (!USBopen())
		return false;
	ScopedGuard close_usb = []() {
		USBclose();
	};

	if (FWopen() != 0)
		return false;
	ScopedGuard close_fw = []() { FWclose(); };

	FileMcd_EmuOpen();

	// Don't close when we return
	close_fw.Cancel();
	close_usb.Cancel();
	close_dev9.Cancel();
	close_pad.Cancel();
	close_spu2.Cancel();
	close_gs.Cancel();
	close_cdvd.Cancel();
	close_state.Cancel();

#if defined(_M_X86)
	s_mxcsr_saved = _mm_getcsr();
#elif defined(_M_ARM64)
	s_mxcsr_saved = static_cast<u32>(a64_getfpcr());
#endif

	s_cpu_implementation_changed = false;
	s_cpu_provider_pack->ApplyConfig();
	SetCPUState(EmuConfig.Cpu.sseMXCSR, EmuConfig.Cpu.sseVU0MXCSR, EmuConfig.Cpu.sseVU1MXCSR);
	SysClearExecutionCache();
	memBindConditionalHandlers();

	ForgetLoadedPatches();
	gsUpdateFrequency(EmuConfig);
	cpuReset();

	s_state.store(VMState::Paused, std::memory_order_release);
	Host::OnVMStarted();

	UpdateRunningGame(true, false);

	SetEmuThreadAffinities();

	PerformanceMetrics::Clear();

	return true;
}

void VMManager::Shutdown(bool save_resume_state)
{
	// we'll probably already be stopping (this is how Qt calls shutdown),
	// but just in case, so any of the stuff we call here knows we don't have a valid VM.
	s_state.store(VMState::Stopping, std::memory_order_release);

	SetTimerResolutionIncreased(false);

	// sync everything
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	GetMTGS().WaitGS();

	{
		LastELF.clear();
		DiscSerial.clear();
		ElfCRC = 0;
		ElfEntry = 0;
		ElfTextRange = {};

		std::unique_lock lock(s_info_mutex);
		s_disc_path.clear();
		s_elf_override.clear();
		s_game_crc = 0;
		s_patches_crc = 0;
		s_game_serial.clear();
		s_game_name.clear();
		Host::OnGameChanged(s_disc_path, s_elf_override, s_game_serial, s_game_name, 0);
	}
	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	UpdateGameSettingsLayer();

	std::string().swap(s_elf_override);

#ifdef _M_X86
	_mm_setcsr(s_mxcsr_saved);
#elif defined(_M_ARM64)
	a64_setfpcr(s_mxcsr_saved);
#endif

	ForgetLoadedPatches();
	R3000A::ioman::reset();
	vtlb_Shutdown();
	USBclose();
	SPU2::Close();
	PADclose();
	DEV9close();
	DoCDVDclose();
	FWclose();
	FileMcd_EmuClose();

	// If the fullscreen UI is running, do a hardware reset on the GS
	// so that the texture cache and targets are all cleared.
	if (s_gs_open_on_initialize)
	{
		GetMTGS().WaitGS(false, false, false);
		GetMTGS().ResetGS(true);
	}
	else
	{
		GetMTGS().WaitForClose();
	}

	PADshutdown();
	DEV9shutdown();

	s_state.store(VMState::Shutdown, std::memory_order_release);
	Host::OnVMDestroyed();
}

void VMManager::Reset()
{
	pxAssert(HasValidVM());

	// If we're running, we're probably going to be executing this at event test time,
	// at vsync, which happens in the middle of event handling. Resetting everything
	// immediately here is a bad idea (tm), in fact, it breaks some games (e.g. TC:NYC).
	// So, instead, we tell the rec to exit execution, _then_ reset. Paused is fine here,
	// since the rec won't be running, so it's safe to immediately reset there.
	if (s_state.load(std::memory_order_acquire) == VMState::Running)
	{
		s_state.store(VMState::Resetting, std::memory_order_release);
		return;
	}

	const bool game_was_started = g_GameStarted;

	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	SysClearExecutionCache();
	memBindConditionalHandlers();
	UpdateVSyncRate(true);
	cpuReset();

	// gameid change, so apply settings
	if (game_was_started)
		UpdateRunningGame(true, false);

	// If we were paused, state won't be resetting, so don't flip back to running.
	if (s_state.load(std::memory_order_acquire) == VMState::Resetting)
		s_state.store(VMState::Running, std::memory_order_release);
}

void VMManager::WaitForSaveStateFlush()
{
	std::unique_lock lock(s_save_state_threads_mutex);
	while (!s_save_state_threads.empty())
	{
		// take a thread from the list and join with it. it won't self detatch then, but that's okay,
		// since we're joining with it here.
		std::thread save_thread(std::move(s_save_state_threads.front()));
		s_save_state_threads.pop_front();
		lock.unlock();
		save_thread.join();
		lock.lock();
	}
}

bool VMManager::ChangeDisc(CDVD_SourceType source, std::string path)
{
	const CDVD_SourceType old_type = CDVDsys_GetSourceType();
	const std::string old_path(CDVDsys_GetFile(old_type));

	CDVDsys_ChangeSource(source);
	if (!path.empty())
		CDVDsys_SetFile(source, std::move(path));

	const bool result = DoCDVDopen();
	if (result)
	{
#if 0
		if (source == CDVD_SourceType::NoDisc) { /* Disc removed */ }
		else { /* Disc changed to '{}' */ }
#endif
	}
	else
	{
		/* Failed to open new disc image '{}'. Reverting to old image */
		CDVDsys_ChangeSource(old_type);
		if (!old_path.empty())
			CDVDsys_SetFile(old_type, std::move(old_path));
		if (!DoCDVDopen())
		{
			/* Failed to switch back to old disc image. Removing disc. */
			CDVDsys_ChangeSource(CDVD_SourceType::NoDisc);
			DoCDVDopen();
		}
	}
	cdvd.Tray.cdvdActionSeconds = 1;
	cdvd.Tray.trayState         = CDVD_DISC_OPEN;
	return result;
}

void VMManager::Execute()
{
	// Check for interpreter<->recompiler switches.
	if (std::exchange(s_cpu_implementation_changed, false))
	{
		// We need to switch the cpus out, and reset the new ones if so.
		s_cpu_provider_pack->ApplyConfig();
		SysClearExecutionCache();
		vtlb_ResetFastmem();
	}

	// Execute until we're asked to stop.
	Cpu->Execute();
}

void VMManager::SetPaused(bool paused)
{
	if (!HasValidVM())
		return;

	Console.WriteLn(paused ? "(VMManager) Pausing..." : "(VMManager) Resuming...");
	SetState(paused ? VMState::Paused : VMState::Running);
}

VsyncMode Host::GetEffectiveVSyncMode()
{
	return VsyncMode::Off;
}

const std::string& VMManager::Internal::GetElfOverride()
{
	return s_elf_override;
}

bool VMManager::Internal::IsExecutionInterrupted()
{
	return s_state.load(std::memory_order_relaxed) != VMState::Running || s_cpu_implementation_changed;
}

void VMManager::Internal::EntryPointCompilingOnCPUThread()
{
	// Classic chicken and egg problem here. We don't want to update the running game
	// until the game entry point actually runs, because that can update settings, which
	// can flush the JIT, etc. But we need to apply patches for games where the entry
	// point is in the patch (e.g. WRC 4). So. Gross, but the only way to handle it really.
	LoadPatches(SysGetDiscID(), ElfCRC, true, false);
	ApplyLoadedPatches(PPT_ONCE_ON_LOAD);
}

void VMManager::Internal::GameStartingOnCPUThread()
{
	UpdateRunningGame(false, true);
	ApplyLoadedPatches(PPT_ONCE_ON_LOAD);
	ApplyLoadedPatches(PPT_COMBINED_0_1);
}

void VMManager::Internal::VSyncOnCPUThread()
{
	// TODO: Move frame limiting here to reduce CPU usage after sleeping...
	ApplyLoadedPatches(PPT_CONTINUOUSLY);
	ApplyLoadedPatches(PPT_COMBINED_0_1);

	Host::CPUThreadVSync();
}

void VMManager::CheckForCPUConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.Cpu == old_config.Cpu &&
		EmuConfig.Gamefixes == old_config.Gamefixes &&
		EmuConfig.Speedhacks == old_config.Speedhacks
		)
		return;

	Console.WriteLn("Updating CPU configuration...");
	SetCPUState(EmuConfig.Cpu.sseMXCSR, EmuConfig.Cpu.sseVU0MXCSR, EmuConfig.Cpu.sseVU1MXCSR);
	SysClearExecutionCache();
	memBindConditionalHandlers();

	if (EmuConfig.Cpu.Recompiler.EnableFastmem != old_config.Cpu.Recompiler.EnableFastmem)
		vtlb_ResetFastmem();

	// did we toggle recompilers?
	if (EmuConfig.Cpu.CpusChanged(old_config.Cpu))
	{
		// This has to be done asynchronously, since we're still executing the
		// cpu when this function is called. Break the execution as soon as
		// possible and reset next time we're called.
		s_cpu_implementation_changed = true;
	}

	if (EmuConfig.Cpu.AffinityControlMode != old_config.Cpu.AffinityControlMode ||
		EmuConfig.Speedhacks.vuThread != old_config.Speedhacks.vuThread)
	{
		SetEmuThreadAffinities();
	}
}

void VMManager::CheckForGSConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.GS == old_config.GS)
		return;

	Console.WriteLn("Updating GS configuration...");

	gsUpdateFrequency(EmuConfig);
	UpdateVSyncRate(true);
	GetMTGS().ApplySettings();
}

void VMManager::CheckForFramerateConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.Framerate == old_config.Framerate)
		return;

	Console.WriteLn("Updating frame rate configuration");
	gsUpdateFrequency(EmuConfig);
	UpdateVSyncRate(true);
}

void VMManager::CheckForPatchConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.EnableCheats == old_config.EnableCheats &&
		EmuConfig.EnableWideScreenPatches == old_config.EnableWideScreenPatches &&
		EmuConfig.EnablePatches == old_config.EnablePatches)
	{
		return;
	}

	ReloadPatches(true, true);
}

void VMManager::CheckForDEV9ConfigChanges(const Pcsx2Config& old_config)
{
	if (EmuConfig.DEV9 == old_config.DEV9)
		return;

	DEV9CheckChanges(old_config);
}

void VMManager::CheckForMemoryCardConfigChanges(const Pcsx2Config& old_config)
{
	bool changed = false;

	for (size_t i = 0; i < std::size(EmuConfig.Mcd); i++)
	{
		if (EmuConfig.Mcd[i].Enabled != old_config.Mcd[i].Enabled ||
			EmuConfig.Mcd[i].Filename != old_config.Mcd[i].Filename)
		{
			changed = true;
			break;
		}
	}

	changed |= (EmuConfig.McdEnableEjection != old_config.McdEnableEjection);
	changed |= (EmuConfig.McdFolderAutoManage != old_config.McdFolderAutoManage);

	if (!changed)
		return;

	FileMcd_EmuClose();
	FileMcd_EmuOpen();

	// force card eject when files change
	for (u32 port = 0; port < 2; port++)
	{
		for (u32 slot = 0; slot < 4; slot++)
		{
			const uint index = FileMcd_ConvertToSlot(port, slot);
			if (EmuConfig.Mcd[index].Enabled != old_config.Mcd[index].Enabled ||
				EmuConfig.Mcd[index].Filename != old_config.Mcd[index].Filename)
				AutoEject::Set(port, slot);
		}
	}

	// force reindexing, mc folder code is janky
	std::string sioSerial;
	{
		std::unique_lock lock(s_info_mutex);
		if (const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(s_game_serial))
			sioSerial = game->memcardFiltersAsString();
		if (sioSerial.empty())
			sioSerial = s_game_serial;
	}
	sioSetGameSerial(sioSerial);
}

void VMManager::CheckForConfigChanges(const Pcsx2Config& old_config)
{
	if (HasValidVM())
	{
		CheckForCPUConfigChanges(old_config);
		CheckForFramerateConfigChanges(old_config);
		CheckForPatchConfigChanges(old_config);
		CheckForDEV9ConfigChanges(old_config);
		CheckForMemoryCardConfigChanges(old_config);
		USB::CheckForConfigChanges(old_config);

		if (EmuConfig.EnableCheats != old_config.EnableCheats ||
			EmuConfig.EnableWideScreenPatches != old_config.EnableWideScreenPatches ||
			EmuConfig.EnableNoInterlacingPatches != old_config.EnableNoInterlacingPatches)
		{
			VMManager::ReloadPatches(true, true);
		}
	}

	// For the big picture UI, we still need to update GS settings, since it's running,
	// and we don't update its config when we start the VM.
	if (HasValidVM() || GetMTGS().IsOpen())
		CheckForGSConfigChanges(old_config);

	Host::CheckForSettingsChanges(old_config);
}

void VMManager::ApplySettings()
{
	// if we're running, ensure the threads are synced
	const bool running = (s_state.load(std::memory_order_acquire) == VMState::Running);
	if (running)
	{
		if (THREAD_VU1)
			vu1Thread.WaitVU();
		GetMTGS().WaitGS(false);
	}

	// Reset to a clean Pcsx2Config. Otherwise things which are optional (e.g. gamefixes)
	// do not use the correct default values when loading.
	Pcsx2Config old_config(std::move(EmuConfig));
	EmuConfig = Pcsx2Config();
	EmuConfig.CopyRuntimeConfig(old_config);
	LoadSettings();
	CheckForConfigChanges(old_config);
}

void VMManager::SetDefaultSettings(SettingsInterface& si)
{
	{
		Pcsx2Config temp_config;
		SettingsSaveWrapper ssw(si);
		temp_config.LoadSave(ssw);
	}

	// Settings not part of the Pcsx2Config struct.
	si.SetBoolValue("EmuCore", "EnableFastBoot", true);

	SetHardwareDependentDefaultSettings(si);
}

#ifdef _WIN32

#include "common/RedtapeWindows.h"

static bool s_timer_resolution_increased = false;

void VMManager::SetTimerResolutionIncreased(bool enabled)
{
	if (s_timer_resolution_increased == enabled)
		return;

	if (enabled)
		s_timer_resolution_increased = (timeBeginPeriod(1) == TIMERR_NOERROR);
	else if (s_timer_resolution_increased)
	{
		timeEndPeriod(1);
		s_timer_resolution_increased = false;
	}
}

#else

void VMManager::SetTimerResolutionIncreased(bool enabled)
{
}

#endif

static std::vector<u32> s_processor_list;
static std::once_flag s_processor_list_initialized;

#if defined(__linux__) || defined(_WIN32)

#include "cpuinfo.h"

static u32 GetProcessorIdForProcessor(const cpuinfo_processor* proc)
{
#if defined(__linux__)
	return static_cast<u32>(proc->linux_id);
#elif defined(_WIN32)
	return static_cast<u32>(proc->windows_processor_id);
#else
	return 0;
#endif
}

static void InitializeCPUInfo()
{
	if (!cpuinfo_initialize())
	{
		Console.Error("Failed to initialize cpuinfo");
		return;
	}

	const u32 cluster_count = cpuinfo_get_clusters_count();
	if (cluster_count == 0)
	{
		Console.Error("Invalid CPU count returned");
		return;
	}

	Console.WriteLn(Color_StrongYellow, "Processor count: %u cores, %u processors", cpuinfo_get_cores_count(), cpuinfo_get_processors_count());
	Console.WriteLn(Color_StrongYellow, "Cluster count: %u", cluster_count);

	static std::vector<const cpuinfo_processor*> ordered_processors;
	for (u32 i = 0; i < cluster_count; i++)
	{
		const cpuinfo_cluster* cluster = cpuinfo_get_cluster(i);
		for (u32 j = 0; j < cluster->processor_count; j++)
		{
			const cpuinfo_processor* proc = cpuinfo_get_processor(cluster->processor_start + j);
			if (!proc)
				continue;

			ordered_processors.push_back(proc);
		}
	}
	// find the large and small clusters based on frequency
	// this is assuming the large cluster is always clocked higher
	// sort based on core, so that hyperthreads get pushed down
	std::sort(ordered_processors.begin(), ordered_processors.end(), [](const cpuinfo_processor* lhs, const cpuinfo_processor* rhs) {
		return (lhs->core->frequency > rhs->core->frequency || lhs->smt_id < rhs->smt_id);
	});

	s_processor_list.reserve(ordered_processors.size());
	std::stringstream ss;
	ss << "Ordered processor list: ";
	for (const cpuinfo_processor* proc : ordered_processors)
	{
		if (proc != ordered_processors.front())
			ss << ", ";

		const u32 procid = GetProcessorIdForProcessor(proc);
		ss << procid;
		if (proc->smt_id != 0)
			ss << "[SMT " << proc->smt_id << "]";

		s_processor_list.push_back(procid);
	}
	Console.WriteLn(ss.str());
}

static void SetMTVUAndAffinityControlDefault(SettingsInterface& si)
{
	VMManager::EnsureCPUInfoInitialized();

	const u32 cluster_count = cpuinfo_get_clusters_count();
	if (cluster_count == 0)
	{
		Console.Error("Invalid CPU count returned");
		return;
	}

	Console.WriteLn("Cluster count: %u", cluster_count);

	for (u32 i = 0; i < cluster_count; i++)
	{
		const cpuinfo_cluster* cluster = cpuinfo_get_cluster(i);
		Console.WriteLn("  Cluster %u: %u cores and %u processors at %u MHz",
			i, cluster->core_count, cluster->processor_count, static_cast<u32>(cluster->frequency /* / 1000000u*/));
	}

	const bool has_big_little = cluster_count > 1;
	Console.WriteLn("Big-Little: %s", has_big_little ? "yes" : "no");

	const u32 big_cores = cpuinfo_get_cluster(0)->core_count + ((cluster_count > 2) ? cpuinfo_get_cluster(1)->core_count : 0u);
	Console.WriteLn("Guessing we have %u big/medium cores...", big_cores);

	if (big_cores >= 3)
	{
		Console.WriteLn("  Enabling MTVU.");
		si.SetBoolValue("EmuCore/Speedhacks", "vuThread", true);
	}
	else
	{
		Console.WriteLn("  Disabling MTVU.");
		si.SetBoolValue("EmuCore/Speedhacks", "vuThread", false);
	}

	const int extra_threads = (big_cores > 3) ? 3 : 2;
	Console.WriteLn("  Setting Extra Software Rendering Threads to %d.", extra_threads);
	si.SetIntValue("EmuCore/GS", "extrathreads", extra_threads);
}

#else

static void InitializeCPUInfo()
{
	Console.WriteLn("(VMManager) InitializeCPUInfo() not implemented.");
}

static void SetMTVUAndAffinityControlDefault(SettingsInterface& si)
{
}

#endif

void VMManager::EnsureCPUInfoInitialized()
{
	std::call_once(s_processor_list_initialized, InitializeCPUInfo);
}

void VMManager::SetEmuThreadAffinities()
{
	EnsureCPUInfoInitialized();

	if (s_processor_list.empty())
	{
		// not supported on this platform
		return;
	}

	if (EmuConfig.Cpu.AffinityControlMode == 0 ||
		s_processor_list.size() < (EmuConfig.Speedhacks.vuThread ? 3 : 2))
	{
		if (EmuConfig.Cpu.AffinityControlMode != 0)
			Console.Error("Insufficient processors for affinity control.");

		GetMTGS().GetThreadHandle().SetAffinity(0);
		vu1Thread.GetThreadHandle().SetAffinity(0);
		s_vm_thread_handle.SetAffinity(0);
		return;
	}

	static constexpr u8 processor_assignment[7][2][3] = {
		//EE xx GS  EE VU GS
		{{0, 2, 1}, {0, 1, 2}}, // Disabled
		{{0, 2, 1}, {0, 1, 2}}, // EE > VU > GS
		{{0, 2, 1}, {0, 2, 1}}, // EE > GS > VU
		{{0, 2, 1}, {1, 0, 2}}, // VU > EE > GS
		{{1, 2, 0}, {2, 0, 1}}, // VU > GS > EE
		{{1, 2, 0}, {1, 2, 0}}, // GS > EE > VU
		{{1, 2, 0}, {2, 1, 0}}, // GS > VU > EE
	};

	// steal vu's thread if mtvu is off
	const u8* this_proc_assigment = processor_assignment[EmuConfig.Cpu.AffinityControlMode][EmuConfig.Speedhacks.vuThread];
	const u32 ee_index = s_processor_list[this_proc_assigment[0]];
	const u32 vu_index = s_processor_list[this_proc_assigment[1]];
	const u32 gs_index = s_processor_list[this_proc_assigment[2]];
	Console.WriteLn("Processor order assignment: EE=%u, VU=%u, GS=%u",
		this_proc_assigment[0], this_proc_assigment[1], this_proc_assigment[2]);

	const u64 ee_affinity = static_cast<u64>(1) << ee_index;
	Console.WriteLn(Color_StrongGreen, "EE thread is on processor %u (0x%llx)", ee_index, ee_affinity);
	s_vm_thread_handle.SetAffinity(ee_affinity);

	if (EmuConfig.Speedhacks.vuThread)
	{
		const u64 vu_affinity = static_cast<u64>(1) << vu_index;
		Console.WriteLn(Color_StrongGreen, "VU thread is on processor %u (0x%llx)", vu_index, vu_affinity);
		vu1Thread.GetThreadHandle().SetAffinity(vu_affinity);
	}
	else
	{
		vu1Thread.GetThreadHandle().SetAffinity(0);
	}

	const u64 gs_affinity = static_cast<u64>(1) << gs_index;
	Console.WriteLn(Color_StrongGreen, "GS thread is on processor %u (0x%llx)", gs_index, gs_affinity);
	GetMTGS().GetThreadHandle().SetAffinity(gs_affinity);
}

void VMManager::SetHardwareDependentDefaultSettings(SettingsInterface& si)
{
	SetMTVUAndAffinityControlDefault(si);
}

const std::vector<u32>& VMManager::GetSortedProcessorList()
{
	EnsureCPUInfoInitialized();
	return s_processor_list;
}

#if 0
bool SaveStateBase::vmFreeze()
{
	const u32 prev_crc = s_current_crc;
	const std::string prev_elf = s_elf_path;
	const bool prev_elf_executed = s_elf_executed;
	Freeze(s_current_crc);
	FreezeString(s_elf_path);
	Freeze(s_elf_executed);

	// We have to test all the variables here, because we could be loading a state created during ELF load, after the ELF has loaded.
	if (IsLoading())
	{
		// Might need new ELF info.
		if (s_elf_path != prev_elf)
		{
			if (s_elf_path.empty())
			{
				// Shouldn't have executed a non-existant ELF.. unless you load state created from a deleted ELF override I guess.
				if (s_elf_executed)
					Console.Error("Somehow executed a non-existant ELF");
				VMManager::ClearELFInfo();
			}
			else
			{
				VMManager::UpdateELFInfo(std::move(s_elf_path));
			}
		}

		if (s_current_crc != prev_crc || s_elf_path != prev_elf || s_elf_executed != prev_elf_executed)
			VMManager::HandleELFChange(true);
	}

	return IsOkay();
}
#endif
