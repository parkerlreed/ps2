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

#include <cstring> /* memset */

#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"

alignas(16) IPU_Fifo ipu_fifo;

void IPU_Fifo::init()
{
	out.readpos = 0;
	out.writepos = 0;
	in.readpos = 0;
	in.writepos = 0;
	memset(in.data, 0, sizeof(in.data));
	memset(out.data, 0, sizeof(out.data));
}

void IPU_Fifo_Input::clear()
{
	memset(data, 0, sizeof(data));
	g_BP.IFC = 0;
	ipuRegs.ctrl.IFC = 0;
	readpos = 0;
	writepos = 0;

	// Because the FIFO is drained it will request more data immediately
	IPU1Status.DataRequested = true;

	if (ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
	{
		CPU_INT(DMAC_TO_IPU, 4);
	}
}

void IPU_Fifo_Output::clear()
{
	memset(data, 0, sizeof(data));
	ipuRegs.ctrl.OFC = 0;
	readpos = 0;
	writepos = 0;
}

void IPU_Fifo::clear()
{
	in.clear();
	out.clear();
}

int IPU_Fifo_Input::write(u32* pMem, int size)
{
	int transsize;
	int firsttrans = std::min(size, 8 - (int)g_BP.IFC);

	g_BP.IFC += firsttrans;
	transsize = firsttrans;

	while (transsize-- > 0)
	{
		CopyQWC(&data[writepos], pMem);
		writepos = (writepos + 4) & 31;
		pMem += 4;
	}

	if (g_BP.IFC == 8)
		IPU1Status.DataRequested = false;

	return firsttrans;
}

int IPU_Fifo_Input::read(void *value)
{
	// wait until enough data to ensure proper streaming.
	if (g_BP.IFC <= 1)
	{
		// IPU FIFO is empty and DMA is waiting so lets tell the DMA we are ready to put data in the FIFO
		IPU1Status.DataRequested = true;

		if(ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
		{
			CPU_INT( DMAC_TO_IPU, 4);
		}

		if (g_BP.IFC == 0) return 0;
		pxAssert(g_BP.IFC > 0);
	}

	CopyQWC(value, &data[readpos]);

	readpos = (readpos + 4) & 31;
	g_BP.IFC--;
	return 1;
}

int IPU_Fifo_Output::write(const u32 *value, uint size)
{
	pxAssertMsg(size>0, "Invalid size==0 when calling IPU_Fifo_Output::write");

	uint origsize = size;
	/*do {*/
		//IPU0dma();

		uint transsize = std::min(size, 8 - (uint)ipuRegs.ctrl.OFC);
		if(!transsize) return 0;

		ipuRegs.ctrl.OFC += transsize;
		size -= transsize;
		while (transsize > 0)
		{
			CopyQWC(&data[writepos], value);
			writepos = (writepos + 4) & 31;
			value += 4;
			--transsize;
		}
	/*} while(true);*/
	if(ipu0ch.chcr.STR)
		IPU_INT_FROM(ipuRegs.ctrl.OFC * BIAS);
	return origsize - size;
}

void IPU_Fifo_Output::read(void *value, uint size)
{
	pxAssert(ipuRegs.ctrl.OFC >= size);
	ipuRegs.ctrl.OFC -= size;

	while (size > 0)
	{
		CopyQWC(value, &data[readpos]);

		readpos = (readpos + 4) & 31;
		value = (u128*)value + 1;
		--size;
	}
}

void ReadFIFO_IPUout(mem128_t* out)
{
	if (!pxAssertDev( ipuRegs.ctrl.OFC > 0, "Attempted read from IPUout's FIFO, but the FIFO is empty!" )) return;
	ipu_fifo.out.read(out, 1);

	// Games should always check the fifo before reading from it -- so if the FIFO has no data
	// its either some glitchy game or a bug in pcsx2.
}

void WriteFIFO_IPUin(const mem128_t* value)
{
	//committing every 16 bytes
	if( ipu_fifo.in.write((u32*)value, 1) == 0 )
	{
		if (ipuRegs.ctrl.BUSY && !CommandExecuteQueued)
		{
			CommandExecuteQueued = true;
			CPU_INT(IPU_PROCESS, 1 * BIAS);
		}
	}
}
