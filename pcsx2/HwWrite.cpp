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


#include "PrecompiledHeader.h"

#include <cstring>

#include "Common.h"
#include "Hardware.h"
#include "Gif_Unit.h"
#include "IopMem.h"

#include "ps2/HwInternal.h"

#include "ps2/pgif.h"
#include "SPU2/spu2.h"
#include "R3000A.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

using namespace R5900;

// Shift the middle 8 bits (bits 4-12) into the lower 8 bits.
// This helps the compiler optimize the switch statement into a lookup table. :)

#define HELPSWITCH(m) (((m)>>4) & 0xff)
#define mcase(src) case HELPSWITCH(src)

template< uint page > void _hwWrite8(u32 mem, u8 value);
template< uint page > void _hwWrite16(u32 mem, u8 value);
template< uint page > void TAKES_R128 _hwWrite128(u32 mem, r128 value);


template<uint page>
void _hwWrite32( u32 mem, u32 value )
{
	// Notes:
	// All unknown registers on the EE are "reserved" as discarded writes and indeterminate
	// reads.  Bus error is only generated for registers outside the first 16k of mapped
	// register space (which is handled by the VTLB mapping, so no need for checks here).

	switch (page)
	{
		case 0x00:	if (!rcntWrite32<0x00>(mem, value)) return;	break;
		case 0x01:	if (!rcntWrite32<0x01>(mem, value)) return;	break;

		case 0x02:
			if (!ipuWrite32(mem, value)) return;
		break;

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Direct FIFO read/write behavior.  We need to create a test that writes
			// data to one of the FIFOs and determine the result.  I'm not quite sure offhand a good
			// way to do that --air
			// Current assumption is that 32-bit and 64-bit writes likely do 128-bit zero-filled
			// writes (upper 96 bits are 0, lower 32 bits are effective).
			u128 zerofill;
			zerofill._u32[0] = 0;
			zerofill._u32[1] = 0;
			zerofill.hi      = 0;
			zerofill._u32[(mem >> 2) & 0x03] = value;

			_hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
		}
		return;

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
				{
					if (!vifWrite32<1>(mem, value)) return;
				}
				else
				{
					if (!vifWrite32<0>(mem, value)) return;
				}
			}
			else switch(mem)
			{
				case (GIF_CTRL):
				{
					// Not exactly sure what RST needs to do
					gifRegs.ctrl._u32 = value & 9;
					if (gifRegs.ctrl.RST) {
						gifUnit.Reset(true); // Should it reset gsSIGNAL?
					}
					gifRegs.stat.PSE = gifRegs.ctrl.PSE;
					return;
				}

				case (GIF_MODE):
				{
					gifRegs.mode._u32 = value;
					//Need to kickstart the GIF if the M3R mask comes off
					if (gifRegs.stat.M3R == 1 && gifRegs.mode.M3R == 0 && (gifch.chcr.STR || gif_fifo.fifoSize))
					{
						CPU_INT(DMAC_GIF, 8);
					}


					gifRegs.stat.M3R = gifRegs.mode.M3R;
					gifRegs.stat.IMT = gifRegs.mode.IMT;
					return;
				}
			}
		break;

		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0b:
		case 0x0c:
		case 0x0d:
		case 0x0e:
			if (!dmacWrite32<page>(mem, value)) return;
		break;

		case 0x0f:
		{
			switch( HELPSWITCH(mem) )
			{
				mcase(INTC_STAT):
					psHu32(INTC_STAT) &= ~value;
					//cpuTestINTCInts();
				return;

				mcase(INTC_MASK):
					psHu32(INTC_MASK) ^= (u16)value;
					cpuTestINTCInts();
				return;

				mcase(SIO_TXFIFO):
				{
					u8* woot = (u8*)&value;
					// [Ps2Confirm] What happens when we write 32 bit values to SIO_TXFIFO?
					// If it works like the IOP, then all 32 bits are written to the FIFO in
					// order.  PCSX2 up to this point simply ignored non-8bit writes to this port.
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[0]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[1]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[2]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[3]);
				}
				return;

				mcase(SBUS_F200):
					// Performs a standard psHu32 assignment (which is the default action anyway).
					//psHu32(mem) = value;
				break;

				mcase(SBUS_F220):
					psHu32(mem) |= value;
				return;

				mcase(SBUS_F230):
					psHu32(mem) &= ~value;
				return;

				mcase(SBUS_F240) :
					if (value & (1 << 19))
					{
						u32 cycle = psxRegs.cycle;
						//pgifInit();
						psxReset();
						PSXCLK =  33868800;
						SPU2::Reset(true);
						setPs1CDVDSpeed(cdvd.Speed);
						psxHu32(0x1f801450) = 0x8;
						psxHu32(0x1f801078) = 1;
						psxRegs.cycle = cycle;
					}
					if(!(value & 0x100))
						psHu32(mem) &= ~0x100;
					else
						psHu32(mem) |= 0x100;
				return;

				mcase(SBUS_F260):
					psHu32(mem) = value;
				return;

				mcase(MCH_RICM)://MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5
					if ((((value >> 16) & 0xFFF) == 0x21) && (((value >> 6) & 0xF) == 1) && (((psHu32(0xf440) >> 7) & 1) == 0))//INIT & SRP=0
						rdram_sdevid = 0;	// if SIO repeater is cleared, reset sdevid
					psHu32(mem) = value & ~0x80000000;	//kill the busy bit
				return;

				mcase(MCH_DRD):
					// Performs a standard psHu32 assignment (which is the default action anyway).
					//psHu32(mem) = value;
				break;

				mcase(DMAC_ENABLEW):
					if (!dmacWrite32<0x0f>(DMAC_ENABLEW, value)) return;
				break;

				default:
					// TODO: psx add the real address in a sbus mcase
					if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
						// Tharr be console spam here! Need to figure out how to print what mode
						PGIFw((mem & 0x1FFFFFFF), value);
						return;
					}

				//mcase(SIO_ISR):
				//mcase(0x1000f410):
				// Mystery Regs!  No one knows!?
				// (unhandled so fall through to default)

			}
		}
		break;
	}

	psHu32(mem) = value;
}

template<uint page>
void hwWrite32( u32 mem, u32 value )
{
	_hwWrite32<page>( mem, value );
}

// --------------------------------------------------------------------------------------
//  hwWrite8 / hwWrite16 / hwWrite64 / hwWrite128
// --------------------------------------------------------------------------------------

template< uint page >
void _hwWrite8(u32 mem, u8 value)
{
	if (mem == SIO_TXFIFO)
	{
		static bool included_newline = false;
		static char sio_buffer[1024];
		static int sio_count;

		if (value == '\r')
		{
			included_newline = true;
			sio_buffer[sio_count++] = '\n';
		}
		else if (!included_newline || (value != '\n'))
		{
			included_newline = false;
			sio_buffer[sio_count++] = value;
		}

		if ((sio_count == std::size(sio_buffer)-1) || (sio_count != 0 && sio_buffer[sio_count-1] == '\n'))
		{
			sio_buffer[sio_count] = 0;
			sio_count = 0;
		}
		return;
	}

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			_hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			return;
	}

	u32 merged = _hwRead32<page,false>(mem & ~0x03);
	((u8*)&merged)[mem & 0x3] = value;

	_hwWrite32<page>(mem & ~0x03, merged);
}

template< uint page >
void hwWrite8(u32 mem, u8 value)
{
	_hwWrite8<page>(mem, value);
}

template< uint page >
void _hwWrite16(u32 mem, u16 value)
{
	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			_hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			return;
	}

	u32 merged = _hwRead32<page,false>(mem & ~0x03);
	((u16*)&merged)[(mem>>1) & 0x1] = value;

	hwWrite32<page>(mem & ~0x03, merged);
}

template< uint page >
void hwWrite16(u32 mem, u16 value)
{
	_hwWrite16<page>(mem, value);
}

template<uint page>
void _hwWrite64( u32 mem, u64 value )
{
	// * Only the IPU has true 64 bit registers.
	// * FIFOs have 128 bit registers that are probably zero-fill.
	// * All other registers likely disregard the upper 32-bits and simply act as normal
	//   32-bit writes.
	switch (page)
	{
		case 0x02:
			if (!ipuWrite64(mem, value)) return;
		break;

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			u128 zerofill;
			zerofill._u32[0] = 0;
			zerofill._u32[1] = 0;
			zerofill.hi      = 0;
			zerofill._u64[(mem >> 3) & 0x01] = value;
			hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
		}
		return;

		default:
			// disregard everything except the lower 32 bits.
			// ... and skip the 64 bit writeback since the 32-bit one will suffice.
			hwWrite32<page>( mem, value );
		return;
	}

	memcpy(&eeHw[(mem) & 0xffff], &value, sizeof(value));
}

template<uint page>
void hwWrite64( u32 mem, mem64_t value )
{
	_hwWrite64<page>(mem, value);
}

template< uint page >
void TAKES_R128 _hwWrite128(u32 mem, r128 srcval)
{
	// FIFOs are the only "legal" 128 bit registers.  Handle them first.
	// all other registers fall back on the 64-bit handler (and from there
	// most of them fall back to the 32-bit handler).

	switch (page)
	{
		case 0x04:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF0(&usrcval);
			}
			return;

		case 0x05:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF1(&usrcval);
			}
			return;

		case 0x06:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_GIF(&usrcval);
			}
			return;

		case 0x07:
			if (mem & 0x10)
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_IPUin(&usrcval);
			}
			return;

		case 0x0F:
			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				PGIFwQword((mem & 0x1FFFFFFF), (void*)&usrcval);
				return;
			}

		default: break;
	}

	hwWrite64<page>(mem, r128_to_u64(srcval));
}

template< uint page >
void TAKES_R128 hwWrite128(u32 mem, r128 srcval)
{
	_hwWrite128<page>(mem, srcval);
}

#define InstantizeHwWrite(pageidx) \
	template void hwWrite8<pageidx>(u32 mem, mem8_t value); \
	template void hwWrite16<pageidx>(u32 mem, mem16_t value); \
	template void hwWrite32<pageidx>(u32 mem, mem32_t value); \
	template void hwWrite64<pageidx>(u32 mem, mem64_t value); \
	template void TAKES_R128 hwWrite128<pageidx>(u32 mem, r128 srcval);

InstantizeHwWrite(0x00);	InstantizeHwWrite(0x08);
InstantizeHwWrite(0x01);	InstantizeHwWrite(0x09);
InstantizeHwWrite(0x02);	InstantizeHwWrite(0x0a);
InstantizeHwWrite(0x03);	InstantizeHwWrite(0x0b);
InstantizeHwWrite(0x04);	InstantizeHwWrite(0x0c);
InstantizeHwWrite(0x05);	InstantizeHwWrite(0x0d);
InstantizeHwWrite(0x06);	InstantizeHwWrite(0x0e);
InstantizeHwWrite(0x07);	InstantizeHwWrite(0x0f);
