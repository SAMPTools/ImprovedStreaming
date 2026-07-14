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
#include <cstdint>
#include <utility>

using namespace plugin;
using namespace std;
using namespace injector;

const int MAX_MB = 2000;
const int MULT_MB_TO_BYTE = 1048576;
constexpr unsigned int MAX_BYTE_LIMIT = (MAX_MB * MULT_MB_TO_BYTE);

// Vanilla SA allocates 26316 CStreamingInfo entries (valid model IDs 0..26315).
// Indexing past this is an out-of-bounds read AND write (via RequestModel) that
// corrupts memory in the host process. Used as a safety ceiling on user-supplied
// range bounds. NOTE: limit adjusters (Open Limit Adjuster / f92la) can raise the
// real limit; if you run one with model IDs above this, raise this constant to match.
const int MODEL_ID_LIMIT = 26316;

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

		// When set, the preload is spread across many frames (one batch per tick)
		// instead of one multi-second blocking freeze. Default off: opt-in, and the
		// world pops in progressively rather than being fully present on frame one.
		static bool amortizeLoad = ini.ReadInteger("Settings", "AmortizeLoad", 0) == 1;

		// Preload work queue, built once then drained (possibly across ticks).
		// Pair = (modelId, streaming flags). Sorted by on-disk position for
		// sequential reads; deduped so overlapping ranges don't re-request.
		static bool preloadBuilt = false;
		static std::vector<std::pair<int, int>> preloadQueue;
		static size_t preloadCursor = 0;

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
				// SHIFT held (at first entry, or any tick during an amortized load)
				// skips / aborts the preload. GetAsyncKeyState reads the real-time key
				// state without a message pump — GetKeyState would be frozen for the
				// whole burst since the main thread never pumps messages while loading.
				if (GetAsyncKeyState(0x10) & 0x8000)
				{
					preloadQueue.clear();
					preloadQueue.shrink_to_fit();
					loadCheck = 4;
				}
				else
				{
					// ---- BUILD PHASE (runs once) ----
					// Read config, preload anims, then collect every enabled range's
					// models into one global queue, dedup, and sort by on-disk position.
					if (!preloadBuilt)
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

						if (preLoadAnims) {
							CTimer::Stop();
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
							CTimer::Update();
							if (logMode >= 1)
							{
								lg << "Finished loading anims." << endl;
							}
						}

						int i = 0;
						while (true)
						{
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

							// Clamp against the model-info array bounds. A user-supplied
							// End=999999 or a defaulted Start=-1 would otherwise read and
							// (via RequestModel) WRITE out of bounds, corrupting host memory.
							if (startId < 1) startId = 1;
							if (endId >= MODEL_ID_LIMIT) endId = MODEL_ID_LIMIT - 1;

							if (logMode >= 0) {
								lg << "Collecting ID Range: " << i << " (" << startId << ".." << endId << ")\n";
								lg.flush();
							}

							int flags = keepLoaded ? eStreamingFlags::MISSION_REQUIRED : eStreamingFlags::GAME_REQUIRED;

							for (int model = startId; model <= endId; model++)
							{
								if (model < 1 || model >= MODEL_ID_LIMIT) continue; // belt-and-suspenders
								if ((ignoreStart <= 0 && ignoreEnd <= 0) || (model > ignoreEnd || model < ignoreStart))
								{
									if (CStreaming::ms_aInfoForModel[model].m_nCdSize != 0)
									{
										if ((biggerThan <= 0 && smallerThan <= 0) || (CStreaming::ms_aInfoForModel[model].m_nCdSize >= biggerThan && CStreaming::ms_aInfoForModel[model].m_nCdSize <= smallerThan))
										{
											if (ignorePedGroup > 0 && CPopCycle::IsPedInGroup(model, ignorePedGroup)) {
												if (logMode >= 1)
												{
													lg << "Model " << model << " is ignored. Pedgroup: " << ignorePedGroup << "\n";
													lg.flush();
												}
												continue;
											}
											preloadQueue.push_back(std::make_pair(model, flags));
										}
									}
								}
							}
						}

						// Dedup by model ID so overlapping ranges don't queue the same
						// model twice (RequestModel would no-op the dupes, but this keeps
						// the queue tight). Keep the first flag seen per ID.
						std::sort(preloadQueue.begin(), preloadQueue.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b)
						{
							return a.first < b.first;
						});
						preloadQueue.erase(std::unique(preloadQueue.begin(), preloadQueue.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b)
						{
							return a.first == b.first;
						}), preloadQueue.end());

						// Sort by on-disk location: first by which .img archive
						// (m_nImgId), then by offset within it (m_nCdPosn). This turns
						// seek-heavy random reads into one forward sweep per archive.
						std::sort(preloadQueue.begin(), preloadQueue.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b)
						{
							const auto& ia = CStreaming::ms_aInfoForModel[a.first];
							const auto& ib = CStreaming::ms_aInfoForModel[b.first];
							if (ia.m_nImgId != ib.m_nImgId) return ia.m_nImgId < ib.m_nImgId;
							return ia.m_nCdPosn < ib.m_nCdPosn;
						});

						preloadCursor = 0;
						preloadBuilt = true;

						if (logMode >= 0) {
							lg << "Preload queue built: " << (int)preloadQueue.size() << " models. AmortizeLoad=" << (amortizeLoad ? 1 : 0) << "\n";
							lg.flush();
						}
					}

					// ---- DRAIN PHASE ----
					// Batch size doubles as the per-frame budget in amortized mode.
					// Clamped so a misconfigured/absent LoadEach (default 0) can't flush
					// once per model (worst-case seek-per-model) or overshoot the margin.
					int loadBatch = ini.ReadInteger("Settings", "LoadEach", 32);
					if (loadBatch < 8)   loadBatch = 32;
					if (loadBatch > 128) loadBatch = 128;

					// CTimer::Stop()/Update() hides the blocking read time from the game
					// clock so the physics timestep doesn't spike after the stall. Done
					// per slice so amortized mode reports a normal dt every frame.
					CTimer::Stop();

					int requestedThisTick = 0;
					bool outOfSpace = false;
					while (preloadCursor < preloadQueue.size())
					{
						if (GetAsyncKeyState(0x10) & 0x8000) { // SHIFT aborts mid-load
							preloadCursor = preloadQueue.size();
							break;
						}

						int model = preloadQueue[preloadCursor].first;
						int flags = preloadQueue[preloadCursor].second;

						// Memory-margin check in 64-bit to avoid the unsigned underflow
						// that a tiny StreamMemoryForced pool would cause in the old
						// (ms_memoryAvailable - 50000000) unsigned subtraction.
						if ((int64_t)CStreaming::ms_memoryUsed > (int64_t)CStreaming::ms_memoryAvailable - 50000000)
						{
							if (CStreaming::ms_memoryAvailable >= MAX_BYTE_LIMIT)
							{
								if (logMode >= 0) {
									lg << "ERROR: Not enough space\n";
									lg.flush();
								}
								CMessages::AddMessageJumpQ((char*)"~r~ERROR Load Whole Map: Not enough space. Try to disable some ranges, configure or use other settings.", 8000, false, false);
								outOfSpace = true;
								break;
							}
							else if (streamMemoryForced > 0 && IncreaseStreamingMemoryLimit(256)) {
								streamMemoryForced = CStreaming::ms_memoryAvailable;
								if (logMode >= 0) {
									lg << "Streaming memory automatically increased to " << streamMemoryForced << " \n";
									lg.flush();
								}
								continue; // re-check margin with the larger pool, same model
							}
							else {
								if (logMode >= 0) {
									lg << "ERROR: Not enough space. Try to increase the streaming memory.\n";
									lg.flush();
								}
								CMessages::AddMessageJumpQ((char*)"~r~ERROR Load Whole Map: Not enough space. Try to increase the streaming memory.", 8000, false, false);
								outOfSpace = true;
								break;
							}
						}

						if (logMode >= 1)
						{
							lg << "Loading " << model << " size " << CStreaming::ms_aInfoForModel[model].m_nCdSize << "\n";
							lg.flush();
						}
						CStreaming::RequestModel(model, flags);
						preloadCursor++;
						requestedThisTick++;

						if (CStreaming::ms_numModelsRequested >= loadBatch) {
							CStreaming::LoadAllRequestedModels(false);
							// In amortized mode, yield the frame after a full batch so the
							// game renders and stays responsive instead of freezing.
							if (amortizeLoad && requestedThisTick >= loadBatch) break;
						}
					}
					CStreaming::LoadAllRequestedModels(false);
					CTimer::Update();

					bool done = outOfSpace || preloadCursor >= preloadQueue.size();
					if (done)
					{
						// last margin check
						if ((int64_t)CStreaming::ms_memoryUsed > (int64_t)CStreaming::ms_memoryAvailable - 50000000)
						{
							if (IncreaseStreamingMemoryLimit(128)) {
								streamMemoryForced = CStreaming::ms_memoryAvailable;
								if (logMode >= 0) {
									lg << "Streaming memory automatically increased to " << streamMemoryForced << " after loading (margin).\n";
								}
							}
						}

						if (logMode >= 0) {
							lg << "Preload finished.\n";
							lg.flush();
						}
						gameStartedAfterLoad = CTimer::m_snTimeInMilliseconds;
						preloadQueue.clear();
						preloadQueue.shrink_to_fit();
						loadCheck = 4;
					}
					// else: amortized load still draining — stay in loadCheck==3, resume next tick
				}
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
							// Free a small bounded batch instead of a single model, so the
							// reaper can keep memory below the ceiling while the player moves
							// fast (high model inflow). Once the pool saturates the game's own
							// MakeSpaceFor kicks in and reintroduces the evict+reload stutter
							// this mod is meant to prevent. Batch stays small so a mistaken
							// eviction is cheap; a bulk purge (RemoveAllUnusedModels) would be
							// a guaranteed frame hitch. Only advance the timer when something
							// was actually freed, so a no-op doesn't burn the interval.
							int removeBudget = (memUsedPercent > 95.0f) ? 4 : 2;
							bool removedAny = false;
							for (int r = 0; r < removeBudget; ++r) {
								if (CStreaming::RemoveLeastUsedModel(0)) removedAny = true;
								else break;
							}
							//CStreaming::MakeSpaceFor(10000000);
							//CMessages::AddMessageJumpQ((char*)"Clear", 500, false, false);
							if (removedAny) lastTimeRemoveUnused = CTimer::m_snTimeInMilliseconds;
						}
					}
				}
			}
		};

    }

	static bool IncreaseStreamingMemoryLimit(unsigned int mb) {
		unsigned int increaseBytes = mb *= MULT_MB_TO_BYTE;
		unsigned int newLimit = CStreaming::ms_memoryAvailable + increaseBytes;
		// Detect unsigned additive wrap (newLimit < base means it overflowed).
		// The old `newLimit <= 0` check was dead on an unsigned type.
		if (newLimit < CStreaming::ms_memoryAvailable) return false;
		if (newLimit >= MAX_BYTE_LIMIT) newLimit = MAX_BYTE_LIMIT;
		CStreaming::ms_memoryAvailable = newLimit;
		return true;
	}

} loadWholeMap;
