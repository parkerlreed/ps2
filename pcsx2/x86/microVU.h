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

#pragma once

#include <deque>
#include <algorithm>
#include <cstring> /* memset/memcpy */
#include <memory>

#include "Common.h"
#include "VU.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"
#include "VirtualMemory.h"
#include "common/emitter/x86emitter.h"
#include "microVU_Misc.h"
#include "microVU_IR.h"

class microBlockManager;

struct microBlockLink
{
	microBlock block;
	microBlockLink* next;
};

struct microBlockLinkRef
{
	microBlock* pBlock;
	u64 quick;
};

struct microRange
{
	s32 start; // Start PC (The opcode the block starts at)
	s32 end;   // End PC   (The opcode the block ends with)
};

#define mProgSize (0x4000 / 4)
struct microProgram
{
	u32                data [mProgSize];     // Holds a copy of the VU microProgram
	microBlockManager* block[mProgSize / 2]; // Array of Block Managers
	std::deque<microRange>* ranges;          // The ranges of the microProgram that have already been recompiled
	u32 startPC; // Start PC of this program
	int idx;     // Program index
};

typedef std::deque<microProgram*> microProgramList;

struct microProgramQuick
{
	microBlockManager* block; // Quick reference to valid microBlockManager for current startPC
	microProgram*      prog;  // The microProgram who is the owner of 'block'
};

struct microProgManager
{
	microIR<mProgSize> IRinfo;             // IR information
	microProgramList*  prog [mProgSize/2]; // List of microPrograms indexed by startPC values
	microProgramQuick  quick[mProgSize/2]; // Quick reference to valid microPrograms for current execution
	microProgram*      cur;                // Pointer to currently running MicroProgram
	int                total;              // Total Number of valid MicroPrograms
	int                isSame;             // Current cached microProgram is Exact Same program as mVU.regs().Micro (-1 = unknown, 0 = No, 1 = Yes)
	int                cleared;            // Micro Program is Indeterminate so must be searched for (and if no matches are found then recompile a new one)
	u32                curFrame;           // Frame Counter
	u8*                x86ptr;             // Pointer to program's recompilation code
	u8*                x86start;           // Start of program's rec-cache
	u8*                x86end;             // Limit of program's rec-cache
	microRegInfo       lpState;            // Pipeline state from where program left off (useful for continuing execution)
};

static const uint mVUdispCacheSize = __pagesize; // Dispatcher Cache Size (in bytes)
static const uint mVUcacheSafeZone =  3; // Safe-Zone for program recompilation (in megabytes)
static const uint mVUcacheReserve = 64; // mVU0, mVU1 Reserve Cache Size (in megabytes)

struct microVU
{

	alignas(16) u32 statFlag[4]; // 4 instances of status flag (backup for xgkick)
	alignas(16) u32 macFlag [4]; // 4 instances of mac    flag (used in execution)
	alignas(16) u32 clipFlag[4]; // 4 instances of clip   flag (used in execution)
	alignas(16) u32 xmmCTemp[4];     // Backup used in mVUclamp2()
	alignas(16) u32 xmmBackup[16][4]; // Backup for xmm0~xmm15

	u32 index;        // VU Index (VU0 or VU1)
	u32 cop2;         // VU is in COP2 mode?  (No/Yes)
	u32 vuMemSize;    // VU Main  Memory Size (in bytes)
	u32 microMemSize; // VU Micro Memory Size (in bytes)
	u32 progSize;     // VU Micro Memory Size (in u32's)
	u32 progMemMask;  // VU Micro Memory Size (in u32's)
	u32 cacheSize;    // VU Cache Size

	microProgManager               prog;     // Micro Program Data
	std::unique_ptr<microRegAlloc> regAlloc; // Reg Alloc Class

	RecompiledCodeReserve* cache_reserve;
	u8* cache;        // Dynarec Cache Start (where we will start writing the recompiled code to)
	u8* dispCache;    // Dispatchers Cache (where startFunct and exitFunct are written to)
	u8* startFunct;   // Function Ptr to the recompiler dispatcher (start)
	u8* exitFunct;    // Function Ptr to the recompiler dispatcher (exit)
	u8* startFunctXG; // Function Ptr to the recompiler dispatcher (xgkick resume)
	u8* exitFunctXG;  // Function Ptr to the recompiler dispatcher (xgkick exit)
	u8* compareStateF;// Function Ptr to search which compares all state.
	u8* waitMTVU;     // Ptr to function to save registers/sync VU1 thread
	u8* copyPLState;  // Ptr to function to copy pipeline state into microVU
	u8* resumePtrXG;  // Ptr to recompiled code position to resume xgkick
	u32 code;         // Contains the current Instruction
	u32 divFlag;      // 1 instance of I/D flags
	u32 VIbackup;     // Holds a backup of a VI reg if modified before a branch
	u32 VIxgkick;     // Holds a backup of a VI reg used for xgkick-delays
	u32 branch;       // Holds branch compare result (IBxx) OR Holds address to Jump to (JALR/JR)
	u32 badBranch;    // For Branches in Branch Delay Slots, holds Address the first Branch went to + 8
	u32 evilBranch;   // For Branches in Branch Delay Slots, holds Address to Jump to
	u32 evilevilBranch;// For Branches in Branch Delay Slots (chained), holds Address to Jump to
	u32 p;            // Holds current P instance index
	u32 q;            // Holds current Q instance index
	u32 totalCycles;  // Total Cycles that mVU is expected to run for
	s32 cycles;       // Cycles Counter

	VURegs& regs() const { return ::vuRegs[index]; }

	__fi REG_VI& getVI(uint reg) const { return regs().VI[reg]; }
	__fi VECTOR& getVF(uint reg) const { return regs().VF[reg]; }
	__fi VIFregisters& getVifRegs() const
	{
		return (index && THREAD_VU1) ? vu1Thread.vifRegs : regs().GetVifRegs();
	}
};

class microBlockManager
{
private:
	microBlockLink *qBlockList, *qBlockEnd; // Quick Search
	microBlockLink *fBlockList, *fBlockEnd; // Full  Search
	std::vector<microBlockLinkRef> quickLookup;

public:
	microBlockManager()
	{
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
	}
	~microBlockManager() { reset(); }
	void reset()
	{
		for (microBlockLink* linkI = qBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		for (microBlockLink* linkI = fBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
		quickLookup.clear();
	};
	microBlock* add(microVU& mVU, microBlock* pBlock)
	{
		microBlock* thisBlock = search(mVU, &pBlock->pState);
		if (!thisBlock)
		{
			u8 fullCmp = pBlock->pState.needExactMatch;

			microBlockLink*& blockList = fullCmp ? fBlockList : qBlockList;
			microBlockLink*& blockEnd  = fullCmp ? fBlockEnd  : qBlockEnd;
			microBlockLink*  newBlock  = (microBlockLink*)_aligned_malloc(sizeof(microBlockLink), 32);
			newBlock->block.jumpCache  = nullptr;
			newBlock->next             = nullptr;

			if (blockEnd)
			{
				blockEnd->next = newBlock;
				blockEnd       = newBlock;
			}
			else
				blockEnd = blockList = newBlock;

			memcpy(&newBlock->block, pBlock, sizeof(microBlock));
			thisBlock = &newBlock->block;

			quickLookup.push_back({&newBlock->block, pBlock->pState.quick64[0]});
		}
		return thisBlock;
	}
	__ri microBlock* search(microVU& mVU, microRegInfo* pState)
	{
		if (pState->needExactMatch) // Needs Detailed Search (Exact Match of Pipeline State)
		{
			microBlockLink* prevI = nullptr;
			for (microBlockLink* linkI = fBlockList; linkI != nullptr; prevI = linkI, linkI = linkI->next)
			{
				if (mVUquickSearch(pState, &linkI->block.pState, sizeof(microRegInfo)))
				{
					if (linkI != fBlockList)
					{
						prevI->next = linkI->next;
						linkI->next = fBlockList;
						fBlockList = linkI;
					}

					return &linkI->block;
				}
			}
		}
		else // Can do Simple Search (Only Matches the Important Pipeline Stuff)
		{
			const u64 quick64 = pState->quick64[0];
			for (const microBlockLinkRef& ref : quickLookup)
			{
				if (ref.quick != quick64) continue;
				if (doConstProp && (ref.pBlock->pState.vi15 != pState->vi15))  continue;
				if (doConstProp && (ref.pBlock->pState.vi15v != pState->vi15v)) continue;
				return ref.pBlock;
			}
		}
		return nullptr;
	}
};


// microVU rec structs
alignas(16) microVU microVU0;
alignas(16) microVU microVU1;

// Main Functions
extern void mVUclear(mV, u32, u32);
extern void mVUreset(microVU& mVU, bool resetReserve);
extern void* mVUblockFetch(microVU& mVU, u32 startPC, uptr pState);
_mVUt extern void* mVUcompileJIT(u32 startPC, uptr ptr);

// Prototypes for Linux
extern void mVUcleanUpVU0();
extern void mVUcleanUpVU1();
mVUop(mVUopU);
mVUop(mVUopL);

// Private Functions
extern void mVUcacheProg(microVU& mVU, microProgram& prog);
extern void mVUdeleteProg(microVU& mVU, microProgram*& prog);
_mVUt extern void* mVUsearchProg(u32 startPC, uptr pState);
extern void* mVUexecuteVU0(u32 startPC, u32 cycles);
extern void* mVUexecuteVU1(u32 startPC, u32 cycles);

// recCall Function Pointer
typedef void (*mVUrecCall)(u32, u32);
typedef void (*mVUrecCallXG)(void);

// Include all the *.inl files (microVU compiles as 1 Translation Unit)
#include "microVU_Clamp.inl"
#include "microVU_Misc.inl"
#include "microVU_Analyze.inl"
#include "microVU_Alloc.inl"
#include "microVU_Upper.inl"
#include "microVU_Lower.inl"
#include "microVU_Tables.inl"
#include "microVU_Flags.inl"
#include "microVU_Branch.inl"
#include "microVU_Compile.inl"
#include "microVU_Execute.inl"
#include "microVU_Macro.inl"
