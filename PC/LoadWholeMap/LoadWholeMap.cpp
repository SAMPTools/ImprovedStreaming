#include "plugin.h"
#include "..\injector\assembly.hpp"
#include "CStreaming.h"
#include "IniReader/IniReader.h"
#include "CIplStore.h"
#include "extensions/ScriptCommands.h"
#include "CMessages.h"
#include "CTimer.h"
#include "CPopCycle.h"
#include "CAnimManager.h"
#include <vector>
#include <algorithm>

using namespace plugin;
using namespace std;
using namespace injector;

const int MAX_MB = 2000;
const int MULT_MB_TO_BYTE = 1048576;
constexpr unsigned int MAX_BYTE_LIMIT = (MAX_MB * MULT_MB_TO_BYTE);

class LoadWholeMap {
public:

    LoadWholeMap() {
		static unsigned int streamMemoryForced = 0;

		static int totalBinaryIPLconfig = 0;
		static int totalBinaryIPLloaded = 0;

		static int loadCheck = 0;
		static float removeUnusedWhenPercent = 0.0f;
		static int lastTimeRemoveUnused = 0;
		static int gameStartedAfterLoad = 0;
		static int removeUnusedIntervalMs = 0;
		static fstream lg;

		static CIniReader ini("ImprovedStreaming.ini");
		static bool loadBinaryIPLs = ini.ReadInteger("Settings", "LoadBinaryIPLs", 0) == 1;
		static bool preLoadAnims = ini.ReadInteger("Settings", "PreLoadAnims", 0) == 1;
		static int logMode = ini.ReadInteger("Settings", "LogMode", -1);

		if (logMode >= 0) {
			lg.open("ImprovedStreaming.log", fstream::out | fstream::trunc);
			lg << "v1.0" << endl;
		}

		Events::initRwEvent += []
		{
			if (GetModuleHandleA("LoadWholeMap.SA.asi")) {
				MessageBoxA(0, "'LoadWholeMap.SA.asi' is now 'ImprovedStreaming.SA.asi'. Delete the old mod.", "ImprovedStreaming.SA.asi", 0);
			}

			/*if (loadBinaryIPLs)
			{
				static std::vector<std::string> IPLStreamNames;

				totalBinaryIPLconfig = 0;
				ifstream stream("LoadWholeMap_BinaryIPLs.dat");
				for (string line; getline(stream, line); ) {
					if (line[0] != ';' && line[0] != '#') {
						while (getline(stream, line) && line.compare("end")) {
							if (line[0] != ';' && line[0] != '#') {
								char name[32];
								int loadWhen; // not used anymore
								if (sscanf(line.c_str(), "%s %i", name, &loadWhen) >= 1)
								{
									IPLStreamNames.push_back(name);
								}
							}
						}
					}
				}

				// Based on ThirteenAG's project2dfx
				// Not working now (even original p2dfx code didn't work)
				struct LoadAllBinaryIPLs
				{
					void operator()(injector::reg_pack&)
					{
						static auto CIplStoreLoad = (char *(__cdecl *)()) 0x5D54A0;
						CIplStoreLoad();

						static auto IplFilePoolLocate = (int(__cdecl *)(const char *name)) 0x404AC0;
						static auto CIplStoreRequestIplAndIgnore = (char *(__cdecl *)(int a1)) 0x405850;

						injector::address_manager::singleton().IsHoodlum() ?
							injector::WriteMemory<char>(0x015651C1 + 3, 0, true) :
							injector::WriteMemory<char>(0x405881 + 3, 0, true);

						for (auto it = IPLStreamNames.cbegin(); it != IPLStreamNames.cend(); it++)
						{
							lg << "Loading IPL " << (string)*it << "\n";
							lg.flush();
							CIplStoreRequestIplAndIgnore(IplFilePoolLocate(it->c_str()));
						}

						injector::address_manager::singleton().IsHoodlum() ?
							injector::WriteMemory<char>(0x015651C1 + 3, 1, true) :
							injector::WriteMemory<char>(0x405881 + 3, 1, true);
					}
				}; injector::MakeInline<LoadAllBinaryIPLs>(0x5D19A4);
			}*/

		};

		// ---------------------------------------------------

		Events::initScriptsEvent.after += []
		{
			loadCheck = 1;
		};

		Events::processScriptsEvent.after += []
		{
			if (loadCheck < 3) // ignore first thicks
			{
				loadCheck++;
				return;
			}

			if (streamMemoryForced > 0) {
				if (CStreaming::ms_memoryAvailable < streamMemoryForced) {
					CStreaming::ms_memoryAvailable = streamMemoryForced;
				}
			}

			/*if (totalBinaryIPLconfig > 0 && totalBinaryIPLloaded < totalBinaryIPLconfig)
			{
				for (unsigned int i = 0; i < GetBinaryIPLconfigVector().size(); i++) {
					if (GetBinaryIPLconfigVector()[i].loaded == false && CTimer::m_snTimeInMilliseconds > GetBinaryIPLconfigVector()[i].loadWhen)
					{
						CIplStore::RequestIplAndIgnore(GetBinaryIPLconfigVector()[i].slot);
						GetBinaryIPLconfigVector()[i].loaded = true;
						totalBinaryIPLloaded++;
					}
				}
			}*/

			if (loadCheck == 3)
			{
				if (!(GetKeyState(0x10) & 0x8000)) // SHIFT
				{

					streamMemoryForced = ini.ReadInteger("Settings", "StreamMemoryForced", 0);
					if (streamMemoryForced > 0)
					{
						if (streamMemoryForced > MAX_MB) {
							streamMemoryForced = MAX_BYTE_LIMIT;
						}
						else {
							streamMemoryForced *= MULT_MB_TO_BYTE;
						}
						CStreaming::ms_memoryAvailable = streamMemoryForced;
					}

					removeUnusedWhenPercent = ini.ReadFloat("Settings", "RemoveUnusedWhenPercent", 0.0f);
					removeUnusedIntervalMs = ini.ReadInteger("Settings", "RemoveUnusedInterval", 60);

					CTimer::Stop();

					if (preLoadAnims) {
						int animBlocksIdStart = injector::ReadMemory<int>(0x48C36B + 2, true);
						for (int id = 1; id < CAnimManager::ms_numAnimBlocks; ++id) {
							if (logMode >= 1)
							{
								lg << "Start loading anim " << id << endl;
							}
							CStreaming::RequestModel(id + animBlocksIdStart, eStreamingFlags::MISSION_REQUIRED);
							CAnimManager::AddAnimBlockRef(id);
						}
						CStreaming::LoadAllRequestedModels(false);
						if (logMode >= 1)
						{
							lg << "Finished loading anims." << endl;
						}
					}

					int i = 0;
					while (true)
					{
						int loadEach = 0;
						int startId = -1;
						int endId = -1;
						int ignoreStart = -1;
						int ignoreEnd = -1;
						int biggerThan = -1;
						int smallerThan = -1;
						int ignorePedGroup = -1;
						bool keepLoaded = true;

						i++;

						string range = "Range" + to_string(i);

						loadEach = ini.ReadInteger(range, "LoadEach", 0);
						startId = ini.ReadInteger(range, "Start", -1);
						endId = ini.ReadInteger(range, "End", -1);
						ignoreStart = ini.ReadInteger(range, "IgnoreStart", -1);
						ignoreEnd = ini.ReadInteger(range, "IgnoreEnd", -1);
						biggerThan = ini.ReadInteger(range, "IfBiggerThan", -1);
						smallerThan = ini.ReadInteger(range, "IfSmallerThan", -1);
						ignorePedGroup = ini.ReadInteger(range, "IgnorePedGroup", -1) - 1;
						keepLoaded = ini.ReadInteger(range, "KeepLoaded", 0) == true;

						if (startId <= 0 && endId <= 0) break;

						if (ini.ReadInteger(range, "Enabled", 0) != 1) continue;

						if (logMode >= 0) {
							lg << "Start loading ID Range: " << i << "\n";
							lg.flush();
						}

						if (endId >= startId)
						{
							// Collect candidate model IDs first, then sort by their on-disk
							// position (m_nCdPosn) before requesting/loading. Iterating in
							// numeric model-ID order does NOT match img archive layout, so
							// the old ascending-ID loop caused random-order CD reads (seeks).
							// Sorting by CD position turns this into mostly sequential reads.
							std::vector<int> modelsToLoad;
							for (int model = startId; model <= endId; model++)
							{
								if ((ignoreStart <= 0 && ignoreEnd <= 0) || (model > ignoreEnd || model < ignoreStart))
								{
									if (CStreaming::ms_aInfoForModel[model].m_nCdSize != 0)
									{
										if ((biggerThan <= 0 && smallerThan <= 0) || (CStreaming::ms_aInfoForModel[model].m_nCdSize >= biggerThan && CStreaming::ms_aInfoForModel[model].m_nCdSize <= smallerThan))
										{
											modelsToLoad.push_back(model);
										}
									}
								}
							}

							std::sort(modelsToLoad.begin(), modelsToLoad.end(), [](int a, int b)
							{
								return CStreaming::ms_aInfoForModel[a].m_nCdPosn < CStreaming::ms_aInfoForModel[b].m_nCdPosn;
							});

							for (int model : modelsToLoad)
							{
								if (GetKeyState(0x10) & 0x8000) break; // SHIFT

								check_limit_to_load:
								if ((signed int)CStreaming::ms_memoryUsed > (signed int)(CStreaming::ms_memoryAvailable - 50000000))
								{
									if (CStreaming::ms_memoryAvailable >= MAX_BYTE_LIMIT)
									{
										if (logMode >= 0) {
											lg << "ERROR: Not enough space\n";
										}
										CMessages::AddMessageJumpQ((char*)"~r~ERROR Load Whole Map: Not enough space. Try to disable some ranges, configure or use other settings.", 8000, false, false);
									}
									else {
										if (streamMemoryForced > 0) {
											if (IncreaseStreamingMemoryLimit(256)) {
												streamMemoryForced = CStreaming::ms_memoryAvailable;
												if (logMode >= 0) {
													lg << "Streaming memory automatically increased to " << streamMemoryForced << " \n";
												}
												goto check_limit_to_load;
											}
										}
										else {
											if (logMode >= 0) {
												lg << "ERROR: Not enough space. Try to increase the streaming memory.\n";
											}
											CMessages::AddMessageJumpQ((char*)"~r~ERROR Load Whole Map: Not enough space. Try to increase the streaming memory.", 8000, false, false);
										}
									}
									if (logMode >= 0) {
										lg.flush();
									}
									break;
								}
								else
								{
									if (ignorePedGroup > 0 && CPopCycle::IsPedInGroup(model, ignorePedGroup)) {
										if (logMode >= 1)
										{
											lg << "Model " << model << " is ignored. Pedgroup: " << ignorePedGroup << "\n";
											lg.flush();
										}
										continue;
									}
									if (logMode >= 1)
									{
										lg << "Loading " << model << " size " << CStreaming::ms_aInfoForModel[model].m_nCdSize << "\n";
										lg.flush();
									}
									CStreaming::RequestModel(model, keepLoaded ? eStreamingFlags::MISSION_REQUIRED : eStreamingFlags::GAME_REQUIRED);
									if (CStreaming::ms_numModelsRequested >= loadEach) CStreaming::LoadAllRequestedModels(false);
								}
							}
							CStreaming::LoadAllRequestedModels(false);
						}
						if (logMode >= 0) {
							lg << "Finished loading ID Range: " << i << "\n";
							lg.flush();
						}
					}

					// last margin check
					if ((signed int)CStreaming::ms_memoryUsed > (signed int)(CStreaming::ms_memoryAvailable - 50000000))
					{
						if (IncreaseStreamingMemoryLimit(128)) {
							streamMemoryForced = CStreaming::ms_memoryAvailable;
							if (logMode >= 0) {
								lg << "Streaming memory automatically increased to " << streamMemoryForced << " after loading (margin).\n";
							}
						}
					}

					if (logMode >= 0) {
						lg.flush();
					}
					CTimer::Update();
					gameStartedAfterLoad = CTimer::m_snTimeInMilliseconds;
				}

				loadCheck = 4;
			}

			if (loadCheck == 4) {
				if (removeUnusedWhenPercent > 0.0 ) {
					float memUsedPercent = (float)((float)CStreaming::ms_memoryUsed / (float)CStreaming::ms_memoryAvailable) * 100.0f;

					if (memUsedPercent >= removeUnusedWhenPercent) {
						// If memory usage is near limit, decrease the remove interval
						int removeUnusedIntervalMsTweaked = removeUnusedIntervalMs;
						if (memUsedPercent > 95.0f && removeUnusedIntervalMsTweaked > 0) {
							removeUnusedIntervalMsTweaked /= 2;
						}
						if ((CTimer::m_snTimeInMilliseconds - lastTimeRemoveUnused) > removeUnusedIntervalMsTweaked) {
							//CStreaming::RemoveAllUnusedModels();
							CStreaming::RemoveLeastUsedModel(0);
							//CStreaming::MakeSpaceFor(10000000);
							//CMessages::AddMessageJumpQ((char*)"Clear", 500, false, false);
							lastTimeRemoveUnused = CTimer::m_snTimeInMilliseconds;
						}
					}
				}
			}
		};

    }

	static bool IncreaseStreamingMemoryLimit(unsigned int mb) {
		unsigned int increaseBytes = mb *= MULT_MB_TO_BYTE;
		unsigned int newLimit = CStreaming::ms_memoryAvailable + increaseBytes;
		if (newLimit <= 0) return false;
		if (newLimit >= MAX_BYTE_LIMIT) newLimit = MAX_BYTE_LIMIT;
		CStreaming::ms_memoryAvailable = newLimit;
		return true;
	}

} loadWholeMap;
