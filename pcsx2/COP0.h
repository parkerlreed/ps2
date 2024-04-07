/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2009  PCSX2 Dev Team
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

struct tlbs;

extern void WriteCP0Status(u32 value);
extern void WriteCP0Config(u32 value);

// Updates the CPU's mode of operation (either, Kernel, Supervisor, or User modes).
// Currently the different modes are not implemented.
// Given this function is called so much, it's commented out for now. (rama)
#define cpuUpdateOperationMode() ((void)0)

extern void WriteTLB(int i);
extern void UnmapTLB(const tlbs& t, int i);
extern void MapTLB(const tlbs& t, int i);

extern void COP0_UpdatePCCR();
extern void COP0_DiagnosticPCCR();
