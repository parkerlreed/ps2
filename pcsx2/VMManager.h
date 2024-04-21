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

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <string>
#include <string_view>
#include <optional>

#include "Config.h"

enum class CDVD_SourceType : uint8_t;

enum class VMState
{
	Shutdown = 0,
	Initializing,
	Running,
	Paused,
	Resetting,
	Stopping,
};

struct VMBootParameters
{
	std::string filename;
	std::string elf_override;
	std::optional<CDVD_SourceType> source_type;

	std::optional<bool> fast_boot;
};

namespace VMManager
{
	/// Returns the current state of the VM.
	VMState GetState();

	/// Alters the current state of the VM.
	void SetState(VMState state);

	/// Returns true if there is an active virtual machine.
	bool HasValidVM();

	/// Returns the serial of the disc/executable currently running.
	std::string GetDiscSerial();

	/// Loads global settings (i.e. EmuConfig).
	void LoadSettings();

	/// Initializes all system components.
	bool Initialize(VMBootParameters boot_params);

	/// Destroys all system components.
	void Shutdown(bool save_resume_state);

	/// Resets all subsystems to a cold boot.
	void Reset();

	/// Runs the VM until the CPU execution is canceled.
	void Execute();

	/// Changes the pause state of the VM, resetting anything needed when unpausing.
	void SetPaused(bool paused);

	/// Reloads settings, and applies any changes present.
	void ApplySettings();

	/// Reloads cheats/patches. If verbose is set, the number of patches loaded will be shown in the OSD.
	void ReloadPatches(bool verbose, bool show_messages_when_disabled);

	/// Changes the disc in the virtual CD/DVD drive. Passing an empty will remove any current disc.
	/// Returns false if the new disc can't be opened.
	bool ChangeDisc(CDVD_SourceType source, std::string path);

	/// Initializes default configuration in the specified file.
	void SetDefaultSettings(SettingsInterface& si);

	/// Returns a list of processors in the system, and their corresponding affinity mask.
	/// This list is ordered by most performant to least performant for pinning threads to.
	const std::vector<u32>& GetSortedProcessorList();

	/// Internal callbacks, implemented in the emu core.
	namespace Internal
	{
		/// Initializes common host state, called on the CPU thread.
		void CPUThreadInitialize(void);

		/// Cleans up common host state, called on the CPU thread.
		void CPUThreadShutdown();

		const std::string& GetElfOverride();
		bool IsExecutionInterrupted();
		void EntryPointCompilingOnCPUThread();
		void GameStartingOnCPUThread();
		void SwappingGameOnCPUThread();
		void VSyncOnCPUThread();
	} // namespace Internal
} // namespace VMManager


namespace Host
{
	/// Called with the settings lock held, when system settings are being loaded (should load input sources, etc).
	void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);

	/// Called after settings are updated.
	void CheckForSettingsChanges(const Pcsx2Config& old_config);

	/// Called when the VM is starting initialization, but has not been completed yet.
	void OnVMStarting();

	/// Called when the VM is created.
	void OnVMStarted();

	/// Called when the VM is shut down or destroyed.
	void OnVMDestroyed();

	/// Called when the VM is paused.
	void OnVMPaused();

	/// Called when the VM is resumed after being paused.
	void OnVMResumed();

	/// Provided by the host; called when the running executable changes.
	void OnGameChanged(const std::string& disc_path, const std::string& elf_override, const std::string& game_serial,
		u32 game_crc);
} // namespace Host
