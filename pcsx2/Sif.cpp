/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include <cstring> /* memset */

#define _PC_	/* disables MIPS opcode macros. */

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"

void sifReset(void)
{
	memset(&sif0, 0, sizeof(sif0));
	memset(&sif1, 0, sizeof(sif1));
}

bool SaveStateBase::sifFreeze()
{
	if (!(FreezeTag("SIFdma")))
		return false;

	Freeze(sif0);
	Freeze(sif1);

	return IsOkay();
}
