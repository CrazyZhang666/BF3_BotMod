// BF3 BotMod core - bot logic for bf3_server_Final.exe (Feb 2013 build, 32-bit). Hot-reloadable: loaded by the
// loader (xinput9_1_0.dll) which owns persistent state (BotModHost). Every hook installed here is undone in
// botmod_shutdown so the DLL can be unloaded and replaced while the server runs.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "host.h"
#include "nav.h"
#include "chat.h"

// ---- Engine addresses (bf3_server_Final.exe, image base 0x400000) ----
#define ADDR_SERVER_GAME_CONTEXT   0x02b3ee2c  // ServerGameContext* global; +0x30 = ServerPlayerManager*
#define ADDR_PM_CREATE_PLAYER      0x00e644b0  // ServerPlayerManager::createPlayer(u32 a1, int id, bool spectator)
#define ADDR_PM_SET_PLAYER_NAME    0x00e6ca00  // ServerPlayerManager::setPlayerName(ServerPlayer*, const char*) - also registers PlayerGhost
#define ADDR_PLAYER_SET_AI         0x00e14490  // ServerPlayer::setIsAIPlayer(bool) - clients hide AI players from the scoreboard
#define ADDR_ADD_TO_TEAM           0x0118ef80  // cdecl bool addToTeam(ServerPlayer*, int teamId, bool, bool) - as used by admin.movePlayer
#define ADDR_CTX_DISPATCH_MESSAGE  0x00549a20  // ServerGameContext::dispatchMessage(Message*) (thiscall on *ADDR_SERVER_GAME_CONTEXT)
#define ADDR_MSG_PLAYER_CREATE_CTOR 0x02307610 // ServerPlayerCreateMessage ctor; +0x28 = ServerPlayer*
#define ADDR_MSG_DTOR              0x005306a0  // Message dtor
#define ADDR_PLAYER_LEVEL_LOADED   0x00e19380  // ServerPlayer::onLevelLoaded() - sets +0x49c, sends ServerPlayer.LevelLoaded
#define ADDR_PLAYER_INIT_FOR_LEVEL 0x00e208e0  // ServerPlayer: runs per-player component init, sets +0x49d (PM does this for all players at level start)
#define ADDR_MSG_ENTERED_LEVEL_CTOR 0x023077f0 // ServerPlayerEnteredLevelMessage ctor; +0x28 = ServerPlayer*
#define ADDR_PLAYER_SET_BLUEPRINT  0x00e207f0  // ServerPlayer::setSelectedSoldierBlueprint(SoldierBlueprint*) (refcounted, stored at +0x480)
#define ADDR_ENTRYINPUT_CTOR       0x008c6440  // EntryInput::EntryInput() (thiscall), object ~0x2e0 bytes
#define ADDR_ENTRYINPUT_SET_LEVEL  0x008bef00  // EntryInput::setLevel(EntryInputAction, float); levels stored at +0x10 + action*4
#define ADDR_CSE_SPAWN             0x00ddbbd0  // ServerCharacterSpawnEntity::spawn(ServerPlayer*, a2, a3, a4), ret 0x10; human HQ deploy calls it as (player, -1, 1, 0)
#define ADDR_GHOST_ADD             0x00d820c0  // ServerGhostManager::addGhost(Ghost*, int)
#define ADDR_PLAYER_SELECT_UNLOCKS 0x00e331b0  // ServerPlayer::onSelectedUnlockAssets(NetworkPlayerSelectedUnlockAssetsMessage*) thiscall, ret 4
#define ADDR_PLAYER_SELECT_WEAPON  0x00e33040  // ServerPlayer::onSelectedWeapon(NetworkPlayerSelectedWeaponMessage*) thiscall, ret 4
#define ADDR_SOLDIER_VTABLE        0x025c3948  // ServerSoldierEntity vtable
#define ADDR_SOLDIER_PREPHYS       0x00e0a3d0  // ServerSoldierEntity::prePhysicsUpdate(frameInfo*), vtable slot 0x74
#define ADDR_SOLDIER_PREPHYS_ENTRY 0x00e0a7c0  // ServerSoldierEntity::prePhysicsUpdateEntry(entryIndex, EntryInput*), vtable slot 0x144

#define OFF_PLAYER_ONLINE_NAME     0x28        // inline char[] read by RCON listPlayers / scoreboard
#define OFF_PLAYER_IS_AI           0x318
#define OFF_PLAYER_TEAM            0x31c
#define OFF_PLAYER_INPUT           0x3c0       // EntryInput* read by the soldier every prePhysicsUpdate; NULL = no input applied
#define OFF_PLAYER_SOLDIER         0x3c4
#define OFF_SOLDIER_POSE       0x2d0     // int: 0 stand, 2 prone (+4: stance being changed to)
#define OFF_PLAYER_BLUEPRINT       0x480
#define OFF_PLAYER_ENTERED_LEVEL   0x49e
#define OFF_SOLDIER_POSITION       0x290       // Vec3 simulated position (stays 0,0,0 until the soldier is stepped)
#define OFF_SOLDIER_SPAWN_POSITION 0x4b0       // Vec3 transform (keeps tracking the seat while in a vehicle)
#define OFF_PLAYER_VEHICLE         0x3d8       // vehicle the player sits in (0 on foot)
#define OFF_PLAYER_SEAT            0x3dc       // seat index in that vehicle
#define ADDR_PLAYER_ENTER_VEHICLE  0x00e37950  // ServerPlayer::enterVehicle(vehicle, seatIndex), thiscall, ret 8
#define ADDR_PLAYER_EXIT_VEHICLE   0x00e365f0  // ServerPlayer::exitVehicle(0, 1) - what the ChangeVehicle (47) button calls
#define ADDR_PLAYER_ATTACH_SOLDIER 0x00e37180  // ServerPlayer::setSoldier(soldier) - re-attach control to the soldier
#define OFF_ENTITY_ENTRIES         0xfc        // entry components [begin, end); entry+0x4c = player in that seat
#define EIA_INTERACT               35
#define EIA_TOGGLE_PARACHUTE       36
#define ORDER_GETIN                15
#define ORDER_GETOUT               16
#define VEHICLE_BOARD_DIST         7.0f
#define GETIN_RADIUS               30.0f
#define OFF_SOLDIER_PITCH          0x21c       // look pitch (radians, negative = down)
#define PITCH_INPUT_SIGN           1.0f        // EIAPitch direction relative to OFF_SOLDIER_PITCH
static float g_pitchTest[MAX_BOTS];            // lookpitch test target per bot, used when g_pitchTestOn
static unsigned char g_pitchTestOn[MAX_BOTS];
static float g_aimPitch[MAX_BOTS];   // (rockets) wanted pitch while engaging
#define OFF_SOLDIER_YAW            0x218       // heading (radians); positive EIAYaw input decreases it (~5 rad/s at 1.0)
// VeniceNetwork.Order (commo rose): order type at +0x48
#define OFF_ORDER_TYPE             0x48
#define ORDER_GOGOGO               8
#define ORDER_FOLLOWME             9
#define ORDER_NEEDBACKUP           14
#define ORDER_REQUESTAMMO          3
#define ORDER_REQUESTHEAL          1
#define ORDER_REQUESTRIDE          7
#define ORDER_REQUESTREPAIR        6
#define REPAIR_CALL_RADIUS         100.0f      // engineers answering "Need repair" (no line of sight needed)
#define REPAIR_CALL_VEHICLE_DIST   15.0f       // on foot: repair the closest friendly vehicle this near the caller
#define ORDER_NEGATIVE             10
#define ORDER_SORRY                11
#define ORDER_ROGERTHAT            13
#define ORDER_THANKYOU             12
#define FOLLOW_RADIUS              60.0f
#define FOLLOW_FRAMES              (5 * 60 * 30)
#define BACKUP_BOTS                2
#define STUCK_SECONDS              3          // trying to move but no 1 m of progress this long -> unstick
#define SAY_DELAY_FRAMES           (2 * 30)
#define SUPPLY_GIVEUP_FRAMES       (20 * 30)  // can't get within reach of the drop spot: Sorry, resume
#define DEFEND_RELEASE_RADIUS      50.0f      // GoGoGo from anyone this close to a post releases it
#define DEFEND_SPREAD              2.5f
#define NEVER                      0x7fffffffu   // bots reply ~2 s after the human's own voice line
#define ENTRYINPUT_SIZE            0x400
#define ADDR_ENTRYINPUT_SET_DT     0x008b9d30  // EntryInputState::setDeltaTime(float), stored at +0x1ec (0 = no movement)
#define BOT_TICK_DT                (1.0f / 30.0f)
#define OFF_INPUT_AIM_VALID        0x1dc       // 1 on human input when the aim below is set
#define OFF_INPUT_AIM_YAW          0x1f4       // authoritative aiming yaw (radians); the soldier faces this way
#define OFF_INPUT_AIM_PITCH        0x1f8       // authoritative aiming pitch (radians)
// EntryInputAction values from the engine's reflection table: Throttle 0, Strafe 1, Brake 2, Yaw 4, Pitch 5, Fire 7,
// CameraPitch 8, CameraYaw 9, Zoom 11, Jump 12, Reload 15, Sprint 32.
#define EIA_THROTTLE 0
#define EIA_STRAFE   1
#define EIA_BRAKE    2   // vehicles: brake, then reverse (S)
#define EIA_YAW      4
#define EIA_PITCH    5
#define EIA_FIRE     7
#define EIA_CHANGE_POSE 13
#define EIA_PRONE    14
#define EIA_ZOOM     11      // aim down the sights (held)
#define EIA_SELECT_WEAPON1 16      // SelectWeaponN selects loadout slot N (slot 1 = sidearm, 4 = gadget 1, 5 = gadget 2)
#define EIA_SWITCH_PRIMARY 25
// Loadout slot -> SelectWeapon action. Observed: SelectWeapon1 = primary (0), 2 = sidearm (1), 3 = slot 2 (engineer/recon
// gadget), 4 = slot 4 (medkit/ammo bag), 5 = slot 5 (defib/claymore/repair/beacon), 6 = grenade (6), 7 = knife (7).
static const int kSlotAction[8] = {16, 17, 18, 18, 19, 20, 21, 22};
#define SLOT_ACTION(s) ((s) >= 0 && (s) < 8 ? kSlotAction[s] : 16)
#define EIA_SELECT_WEAPON3 18
#define EIA_SELECT_WEAPON2 17   // in a tank's driver seat: weapon 1 = the main gun, weapon 2 = the coax (2026-10-12, live test)

typedef void *(__attribute__((thiscall)) *PM_CreatePlayer_t)(void *pm, unsigned a1, int id, int spectator);
typedef void (__attribute__((thiscall)) *PM_SetPlayerName_t)(void *pm, void *player, const char *name);
typedef void (__attribute__((thiscall)) *Player_SetAI_t)(void *player, int isAI);
typedef void (__attribute__((thiscall)) *ThisVoid_t)(void *self);
typedef void (__attribute__((thiscall)) *DispatchMessage_t)(void *ctx, void *msg);
typedef void (__attribute__((thiscall)) *SetPtr_t)(void *self, void *p);
typedef void (__attribute__((thiscall)) *SetLevel_t)(void *input, int action, float level);
typedef void (__attribute__((thiscall)) *SetFloat_t)(void *self, float v);
typedef void *(__attribute__((thiscall)) *CseSpawn_t)(void *self, void *player, unsigned a2, unsigned a3, unsigned a4);
typedef void (__attribute__((thiscall)) *PlayerMsg_t)(void *player, void *msg);
typedef char (__cdecl *AddToTeam_t)(void *player, int teamId, int a, int b);

static BotModHost *H;
static const char *kOrderNames[] = {"None", "RequestHeal", "LetMeHeal", "RequestAmmo", "LetMeGiveAmmo", "LetMeRepair", "RequestRepair",
    "RequestRide", "GoGoGo", "FollowMe", "Negative", "Sorry", "ThankYou", "RogerThat", "NeedBackup", "GetIn", "GetOut", "Attack"};
#define bot_log H->log
// Set on shutdown: every memory scan (worker threads) stops at the next region, so a reload never waits on a
// long scan (the server's watchdog kills the process if the game loop stalls ~14 s).
static volatile LONG g_abortScans;
static float *bot_aim(int i);
struct BotNav;
static void follow_path(int i, char *soldier, struct BotNav *nav);
static int *aim_mode(void);
#include <math.h>

// ---- Reversible patches ----
typedef struct Patch { unsigned char *addr; int len; unsigned char orig[16]; } Patch;
static Patch g_patches[32];
static int g_patchCount;

static int patch_bytes(void *addr, const void *bytes, int len)
{
    Patch *p = &g_patches[g_patchCount++];
    p->addr = addr;
    p->len = len;
    memcpy(p->orig, addr, len);
    DWORD old;
    VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(addr, bytes, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 1;
}

static void unpatch_all(void)
{
    while (g_patchCount > 0) {
        Patch *p = &g_patches[--g_patchCount];
        DWORD old;
        VirtualProtect(p->addr, p->len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p->addr, p->orig, p->len);
        VirtualProtect(p->addr, p->len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p->addr, p->len);
    }
}

static int detour(unsigned addr, const unsigned char *expect, int len, void *hook, const char *what)
{
    unsigned char *target = (unsigned char *)addr;
    if (memcmp(target, expect, len) != 0) { bot_log("ERROR: %s prologue mismatch - wrong server build?", what); return 0; }
    unsigned char jmp[16];
    jmp[0] = 0xE9;
    *(DWORD *)(jmp + 1) = (DWORD)((unsigned char *)hook - (target + 5));
    for (int i = 5; i < len; i++) jmp[i] = 0x90;
    patch_bytes(target, jmp, len);
    bot_log("hooked %s @ %p", what, target);
    return 1;
}

// Replaces a few instruction bytes after checking they are what this build should have.
static int patch_code(unsigned addr, const unsigned char *expect, const unsigned char *bytes, int len, const char *what)
{
    if (memcmp((void *)addr, expect, len) != 0) { bot_log("ERROR: %s bytes mismatch - wrong server build?", what); return 0; }
    patch_bytes((void *)addr, bytes, len);
    bot_log("patched %s @ %08x", what, addr);
    return 1;
}

static int patch_vslot(unsigned slotAddr, unsigned expect, void *hook, const char *what)
{
    if (*(unsigned *)slotAddr != expect) { bot_log("ERROR: vtable slot for %s mismatch (%08x)", what, *(unsigned *)slotAddr); return 0; }
    patch_bytes((void *)slotAddr, &hook, 4);
    bot_log("hooked %s (vtable)", what);
    return 1;
}

// ---- Players ----
static void *player_manager(void)
{
    char *ctx = *(char **)ADDR_SERVER_GAME_CONTEXT;
    return ctx ? *(void **)(ctx + 0x30) : NULL;
}

static int player_team(char *pl) { return *(int *)(pl + OFF_PLAYER_TEAM); }

static void *find_player(const char *name)
{
    char *pm = player_manager();
    if (!pm) return NULL;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned i = 0; i < count; i++) {
        char *pl = arr[i];
        if (pl && !_stricmp(pl + OFF_PLAYER_ONLINE_NAME, name)) return pl;
    }
    return NULL;
}

static int bot_index(void *pl)
{
    for (int i = 0; i < H->botCount; i++) if (H->bots[i] == pl) return i;
    return -1;
}

static int is_bot(void *pl) { return bot_index(pl) >= 0; }

static int soldier_is_bot(void *soldier)
{
    for (int i = 0; i < H->botCount; i++)
        if (*(void **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER) == soldier) return 1;
    return 0;
}

static void dispatch_player_message(unsigned ctor, void *player)
{
    void *ctx = *(void **)ADDR_SERVER_GAME_CONTEXT;
    unsigned char msg[0x80] __attribute__((aligned(16))) = {0};
    ((ThisVoid_t)ctor)(msg);
    *(void **)(msg + 0x28) = player;
    ((DispatchMessage_t)ADDR_CTX_DISPATCH_MESSAGE)(ctx, msg);
    ((ThisVoid_t)ADDR_MSG_DTOR)(msg);
}

static void cmd_add(const char *name, int team, int isAI)
{
    void *pm = player_manager();
    if (!pm) { bot_log("add: no ServerPlayerManager yet (level not loaded?)"); return; }
    if (H->botCount >= MAX_BOTS) { bot_log("add: bot limit %d reached", MAX_BOTS); return; }

    char autoName[32];
    if (!name || !*name) {
        snprintf(autoName, sizeof autoName, "BOT_%02d", H->botCount + 1);
        name = autoName;
    }
    char *player = ((PM_CreatePlayer_t)ADDR_PM_CREATE_PLAYER)(pm, 0, -1, 0);
    if (!player) { bot_log("add: createPlayer returned NULL (server full?)"); return; }
    ((PM_SetPlayerName_t)ADDR_PM_SET_PLAYER_NAME)(pm, player, name);
    strncpy(player + OFF_PLAYER_ONLINE_NAME, name, 31);
    player[OFF_PLAYER_ONLINE_NAME + 31] = 0;
    ((Player_SetAI_t)ADDR_PLAYER_SET_AI)(player, isAI);

    // A connected human's player points at the EntryInput its network input handler fills each tick.
    // Bots get their own; lives for the whole process (the engine never frees it).
    void *input = _aligned_malloc(ENTRYINPUT_SIZE, 16);
    memset(input, 0, ENTRYINPUT_SIZE);
    ((ThisVoid_t)ADDR_ENTRYINPUT_CTOR)(input);
    *(void **)(player + OFF_PLAYER_INPUT) = input;

    // Same notifications a human join sends so the scoreboard / replication systems pick the player up.
    dispatch_player_message(ADDR_MSG_PLAYER_CREATE_CTOR, player);
    ((ThisVoid_t)ADDR_PLAYER_LEVEL_LOADED)(player);
    ((ThisVoid_t)ADDR_PLAYER_INIT_FOR_LEVEL)(player);
    dispatch_player_message(ADDR_MSG_ENTERED_LEVEL_CTOR, player);   // what the server does once a client has loaded the level
    player[OFF_PLAYER_ENTERED_LEVEL] = 1;
    player[0x4a0] = 0;

    if (team <= 0) team = (H->botCount % 2) + 1;
    char ok = ((AddToTeam_t)ADDR_ADD_TO_TEAM)(player, team, 0, 1);
    int i = H->botCount++;
    H->bots[i] = player;
    H->botInput[i] = input;
    H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
    bot_log("add: created '%s' player=%p team=%d ai=%d (%s)", name, player, team, isAI, ok ? "ok" : "addToTeam FAILED");
}

// ---- CharacterSpawnEntity::spawn observer: remember the spawn entity + args + blueprint each team's humans use ----
static const unsigned char kCseSpawnPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}; // push ebp; mov ebp,esp; and esp,-16

// A human deploying from a non-flag spawn point (an HQ the capture points do not reveal, e.g. Wake's US island HQ):
// remembered here, judged a moment later from where the soldier appeared (hq_learn_tick).
typedef struct PendingHq { void *entity; unsigned a2, a3, a4; char *player; unsigned at; } PendingHq;
static PendingHq g_pendingHq;
// The team a spawn point belongs to (CharacterSpawnEntity data +0x08), 0 if not set / unreadable.
static int spawn_team(void *spawn)
{
    unsigned char *data = spawn && !IsBadReadPtr((char *)spawn + 0x14, 4) ? *(unsigned char **)((char *)spawn + 0x14) : NULL;
    return data && !IsBadReadPtr(data + 8, 1) && (data[8] == 1 || data[8] == 2) ? data[8] : 0;
}
typedef struct HqProbe { char *bot; int team, cand; unsigned at; } HqProbe;
#define MAX_HQ_PROBES 4
static HqProbe g_hqProbes[MAX_HQ_PROBES];

void __cdecl on_cse_spawn(void *self, unsigned *stack)
{
    char *pl = (char *)stack[1];
    if (pl && !is_bot(pl)) {
        g_pendingHq.entity = self; g_pendingHq.a2 = stack[2]; g_pendingHq.a3 = stack[3]; g_pendingHq.a4 = stack[4];
        g_pendingHq.player = pl; g_pendingHq.at = H->frame ? H->frame : 1;
        int team = player_team(pl);
        if ((team == 1 || team == 2) && !H->teamSpawn[team].entity) {   // only until the team's HQ is known: a human may
            H->teamSpawn[team].entity = self;                             // deploy on a flag, a squad mate, or the "wrong" HQ
            H->teamSpawn[team].a2 = stack[2];
            H->teamSpawn[team].a3 = stack[3];
            H->teamSpawn[team].a4 = stack[4];
        }
        if (team == 1 || team == 2) {
            if (*(void **)(pl + OFF_PLAYER_BLUEPRINT)) H->teamBlueprint[team] = *(void **)(pl + OFF_PLAYER_BLUEPRINT);
        }
    }
}

extern void hook_cse_spawn(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_cse_spawn\n"
    "_hook_cse_spawn:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  push ecx\n"
    "  call _on_cse_spawn\n"
    "  add esp, 8\n"
    "  popfd\n"
    "  popad\n"
    "  push ebp\n"
    "  mov ebp, esp\n"
    "  and esp, 0xfffffff0\n"
    "  push 0x00ddbbd6\n"
    "  ret\n"
    ".att_syntax prefix\n");

// ---- ServerGhostManager::addGhost observer (diagnostics) ----
static const unsigned char kGhostAddPrologue[6] = {0x83, 0xEC, 0x10, 0x53, 0x55, 0x56}; // sub esp,16; push ebx; push ebp; push esi

static const char *ghost_type_name(void *ghost)
{
    // Ghost vtable slot 3 is "push <name>; call ...; add esp,4; ret" (e.g. "PlayerGhost").
    unsigned char *fn = (*(unsigned char ***)ghost)[3];
    return fn[0] == 0x68 ? *(const char **)(fn + 1) : "?";
}

void __cdecl on_ghost_add(unsigned *stack)
{
    if (!H->traceGhosts) return;
    void *ghost = (void *)stack[1];
    bot_log("  addGhost %s ghost=%p arg=%u ret=%08x", ghost ? ghost_type_name(ghost) : "(null)", ghost, stack[2], stack[0]);
}

extern void hook_ghost_add(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_ghost_add\n"
    "_hook_ghost_add:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  call _on_ghost_add\n"
    "  add esp, 4\n"
    "  popfd\n"
    "  popad\n"
    "  sub esp, 0x10\n"
    "  push ebx\n"
    "  push ebp\n"
    "  push esi\n"
    "  push 0x00d820c6\n"
    "  ret\n"
    ".att_syntax prefix\n");

// ---- Kit mirroring: when a human's unlock/weapon selection arrives, apply the same message to the bots on their team ----
static const unsigned char kSelectUnlocksPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8}; // push ebp; mov ebp,esp; and esp,-8
static const unsigned char kSelectWeaponPrologue[5] = {0x83, 0xEC, 0x24, 0x53, 0x55};       // sub esp,0x24; push ebx; push ebp

// Captured kit per team, so bots can be kitted later without a human present. Lives in host->reserved (survives reloads).
// Message layout used by the handlers: +0x38 asset*, +0x3c / +0x40 fb arrays (element pointer, count at [-4]).
#define KIT_MSG_SIZE 0x80
#define KIT_MAX_WEAPONS 8
typedef struct KitStore {
    unsigned char unlockMsg[KIT_MSG_SIZE];
    int hasUnlocks;
    unsigned char weaponMsg[KIT_MAX_WEAPONS][KIT_MSG_SIZE];
    int nWeapons;
} KitStore;

static KitStore **kit_slots(void) { return (KitStore **)H->reserved; }   // [0] unused, [1] team 1, [2] team 2
static unsigned long long *kitted_mask(void) { return (unsigned long long *)(H->reserved + 2560); } // bit i = bot i has received a kit
static float *bot_aim(int i) { return (float *)(H->reserved + 1024) + i * 2; }  // yaw, pitch (radians) per bot (+1024..+1536)
static int *aim_mode(void) { return (int *)(H->reserved + 20); }               // 0 = leave aim fields alone, 1 = write them

static KitStore *team_kit(int team, int create)
{
    KitStore **slots = kit_slots();
    if (team != 1 && team != 2) return NULL;
    if (!slots[team] && create) slots[team] = calloc(1, sizeof(KitStore));
    return slots[team];
}

static void *copy_fb_array(void *elems)
{
    if (!elems || IsBadReadPtr((unsigned *)elems - 1, 4)) return NULL;
    unsigned n = ((unsigned *)elems)[-1];
    if (n > 256 || IsBadReadPtr(elems, n * sizeof(unsigned))) return NULL;   // not an fb array / garbage
    unsigned *copy = malloc((n + 1) * sizeof(unsigned));   // intentionally never freed: referenced by stored kits
    if (!copy) return NULL;
    copy[0] = n;
    memcpy(copy + 1, elems, n * sizeof(unsigned));
    return copy + 1;
}

// Name of a DataContainer asset (char* at +0x08), "?" if the pointer is not a readable asset.
static const char *asset_name(void *asset)
{
    if (!asset || (unsigned)asset < 0x10000 || IsBadReadPtr(asset, 12)) return "?";
    const char *n = *(const char **)((char *)asset + 8);
    return n && (unsigned)n >= 0x10000 && !IsBadStringPtrA(n, 128) ? n : "?";
}

// Unlock messages carry fb arrays at +0x3c and +0x40; weapon messages carry a plain asset pointer at +0x3c and an
// array at +0x40. Arrays are deep-copied (the message frees its own after dispatch).
static void capture_msg(unsigned char *dst, const unsigned char *msg, int weapon)
{
    memcpy(dst, msg, KIT_MSG_SIZE);
    if (!weapon) *(void **)(dst + 0x3c) = copy_fb_array(*(void **)(msg + 0x3c));
    *(void **)(dst + 0x40) = copy_fb_array(*(void **)(msg + 0x40));
}

static unsigned fb_count(void *arr)
{
    if (!arr || IsBadReadPtr((unsigned *)arr - 1, 4)) return 0;
    unsigned n = ((unsigned *)arr)[-1];
    return n > 256 ? 0 : n;
}

// ---- Loadout library: one captured loadout per team + kit (class), persisted to BotMod_loadouts.txt ----
// Unlock message: +0x38 kit asset (e.g. Gameplay/Kits/USSupport), +0x3c appearance array, +0x40 specialization array.
// Weapon message:  +0x38 slot index, +0x3c weapon unlock asset, +0x40 attachment array.
#define MAX_LOADOUTS 16
typedef struct Loadout {
    int team;
    char kit[64];
    unsigned char unlockMsg[KIT_MSG_SIZE];
    int nWeapons;
    unsigned char weaponMsg[KIT_MAX_WEAPONS][KIT_MSG_SIZE];
} Loadout;
typedef struct LoadoutLib { int count; Loadout l[MAX_LOADOUTS]; } LoadoutLib;
static LoadoutLib *loadout_lib(void)
{
    LoadoutLib **slot = (LoadoutLib **)(H->reserved + 44);
    if (!*slot) *slot = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LoadoutLib));
    return *slot;
}
static int g_botLoadout[MAX_BOTS] = {-1};   // library index used by each bot (-1 = none yet)

static const char *kit_class(const char *kit)
{
    const char *k = strrchr(kit, '/');
    k = k ? k + 1 : kit;
    return (!strncmp(k, "US", 2) || !strncmp(k, "RU", 2)) ? k + 2 : k;   // USSupport -> Support
}

static void log_loadout(const Loadout *L, const char *from)
{
    const unsigned char *u = L->unlockMsg;
    void **app = *(void ***)(u + 0x3c), **spec = *(void ***)(u + 0x40);
    bot_log("loadout: team %d %s (%s)%s%s", L->team, kit_class(L->kit), L->kit, from ? " from " : "", from ? from : "");
    for (unsigned k = 0; k < fb_count(app); k++) bot_log("    appearance: %s", asset_name(app[k]));
    for (unsigned k = 0; k < fb_count(spec); k++) bot_log("    specialization: %s", asset_name(spec[k]));
    for (int w = 0; w < L->nWeapons; w++) {
        const unsigned char *m = L->weaponMsg[w];
        char att[512] = "";
        void **a = *(void ***)(m + 0x40);
        for (unsigned k = 0; k < fb_count(a) && strlen(att) < 400; k++) {
            const char *n = asset_name(a[k]), *s = strrchr(n, '/');
            strcat(att, k ? ", " : "");
            strcat(att, s ? s + 1 : n);
        }
        bot_log("    slot %d: %s  [%s]", *(int *)(m + 0x38), asset_name(*(void **)(m + 0x3c)), att);
    }
}

// Text format, one message per line:  U|W <hex 0x80 bytes> ; then pointer checks  P <off> <ptr> <name>  and arrays
// A <off> <n> <ptr>:<name> ...   On load each pointer is verified by its asset name (assets load at the same addresses).
static void save_loadouts(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 32];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(path, sizeof path, "%s\\BotMod_loadouts.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) { bot_log("loadouts: cannot write %s", path); return; }
    LoadoutLib *lib = loadout_lib();
    for (int i = 0; i < lib->count; i++) {
        Loadout *L = &lib->l[i];
        fprintf(f, "LOADOUT %d %s %d\n", L->team, L->kit, L->nWeapons);
        for (int m = -1; m < L->nWeapons; m++) {
            const unsigned char *msg = m < 0 ? L->unlockMsg : L->weaponMsg[m];
            fprintf(f, "%c ", m < 0 ? 'U' : 'W');
            for (int k = 0; k < KIT_MSG_SIZE; k++) fprintf(f, "%02x", msg[k]);
            fprintf(f, "\n");
            if (m < 0) fprintf(f, "P 56 %p %s\n", *(void **)(msg + 0x38), asset_name(*(void **)(msg + 0x38)));
            else fprintf(f, "P 60 %p %s\n", *(void **)(msg + 0x3c), asset_name(*(void **)(msg + 0x3c)));
            for (int off = (m < 0 ? 0x3c : 0x40); off <= 0x40; off += 4) {
                void **a = *(void ***)(msg + off);
                unsigned n = fb_count(a);
                fprintf(f, "A %d %u", off, n);
                for (unsigned k = 0; k < n; k++) fprintf(f, " %p:%s", a[k], asset_name(a[k]));
                fprintf(f, "\n");
            }
        }
    }
    fclose(f);
    bot_log("loadouts: saved %d to %s", lib->count, path);
}

// ---- Asset lookup by name (asset addresses change between server runs and levels) ----
// Pass 1 finds each name string in memory; pass 2 finds objects whose +0x08 points at one of those strings.
#define MAX_FIXUPS 1024
typedef struct Fixup { void **slot; char name[160]; int loadout; unsigned vtable; } Fixup;   // vtable 0 = any engine object

static void resolve_assets(Fixup *fx, int n)
{
    enum { CHUNK = 1 << 16 };
    static unsigned char buf[CHUNK + 256];
    // every copy of each name string (the asset points at one specific copy)
    #define MAX_COPIES 256
    static const char *strAddr[MAX_FIXUPS][MAX_COPIES];
    static int nCopies[MAX_FIXUPS];
    memset(nCopies, 0, sizeof nCopies);
    MEMORY_BASIC_INFORMATION mbi;
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned char *a = (unsigned char *)0x10000; (unsigned)a < 0xffff0000u && !g_abortScans; a = (unsigned char *)mbi.BaseAddress + mbi.RegionSize) {
            if (!VirtualQuery(a, &mbi, sizeof mbi) || !mbi.RegionSize) break;
            DWORD prot = mbi.Protect & 0xff;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE)) ||
                !(prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ)) continue;
            for (SIZE_T off = 0; off < mbi.RegionSize && !g_abortScans; off += CHUNK) {
                SIZE_T want = mbi.RegionSize - off < CHUNK ? mbi.RegionSize - off : CHUNK, got = 0;
                char *base = (char *)mbi.BaseAddress + off;
                if (!ReadProcessMemory(GetCurrentProcess(), base, buf, want, &got) || !got) continue;
                if (pass == 0) {
                    for (int f = 0; f < n; f++) {
                        size_t len = strlen(fx[f].name) + 1;
                        for (unsigned char *p = buf; p + len <= buf + got && nCopies[f] < MAX_COPIES; p++) {
                            p = memchr(p, fx[f].name[0], buf + got - p);
                            if (!p || p + len > buf + got) break;
                            if (!memcmp(p, fx[f].name, len)) strAddr[f][nCopies[f]++] = base + (p - buf);
                        }
                    }
                } else {
                    for (SIZE_T k = 8; k + 4 <= got; k += 4) {
                        const char *v = *(const char **)(buf + k);
                        if (!v) continue;
                        for (int f = 0; f < n; f++) {
                            if (*fx[f].slot != (void *)-1) continue;
                            int hit = 0;
                            for (int cpy = 0; cpy < nCopies[f] && !hit; cpy++) hit = strAddr[f][cpy] == v;
                            if (!hit) continue;
                            // must be a real engine object: vtable (first field) inside the exe image (asset vtables sit ~0x025xxxxx)
                            unsigned vt = *(unsigned *)(buf + k - 8);
                            if (k < 8 || vt < 0x02028000 || vt >= 0x02b00000) continue;
                            if (fx[f].vtable && vt != fx[f].vtable) continue;
                            *fx[f].slot = base + k - 8;
                        }
                    }
                }
            }
        }
    }
}

// ---- Asset type cache: the addresses change every load, the types do not. BotMod_assetvt.txt keeps "vtable name"
// for every asset found once; later loads find them in one pass over memory looking only for those few types and
// checking the name at +0x08 (well under a second, instead of the ~80 s two-pass name search). ----
static void asset_cache_path(char *out, int n)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(out, n, "%s\\BotMod_assetvt.txt", dir);
}
#define MAX_ASSET_CACHE 2048
static struct { unsigned vt; char name[160]; } g_assetCache[MAX_ASSET_CACHE];
static int g_assetCacheN;
static void asset_cache_load(void)
{
    char path[MAX_PATH + 32], line[256];
    asset_cache_path(path, sizeof path);
    g_assetCacheN = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (g_assetCacheN < MAX_ASSET_CACHE && fgets(line, sizeof line, f)) {
        unsigned vt; char name[160];
        if (sscanf(line, "%x %159s", &vt, name) == 2) { g_assetCache[g_assetCacheN].vt = vt; strcpy(g_assetCache[g_assetCacheN].name, name); g_assetCacheN++; }
    }
    fclose(f);
}
static unsigned asset_cache_vt(const char *name)
{
    for (int k = 0; k < g_assetCacheN; k++) if (!strcmp(g_assetCache[k].name, name)) return g_assetCache[k].vt;
    return 0;
}
static void asset_cache_save(Fixup *fx, int n)
{
    int added = 0;
    for (int f = 0; f < n; f++) {
        void *a = *fx[f].slot;
        if (a == (void *)-1 || !a || IsBadReadPtr(a, 4) || asset_cache_vt(fx[f].name) || g_assetCacheN >= MAX_ASSET_CACHE) continue;
        g_assetCache[g_assetCacheN].vt = *(unsigned *)a;
        strcpy(g_assetCache[g_assetCacheN].name, fx[f].name);
        g_assetCacheN++; added++;
    }
    if (!added) return;
    char path[MAX_PATH + 32];
    asset_cache_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int k = 0; k < g_assetCacheN; k++) fprintf(f, "%08x %s\n", g_assetCache[k].vt, g_assetCache[k].name);
    fclose(f);
}
// One pass: words equal to a cached type, whose +0x08 name matches a wanted asset of that type.
static int fast_resolve_assets(Fixup *fx, int n)
{
    unsigned vts[64];
    int nvt = 0, found = 0;
    unsigned fvt[MAX_FIXUPS];
    for (int f = 0; f < n; f++) {
        fvt[f] = fx[f].vtable ? fx[f].vtable : asset_cache_vt(fx[f].name);
        if (!fvt[f]) continue;
        int have = 0;
        for (int k = 0; k < nvt; k++) if (vts[k] == fvt[f]) have = 1;
        if (!have && nvt < 64) vts[nvt++] = fvt[f];
    }
    if (!nvt) return 0;
    static int dupes[MAX_FIXUPS];
    memset(dupes, 0, sizeof dupes);
    enum { CHUNK = 1 << 16 };
    static unsigned buf[CHUNK / 4];
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned char *a = (unsigned char *)0x10000; (unsigned)a < 0xffff0000u && !g_abortScans; a = (unsigned char *)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || !mbi.RegionSize) break;
        DWORD prot = mbi.Protect & 0xff;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE)) ||
            !(prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_READ)) continue;
        for (SIZE_T off = 0; off < mbi.RegionSize; off += CHUNK) {
            SIZE_T want = mbi.RegionSize - off < CHUNK ? mbi.RegionSize - off : CHUNK, got = 0;
            char *base = (char *)mbi.BaseAddress + off;
            if (base + want > (char *)buf && base < (char *)buf + sizeof buf) continue;
            if (!ReadProcessMemory(GetCurrentProcess(), base, buf, want, &got) || !got) continue;
            for (SIZE_T w = 0; w < got / 4; w++) {
                unsigned v = buf[w];
                if (v < 0x02028000u || v >= 0x02b00000u) continue;
                int k;
                for (k = 0; k < nvt && vts[k] != v; k++) ;
                if (k == nvt) continue;
                char *obj = base + w * 4;
                const char *nm = asset_name(obj);
                if (nm[0] == '?') continue;
                for (int f = 0; f < n; f++) {
                    if (fvt[f] != v || strcmp(nm, fx[f].name)) continue;
                    if (*fx[f].slot == (void *)-1) { *fx[f].slot = obj; found++; dupes[f] = 1; }
                    else if (*fx[f].slot != obj) {   // a second object by that name and type: log both (stale copy of another level?)
                        unsigned *o1 = (unsigned *)*fx[f].slot, *o2 = (unsigned *)obj;
                        // the live one is the one the level holds references to (+4 low bits: reference count - a freed
                        // copy left by the previous map shows 1); the stale one crashed the server when a bot used it
                        int keepNew = (o2[1] & 0xffffff) > (o1[1] & 0xffffff);
                        if (dupes[f]++ < 3)
                            bot_log("loadouts: %s twice: %p [refs %u] and %p [refs %u] - using %p", fx[f].name, o1, o1[1] & 0xffffff, o2,
                                    o2[1] & 0xffffff, keepNew ? o2 : o1);
                        if (keepNew) *fx[f].slot = obj;
                    }
                }
            }
        }
    }
    return found;
}

#define ADDR_WEAPON_UNLOCK_VTABLE  0x02504b34  // the weapon unlock assets a client selects (every kit item)
static void load_loadouts(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 32], line[8192];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(path, sizeof path, "%s\\BotMod_loadouts.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { bot_log("loadouts: no %s yet", path); return; }
    static Fixup fx[MAX_FIXUPS];
    int nfx = 0, bad[MAX_LOADOUTS] = {0};
    static unsigned *xuSlot[64], *xuBase[64];   // kitextra "unlock" entries (left out where not loaded)
    int nxu = 0;
    LoadoutLib *lib = loadout_lib();
    lib->count = 0;
    Loadout *L = NULL;
    unsigned char *msg = NULL;
    // records a pointer slot: kept if it still names the right asset, otherwise marked for lookup by name
    // vt: the type the slot must hold (0 = any). A weapon slot must be a weapon unlock: on MP_Subway the ammo box's name
    // also belongs to another object (vt 0x0287f330), and handing that to a bot crashed the server.
    #define FIXUP_T(slotp, nm, vt) do { void **s_ = (void **)(slotp); \
        if (!strcmp((nm), "?")) break; \
        /* never kept because the old address still names the asset: after a map change that is often the previous            level's freed copy (Bazaar -> Seine handed bots freed assets and corrupted memory) - every load looks all up */ \
        if (nfx < MAX_FIXUPS) { fx[nfx].slot = s_; strncpy(fx[nfx].name, (nm), sizeof fx[nfx].name - 1); fx[nfx].loadout = lib->count; fx[nfx].vtable = (vt); nfx++; *s_ = (void *)-1; } \
        else bad[lib->count] = 1; } while (0)
    #define FIXUP(slotp, nm) FIXUP_T(slotp, nm, 0)
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "LOADOUT ", 8)) {
            if (L) lib->count++;
            L = lib->count < MAX_LOADOUTS ? &lib->l[lib->count] : NULL;
            if (!L) break;
            memset(L, 0, sizeof *L);
            int nw = 0;
            sscanf(line + 8, "%d %63s %d", &L->team, L->kit, &nw);
            msg = NULL;
        } else if (L && (line[0] == 'U' || line[0] == 'W') && line[1] == ' ') {
            msg = line[0] == 'U' ? L->unlockMsg : (L->nWeapons < KIT_MAX_WEAPONS ? L->weaponMsg[L->nWeapons++] : NULL);
            if (!msg) continue;
            for (int k = 0; k < KIT_MSG_SIZE; k++) { unsigned v; sscanf(line + 2 + 2 * k, "%2x", &v); msg[k] = (unsigned char)v; }
        } else if (L && msg && line[0] == 'P') {
            int off; void *ptr; char name[256];
            if (sscanf(line + 2, "%d %p %255s", &off, &ptr, name) == 3) {
                *(void **)(msg + off) = ptr;
                int weaponAsset = msg != L->unlockMsg && off == 0x3c;
                FIXUP_T(msg + off, name, weaponAsset ? ADDR_WEAPON_UNLOCK_VTABLE : 0);
            }
        } else if (L && msg && line[0] == 'A') {
            int off; unsigned n; int pos = 0;
            if (sscanf(line + 2, "%d %u%n", &off, &n, &pos) < 2 || n > 256) { bad[lib->count] = 1; continue; }
            unsigned *arr = malloc((n + 1) * sizeof(unsigned));   // kept for the process lifetime
            arr[0] = n;
            char *p = line + 2 + pos;
            for (unsigned k = 0; k < n; k++) {
                void *ptr; char name[256]; int used = 0;
                if (sscanf(p, " %p:%255s%n", &ptr, name, &used) < 2) { bad[lib->count] = 1; break; }
                p += used;
                arr[k + 1] = (unsigned)ptr;
                FIXUP(&arr[k + 1], name);
            }
            *(void **)(msg + off) = (void *)(arr + 1);   // empty lists must still point at a count of 0 (engine reads [-1])
        }
    }
    if (L) lib->count++;
    fclose(f);
    {   // BotMod_kitextra.txt "<class> <slot> <asset>": that item in that slot for every kit of the class (the slot's own
        // item replaced, its attachments dropped) - e.g. "Engineer 1 Weapons/Gadgets/M15/U_M15": AT mines in the pistol slot
        char epath[MAX_PATH + 32];
        snprintf(epath, sizeof epath, "%s\\BotMod_kitextra.txt", dir);
        FILE *fe = fopen(epath, "r");
        char el[512];
        while (fe && fgets(el, sizeof el, fe)) {
            char cls[32], asset[256], kind[16];
            int slot;
            if (el[0] == '#') continue;
            if (sscanf(el, "%31s %15s %255s", cls, kind, asset) == 3 && !_stricmp(kind, "unlock")) {
                // "<class|*> unlock <asset>": one more entry in the kit's unlock list (+0x40) - vehicle unlocks ride there
                // too (e.g. Persistence/Unlocks/Vehicles/JetFlareLauncher: a bot's jet has no countermeasure without it)
                int changed = 0;
                for (int k = 0; k < lib->count; k++) {
                    Loadout *X = &lib->l[k];
                    if (strcmp(cls, "*") && _stricmp(kit_class(X->kit), cls)) continue;
                    unsigned *old = *(unsigned **)(X->unlockMsg + 0x40);
                    unsigned n = old ? old[-1] : 0;
                    if (n >= 64 || nxu >= (int)(sizeof xuSlot / sizeof xuSlot[0])) continue;
                    unsigned *nu = malloc((n + 2) * sizeof(unsigned));   // (kept for the process lifetime, like the loaded arrays)
                    if (!nu) continue;
                    nu[0] = n + 1;
                    if (n) memcpy(nu + 1, old, n * sizeof(unsigned));
                    for (int q = 0; q < nfx; q++)   // pending lookups into the old array follow it to the new one
                        if (old && (unsigned *)fx[q].slot >= old && (unsigned *)fx[q].slot < old + n) fx[q].slot = (void **)(nu + 1 + ((unsigned *)fx[q].slot - old));
                    for (int q = 0; q < nxu; q++)
                        if (old && xuBase[q] == old) { xuSlot[q] = nu + 1 + (xuSlot[q] - old); xuBase[q] = nu + 1; }
                    *(unsigned **)(X->unlockMsg + 0x40) = nu + 1;
                    int save = lib->count;   // (FIXUP files the lookup under lib->count)
                    lib->count = k;
                    FIXUP(nu + 1 + n, asset);
                    lib->count = save;
                    xuSlot[nxu] = nu + 1 + n; xuBase[nxu] = nu + 1; nxu++;
                    changed++;
                }
                bot_log("loadouts: %s kits get the unlock %s (%d kit(s), BotMod_kitextra.txt)", cls, asset, changed);
                continue;
            }
            if (sscanf(el, "%31s %15s %255s", cls, kind, asset) == 3 && !_stricmp(kind, "nounlock")) {
                // "<class|*> nounlock <asset>": that unlock out of the kits (a captured human kit brought IfvTOW: it took the
                // IFV's second weapon from the coax - the user, 2026-10-13). The entry is renamed to another unlock the kit
                // already has: the selected unlocks are a set, so the duplicate is the same as no entry.
                int changed = 0;
                for (int q = 0; q < nfx; q++) {
                    if (_stricmp(fx[q].name, asset)) continue;
                    int k = fx[q].loadout;
                    if (k < 0 || k >= lib->count || (strcmp(cls, "*") && _stricmp(kit_class(lib->l[k].kit), cls))) continue;
                    unsigned *ua = *(unsigned **)(lib->l[k].unlockMsg + 0x40);
                    unsigned un = ua ? ua[-1] : 0;
                    if (!ua || (unsigned *)fx[q].slot < ua || (unsigned *)fx[q].slot >= ua + un) continue;   // not in its unlock list
                    for (int r = 0; r < nfx; r++)
                        if (r != q && fx[r].loadout == k && (unsigned *)fx[r].slot >= ua && (unsigned *)fx[r].slot < ua + un && _stricmp(fx[r].name, asset)) {
                            snprintf(fx[q].name, sizeof fx[q].name, "%s", fx[r].name);
                            changed++;
                            break;
                        }
                }
                bot_log("loadouts: %s kits lose the unlock %s (%d entr%s, BotMod_kitextra.txt)", cls, asset, changed, changed == 1 ? "y" : "ies");
                continue;
            }
            if (sscanf(el, "%31s %15s %255s", cls, kind, asset) == 3 && !_stricmp(kind, "perk")) {   // the specialization
                int changed = 0;
                for (int q = 0; q < nfx; q++) {
                    int k = fx[q].loadout;
                    if (k < 0 || k >= lib->count || (strcmp(cls, "*") && _stricmp(kit_class(lib->l[k].kit), cls))) continue;
                    if (!strstr(fx[q].name, "Specialization")) continue;   // (NoSpecialization or another perk)
                    snprintf(fx[q].name, sizeof fx[q].name, "%s", asset);
                    fx[q].vtable = 0;
                    changed++;
                }
                bot_log("loadouts: %s kits get the perk %s (%d kit(s), BotMod_kitextra.txt)", cls, asset, changed);
                continue;
            }
            if (sscanf(el, "%31s %d %255s", cls, &slot, asset) != 3 || slot < 0 || slot > 9) continue;
            for (int k = 0; k < lib->count; k++) {
                Loadout *X = &lib->l[k];
                if (_stricmp(kit_class(X->kit), cls)) continue;
                unsigned char *m = NULL;
                for (int w = 0; w < X->nWeapons && !m; w++) if (*(int *)(X->weaponMsg[w] + 0x38) == slot) m = X->weaponMsg[w];
                if (!m && X->nWeapons > 0 && X->nWeapons < KIT_MAX_WEAPONS) {   // a slot the kit leaves empty: a copy of its first entry
                    m = X->weaponMsg[X->nWeapons++];
                    memcpy(m, X->weaponMsg[0], KIT_MSG_SIZE);
                    *(int *)(m + 0x38) = slot;
                }
                if (!m) continue;
                unsigned *none = malloc(2 * sizeof(unsigned));   // no attachments (kept for the process lifetime)
                none[0] = 0;
                *(void **)(m + 0x40) = (void *)(none + 1);
                for (int q = 0; q < nfx; q++)   // the slot's own item is no longer wanted there (no lookup for it)
                    if (fx[q].slot == (void **)(m + 0x3c)) { fx[q] = fx[--nfx]; q--; }
                int save = lib->count;   // (FIXUP_T files the fixup under lib->count)
                lib->count = k;
                FIXUP_T(m + 0x3c, asset, ADDR_WEAPON_UNLOCK_VTABLE);
                lib->count = save;
                bot_log("loadouts: team %d %s gets %s in slot %d (BotMod_kitextra.txt)", X->team, kit_class(X->kit), asset, slot);
            }
        }
        if (fe) fclose(fe);
    }
    #undef FIXUP
    #undef FIXUP_T
    if (nfx) {
        DWORD t0 = GetTickCount();
        asset_cache_load();
        int fast = fast_resolve_assets(fx, nfx);
        static Fixup rest[MAX_FIXUPS];   // what the cache did not cover: the slow name search
        int nrest = 0;
        for (int k = 0; k < nfx; k++) if (*fx[k].slot == (void *)-1) rest[nrest++] = fx[k];
        if (nrest) resolve_assets(rest, nrest);
        asset_cache_save(fx, nfx);
        for (int q = 0; q < nxu; q++)   // an extra unlock not loaded on this level: left out (the kit stays usable)
            if (*xuSlot[q] == (unsigned)-1) {
                if (xuSlot[q] == xuBase[q]) { *xuSlot[q] = 0; xuBase[q][-1] = 0; }   // it was the only entry: an empty list
                else *xuSlot[q] = xuBase[q][0];                                        // a repeat of the first entry
                bot_log("loadouts: an extra unlock is not loaded on this level - left out");
            }
        bot_log("loadouts: %d asset(s) from the type cache, %d by the name search", fast, nrest);
        int unresolved = 0;
        for (int k = 0; k < nfx; k++) if (*fx[k].slot == (void *)-1) { bad[fx[k].loadout] = 1; unresolved++; }
        bot_log("loadouts: looked up %d moved asset(s) by name in %lu ms, %d not found", nfx, GetTickCount() - t0, unresolved);
    }
    // drop loadouts with unresolved assets (not loaded on this level)
    int w = 0;
    for (int i = 0; i < lib->count; i++) {
        if (bad[i]) { bot_log("loadouts: skipping team %d %s (assets not loaded on this level)", lib->l[i].team, kit_class(lib->l[i].kit)); continue; }
        if (w != i) lib->l[w] = lib->l[i];
        w++;
    }
    lib->count = w;
    bot_log("loadouts: %d usable from %s", lib->count, path);
    for (int i = 0; i < lib->count; i++) bot_log("  #%d team %d %s: %d weapon slot(s)", i, lib->l[i].team, kit_class(lib->l[i].kit), lib->l[i].nWeapons);
}
// Per-human batch: a deploy sends the weapon selections, then the unlocks.
#define MAX_PENDING 16
typedef struct PendingKit { void *player; unsigned lastFrame; int n; unsigned char weapons[KIT_MAX_WEAPONS][KIT_MSG_SIZE]; } PendingKit;
static PendingKit g_pending[MAX_PENDING];

static PendingKit *pending_for(void *player)
{
    PendingKit *freeSlot = NULL;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].player == player) return &g_pending[i];
        if (!g_pending[i].player && !freeSlot) freeSlot = &g_pending[i];
    }
    if (!freeSlot) freeSlot = &g_pending[0];
    memset(freeSlot, 0, sizeof *freeSlot);
    freeSlot->player = player;
    return freeSlot;
}

// Items bots must not carry on a given map (a kit item that crashes the server there): BotMod\kitskip.txt, one rule
// per line "<map code or *> <part of the item's asset name>", e.g. "MP_Subway Claymore". Re-read on every level.
#define MAX_KITSKIP 32
static char g_kitSkipMap[MAX_KITSKIP][32], g_kitSkipItem[MAX_KITSKIP][64];
static int g_kitSkipCount;
static void botmod_file(char *out, int n, const char *name);
static void load_kitskip(void)
{
    char path[MAX_PATH + 64], line[160];
    botmod_file(path, sizeof path, "kitskip.txt");
    g_kitSkipCount = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_kitSkipCount < MAX_KITSKIP) {
        char m[32], it[64];
        if (line[0] == '#' || sscanf(line, "%31s %63s", m, it) != 2) continue;
        strcpy(g_kitSkipMap[g_kitSkipCount], m); strcpy(g_kitSkipItem[g_kitSkipCount], it); g_kitSkipCount++;
    }
    fclose(f);
}
static const char *current_level(void);
static int kit_item_skipped(const char *asset)
{
    const char *lvl = current_level();
    for (int k = 0; k < g_kitSkipCount; k++)
        if ((!strcmp(g_kitSkipMap[k], "*") || (lvl && !strcmp(g_kitSkipMap[k], lvl))) && strstr(asset, g_kitSkipItem[k])) return 1;
    return 0;
}

static int apply_loadout(char *bot, int idx)
{
    LoadoutLib *lib = loadout_lib();
    if (idx < 0 || idx >= lib->count) return 0;
    Loadout *L = &lib->l[idx];
    unsigned char msg[KIT_MSG_SIZE] __attribute__((aligned(16)));
    for (int w = 0; w < L->nWeapons; w++) {   // same order a client sends them: weapons, then unlocks
        const char *asset = asset_name(*(void **)(L->weaponMsg[w] + 0x3c));
        if (kit_item_skipped(asset)) {
            static unsigned logged;
            if (logged++ < 20) bot_log("kit: leaving %s out of %s's kit on this map (kitskip.txt)", asset, bot + OFF_PLAYER_ONLINE_NAME);
            continue;
        }
        memcpy(msg, L->weaponMsg[w], KIT_MSG_SIZE);
        ((PlayerMsg_t)ADDR_PLAYER_SELECT_WEAPON)(bot, msg);
    }
    memcpy(msg, L->unlockMsg, KIT_MSG_SIZE);
    ((PlayerMsg_t)ADDR_PLAYER_SELECT_UNLOCKS)(bot, msg);
    return 1;
}

// Class a bot plays, from its name ("[BOT] Medic US" -> Assault); NULL = any.
static const char *bot_role_class(const char *name)
{
    if (strstr(name, "Pilot")) return "Engineer";   // pilots fly / drive as Engineers (repair tool, launcher)
    if (strstr(name, "Medic") || strstr(name, "Assault")) return "Assault";
    if (strstr(name, "Engineer")) return "Engineer";
    if (strstr(name, "Support")) return "Support";
    if (strstr(name, "Recon")) return "Recon";
    return NULL;
}

// Loadout for the bot: its role's class for its team if captured, otherwise cycle through the team's loadouts (-1 = none).
static int pick_loadout(char *bot)
{
    LoadoutLib *lib = loadout_lib();
    int team = player_team(bot), nth = 0, candidates[MAX_LOADOUTS], nc = 0;
    const char *role = bot_role_class(bot + OFF_PLAYER_ONLINE_NAME);
    if (role)
        for (int i = 0; i < lib->count; i++)
            if (lib->l[i].team == team && lib->l[i].nWeapons && !strcmp(kit_class(lib->l[i].kit), role)) return i;
    for (int i = 0; i < H->botCount && H->bots[i] != bot; i++) if (player_team(H->bots[i]) == team) nth++;
    for (int i = 0; i < lib->count; i++) if (lib->l[i].team == team && lib->l[i].nWeapons) candidates[nc++] = i;
    return nc ? candidates[nth % nc] : -1;
}

static void apply_kit(char *bot)
{
    LoadoutLib *lib = loadout_lib();
    int team = player_team(bot), bi = bot_index(bot);
    int idx = pick_loadout(bot);
    if (idx < 0) { bot_log("kit: no loadout captured for team %d yet", team); return; }
    if (apply_loadout(bot, idx)) {
        if (bi >= 0) g_botLoadout[bi] = idx;
        *kitted_mask() |= 1ull << bi;
        bot_log("kit: %s gets team %d %s", bot + OFF_PLAYER_ONLINE_NAME, team, kit_class(lib->l[idx].kit));
    }
}

// Loadout capture is off unless BotMod_capture.on exists next to the server exe ('capture on|off' toggles it),
// so the library stays fixed no matter what humans deploy as.
static int capture_enabled(void)
{
    static unsigned checkedAt;
    static int enabled;
    if (!checkedAt || H->frame - checkedAt > 150) {
        char dir[MAX_PATH], path[MAX_PATH + 32];
        GetModuleFileNameA(NULL, dir, sizeof dir);
        *strrchr(dir, '\\') = 0;
        snprintf(path, sizeof path, "%s\\BotMod_capture.on", dir);
        enabled = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        checkedAt = H->frame ? H->frame : 1;
    }
    return enabled;
}

static int player_exists(char *pl);
// Class each human last deployed as (sent with their unlock selection on every deploy).
static char *g_humanKitPlayer[64];
static char g_humanEngineer[64];
static int player_is_engineer(char *pl)
{
    if (is_bot(pl)) return strstr(pl + OFF_PLAYER_ONLINE_NAME, "Engineer") != NULL;
    for (int k = 0; k < 64; k++) if (g_humanKitPlayer[k] == pl) return g_humanEngineer[k];
    return 0;
}

void __cdecl on_select_unlocks(void *player, unsigned *stack)
{
    if (!is_bot(player)) {
        const char *k = asset_name(*(void **)((unsigned char *)stack[1] + 0x38));
        int slot = -1;
        for (int j = 0; j < 64 && slot < 0; j++) if (g_humanKitPlayer[j] == player) slot = j;
        for (int j = 0; j < 64 && slot < 0; j++) if (!g_humanKitPlayer[j] || !player_exists(g_humanKitPlayer[j])) slot = j;
        if (slot >= 0) { g_humanKitPlayer[slot] = player; g_humanEngineer[slot] = !strcmp(kit_class(k), "Engineer"); }
    }
    if (is_bot(player) || !capture_enabled()) return;
    unsigned char *msg = (unsigned char *)stack[1];
    int team = player_team(player);
    if (team != 1 && team != 2) return;
    PendingKit *pk = pending_for(player);
    const char *kit = asset_name(*(void **)(msg + 0x38));
    if (!strcmp(kit, "?")) return;
    LoadoutLib *lib = loadout_lib();
    int idx = -1;
    for (int i = 0; i < lib->count; i++) if (lib->l[i].team == team && !strcmp(lib->l[i].kit, kit)) idx = i;
    if (idx < 0) { if (lib->count >= MAX_LOADOUTS) return; idx = lib->count++; }
    Loadout *L = &lib->l[idx];
    int newWeapons = H->frame - pk->lastFrame <= 90 ? pk->n : 0;   // weapons sent just before these unlocks
    if (!newWeapons && L->nWeapons) {
        // a plain redeploy does not resend weapons: keep the ones we already have for this kit
        capture_msg(L->unlockMsg, msg, 0);
        bot_log("loadout: team %d %s unlocks refreshed from %s (weapons kept)", team, kit_class(kit), (char *)player + OFF_PLAYER_ONLINE_NAME);
        save_loadouts();
        return;
    }
    if (!newWeapons) { bot_log("loadout: %s sent no weapon list for %s - change loadout once to capture it", (char *)player + OFF_PLAYER_ONLINE_NAME, kit); }
    memset(L, 0, sizeof *L);
    L->team = team;
    strncpy(L->kit, kit, sizeof L->kit - 1);
    capture_msg(L->unlockMsg, msg, 0);
    L->nWeapons = newWeapons;
    memcpy(L->weaponMsg, pk->weapons, sizeof(L->weaponMsg[0]) * L->nWeapons);
    pk->n = 0;
    log_loadout(L, (char *)player + OFF_PLAYER_ONLINE_NAME);
    save_loadouts();
}

static int g_kitWatch;   // console: kitwatch 0/1 - log every weapon a human selects (diagnostics, nothing saved)
static void log_weapon_msg(const char *who, const unsigned char *msg)
{
    void *asset = *(void **)(msg + 0x3c);
    char hex[300];
    int n = 0;
    for (int o = 0; o < 0x60 && n < (int)sizeof hex - 10; o += 4) n += snprintf(hex + n, sizeof hex - n, "%08x ", *(unsigned *)(msg + o));
    bot_log("kitwatch: %s slot %d asset %p (vt %08x) %s | %s", who, *(int *)(msg + 0x38), asset,
            asset && !IsBadReadPtr(asset, 4) ? *(unsigned *)asset : 0, asset_name(asset), hex);
}
void __cdecl on_select_weapon(void *player, unsigned *stack)
{
    if (g_kitWatch && !is_bot(player)) log_weapon_msg((char *)player + OFF_PLAYER_ONLINE_NAME, (unsigned char *)stack[1]);
    if (is_bot(player) || !capture_enabled()) return;
    unsigned char *msg = (unsigned char *)stack[1];
    PendingKit *pk = pending_for(player);
    if (H->frame - pk->lastFrame > 60) pk->n = 0;   // >2 s gap: new batch
    pk->lastFrame = H->frame;
    if (pk->n < KIT_MAX_WEAPONS) capture_msg(pk->weapons[pk->n++], msg, 1);
}
extern void hook_select_unlocks(void);
extern void hook_select_weapon(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_select_unlocks\n"
    "_hook_select_unlocks:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  push ecx\n"
    "  call _on_select_unlocks\n"
    "  add esp, 8\n"
    "  popfd\n"
    "  popad\n"
    "  push ebp\n"
    "  mov ebp, esp\n"
    "  and esp, 0xfffffff8\n"
    "  push 0x00e331b6\n"
    "  ret\n"
    ".globl _hook_select_weapon\n"
    "_hook_select_weapon:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  push ecx\n"
    "  call _on_select_weapon\n"
    "  add esp, 8\n"
    "  popfd\n"
    "  popad\n"
    "  sub esp, 0x24\n"
    "  push ebx\n"
    "  push ebp\n"
    "  push 0x00e33045\n"
    "  ret\n"
    ".att_syntax prefix\n");

// ---- Soldier update diagnostics + experimental bot stepping ----
static unsigned g_applyCalls, g_prePhysCalls;

void __cdecl on_soldier_apply(void *soldier, unsigned *stack)
{
    if (!H->traceInput || !soldier_is_bot(soldier)) return;
    if (g_applyCalls++ % 30) return;
    float *lv = (float *)((char *)stack[2] + 0x10);
    float *pos = (float *)((char *)soldier + OFF_SOLDIER_POSITION);
    bot_log("  prePhysicsUpdateEntry soldier=%p entry=%u input=%p levels=[%.2f %.2f %.2f] pos=(%.1f,%.1f,%.1f) calls=%u",
            soldier, stack[1], (void *)stack[2], lv[0], lv[1], lv[2], pos[0], pos[1], pos[2], g_applyCalls);
}

void __cdecl on_soldier_prephys(void *soldier, unsigned *stack)
{
    if (!H->traceInput) return;
    if (g_prePhysCalls++ % 61) return;   // odd modulus so alternating soldiers all get sampled
    unsigned *arg = (unsigned *)stack[1];
    // pushad saved the caller's registers just below the return address: esi (the update-list owner) is at stack[-7].
    unsigned *owner = (unsigned *)stack[-7];
    unsigned listBegin = owner ? owner[0x54 / 4] : 0, listEnd = owner ? owner[0x58 / 4] : 0;
    bot_log("  prePhysicsUpdate soldier=%p bot=%d ret=%08x arg=%p [%08x %08x %08x %08x] owner=%p vt=%08x list=%08x..%08x (%u entries)",
            soldier, soldier_is_bot(soldier), stack[0], arg, arg ? arg[0] : 0, arg ? arg[1] : 0, arg ? arg[2] : 0, arg ? arg[3] : 0,
            owner, owner ? owner[0] : 0, listBegin, listEnd, (listEnd - listBegin) / 12);
}

extern void hook_soldier_apply(void);
extern void hook_soldier_prephys(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_soldier_apply\n"
    "_hook_soldier_apply:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  push ecx\n"
    "  call _on_soldier_apply\n"
    "  add esp, 8\n"
    "  popfd\n"
    "  popad\n"
    "  push 0x00e0a7c0\n"
    "  ret\n"
    ".globl _hook_soldier_prephys\n"
    "_hook_soldier_prephys:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  push ecx\n"
    "  call _on_soldier_prephys\n"
    "  add esp, 8\n"
    "  popfd\n"
    "  popad\n"
    "  push 0x00e0a3d0\n"
    "  ret\n"
    ".att_syntax prefix\n");

// ---- Bot soldier stepping ----
// In MP (SingleStepCharacter) a human soldier only moves when its connection single-steps it once per received move:
// soldier->singleStep(entry) applies player+0x3c0 input, advances the character physics by the input's deltaTime and
// finishes the move. Bots have no connection, so we single-step their soldiers ourselves once per server frame.
#define SOLDIER_VSLOT_SINGLE_STEP  0x140       // ServerSoldierEntity::singleStep(entryIndex) = 0x00e009c0
typedef void (__attribute__((thiscall)) *SingleStep_t)(void *soldier, int entry);

static void step_bot_soldiers(void)
{
    for (int i = 0; i < H->botCount; i++) {
        void *s = *(void **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
        if (!s) continue;
        SingleStep_t fn = (SingleStep_t)(*(void ***)s)[SOLDIER_VSLOT_SINGLE_STEP / 4];
        fn(s, 0);
    }
}
// ---- Steering ----
// Per-bot navigation state in host->reserved (survives reloads): mode 0 = manual (move command), 1 = face heading,
// 2 = go to point (face it, walk, stop within GOTO_ARRIVE_DIST).
typedef struct BotNav { int mode; float targetYaw; float x, z; } BotNav;   // mode 3 = follow planned path
#define GOTO_ARRIVE_DIST 2.0f
static BotNav *bot_nav(int i) { return (BotNav *)(H->reserved + 1536) + i; }   // +1536..+2560

static float wrap_pi(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Heading that points the soldier along (dx, dz). Measured: yaw 0 walks +z, yaw -90deg walks -x => forward = (sin yaw, cos yaw).
static float yaw_towards(float dx, float dz) { return atan2f(dx, dz); }

// Stuck handling independent of re-planning: trying to move but no 1 m of progress in 3 s -> back off and sidestep for
// a second, block the grid links in that direction and re-plan.
typedef struct Progress { float x, z; unsigned frame; unsigned unstickUntil; float strafe; unsigned directUntil; int directTries; } Progress;
static Progress g_progress[MAX_BOTS];
static void on_bot_stuck(int i, char *soldier);
static void press_button(int i, int action, int frames);
#define EIA_JUMP 12
// Stuck against something knee/waist high (barriers, low walls, crates): jumping into it vaults over - as long as
// forward stays held until the vault is done (VAULT_HOLD_FRAMES), whatever the stuck handling wanted next.
#define VAULT_HOLD_FRAMES 30
static unsigned g_vaultUntil[MAX_BOTS];
static float g_vaultFrom[MAX_BOTS][3];
static void stuck_jump(int i, char *soldier, const char *why)
{
    static unsigned lastAt[MAX_BOTS];
    if (H->frame - lastAt[i] < 40) return;
    lastAt[i] = H->frame;
    press_button(i, EIA_JUMP, 4);
    g_vaultUntil[i] = H->frame + VAULT_HOLD_FRAMES;
    if (soldier) memcpy(g_vaultFrom[i], soldier + OFF_SOLDIER_POSITION, 12);
    static unsigned logged;
    if (logged++ < 50) bot_log("path: %s jumps (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    (void)soldier;
}
static void path_goal(int i, float *x, float *z);

// ---- Fences: a bot stuck against a chain-link fence (recognised by the material its ray hits - the "sonar
// signature" catalogue below) knifes through it like a player would, then walks on. If it is still stuck at the
// same spot afterwards the usual escape runs: jump, walk straight, back off and re-route. ----
typedef struct FenceKind { unsigned material; const char *name; int knifeable; } FenceKind;
static const FenceKind kFences[] = {
    {0x03003a83, "chain-link fence", 1},    // MP_007: GroupPhysicsEntity part, ~2 cm, 4-6 m tall, two-sided; a knife cuts it
    {0x200014d5, "sheet-metal fence", 0},   // MP_007: GroupPhysicsEntity; knives do nothing - go round
};
#define EIA_FIRE_KNIFE 7
static unsigned g_fenceUntil[MAX_BOTS], g_fenceAt[MAX_BOTS];
static unsigned char g_fenceTries[MAX_BOTS];   // knife attempts at the current spot (2nd one sidesteps off a post first)
static float g_fenceStrafe[MAX_BOTS];
static float g_fencePos[MAX_BOTS][3];
static unsigned char g_lastHitRest[0x40];
static int ray_cast(const float *from3, const float *to3, float *hitPos, float *hitNormal);
static unsigned g_rayFlags;
void schedule_press(int i, int action, unsigned at);
static unsigned hit_material(void)   // MaterialContainerPair flags+index of the last ray hit (0 = unknown)
{
    char *mp = *(char **)(g_lastHitRest + 4);
    return mp && !IsBadReadPtr(mp, 12) ? *(unsigned *)(mp + 8) : 0;
}
// The catalogued fence right in front of the bot at chest height, or NULL
static const FenceKind *fence_ahead(int i, char *soldier)
{
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION), yaw = *(float *)(soldier + OFF_SOLDIER_YAW);
    float a[3] = {pos[0], pos[1] + 1.2f, pos[2]}, b[3] = {pos[0] + sinf(yaw) * 1.5f, pos[1] + 1.2f, pos[2] + cosf(yaw) * 1.5f}, h[3];
    unsigned saved = g_rayFlags;
    g_rayFlags = 0x20;   // RAY_DONT_CHECK_CHARACTER
    int r = ray_cast(a, b, h, NULL);
    g_rayFlags = saved;
    if (!r) return NULL;
    unsigned m = hit_material();
    for (unsigned k = 0; k < sizeof kFences / sizeof kFences[0]; k++) if (m == kFences[k].material) return &kFences[k];
    static unsigned logged;
    if ((m >> 24) == 0x03 && logged++ < 30)   // see-through + penetrable, but not catalogued yet
        bot_log("fence: %s is stuck at an uncatalogued see-through material %08x at (%.1f, %.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, m, h[0], h[1], h[2]);
    return NULL;
}
// 1: knifing a fence; 2: a fence no knife gets through (go round now); 0: no catalogued fence / tried here already
static int try_cut_fence(int i, char *soldier)
{
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    float dx = pos[0] - g_fencePos[i][0], dz = pos[2] - g_fencePos[i][2];
    int again = g_fenceAt[i] && H->frame - g_fenceAt[i] < 20 * 30 && dx * dx + dz * dz < 3.0f * 3.0f;
    if (again && g_fenceTries[i] >= 2) return 0;   // tried here twice already
    if (!again) g_fenceTries[i] = 0;
    const FenceKind *fk = fence_ahead(i, soldier);
    if (!fk) return 0;
    if (!fk->knifeable) {
        bot_log("fence: %s is stuck at a %s at (%.1f, %.1f) - going round", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, fk->name, pos[0], pos[2]);
        g_fenceAt[i] = H->frame;
        memcpy(g_fencePos[i], pos, 12);
        return 2;
    }
    unsigned f = H->frame;
    g_fenceTries[i]++;
    g_fenceStrafe[i] = g_fenceTries[i] > 1 ? ((rand() & 1) ? 0.8f : -0.8f) : 0.0f;   // 2nd try: a step along the fence (off a post)
    if (g_fenceStrafe[i] != 0) f += 20;
    schedule_press(i, SLOT_ACTION(7), f + 1);   // knife out
    schedule_press(i, EIA_FIRE_KNIFE, f + 25);   // slash, slash, slash
    schedule_press(i, EIA_FIRE_KNIFE, f + 45);
    schedule_press(i, EIA_FIRE_KNIFE, f + 65);
    schedule_press(i, SLOT_ACTION(0), f + 90);   // rifle back out
    g_fenceUntil[i] = f + 95;
    if (g_fenceStrafe[i] != 0) bot_log("fence: %s steps along the fence and tries again", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
    g_fenceAt[i] = H->frame;
    memcpy(g_fencePos[i], pos, 12);
    bot_log("fence: %s knifes through the %s at (%.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, fk->name, pos[0], pos[2]);
    return 1;
}

// For the nav builder: what the last ray hit (engine pointers straight from the hit record - no checks, this runs
// for millions of rays).
#define ADDR_WATER_PHYSICS_VTABLE 0x02486a40
static int nav_hit_class(void)
{
    char *body = *(char **)g_lastHitRest, *mp = *(char **)(g_lastHitRest + 4);
    if (body && *(unsigned *)body == ADDR_WATER_PHYSICS_VTABLE) return NAV_HIT_WATER;
    if (mp) {
        unsigned m = *(unsigned *)(mp + 8);
        for (unsigned k = 0; k < sizeof kFences / sizeof kFences[0]; k++) if (m == kFences[k].material && kFences[k].knifeable) return NAV_HIT_FENCE;
    }
    return 0;
}

static int climb_ladder(int i, char *soldier, BotNav *nav);
// ---- Stairs (and escalators / steep ramps), up or down - like ladders: at the foot or the top the bot stops, turns to
// face straight along the flight (its real direction, from the slope of the ground - not the grid), walks it without
// turning, and only goes back to normal navigation 2 m after the ground is level again. Bots used to take flights at an
// angle from a waypoint off to the side and walk into the rails. ----
#define STAIR_RISE        0.45f    // ground 1.5 m ahead this much higher / lower: stairs
#define STAIR_RELEASE     2.0f     // level ground walked after the flight before normal navigation resumes
#define STAIR_MAX_FRAMES  (12 * 30)
static unsigned char g_stairOn[MAX_BOTS], g_stairLevel[MAX_BOTS];
static float g_stairAx[MAX_BOTS], g_stairAz[MAX_BOTS], g_stairLevelPos[MAX_BOTS][3], g_stairMovedPos[MAX_BOTS][3];
static unsigned g_stairAt[MAX_BOTS], g_stairCheckAt[MAX_BOTS], g_stairMovedAt[MAX_BOTS];
static float wrap_err(float targetYaw, float yaw);
static float dist2(const float *a, const float *b);
static void reroute_from_here(int i, char *soldier, const char *why);
static int ground_at(float x, float y, float z, float *h)
{
    float a[3] = {x, y + 1.5f, z}, b[3] = {x, y - 3.0f, z}, hit[3];
    if (!ray_cast(a, b, hit, NULL)) return 0;
    *h = hit[1];
    return 1;
}
static int stair_tick(int i, char *soldier, BotNav *nav)
{
    const char *name = (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME;
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    if (nav->mode != 3) { g_stairOn[i] = 0; return 0; }
    if (!g_stairOn[i]) {
        if (H->frame < g_stairCheckAt[i]) return 0;
        g_stairCheckAt[i] = H->frame + 5;   // ~6 looks a second
        float dx = nav->x - pos[0], dz = nav->z - pos[2], d = sqrtf(dx * dx + dz * dz), h0, h1;
        if (d < 1.0f) return 0;
        dx /= d; dz /= d;
        if (!ground_at(pos[0], pos[1], pos[2], &h0) || !ground_at(pos[0] + dx * 1.5f, pos[1], pos[2] + dz * 1.5f, &h1)) return 0;
        float rise = h1 - h0;
        if (fabsf(rise) < STAIR_RISE) return 0;
        // the flight's own direction: the slope of the ground 2 m ahead (1 m either side, both axes)
        float cx = pos[0] + dx * 2.0f, cz = pos[2] + dz * 2.0f, xp, xm, zp, zm;
        float yRef = pos[1] + (rise > 0 ? 1.0f : -1.0f);
        if (!ground_at(cx + 1.0f, yRef, cz, &xp) || !ground_at(cx - 1.0f, yRef, cz, &xm) || !ground_at(cx, yRef, cz + 1.0f, &zp) ||
            !ground_at(cx, yRef, cz - 1.0f, &zm)) return 0;
        float gx = (xp - xm) * 0.5f, gz = (zp - zm) * 0.5f, gl = sqrtf(gx * gx + gz * gz);
        if (gl < 0.2f) return 0;   // not a slope there (a single step / a ledge)
        float ax = gx / gl, az = gz / gl;
        if (rise < 0) { ax = -ax; az = -az; }   // going down: face downhill
        if (ax * dx + az * dz < 0.3f) return 0;   // a flight we are only passing beside
        g_stairOn[i] = 1; g_stairLevel[i] = 0;
        g_stairAx[i] = ax; g_stairAz[i] = az;
        g_stairAt[i] = g_stairMovedAt[i] = H->frame;
        memcpy(g_stairMovedPos[i], pos, 12);
        bot_log("stairs: %s lines up with a flight going %s at (%.1f, %.1f), heading %.0f deg", name, rise > 0 ? "up" : "down", pos[0], pos[2],
                yaw_towards(ax, az) * 57.2958f);
    }
    // on the flight: face along it (stop to turn first), walk straight
    if (H->frame - g_stairAt[i] > STAIR_MAX_FRAMES) { g_stairOn[i] = 0; g_stairCheckAt[i] = H->frame + 5 * 30; bot_log("stairs: %s gives up on the flight (too long)", name); return 0; }
    float ax = g_stairAx[i], az = g_stairAz[i];
    float err = wrap_err(yaw_towards(ax, az), *(float *)(soldier + OFF_SOLDIER_YAW));
    nav->targetYaw = yaw_towards(ax, az);
    H->botMove[i][0] = fabsf(err) < 0.12f ? 1.0f : 0.0f;
    H->botMove[i][1] = 0;
    H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -err * 2.5f));
    follow_path(i, soldier, nav);   // (waypoints passed on the way still count)
    if (nav->mode != 3) { g_stairOn[i] = 0; return 0; }
    // level again under the bot (front and back of it the same height): STAIR_RELEASE m more, then normal navigation
    float hf, hb;
    if (ground_at(pos[0] + ax * 0.8f, pos[1], pos[2] + az * 0.8f, &hf) && ground_at(pos[0] - ax * 0.8f, pos[1], pos[2] - az * 0.8f, &hb) &&
        fabsf(hf - hb) < 0.25f) {
        if (!g_stairLevel[i]) { g_stairLevel[i] = 1; memcpy(g_stairLevelPos[i], pos, 12); }
        else if (dist2(pos, g_stairLevelPos[i]) >= STAIR_RELEASE * STAIR_RELEASE) {
            g_stairOn[i] = 0; g_stairCheckAt[i] = H->frame + 30;
            bot_log("stairs: %s is off the flight at (%.1f, %.1f)", name, pos[0], pos[2]);
            reroute_from_here(i, soldier, "off the stairs");
            return 0;
        }
    } else g_stairLevel[i] = 0;
    // not getting anywhere on it (blocked): hand back to the normal stuck handling for a while
    if (dist2(pos, g_stairMovedPos[i]) > 0.5f * 0.5f) { memcpy(g_stairMovedPos[i], pos, 12); g_stairMovedAt[i] = H->frame; }
    else if (H->frame - g_stairMovedAt[i] > 3 * 30 && fabsf(err) < 0.12f) {
        g_stairOn[i] = 0; g_stairCheckAt[i] = H->frame + 6 * 30;
        bot_log("stairs: %s is blocked on the flight at (%.1f, %.1f) - normal navigation", name, pos[0], pos[2]);
        return 0;
    }
    g_progress[i].frame = H->frame; g_progress[i].x = pos[0]; g_progress[i].z = pos[2];   // (no stuck alarm while turning onto it)
    return 1;
}

static void steer_bot(int i, char *soldier)
{
    BotNav *nav = bot_nav(i);
    if (nav->mode == 0 || !soldier) return;
    if (H->frame < g_fenceUntil[i]) {   // knifing a fence
        float *fp = (float *)(soldier + OFF_SOLDIER_POSITION);
        H->botMove[i][0] = 0; H->botMove[i][2] = 0;
        H->botMove[i][1] = H->frame - g_fenceAt[i] < 20 ? g_fenceStrafe[i] : 0;   // the sidestep of a second try
        g_progress[i].frame = H->frame; g_progress[i].x = fp[0]; g_progress[i].z = fp[2]; g_progress[i].directTries = 0;
        return;
    }
    if (climb_ladder(i, soldier, nav)) return;
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    float throttle = 0;
    Progress *pg = &g_progress[i];
    if (nav->mode == 4) {   // push: keep walking into the point (e.g. against a vehicle being repaired), no stuck logic
        nav->targetYaw = yaw_towards(nav->x - pos[0], nav->z - pos[2]);
        float y = *(float *)(soldier + OFF_SOLDIER_YAW), t = fmodf(nav->targetYaw, 6.2831853f);
        if (t > 0) t -= 6.2831853f;
        float up = t - y, down = up > 0 ? y - t + 6.2831853f : y - t, err = (up > 0 && up < down) ? up : -down;
        H->botMove[i][0] = fabsf(err) < 0.6f ? 1.0f : 0.0f;
        H->botMove[i][1] = 0;
        H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -err * 2.0f));
        pg->frame = H->frame;
        pg->x = pos[0]; pg->z = pos[2];
        return;
    }
    if (H->frame < g_vaultUntil[i]) {   // jumped: keep walking into it so the vault completes
        H->botMove[i][0] = 1;
        H->botMove[i][1] = 0;
        H->botMove[i][2] = 0;
        if (pg->unstickUntil > H->frame) pg->unstickUntil = g_vaultUntil[i] + 30;   // back off afterwards, if still needed
        return;
    }
    if (g_vaultUntil[i] && H->frame >= g_vaultUntil[i]) {   // vault over: if it got us across, no backing off
        float vx = pos[0] - g_vaultFrom[i][0], vz = pos[2] - g_vaultFrom[i][2];
        g_vaultUntil[i] = 0;
        if (vx * vx + vz * vz > 1.0f) {
            pg->unstickUntil = 0; pg->frame = H->frame; pg->x = pos[0]; pg->z = pos[2]; pg->directTries = 0;
            reroute_from_here(i, soldier, "after a vault");
        }
    }
    if (H->frame < pg->unstickUntil) {   // escape manoeuvre in progress
        H->botMove[i][0] = -1;
        H->botMove[i][1] = pg->strafe;
        H->botMove[i][2] = 0;
        return;
    }
    if (H->frame >= pg->directUntil && stair_tick(i, soldier, nav)) return;
    // Direct-walk fallback: face the final destination and walk straight at it, ignoring the grid.
    // Lets a human "pull" a bot out of a stuck spot (or into a building) by walking where it should go.
    if (H->frame < pg->directUntil) {
        {   // still not getting anywhere while walking straight: jump every 1.5 s
            static float jx[MAX_BOTS], jz[MAX_BOTS];
            static unsigned jat[MAX_BOTS];
            if (H->frame - jat[i] >= 45) {
                float mx = pos[0] - jx[i], mz = pos[2] - jz[i];
                if (jat[i] && mx * mx + mz * mz < 0.3f * 0.3f) stuck_jump(i, soldier, "still blocked");
                jx[i] = pos[0]; jz[i] = pos[2]; jat[i] = H->frame;
            }
        }
        float gx = nav->x, gz = nav->z;
        if (nav->mode == 3) path_goal(i, &gx, &gz);
        float dx = gx - pos[0], dz = gz - pos[2];
        nav->targetYaw = yaw_towards(dx, dz);
        float y = *(float *)(soldier + OFF_SOLDIER_YAW), t = fmodf(nav->targetYaw, 6.2831853f);
        if (t > 0) t -= 6.2831853f;
        float up = t - y, down = up > 0 ? y - t + 6.2831853f : y - t, err = (up > 0 && up < down) ? up : -down;
        H->botMove[i][0] = fabsf(err) < 0.8f ? 1.0f : 0.0f;
        H->botMove[i][1] = 0;
        H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -err * 2.0f));
        return;
    }
    if (nav->mode == 3) {
        follow_path(i, soldier, nav);
        if (nav->mode == 3) {
            nav->targetYaw = yaw_towards(nav->x - pos[0], nav->z - pos[2]);
        }
    } else if (nav->mode == 2) {
        float dx = nav->x - pos[0], dz = nav->z - pos[2];
        float dist = sqrtf(dx * dx + dz * dz);
        if (dist < GOTO_ARRIVE_DIST) {
            nav->mode = 1;   // arrived: keep current heading, stop
            nav->targetYaw = *(float *)(soldier + OFF_SOLDIER_YAW);
            if (H->traceInput) bot_log("goto: %s arrived (%.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, pos[0], pos[2]);
        } else {
            nav->targetYaw = yaw_towards(dx, dz);
        }
    }
    // The engine keeps heading in (-2pi, 0]: positive EIAYaw lowers it and wraps below -2pi, negative raises it but
    // stops at 0. So put the target in the same range and only turn "up" when that does not need to cross 0.
    float yaw = *(float *)(soldier + OFF_SOLDIER_YAW);
    float t = fmodf(nav->targetYaw, 6.2831853f);
    if (t > 0) t -= 6.2831853f;
    float up = t - yaw;                                  // > 0: reachable by raising without crossing 0
    float down = up > 0 ? yaw - t + 6.2831853f : yaw - t;  // lowering always works (wraps)
    float err = (up > 0 && up < down) ? up : -down;      // signed: + raise, - lower
    float yawLevel = -err * 2.0f;   // positive input lowers yaw
    if (yawLevel > 1) yawLevel = 1;
    if (yawLevel < -1) yawLevel = -1;
    if ((nav->mode == 2 || nav->mode == 3) && fabsf(err) < 0.6f) throttle = 1;   // only walk once roughly facing the target
    H->botMove[i][0] = throttle;
    H->botMove[i][1] = 0;
    H->botMove[i][2] = yawLevel;
    float mx = pos[0] - pg->x, mz = pos[2] - pg->z, moved2 = mx * mx + mz * mz;
    int fenceAct = 0;
    if ((throttle <= 0 && nav->mode != 2 && nav->mode != 3) || !pg->frame || moved2 > 1.0f) {   // (turning on the spot = no progress)
        if (moved2 > 1.0f) pg->directTries = 0;
        pg->x = pos[0]; pg->z = pos[2]; pg->frame = H->frame;
    } else if (moved2 < 0.09f && H->frame - pg->frame > 30 && pg->directTries == 0 && (fenceAct = try_cut_fence(i, soldier))) {
        // a catalogued fence in the way: knifed (steer_bot holds still meanwhile), or one no knife gets through - go round
        if (fenceAct == 2) {
            pg->frame = H->frame;
            pg->unstickUntil = H->frame + 30;
            pg->strafe = (rand() & 1) ? 1.0f : -1.0f;
            on_bot_stuck(i, soldier);
        }
    } else if (moved2 < 0.09f && H->frame - pg->frame > 30 && pg->directTries == 0) {
        // not moving at all for 1 s: walk straight at the destination for 3 s
        pg->directTries = 1;
        pg->directUntil = H->frame + 90;
        pg->frame = H->frame;
        bot_log("path: %s not moving, walking straight at its destination", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
        stuck_jump(i, soldier, "blocked");
    } else if (H->frame - pg->frame > STUCK_SECONDS * 30) {
        // still no progress: back off, sidestep, block those links and re-plan
        pg->frame = H->frame;
        pg->directTries = 0;
        pg->unstickUntil = H->frame + 30;
        pg->strafe = (rand() & 1) ? 1.0f : -1.0f;
        stuck_jump(i, soldier, "last try before backing off");
        on_bot_stuck(i, soldier);
    }
    if (H->traceInput && H->frame % 15 == 0)
        bot_log("  steer %s mode=%d target=%.1f yaw=%.1f err=%.1f yawIn=%.2f thr=%.0f pos=(%.1f,%.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                nav->mode, nav->targetYaw * 57.2958f, *(float *)(soldier + OFF_SOLDIER_YAW) * 57.2958f, err * 57.2958f, yawLevel, throttle, pos[0], pos[2]);
}

// ---- Physics raycast ----
// bool GameWorld::rayCast(const char* name, int one, Vec4* from, Vec4* to, RayCastHit* hit, u32 flags, void* ignore), ret 0x1c
#define ADDR_GAME_WORLD            0x02b3ee30  // ServerGameWorld* global
#define ADDR_GAMEWORLD_RAYCAST     0x0085c130
// Flag bits measured on this build (they differ from later builds / Venice Unleashed): 0x08 skips terrain, 0x20 skips characters.
#define RAY_DONT_CHECK_TERRAIN     0x08
#define RAY_DONT_CHECK_CHARACTER   0x20
#define RAY_CHECK_DETAIL_MESH      0x01   // vegetation (palm trunks, trees) is detail mesh: invisible without this
#define RAY_FLAGS_NAV              RAY_DONT_CHECK_CHARACTER
typedef char (__attribute__((thiscall)) *RayCast_t)(void *world, const char *name, int one, const float *from, const float *to,
                                                    void *hit, unsigned flags, void *ignore);
typedef struct RayHit { float pos[4]; float normal[4]; unsigned char rest[0x80]; } RayHit;

// Returns 1 on hit; hitPos/hitNormal (may be NULL) receive xyz.
static unsigned g_rayFlags = RAY_FLAGS_NAV;
// The physics profiler keeps the raycast name pointer: it must outlive this DLL (a string literal here dangles after a
// hot reload and crashes the profiler). One heap copy per process, shared by every core generation (host->reserved+328).
static const char *ray_name(void)
{
    char **slot = (char **)(H->reserved + 328);
    if (!*slot) {
        *slot = HeapAlloc(GetProcessHeap(), 0, 8);
        if (!*slot) return "BotMod";
        memcpy(*slot, "BotMod", 7);
    }
    return *slot;
}

static int ray_cast(const float *from3, const float *to3, float *hitPos, float *hitNormal)
{
    void *world = *(void **)ADDR_GAME_WORLD;
    if (!world) return 0;
    float from[4] __attribute__((aligned(16))) = {from3[0], from3[1], from3[2], 0};
    float to[4] __attribute__((aligned(16))) = {to3[0], to3[1], to3[2], 0};
    RayHit hit __attribute__((aligned(16)));
    memset(&hit, 0, sizeof hit);
    *(int *)(hit.rest + 0x08) = -1;   // hit+0x28 / +0x2c initialised to -1 by the engine's own callers
    *(int *)(hit.rest + 0x0c) = -1;
    char r = ((RayCast_t)ADDR_GAMEWORLD_RAYCAST)(world, ray_name(), 1, from, to, &hit, g_rayFlags, NULL);
    if (r) {
        if (hitPos) memcpy(hitPos, hit.pos, 12);
        if (hitNormal) memcpy(hitNormal, hit.normal, 12);
        memcpy(g_lastHitRest, hit.rest, sizeof g_lastHitRest);
    }
    return r != 0;
}

static int ray_cast_no_terrain(const float *from3, const float *to3, float *hitPos, float *hitNormal)
{
    unsigned saved = g_rayFlags;
    g_rayFlags |= 0x08;   // skip terrain
    int r = ray_cast(from3, to3, hitPos, hitNormal);
    g_rayFlags = saved;
    return r;
}

// ---- Navigation grid + path following ----
#define NAV_RAYS_PER_FRAME   4000
#define NAV_MS_PER_FRAME     6.0
#define PATH_MAX_POINTS      128
#define WAYPOINT_REACHED     1.5f    // advance early only if the straight line from here to the next waypoint is clear
#define WAYPOINT_EXACT       0.5f
static NavGrid **nav_slot(void) { return (NavGrid **)(H->reserved + 24); }   // survives reloads
static int *nav_building(void) { return (int *)(H->reserved + 28); }

typedef struct BotPath { int count, next; short link[PATH_MAX_POINTS]; unsigned climbAt; unsigned char climbPhase; float pts[PATH_MAX_POINTS * 3]; float goalX, goalY, goalZ; float lastX, lastZ; unsigned lastProgressFrame; } BotPath;
static BotPath g_paths[MAX_BOTS];   // lost on reload (bots just stop following)
static void path_goal(int i, float *x, float *z) { *x = g_paths[i].goalX; *z = g_paths[i].goalZ; }
static int plan_path(int i, char *soldier, float tx, float ty, float tz);
// After a vault, a flight of stairs / an escalator or a ladder the bot is somewhere the old route did not expect (the
// vault landing past the waypoint, the top of an escalator beside the rails): plan again from where it really is to the
// same goal - the new first leg goes straight to the way on (the old one turned it into the nearest wall, or back over
// what it just vaulted).
static void reroute_from_here(int i, char *soldier, const char *why)
{
    BotNav *nav = bot_nav(i);
    BotPath *p = &g_paths[i];
    if (nav->mode != 3 || !soldier || (p->goalX == 0 && p->goalZ == 0)) return;
    float gx = p->goalX, gy = p->goalY, gz = p->goalZ;
    if (plan_path(i, soldier, gx, gy, gz)) {
        nav->mode = 3;
        bot_log("path: %s re-routes from where it is now (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    }
}

static void nav_file(char *out, int n, const char *level)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(out, n, "%s\\BotMod_nav_%s.bin", dir, level);
}

// Learned link removals (nav_repair_around) are written back into the map's nav file, so every server benefits.
static int g_navDirty;
static void nav_save_if_dirty(void)
{
    NavGrid *g = *nav_slot();
    if (!g_navDirty || !g || g->phase != 2) return;
    char path[MAX_PATH + 64];
    nav_file(path, sizeof path, g->level);
    bot_log("nav: saving learned repairs to %s (%s)", path, nav_save(g, path) ? "ok" : "FAILED");
    g_navDirty = 0;
}

static void ladders_link(void);
static int g_navDirty;
// After a grid is loaded: the strict staircase re-check, a few thousand rays per frame; saved with the map if it changed.
static void nav_stairfix_tick(void)
{
    static NavGrid *done;
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2 || g == done || *nav_building()) return;
    int removed = 0;
    if (nav_fix_stairs_step(g, ray_cast, 1500, &removed)) {
        done = g;
        int restored = nav_stairs_restored(), rails = nav_rails_removed(), added = nav_stairs_added();
        bot_log("nav: stair re-check on %s done - %d false staircase link(s) (wall faces) removed, %d drop(s) (escalator tops ...) restored, "
                "%d staircase(s) up added, %d rail / wall top(s) no longer walkable", g->level, removed, restored, added, rails);
        if (removed || restored || rails || added) g_navDirty = 1;
    }
}
static void nav_tick(void)
{
    nav_stairfix_tick();
    NavGrid *g = *nav_slot();
    if (!g || !*nav_building()) return;
    int wasPhase = g->phase;
    nav_set_terrain_test(ray_cast_no_terrain);
    nav_set_hit_classifier(nav_hit_class);
    if (nav_build_step(g, ray_cast, NAV_RAYS_PER_FRAME, NAV_MS_PER_FRAME)) {
        *nav_building() = 0;
        int walk = 0, edges = 0, water = 0, fences = 0, doors = 0;
        for (int i = 0; i < g->w * g->h * NAV_LAYERS; i++) {
            walk += g->flags[i] & NAV_WALKABLE;
            water += (g->flags[i] & NAV_WATER) != 0;
            for (int d = 0; d < 8; d++) { edges += (g->edges[i] >> d) & 1; if (d & 1) doors += (g->fences[i] >> d) & 1; else fences += (g->fences[i] >> d) & 1; }
        }
        bot_log("navbuild: %d water node(s), %d knifeable fence crossing(s), %d doorway(s)", water, fences / 2, doors / 2);
        char path[MAX_PATH + 64];
        nav_file(path, sizeof path, g->level);
        bot_log("navbuild: done %s %dx%d cells, %d walkable, %d edges, %u rays in %.0f ms -> %s (%s)", g->level, g->w, g->h, walk,
                edges / 2, g->raysCast, g->buildMs, path, nav_save(g, path) ? "saved" : "SAVE FAILED");
        ladders_link();
    } else if (g->phase != wasPhase || H->frame % 300 == 0) {
        int n = g->w * g->h;
        bot_log("navbuild: phase %d %.0f%% (%u rays, %.0f ms)", g->phase, 100.0 * g->cursor / (g->phase ? n * 4.0 : n), g->raysCast, g->buildMs);
    }
}

// ty = height of the goal (picks the right storey / pier deck vs. the water or seabed under it)
static int plan_path_real(int i, char *soldier, float tx, float ty, float tz);
static double perf_now(void);
static void perf_add(int k, double ms);
static unsigned g_pathPlans;
// Route plans per tick for the optional planners (getting to a vehicle, cover / firing spots): each may try many
// targets, and on a slow CPU a burst of them stalled the server (MP_017: 4,100-18,600 plans per 30 s on the AM1).
// They check plan_budget_left() first and simply try again later. Walking to flags is not limited.
#define PLAN_BUDGET_PER_TICK 3
static unsigned g_planFrame;
static int g_plansThisFrame;
static unsigned g_planBudgetHits;
static double g_planMsThisFrame;
#define PLAN_MS_PER_TICK 8.0    // route-planning time per tick before the rest waits (30 ms let Damavand's 860 plans / 30 s take the
                                // whole tick: 14 ticks/s and unplayable lag, 2026-10-13; earlier rubber banding: 400 ms thinks)
// A rolling budget: each tick earns PLAN_MS_AVG ms of planning (up to a small reserve); a plan spends what it really took,
// so one 40 ms A* on Damavand is paid off over the next ticks instead of costing every tick (25 ticks/s with a per-tick cap)
#define PLAN_MS_AVG 6.0
static double g_planCredit = 8.0;
static unsigned g_planCreditFrame;
static int plan_budget_left(void)
{
    if (g_planFrame != H->frame) { g_planFrame = H->frame; g_plansThisFrame = 0; g_planMsThisFrame = 0; }
    if (g_planCreditFrame != H->frame) {
        g_planCredit += PLAN_MS_AVG * (double)(H->frame - g_planCreditFrame > 30 ? 30 : H->frame - g_planCreditFrame);
        if (g_planCredit > 16.0) g_planCredit = 16.0;
        g_planCreditFrame = H->frame;
    }
    if (g_plansThisFrame < PLAN_BUDGET_PER_TICK && g_planMsThisFrame < PLAN_MS_PER_TICK && g_planCredit > 0) return 1;
    g_planBudgetHits++;
    return 0;
}
static int g_planRefused;           // the last plan_path was turned away by the budget (not "no route")
static unsigned g_straightAt[MAX_BOTS];   // go_towards found no route and walks straight (frame)
static int plan_path(int i, char *soldier, float tx, float ty, float tz)
{
    g_planRefused = 0;
    if (!plan_budget_left()) { g_planRefused = 1; return 0; }   // this tick's planning time is spent: "no route yet" - the caller tries again later (most
                                         // callers never asked: Damavand's routes took the whole tick, 14 ticks/s)
    g_plansThisFrame++;
    double a = perf_now();
    int r = plan_path_real(i, soldier, tx, ty, tz);
    g_planMsThisFrame += perf_now() - a;
    g_planCredit -= perf_now() - a;
    perf_add(7, perf_now() - a);   // PF_PATH
    g_pathPlans++;
    return r;
}
static int plan_path_real(int i, char *soldier, float tx, float ty, float tz)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) {
        static unsigned warnedAt;
        if (H->frame - warnedAt > 300) { warnedAt = H->frame; bot_log("path: navigation grid not ready yet (building / navload)"); }
        return 0;
    }
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    BotPath *p = &g_paths[i];
    p->count = nav_find_path(g, ray_cast, pos[0], pos[1], pos[2], tx, ty, tz, p->pts, PATH_MAX_POINTS);
    for (int k = 0; k < p->count; k++) p->link[k] = (short)nav_path_link(k);
    p->climbAt = 0;
    p->climbPhase = 0;
    p->next = 0;
    p->goalX = tx;
    p->goalY = ty;
    p->goalZ = tz;
    p->lastX = pos[0];
    p->lastZ = pos[2];
    p->lastProgressFrame = H->frame;
    return p->count;
}

// Mode 3: follow the planned waypoints (sets nav->x/z to the current waypoint, then behaves like goto).
static unsigned g_wpNearAt[MAX_BOTS];
static int g_wpNearIdx[MAX_BOTS];
static void follow_path(int i, char *soldier, BotNav *nav)
{
    BotPath *p = &g_paths[i];
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    if (p->next < p->count && p->link[p->next] < 0) {
        float dx = p->pts[p->next * 3] - pos[0], dz = p->pts[p->next * 3 + 2] - pos[2];
        if (dx * dx + dz * dz < 1.0f) {
            if (g_wpNearIdx[i] != p->next || !g_wpNearAt[i]) { g_wpNearIdx[i] = p->next; g_wpNearAt[i] = H->frame ? H->frame : 1; }
            else if (H->frame - g_wpNearAt[i] > 30 && p->next + 1 < p->count) {
                p->next++;
                g_wpNearAt[i] = 0;
                if (H->traceInput) bot_log("path: %s hovered at a waypoint - moving on", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
            }
        } else g_wpNearAt[i] = 0;
    }
    while (p->next < p->count) {
        if (p->link[p->next] >= 0) break;   // a ladder top: climb_ladder() takes it from here
        float dx = p->pts[p->next * 3] - pos[0], dz = p->pts[p->next * 3 + 2] - pos[2], d2 = dx * dx + dz * dz;
        if (d2 > WAYPOINT_REACHED * WAYPOINT_REACHED) break;
        if (p->next + 1 < p->count && p->link[p->next + 1] >= 0 && d2 > 0.4f * 0.4f) break;   // the ladder foot: exactly
        int last = p->next + 1 >= p->count;
        if (d2 > WAYPOINT_EXACT * WAYPOINT_EXACT && !last) {
            float feet[3] = {pos[0], pos[1], pos[2]};
            if (!nav_segment_clear(ray_cast, feet, &p->pts[(p->next + 1) * 3])) break;   // keep walking to the corner
        }
        p->next++;
    }
    if (p->next >= p->count) {
        nav->mode = 1;
        nav->targetYaw = *(float *)(soldier + OFF_SOLDIER_YAW);
        bot_log("path: %s arrived (%.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, pos[0], pos[2]);
        return;
    }
    nav->x = p->pts[p->next * 3];
    nav->z = p->pts[p->next * 3 + 2];
}

// ---- Ladders (up only - nobody climbs down in BF3: bots walk off the edge / pull the chute) ----
// The engine does the climbing: a soldier on the ladder's centre line, facing it and walking forward goes up and steps
// off at the top. Path links (nav_add_link) end at the ladder top; the bot lines up 0.9 m out, faces the ladder, holds
// forward while strafing onto the centre line, and carries on once it is up.
#define LADDER_STAND_OUT       0.9f
#define LADDER_CLIMB_FRAMES    (12 * 30)
typedef struct LadderLink { int ladder; float n[3], groundY; } LadderLink;   // groundY: the floor the climb starts from   // n: horizontal, from the ladder out to the climbing side
static LadderLink g_ladderLink[256];
static float wrap_err(float targetYaw, float yaw)
{
    float t = fmodf(targetYaw, 6.2831853f);
    if (t > 0) t -= 6.2831853f;
    float up = t - yaw, down = up > 0 ? yaw - t + 6.2831853f : yaw - t;
    return (up > 0 && up < down) ? up : -down;
}
static const float *ladder_foot(int k);
static float ladder_top_y(int k);
// Returns 1 while it has the controls.
static int climb_ladder(int i, char *soldier, BotNav *nav)
{
    BotPath *p = &g_paths[i];
    if (nav->mode != 3 || p->next >= p->count || p->link[p->next] < 0) return 0;
    int id = p->link[p->next];
    LadderLink *ll = &g_ladderLink[id];
    const float *foot = ladder_foot(ll->ladder);
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION), yaw = *(float *)(soldier + OFF_SOLDIER_YAW);
    float topY = ladder_top_y(ll->ladder);
    if (pos[1] > topY - 0.6f) {   // up: walk on to the exit and beyond
        bot_log("ladder: %s is up (%.1f m climbed)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, pos[1] - foot[1]);
        p->link[p->next] = -1;
        p->climbAt = 0;
        p->climbPhase = 0;
        reroute_from_here(i, soldier, "up the ladder");
        return 0;
    }
    if (!p->climbAt) p->climbAt = H->frame;
    if (H->frame - p->climbAt > LADDER_CLIMB_FRAMES) {   // could not get on / up: forget this ladder for the round
        bot_log("ladder: %s could not climb the ladder at (%.1f, %.1f) - dropping it", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, foot[0], foot[2]);
        nav_disable_link(id);
        p->climbAt = 0;
        p->climbPhase = 0;
        if (!plan_path(i, soldier, p->goalX, p->goalY, p->goalZ)) nav->mode = 1;
        return 0;
    }
    float stand[3] = {foot[0] + ll->n[0] * LADDER_STAND_OUT, foot[1], foot[2] + ll->n[2] * LADDER_STAND_OUT};
    float dx = stand[0] - pos[0], dz = stand[2] - pos[2], d = sqrtf(dx * dx + dz * dz);
    float faceYaw = yaw_towards(-ll->n[0], -ll->n[2]);
    // sideways error from the centre line, along the bot's left when facing the ladder (+strafe moves left)
    float fx = -ll->n[0], fz = -ll->n[2], lx = -fz, lz = fx;
    float side = (pos[0] - foot[0]) * lx + (pos[2] - foot[2]) * lz;
    int onLadder = pos[1] > ll->groundY + 0.4f;
    Progress *pg = &g_progress[i];
    pg->frame = H->frame; pg->x = pos[0]; pg->z = pos[2]; pg->directTries = 0;   // no stuck handling meanwhile
    if (!onLadder && !p->climbPhase && d > 2.5f && p->next > 0) {   // not at the foot yet: walk the path to it first
        p->next--;
        p->climbAt = 0;
        return 0;
    }
    if (!onLadder && !p->climbPhase && d < 0.3f) p->climbPhase = 1;
    if (!onLadder && !p->climbPhase) {   // walking up to the stand point
        nav->targetYaw = yaw_towards(dx, dz);
        float err = wrap_err(nav->targetYaw, yaw);
        H->botMove[i][0] = fabsf(err) < 0.5f ? (d < 1.5f ? 0.4f : 1.0f) : 0.0f;
        H->botMove[i][1] = 0;
        H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -err * 2.0f));
        return 1;
    }
    nav->targetYaw = faceYaw;
    float err = wrap_err(faceYaw, yaw);
    H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -err * 2.0f));
    if (onLadder) { H->botMove[i][0] = 1.0f; H->botMove[i][1] = 0; return 1; }
    float strafe = fmaxf(-0.6f, fminf(0.6f, -side * 2.5f));
    H->botMove[i][1] = fabsf(side) > 0.08f ? strafe : 0.0f;
    H->botMove[i][0] = fabsf(err) < 0.25f && fabsf(side) < 0.25f ? 1.0f : 0.0f;
    return 1;
}

// ---- Conquest flags ----
#define ADDR_CAPTURE_POINT_VTABLE  0x02696660  // ServerCapturePointEntity
#define OFF_CP_CONTROL_TEAM        0xb0        // team currently inside / controlling the capture area (0 = none)
#define OFF_CP_OWNER_TEAM          0xb4        // owning team (0 = neutral)
#define OFF_CP_OWNER_OBJECT        0x10        // shared with the spawn entities of the same flag/HQ blueprint
#define OFF_CP_DATA                0x14        // CapturePointEntityData*
#define OFF_CPDATA_TAKEABLE        0xb8        // byte: 1 on conquest flags, 0 on HQ bases
#define OFF_CP_POSITION            0x120       // Vec3
#define MAX_FLAGS                  16
static char *g_flags[MAX_FLAGS];
static int g_flagCount;
static void *g_flagSpawn[MAX_FLAGS][3];   // each flag has one CharacterSpawnEntity per team (data +0x08 byte = team)
static float g_hqPos[3][3];

// Scans committed memory for objects with the given vtable at +0 (16-byte aligned). Regions are copied with
// ReadProcessMemory so guard pages / concurrently freed memory cannot fault the server.
// align: objects are 8-byte aligned except a few components (4); a finer step also finds more false matches, and
// the callers cap the result count, so use 4 only where needed.
static int scan_objects_aligned(unsigned vtable, char **out, int max, int align);
static int scan_objects(unsigned vtable, char **out, int max) { return scan_objects_aligned(vtable, out, max, 8); }
static int scan_objects_aligned(unsigned vtable, char **out, int max, int align)
{
    enum { CHUNK = 1 << 16 };
    unsigned buf[CHUNK / 4];   // per call: scans also run on worker threads
    int count = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned char *a = (unsigned char *)0x10000; (unsigned)a < 0xffff0000u && count < max && !g_abortScans;
         a = (unsigned char *)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || !mbi.RegionSize) break;
        DWORD prot = mbi.Protect & 0xff;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE)) ||
            (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE)) continue;
        for (SIZE_T off = 0; off < mbi.RegionSize && count < max; off += CHUNK) {
            SIZE_T n = mbi.RegionSize - off < CHUNK ? mbi.RegionSize - off : CHUNK, got = 0;
            char *base = (char *)mbi.BaseAddress + off;
            if (base + n > (char *)buf && base < (char *)buf + sizeof buf) continue;   // (our own read buffer: copies, not objects)
            if (!ReadProcessMemory(GetCurrentProcess(), base, buf, n, &got)) continue;
            for (SIZE_T k = 0; k + 4 <= got && count < max; k += (SIZE_T)align)
                if (buf[k / 4] == vtable) out[count++] = base + k;
        }
    }
    return count;
}

// One pass over memory for several vtables at once (each with its own alignment): the scan worker used to walk all of
// memory once per type.
typedef struct ScanType { unsigned vtable; int align; char **out; int max; int count; } ScanType;
static void scan_objects_multi(ScanType *t, int nt)
{
    enum { CHUNK = 1 << 16 };
    static unsigned buf[CHUNK / 4];   // only the scan worker calls this
    for (int k = 0; k < nt; k++) t[k].count = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned char *a = (unsigned char *)0x10000; (unsigned)a < 0xffff0000u && !g_abortScans;
         a = (unsigned char *)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi) || !mbi.RegionSize) break;
        DWORD prot = mbi.Protect & 0xff;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE)) ||
            (prot != PAGE_READWRITE && prot != PAGE_EXECUTE_READWRITE)) continue;
        for (SIZE_T off = 0; off < mbi.RegionSize; off += CHUNK) {
            SIZE_T n = mbi.RegionSize - off < CHUNK ? mbi.RegionSize - off : CHUNK, got = 0;
            char *base = (char *)mbi.BaseAddress + off;
            if (base + n > (char *)buf && base < (char *)buf + sizeof buf) continue;   // (our own read buffer)
            if (!ReadProcessMemory(GetCurrentProcess(), base, buf, n, &got)) continue;
            for (SIZE_T w = 0; w + 1 <= got / 4; w++) {
                unsigned v = buf[w];
                if (v < 0x02400000u || v > 0x02700000u) continue;   // every type we look for has its vtable in this range
                for (int k = 0; k < nt; k++)
                    if (v == t[k].vtable && t[k].count < t[k].max && ((w * 4) % t[k].align) == 0) t[k].out[t[k].count++] = base + w * 4;
            }
        }
    }
}

// Takeable flags = capture points whose data marks them takeable (HQ bases are not), ignoring stale objects at the origin.
static void find_flags(void)
{
    char *cps[MAX_FLAGS * 2];
    int ncp = scan_objects(ADDR_CAPTURE_POINT_VTABLE, cps, MAX_FLAGS * 2);
    g_flagCount = 0;
    for (int i = 0; i < ncp && g_flagCount < MAX_FLAGS; i++) {
        char *data = *(char **)(cps[i] + OFF_CP_DATA);
        float *pos = (float *)(cps[i] + OFF_CP_POSITION);
        if (!data || IsBadReadPtr(data + OFF_CPDATA_TAKEABLE, 1) || !data[OFF_CPDATA_TAKEABLE]) continue;
        if (pos[0] == 0 && pos[1] == 0 && pos[2] == 0) continue;
        g_flags[g_flagCount++] = cps[i];
    }
    char *spawns[128];
    int nsp = scan_objects(0x025bbbc0, spawns, 128), linked = 0;
    memset(g_flagSpawn, 0, sizeof g_flagSpawn);
    for (int f = 0; f < g_flagCount; f++)
        for (int s = 0; s < nsp; s++) {
            if (*(void **)(spawns[s] + OFF_CP_OWNER_OBJECT) != *(void **)(g_flags[f] + OFF_CP_OWNER_OBJECT)) continue;
            unsigned char *data = *(unsigned char **)(spawns[s] + 0x14);
            if (!data || IsBadReadPtr(data + 8, 1) || (data[8] != 1 && data[8] != 2)) continue;
            g_flagSpawn[f][data[8]] = spawns[s];
            linked++;
        }
    bot_log("flags: %d capture point(s), %d takeable flag(s), %d flag spawn(s)", ncp, g_flagCount, linked);
}
static int flag_owner(int f) { return *(int *)(g_flags[f] + OFF_CP_OWNER_TEAM); }
static float *flag_pos(int f) { return (float *)(g_flags[f] + OFF_CP_POSITION); }

// ---- Bot brain: respawn at HQ, capture the nearest flag the team does not own, repeat ----
#define RESPAWN_DELAY_FRAMES  (5 * 30)
#define FLAG_SPREAD           4.0f      // bots aim for a random spot this close to the flag centre
enum { TASK_NONE, TASK_FOLLOW, TASK_DEFEND, TASK_SUPPLY, TASK_REPAIR, TASK_REVIVE, TASK_COVER, TASK_ROCKET, TASK_FIGHT };
static const char *kTaskNames[] = {"objectives", "follow", "defend", "supply", "repair", "revive", "cover", "rocket", "fight"};
static void bot_say_later(int i, int type, unsigned delayFrames);
// ---- Downed teammates: a soldier that disappears from its player is a body that can be revived for ~10 s ----
#define DOWNED_WINDOW_FRAMES   (10 * 30)
#define REVIVE_RANGE           35.0f     // ~10 s walk
#define REVIVE_REACH           1.3f      // stand this close to the body
#define REVIVE_PITCH           -1.2f     // look down at the body (~70 degrees)
#define BOT_REVIVE_WAIT_FRAMES (10 * 30) // a downed bot with a medic coming waits this long before redeploying
typedef struct Downed { char *player; float pos[3]; unsigned at; int team; int medic; char *body; } Downed;   // medic: bot index + 1; body: the dead soldier
static Downed g_downed[64];
static char *g_downLastSoldier[64];
static char *g_downLastPlayer[64];
static float g_downLastPos[64][3];
static int g_downLastInVehicle[64];
static char *g_downLastBody[64];

// Where a dead soldier's ragdoll lies: its physics keeps moving after death while the soldier transform freezes.
// [body+0x30]+0xc0 and [body+0x4d4]+0x90 both follow the ragdoll; the frozen transform is the last resort.
static int ragdoll_pos(char *body, float *out)
{
    float *tr = (float *)(body + OFF_SOLDIER_SPAWN_POSITION);
    static const int kRagdoll[2][2] = {{0x30, 0xc0}, {0x4d4, 0x90}};
    for (int k = 0; k < 2; k++) {
        char *q = *(char **)(body + kRagdoll[k][0]);
        if (!q || IsBadReadPtr(q + kRagdoll[k][1], 12)) continue;
        float *v = (float *)(q + kRagdoll[k][1]);
        float dx = v[0] - tr[0], dy = v[1] - tr[1], dz = v[2] - tr[2];
        if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f || v[0] != v[0]) continue;   // not a position near the body
        memcpy(out, v, 12);
        return 1;
    }
    memcpy(out, tr, 12);
    return 0;
}

// Follow each fresh body while its soldier object exists.
static void downed_follow_bodies(void)
{
    for (int d = 0; d < 64; d++) {
        Downed *e = &g_downed[d];
        if (!e->player || !e->body || H->frame - e->at > DOWNED_WINDOW_FRAMES) continue;
        if (IsBadReadPtr(e->body, 0x4e0) || *(unsigned *)e->body != ADDR_SOLDIER_VTABLE) { e->body = NULL; continue; }
        ragdoll_pos(e->body, e->pos);
    }
}

static unsigned g_standUpAt[MAX_BOTS];   // frame to press prone (toggle) once after a revive
static char *g_lastSpawnedBot[MAX_BOTS]; static unsigned g_lastSpawnedAt[MAX_BOTS];   // our own (re)spawns, by player

// A soldier that came back for `pl` was a revive (not a redeploy): bots - we did not spawn it; humans - near the body.
static int came_back_by_revive(char *pl, char *s, const float *body)
{
    if (is_bot(pl)) {
        for (int k = 0; k < MAX_BOTS; k++) if (g_lastSpawnedBot[k] == pl && H->frame - g_lastSpawnedAt[k] < 60) return 0;
        return 1;
    }
    float *np = (float *)(s + OFF_SOLDIER_SPAWN_POSITION);
    float dx = np[0] - body[0], dz = np[2] - body[2];
    return dx * dx + dz * dz < 10.0f * 10.0f;
}
static void press_button(int i, int action, int frames);

static int fired_recently(char *pl);
static void downed_tick(void)
{
    static unsigned primaryAt[MAX_BOTS];
    for (int i = 0; i < H->botCount; i++) {
        if (g_standUpAt[i] && H->frame >= g_standUpAt[i]) {   // after a revive: primary out (the crawl check below stands it up if needed)
            g_standUpAt[i] = 0;
            primaryAt[i] = H->frame + 20;
        }
        if (primaryAt[i] && H->frame >= primaryAt[i]) { primaryAt[i] = 0; press_button(i, SLOT_ACTION(0), 3); }   // primary weapon out
    }
    if (H->frame % 3) return;
    char *pm = player_manager();
    if (!pm) return;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count && k < 64; k++) {
        char *pl = arr[k];
        if (pl != g_downLastPlayer[k]) { g_downLastPlayer[k] = pl; g_downLastSoldier[k] = NULL; }
        if (!pl) continue;
        char *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
        if (s) {
            if (!g_downLastSoldier[k])   // just came back: revived at the body (not a redeploy elsewhere)?
                for (int d = 0; d < 64; d++) {
                    Downed *e = &g_downed[d];
                    if (e->player != pl || H->frame - e->at > DOWNED_WINDOW_FRAMES + 60) continue;
                    int bi = bot_index(pl);
                    if (bi >= 0 && came_back_by_revive(pl, s, e->pos)) {
                        g_standUpAt[bi] = H->frame + 30;   // revived soldiers get up prone: stand the bot up
                        bot_say_later(bi, ORDER_THANKYOU, 45);   // thanks to whoever revived it
                        bot_log("revive: %s is back up - standing up", pl + OFF_PLAYER_ONLINE_NAME);
                    }
                }
            memcpy(g_downLastPos[k], s + OFF_SOLDIER_SPAWN_POSITION, 12);
            g_downLastSoldier[k] = s;
            g_downLastInVehicle[k] = *(char **)(pl + OFF_PLAYER_VEHICLE) != NULL;
            // back on their feet: whatever downed record they had is over
            for (int d = 0; d < 64; d++) if (g_downed[d].player == pl) g_downed[d].player = NULL;
            continue;
        }
        if (!g_downLastSoldier[k]) continue;
        g_downLastBody[k] = g_downLastSoldier[k];
        g_downLastSoldier[k] = NULL;
        if (g_downLastInVehicle[k]) continue;   // killed inside a vehicle: nothing to revive
        for (int d = 0; d < 64; d++) {
            Downed *e = &g_downed[d];
            if (e->player && H->frame - e->at < DOWNED_WINDOW_FRAMES) continue;
            e->player = pl;
            memcpy(e->pos, g_downLastPos[k], 12);
            e->at = H->frame;
            e->team = player_team(pl);
            e->medic = 0;
            e->body = g_downLastBody[k];
            bot_log("revive: %s went down at (%.1f, %.1f)", pl + OFF_PLAYER_ONLINE_NAME, e->pos[0], e->pos[2]);
            break;
        }
    }
}

static Downed *downed_of(char *pl)
{
    for (int d = 0; d < 64; d++) if (g_downed[d].player == pl && H->frame - g_downed[d].at < DOWNED_WINDOW_FRAMES) return &g_downed[d];
    return NULL;
}
typedef struct BotBrain {
    int enabled; int targetFlag; unsigned deadSince; int waiting; int enroute; int avoidFlag; unsigned avoidUntil;
    // Human-issued task (Q menu); overrides the flag loop until released with GoGoGo by the commander.
    int task;
    char *commander;         // human who issued the current task (follow / backup)
    char *serviceTarget;     // human being served (ammo / medkit)
    int supplySlot;          // loadout weapon slot of the gadget to drop
    float supplyPos[3];      // where the human stood when calling: the gadget goes there
    unsigned taskUntil;      // frame when the task expires
    unsigned replanAt;       // next frame to refresh the path to the moving human
    int step; unsigned stepAt;           // supply drop sequence
    int prevTask; char *prevCommander;   // what to resume after the supply run
    int sayNext; unsigned sayAt;         // queued radio line (e.g. Sorry after Negative)
    unsigned retryAt;                    // no reachable flag: wait before trying again
    float postPos[3];                    // defend: where to hold
    float postFacing;                    // defend: heading to cover (away from the called spot)
    int crouched;                        // defend: crouch requested
} BotBrain;
#define FLAG_RETRY_OFFSETS    4
#define FLAG_AVOID_FRAMES     (10 * 30)
// host->reserved layout: 0..11 kit slots, 16 kitted mask, 20 aim mode, 24 nav grid, 28 nav building, 32/36/40/48/52/56 brain array ptr (v1..v6), 44 loadout library,
// 64..319 aim, 512..1023 nav state. The brain array lives on the process heap so it survives reloads.
static BotBrain *bot_brain(int i)
{
    BotBrain **slot = (BotBrain **)(H->reserved + 56);   // v6 layout (older pointers at +32..+52 are abandoned)
    if (!*slot) *slot = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(BotBrain) * MAX_BOTS);
    return *slot + i;
}

static int pick_flag(int team, const float *from, int avoid)
{
    int best = -1;
    float bestD = 1e30f;
    for (int f = 0; f < g_flagCount; f++) {
        if (flag_owner(f) == team || f == avoid) continue;
        float dx = flag_pos(f)[0] - from[0], dz = flag_pos(f)[2] - from[2], d = dx * dx + dz * dz;
        if (d < bestD) { bestD = d; best = f; }
    }
    return best;
}

// ---- Squad objectives: every member of a squad goes for the same flag. Flags the team does not own are ranked by
// distance from the squad (its living members, else the team HQ): odd squads take the closest, even squads the
// second closest (or the only one left). Re-chosen when the flag falls to the team or every 30 s.
#define SQUAD_OBJECTIVE_REFRESH  (30 * 30)
static int g_squadTarget[3][33];
static unsigned g_squadTargetAt[3][33];
static int player_squad(char *pl);

// Position of a bot-led squad among its team's bot-led squads (1 = lowest squad id), 0 if the squad is not bot-led.
// Human squads are skipped, so bot squads alternate odd / even no matter which squad numbers players hold.
static int bot_squad_rank(int team, int squad)
{
    char *pm = player_manager();
    if (!pm) return 0;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    unsigned botLed = 0;   // bit per squad id
    for (unsigned k = 0; k < count; k++) {
        char *pl = arr[k];
        if (!pl || player_team(pl) != team || !is_bot(pl) || !pl[0x788]) continue;   // 0x788: squad leader flag
        int s = player_squad(pl);
        if (s >= 1 && s <= 31) botLed |= 1u << s;
    }
    if (!(botLed & (1u << squad))) return 0;
    int rank = 0;
    for (int s = 1; s <= squad; s++) if (botLed & (1u << s)) rank++;
    return rank;
}

// ---- Squad allocation per team (re-run when flag ownership or the bot squads change, at least every 30 s) ----
// Attack: each flag the team does not own gets the 2 closest bot squads. Defend: the rest guard friendly flags, the
// one nearest the enemy HQ first. With no friendly flag left the rest attack too (odd / even over the two closest).
#define SQUAD_ALLOC_REFRESH   (30 * 30)
#define ATTACKERS_PER_FLAG    2
#define PATROL_RADIUS         15.0f
static unsigned char g_squadDefend[3][33];
static unsigned g_allocAt[3], g_allocSig[3];

static unsigned bot_squads_mask(int team)
{
    unsigned mask = 0;
    for (int s = 1; s <= 31; s++) if (bot_squad_rank(team, s)) mask |= 1u << s;
    return mask;
}

static void squad_centre(int team, int squad, float *ref)
{
    int n = 0;
    ref[0] = ref[1] = ref[2] = 0;
    for (int i = 0; i < H->botCount; i++) {
        char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
        if (!s || player_team(pl) != team || player_squad(pl) != squad) continue;
        float *p = (float *)(s + OFF_SOLDIER_POSITION);
        ref[0] += p[0]; ref[2] += p[2]; n++;
    }
    if (n) { ref[0] /= n; ref[2] /= n; } else { ref[0] = g_hqPos[team][0]; ref[2] = g_hqPos[team][2]; }
}

static float dist2(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return dx * dx + dz * dz; }

static void allocate_squads(int team, unsigned mask)
{
    int sq[32], ns = 0;
    float cen[32][3], teamCen[3] = {0, 0, 0};
    for (int s = 1; s <= 31; s++) if (mask & (1u << s)) { squad_centre(team, s, cen[ns]); teamCen[0] += cen[ns][0]; teamCen[2] += cen[ns][2]; sq[ns++] = s; }
    if (!ns) return;
    teamCen[0] /= ns; teamCen[2] /= ns;
    const float *enemyHq = g_hqPos[team == 1 ? 2 : 1];
    int att[16], na = 0, def[16], nd = 0;
    for (int f = 0; f < g_flagCount && f < 16; f++) {   // insertion sorts: attack by distance from the team, defend by distance from the enemy HQ
        if (flag_owner(f) != team) { int k = na++; while (k > 0 && dist2(flag_pos(att[k - 1]), teamCen) > dist2(flag_pos(f), teamCen)) { att[k] = att[k - 1]; k--; } att[k] = f; }
        else { int k = nd++; while (k > 0 && dist2(flag_pos(def[k - 1]), enemyHq) > dist2(flag_pos(f), enemyHq)) { def[k] = def[k - 1]; k--; } def[k] = f; }
    }
    int target[32], defend[32];
    for (int k = 0; k < ns; k++) target[k] = -1;
    for (int a = 0; a < na; a++)                            // closest free squads to each flag to take
        for (int n = 0; n < ATTACKERS_PER_FLAG; n++) {
            int best = -1;
            for (int k = 0; k < ns; k++)
                if (target[k] < 0 && (best < 0 || dist2(cen[k], flag_pos(att[a])) < dist2(cen[best], flag_pos(att[a])))) best = k;
            if (best < 0) break;
            target[best] = att[a]; defend[best] = 0;
        }
    for (int k = 0, r = 0; k < ns; k++) {                   // the rest: hold our flags, front line first
        if (target[k] >= 0) continue;
        if (nd) { target[k] = def[r % nd]; defend[k] = 1; }
        else if (na) { target[k] = att[(r % 2 == 0 || na < 2) ? 0 : 1]; defend[k] = 0; }
        r++;
    }
    for (int k = 0; k < ns; k++) {
        int s = sq[k];
        if (g_squadTarget[team][s] - 1 != target[k] || g_squadDefend[team][s] != defend[k])
            bot_log("squad: %s squad %d -> %s flag %d", team == 1 ? "US" : "RU", s, target[k] < 0 ? "none" : defend[k] ? "defend" : "attack", target[k]);
        g_squadTarget[team][s] = target[k] + 1;
        g_squadDefend[team][s] = (unsigned char)defend[k];
    }
}

static int squad_objective(int team, int squad)
{
    if (team < 1 || team > 2 || squad < 1 || squad > 31 || !g_flagCount) return -1;
    if (H->frame % 15 == 0 || !g_allocAt[team]) {   // cheap checks at most twice a second
        unsigned sig = bot_squads_mask(team) * 2654435761u;
        for (int f = 0; f < g_flagCount; f++) sig = sig * 31u + (unsigned)flag_owner(f);
        if (sig != g_allocSig[team] || H->frame - g_allocAt[team] > SQUAD_ALLOC_REFRESH || !g_allocAt[team]) {
            g_allocSig[team] = sig;
            g_allocAt[team] = H->frame ? H->frame : 1;
            allocate_squads(team, bot_squads_mask(team));
        }
    }
    if (!bot_squad_rank(team, squad)) return -1;
    int t = g_squadTarget[team][squad] - 1;
    return t >= 0 && t < g_flagCount ? t : -1;
}

static int squad_defending(int team, int squad)
{
    return team >= 1 && team <= 2 && squad >= 1 && squad <= 31 && g_squadDefend[team][squad];
}

// ---- Local objective: after getting out of a vehicle (or being released with GoGoGo) a bot takes the flag nearest
// to where it stands - attack if it is not ours, guard if it is - until that flag is taken or 90 s pass ----
#define LOCAL_OBJECTIVE_FRAMES (90 * 30)
static int g_localFlag[MAX_BOTS];        // flag index + 1 (0 = none)
static unsigned g_localUntil[MAX_BOTS];

static unsigned g_noRouteSince[MAX_BOTS];   // first failed flag route of a run of failures (0 = routing fine)
static void kill_bot(char *player);
static void choose_local_objective(int i, const char *why)
{
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!s || !g_flagCount) return;
    float *me = (float *)(s + OFF_SOLDIER_SPAWN_POSITION);   // transform: right even the frame we step out
    int best = -1, team = player_team(pl);
    float bestD = 1e30f;
    for (int pass = 0; pass < 2 && best < 0; pass++)   // a flag not ours first; all ours: the nearest to guard
        for (int f = 0; f < g_flagCount; f++) {
            if (!pass && flag_owner(f) == team) continue;
            float d = dist2(flag_pos(f), me);
            if (d < bestD) { bestD = d; best = f; }
        }
    if (best < 0) return;
    g_localFlag[i] = best + 1;
    g_localUntil[i] = H->frame + LOCAL_OBJECTIVE_FRAMES;
    BotBrain *b = bot_brain(i);
    b->targetFlag = -1;
    b->enroute = b->waiting = 0;
    b->avoidUntil = 0;
    b->avoidFlag = -1;
    bot_nav(i)->mode = 0;
    H->botMove[i][0] = H->botMove[i][2] = 0;
    bot_log("brain: %s (%s) re-evaluates: nearest flag %d, %.0f m away - %s it", pl + OFF_PLAYER_ONLINE_NAME, why, best, sqrtf(bestD),
            flag_owner(best) == player_team(pl) ? "guard" : "attack");
}

// The bot's current local objective (-1 = none / expired / attack flag already taken); *defend says guard or attack.
static int local_objective(int i, int team, int *defend)
{
    int f = g_localFlag[i] - 1;
    if (f < 0 || f >= g_flagCount || H->frame > g_localUntil[i]) { g_localFlag[i] = 0; return -1; }
    *defend = flag_owner(f) == team;
    return f;
}

static void cmd_spawn(const char *name);
static void teams_fix_all(void);
static void bot_say(int i, int orderType);
static void bot_say_later(int i, int type, unsigned delayFrames);
#define POSE_PRESS_FRAMES 8   // prone needs the button held a few frames (2 frames is ignored)
// inwatch: a player's input levels (EntryInput +0x10, a float per action) and stance as they change - research
static char *g_inWatchPl;
static unsigned g_inWatchUntil;
static float g_inWatchPrev[80];
static unsigned g_inWatchRaw[256];
static int g_inWatchPose;
// Crouch on a held spot: EIA 33 toggles crouch for a bot (found by trying every action on a passive bot and reading its
// stance - 43, the action a player's toggle-crouch key shows, and 13 do nothing for a bot); jump (EIA 12) stands it up
// again, as for prone. Both only from the right stance (soldier +0x2d0: 0 stand, 1 crouch, 2 prone; +0x2d4 the stance
// it is changing to), so the toggle can never put a bot the wrong way round.
static int g_crouchAction = 33;
// Sprint is not an action level: a player's input carries it as two flag bits (+0x1e0 bit 0x00400000 and +0x1e4 bit
// 0x8, set together for exactly as long as the sprint key is held - seen with inwatch). Set for a bot while
// g_sprintUntil[i] is ahead.
#define OFF_INPUT_FLAGS_A   0x1e0
#define OFF_INPUT_FLAGS_B   0x1e4
#define SPRINT_BIT_A        0x00400000u
#define SPRINT_BIT_B        0x00000008u
static unsigned g_sprintUntil[MAX_BOTS];
static void bot_crouch(int i)
{
    char *s = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
    if (!s || g_crouchAction < 0) return;
    if (*(int *)(s + OFF_SOLDIER_POSE) == 0 && *(int *)(s + OFF_SOLDIER_POSE + 4) == 0) press_button(i, g_crouchAction, POSE_PRESS_FRAMES);
}
static void bot_stand(int i)
{
    char *s = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
    if (!s) return;
    int pose = *(int *)(s + OFF_SOLDIER_POSE), want = *(int *)(s + OFF_SOLDIER_POSE + 4);
    if (pose == 1 || pose == 2 || want == 1 || want == 2) press_button(i, 12, 4);   // jump: up
}
static int task_tick(int i, BotBrain *b, char *soldier);
static char *player_vehicle(char *pl);
static int gun_ok(char *w);
static int vweapon_ballistics(char *w, float *speed, float *grav, const char **fx);
static float *vehicle_xf(char *veh);
static void missile_tick(void);
static int cm_beam_heading(int i, float hdg, float *want);
static void queue_bot_removal(int i);
enum { VK_OTHER, VK_JET, VK_HELI, VK_TANK, VK_IFV, VK_LIGHT, VK_BOAT };   // vehicle_kind()
static int vehicle_kind(char *veh);
static int vehicle_ours_side(char *v, int team);
static int vehicle_nation(char *v);   // 1 US-built, 2 RU-built, 0 unknown (by its name)
static int pilot_has_rides(int i);
static int is_pilot(int i);
static void vehicle_mark_bad(char *v, int sec);
static int driver_seat_free(char *veh);
static int pilot_heading_for_ride(int i);
static int pilot_ride_ours(char *v, int team);
static int pilot_ride_kind(int k);   // a Pilot whose team has tanks / jets on this map: HQ spawns, waits for one
static unsigned char g_flyTakeoff[MAX_BOTS];
static float g_flyTakeoffAlt[MAX_BOTS];   // altitude where the take-off run began (a runway's AGL ray reads nothing: starts underground)
static unsigned g_jetUpAt[3];   // (jet_idle_tick: a jet of that side last seen flying)
static void *g_cse[64];
static int g_cseCount;
static char g_cseLevel[64];
static void cse_scan(void);
static BotTeamSpawn g_origHq[3];   // the level's own HQ spawn per team, before a remembered / probed one replaced it (Wake: the US
                                   // carrier's lower deck - BotMod uses the island; Pilots spawn at the original, by the jets)
static int air_spawn(int i);
static unsigned g_intoJet[MAX_BOTS];   // a Pilot to put straight into a free jet of ours once it is alive (until this frame: a round's
                                       // first spawns come before the jets do)
static int g_airSpawnJetsOnly;   // outside Air Superiority: only a spawn point holding a live, parked jet (one put a Pilot in a Venom)   // Air Superiority: a Pilot spawns straight into a jet (a CharacterSpawnEntity with one)
static void pilot_air_tick(int i);
// Air leash (the user, 2026-10-13: dogfights carried jets out of the map and they blew up): no aircraft goes further
// than this from the centre of all the flags. BotMod\BotMod_bounds.txt "<level> <metres>"; 1500 if not listed.
// Nebandan (XP5_002): the user landed 1926 m out, 200-300 m inside the border -> 1700.
static float air_leash(float *cx, float *cz);
static void bot_exit_vehicle(int i, int parachute);
// A bot in a vehicle that nothing drives for it: no drive of its own, no human aboard, no bot driving it - its drive
// state was lost (a core reload wipes it: Recon RU sat in a buggy nosed into a building, nobody at the controls). Out
// after 3 s; the brain carries on and may take a vehicle again properly.
static unsigned g_strandedSince[MAX_BOTS];
static void stranded_in_vehicle_tick(int i);
static void place_class_gadget(int i, char *soldier);
static int pilot_heading_for_ride(int i);
static unsigned g_taxiNoFlagUntil[MAX_BOTS][16];   // a taxi found no road to that flag (Seine's quay flag): others first, a while
void schedule_press(int i, int action, unsigned at);
static int bot_gadget_slot(int i, const char *k1, const char *k2);
static int bot_ammo(int i, int slot);
static int *wanted_bots(void);
static void save_bot_count(void);
static int g_rosterAuto;
static int player_squad(char *pl);
static int squad_request_recent(char *pl);
static int squad_objective(int team, int squad);
static int squad_defending(int team, int squad);
static void choose_local_objective(int i, const char *why);
static void repair_scan(int i, char *soldier);
static void revive_scan(int i, char *soldier);
static void cover_react(int i, char *soldier);
static void rocket_scan(int i, char *soldier);
static void vehicle_threat_scan(int i, char *soldier);
static void self_requests(int i, char *soldier);
static float g_lastHealth[MAX_BOTS];   // (defined with the cover code)
static int g_spareMax[MAX_BOTS], g_launcherMax[MAX_BOTS];   // this life's full reserve (primary) / full launcher load
static int soldier_ammo2(char *soldier, int slot, int *loaded, int *spare);
static int pickup_tick(int i, BotBrain *b, char *soldier);
static char *g_testVictim;
static unsigned char g_brainHeld[MAX_BOTS];
static unsigned g_vehChanceUntil[MAX_BOTS];   // just got out / just took a flag: may grab a ground vehicle nearby
static void start_fight(int i, char *target, int retreat, const char *why);
static int g_rocketsThisLife[MAX_BOTS];
// Enemy ground vehicles near a bot without a launcher: over VEH_SLOW_SPEED it takes cover; at or under it, within throwing
// range and with a grenade, it throws one first. Cover lasts until the vehicle is gone / empty / beyond VEH_THREAT_RANGE /
// out of sight for a while; then back to what it was doing.
#define VEH_THREAT_RANGE       40.0f
#define VEH_SLOW_SPEED         3.0f
#define GRENADE_MIN_RANGE      6.0f
#define GRENADE_MAX_RANGE      30.0f
#define GRENADE_SPEED          17.0f     // m/s off the hand (estimate - calibrate from "grenade:" log lines)
static char *g_threatVeh[MAX_BOTS];      // the vehicle a bot is in cover from (NULL: a soldier / none)
static unsigned char g_coverOpen[MAX_BOTS];   // the cover spot is open ground (no cover within reach): lie prone there
static unsigned char g_coverProne[MAX_BOTS];  // lying prone in cover (the stand-up jump waits until cover ends)
static unsigned g_threatVehHidden[MAX_BOTS], g_vehThreatCooldown[MAX_BOTS];
static unsigned char g_rocketGrenade[MAX_BOTS];   // TASK_ROCKET with a hand grenade instead of a launcher
static int g_grenadesThisLife[MAX_BOTS];
static const char *vehicle_threat_gone(int i, const float *me);
static int player_exists(char *pl);
static float dist2(const float *a, const float *b);
static void start_repair(int i, char *veh, int slot);
struct VehTrack;
static int vehicle_list(char ***out);
static int g_claymores[MAX_BOTS], g_tugs[MAX_BOTS], g_mines[MAX_BOTS];
static const unsigned char *drive_mask(void);
static float g_lastGadgetPos[MAX_BOTS][2];
static void press_button(int i, int action, int frames);
static int launcher_slot(int i);
static int bot_ammo(int i, int slot);
struct KitStore;

// ---- Flag sectors: the bots going to a flag split the ring around it evenly; each looks for a spot in its own
// sector, inside the capture area, with something low between it and the outside, and faces outward. ----
#define SECTOR_ATTACK_MIN      5.0f
#define SECTOR_ATTACK_MAX      9.0f
#define SECTOR_DEFEND_MAX      14.0f
static float g_sectorYaw[MAX_BOTS];
static unsigned char g_hasSector[MAX_BOTS];
static int solid_between(const float *from, const float *to, float *hitOut);
static int flag_sector_spot(int i, char *soldier, int f, int defend, float *out, float floorY)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return 0;
    int team = player_team(H->bots[i]), n = 0, rank = 0;
    for (int j = 0; j < H->botCount; j++) {
        if (player_team(H->bots[j]) != team || !*(char **)(H->bots[j] + OFF_PLAYER_SOLDIER)) continue;
        if (j != i && bot_brain(j)->targetFlag != f) continue;
        n++;
        if (j < i) rank++;
    }
    if (n < 1) n = 1;
    const float *fp = flag_pos(f);
    float ang0 = f * 1.3f + 6.2831853f * rank / n;
    float rmax = defend ? SECTOR_DEFEND_MAX : SECTOR_ATTACK_MAX;
    float half = 3.14159265f / n;                    // this bot's slice of the ring
    if (half > 0.6f) half = 0.6f;
    float best[3], bestYaw = 0;
    int have = 0;
    for (int t = 0; t < 12; t++) {
        float ang = ang0 + (defend ? ((rand() % 200) - 100) / 100.0f * half : ((t % 3) - 1) * half * 0.6f);
        float r = SECTOR_ATTACK_MIN + (rmax - SECTOR_ATTACK_MIN) * ((t / 3) / 3.0f + (defend ? (rand() % 100) / 300.0f : 0));
        float dx = sinf(ang), dz = cosf(ang), pt[3];
        if (!nav_snap(g, fp[0] + dx * r, floorY, fp[2] + dz * r, pt) || fabsf(pt[1] - floorY) > 2.5f) continue;   // (on that floor)
        float threat[3] = {fp[0] + dx * (r + 35.0f), pt[1] + 1.6f, fp[2] + dz * (r + 35.0f)}, body[3] = {pt[0], pt[1] + 0.9f, pt[2]}, hit[3];
        int cover = solid_between(threat, body, hit) && dist2(hit, pt) < 2.5f * 2.5f;
        if (!have || cover) { memcpy(best, pt, 12); bestYaw = yaw_towards(dx, dz); have = 1; }
        if (cover) break;
    }
    if (!have) return 0;
    memcpy(out, best, 12);
    g_sectorYaw[i] = bestYaw;
    return 1;
}

unsigned char g_gliding[MAX_BOTS];   // in the air (free fall / parachute): fall_tick steers, the brain waits
static void brain_tick(int i)
{
    BotBrain *b = bot_brain(i);
    if (!b->enabled) {
        char *pl = H->bots[i], *name = pl + OFF_PLAYER_ONLINE_NAME;
        int want = strstr(name, " US") ? 1 : strstr(name, " RU") ? 2 : 0;
        if (want && !*(char **)(pl + OFF_PLAYER_SOLDIER) && player_team(pl) != want && (H->frame + i) % 150 == 0) {
            char ok = ((AddToTeam_t)ADDR_ADD_TO_TEAM)(pl, want, 0, 1);
            bot_log("roster: moved passive '%s' back to team %d (%s)", name, want, ok ? "ok" : "FAILED");
        }
        return;
    }
    if (g_gliding[i] && *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER)) return;
    char *pl = H->bots[i];
    char *soldier = *(char **)(pl + OFF_PLAYER_SOLDIER);
    const char *name = pl + OFF_PLAYER_ONLINE_NAME;
    if (!soldier) {
        g_lastHealth[i] = 0;
        g_rocketsThisLife[i] = 0;
        g_grenadesThisLife[i] = 0;
        g_spareMax[i] = g_launcherMax[i] = 0;
        if (!b->deadSince) {
            b->deadSince = H->frame ? H->frame : 1;
            b->targetFlag = -1;
            b->enroute = b->waiting = 0;
            b->task = TASK_NONE;   // dying releases any human order: back to the flag loop after respawn
            b->crouched = 0;       // a respawned soldier stands
            b->commander = b->serviceTarget = b->prevCommander = NULL;
            b->prevTask = TASK_NONE;
            bot_nav(i)->mode = 0;
            H->botMove[i][0] = H->botMove[i][2] = 0;
        }
        else if (H->frame - b->deadSince > RESPAWN_DELAY_FRAMES &&
                 (!downed_of(pl) || !downed_of(pl)->medic || H->frame - b->deadSince > BOT_REVIVE_WAIT_FRAMES)) {
            b->deadSince = 0;
            g_claymores[i] = g_tugs[i] = g_mines[i] = 0;
            g_lastGadgetPos[i][0] = g_lastGadgetPos[i][1] = 1e9f;
            cmd_spawn(name);
        }
        return;
    }
    b->deadSince = 0;
    {   // Stance (soldier +0x2d0: 0 standing, 2 prone; +0x2d4 the stance it is changing to). Bots never go prone on
        // purpose - revives and some falls leave them there. Jump stands a soldier up (one-way, unlike the prone key,
        // which is a toggle and could put it back down). Crouch is held, not toggled: it ends by itself.
        static unsigned nextTry[MAX_BOTS];
        int pose = *(int *)(soldier + OFF_SOLDIER_POSE), want = *(int *)(soldier + OFF_SOLDIER_POSE + 4);
        if (pose == 1 && want == 1 && (bot_nav(i)->mode == 2 || bot_nav(i)->mode == 3) && H->frame >= nextTry[i]) {   // crouched, setting off
            press_button(i, EIA_JUMP, 4);   // up first (space), then walk
            nextTry[i] = H->frame + 30;
            b->crouched = 0;
        }
        if (pose == 2 && want == 2 && H->frame >= nextTry[i] && !g_coverProne[i] && !b->crouched) {   // (not one lying on its spot)
            press_button(i, EIA_JUMP, 4);
            nextTry[i] = H->frame + 2 * 30;
            bot_log("brain: %s is prone - jumps to stand up", name);
        }
    }
    if (b->sayNext && H->frame >= b->sayAt) { bot_say(i, b->sayNext); b->sayNext = 0; }
    stranded_in_vehicle_tick(i);
    {   // anywhere worth running to, at a run: whatever the bot is walking to (a flag, a vehicle, a player it follows, a body
        // to revive, a supply run, a post) more than 20 m off, it sprints - not in a fight or lining up a rocket, not
        // crouched / prone, not in a vehicle. The last 20 m at a walk so it does not overrun.
        BotNav *sn = bot_nav(i);
        int pose = *(int *)(soldier + OFF_SOLDIER_POSE);
        if ((sn->mode == 2 || sn->mode == 3) && b->task != TASK_FIGHT && b->task != TASK_ROCKET && pose == 0 && !player_vehicle(H->bots[i])) {
            float gx = sn->x, gz = sn->z;
            if (sn->mode == 3) path_goal(i, &gx, &gz);
            float *me = (float *)(soldier + OFF_SOLDIER_POSITION), dx = gx - me[0], dz = gz - me[2];
            float sprintOver = is_pilot(i) && b->task == TASK_DEFEND && pilot_heading_for_ride(i) ? 100.0f : 20.0f;   // (a Pilot to its ride: >100 m)
            if (dx * dx + dz * dz > sprintOver * sprintOver) g_sprintUntil[i] = H->frame + 4;
        }
    }
    cover_react(i, soldier);
    if (H->frame % 15 == (unsigned)(i % 15)) { rocket_scan(i, soldier); vehicle_threat_scan(i, soldier); repair_scan(i, soldier); revive_scan(i, soldier); }
    if (H->frame % 30 == (unsigned)(i % 30)) self_requests(i, soldier);
    {
        int l, s;
        if (soldier_ammo2(soldier, 0, &l, &s) && s > g_spareMax[i]) g_spareMax[i] = s;
        int ls = launcher_slot(i), la = ls >= 0 ? bot_ammo(i, ls) : -1;
        if (la > g_launcherMax[i]) g_launcherMax[i] = la;
    }
    if (pickup_tick(i, b, soldier)) return;
    if (b->task != TASK_NONE && task_tick(i, b, soldier)) return;
    if (!player_vehicle(pl) && !*(void **)(pl + 0x3e8)) {   // out of a vehicle but not controlling the soldier: re-attach
        ((SetPtr_t)ADDR_PLAYER_ATTACH_SOLDIER)(pl, soldier);
        bot_log("vehicle: re-attached %s to its soldier", name);
    }
    static unsigned char wasRiding[MAX_BOTS];
    if (player_vehicle(pl)) {   // riding with no one to follow: get out and play
        wasRiding[i] = 1;
        if (H->frame % 60 == 0) bot_exit_vehicle(i, 1);
        return;
    }
    if (wasRiding[i]) {   // just got out: plan again from here (a follower keeps following - handled by its task)
        wasRiding[i] = 0;
        if (b->task == TASK_NONE) choose_local_objective(i, "got out of a vehicle");
    }
    if (!g_flagCount) return;
    int team = player_team(pl), squad = player_squad(pl);
    int sq = squad_objective(team, squad), defend = sq >= 0 && squad_defending(team, squad);
    {   // a local objective (after a vehicle ride / GoGoGo) wins over the squad's while it lasts
        int ld = 0, lf = local_objective(i, team, &ld);
        if (lf >= 0) {
            if (ld && !defend && b->targetFlag == lf && flag_owner(lf) == team && !b->waiting && !b->enroute) { g_localFlag[i] = 0; lf = -1; }   // taken: back to the squad
            if (lf >= 0) { sq = lf; defend = ld; }
        }
    }
    int cur = b->targetFlag;
    if (cur >= 0 && flag_owner(cur) == team && !(defend && cur == sq)) {
        bot_log("brain: %s's team took flag %d", name, cur);
        g_vehChanceUntil[i] = H->frame + 20 * 30;   // a vehicle for the next leg?
        cur = -1;
        b->waiting = 0;
    }
    if (b->avoidUntil && H->frame > b->avoidUntil) { b->avoidUntil = 0; b->avoidFlag = -1; }
    if (H->frame < b->retryAt) return;
    if (sq >= 0 && b->avoidUntil && sq == b->avoidFlag) { sq = -1; defend = 0; }   // cannot reach the squad's flag right now: own choice
    if (sq >= 0 && cur >= 0 && cur != sq) {                        // squad objective changed: re-route
        bot_log("brain: %s follows its squad to %s flag %d", name, defend ? "defend" : "attack", sq);
        cur = -1;
        b->enroute = b->waiting = 0;
    }
    int patrol = defend && cur == sq && b->waiting && H->frame >= b->stepAt;   // guarding: time to move to another spot
    if (cur < 0 || patrol || (H->frame % 30 == 0 && !b->enroute && !b->waiting)) {
        float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
        int f = sq >= 0 ? sq : pick_flag(team, pos, b->avoidUntil ? b->avoidFlag : -1);
        if (f < 0) { b->targetFlag = -1; return; }   // team owns everything reachable: idle
        if (f != cur || patrol || (!b->enroute && !b->waiting)) {
            b->targetFlag = f;
            b->waiting = 0;
            b->enroute = 0;
            g_hasSector[i] = 0;
            // the floors at the flag, nearest its own height first: where floors stack (Metro: a walkway over the platform)
            // the flag's height can pick one there is no way onto - then the next floor of the capture zone
            float floors[3] = {flag_pos(f)[1]};
            int nfl = 1;
            {
                NavGrid *g = *nav_slot();
                if (g && g->phase == 2) { nfl = nav_floors_at(g, flag_pos(f)[0], flag_pos(f)[1], flag_pos(f)[2], floors, 3); if (!nfl) { floors[0] = flag_pos(f)[1]; nfl = 1; } }
            }
            int deferred = 0;
            for (int fl = 0; fl < nfl && !b->enroute && !deferred; fl++) {
                float fy = floors[fl];
                if (!plan_budget_left()) { deferred = 1; break; }
                {   // own sector of the ring around the flag
                    float sp[3];
                    if (flag_sector_spot(i, soldier, f, defend, sp, fy) && plan_path(i, soldier, sp[0], sp[1], sp[2])) { b->enroute = 1; g_hasSector[i] = 1; }
                }
                for (int attempt = 0; attempt <= (nfl > 1 ? 0 : FLAG_RETRY_OFFSETS) && !b->enroute; attempt++) {   // (several floors: one try each)
                    float spread = defend ? (attempt ? 6.0f : PATROL_RADIUS) : attempt ? 8.0f : FLAG_SPREAD;
                    float tx = flag_pos(f)[0] + ((rand() % 200) - 100) / 100.0f * spread;
                    float tz = flag_pos(f)[2] + ((rand() % 200) - 100) / 100.0f * spread;
                    if (plan_path(i, soldier, tx, fy, tz)) b->enroute = 1;
                }
                if (b->enroute && fl) bot_log("brain: %s goes for flag %d on the floor at %.1f m (none to the one at the flag's height)", name, f, fy);
            }
            if (deferred && !b->enroute) { b->targetFlag = -1; b->retryAt = H->frame + 3; return; }   // out of planning time: next tick
            if (b->enroute) g_noRouteSince[i] = 0;
            else if (!g_noRouteSince[i]) g_noRouteSince[i] = H->frame ? H->frame : 1;
            else if (H->frame - g_noRouteSince[i] > 15 * 30) {   // no way to any flag for 15 s: trapped (a pocket the nav has no
                g_noRouteSince[i] = 0;                            // way out of - e.g. dropped onto Seine's lower quay): redeploy
                bot_log("brain: %s is trapped at (%.0f, %.1f, %.0f) - no route to any flag for 15 s, redeploying", name, pos[0], pos[1], pos[2]);
                kill_bot(H->bots[i]);
                return;
            }
            if (b->enroute) {
                if (b->crouched) { bot_stand(i); b->crouched = 0; }   // stand up to move
                bot_nav(i)->mode = 3;
                if (!patrol)
                    bot_log("brain: %s %s flag %d at (%.0f, %.0f), owner %d", name, defend ? "going to guard" : "heading for", f, flag_pos(f)[0],
                            flag_pos(f)[2], flag_owner(f));
            } else {
                bot_log("brain: %s has no route to flag %d, trying others for 10s (%s)", name, f, nav_last_error());
                b->targetFlag = -1;
                b->avoidFlag = f;
                b->avoidUntil = H->frame + FLAG_AVOID_FRAMES;
                b->retryAt = H->frame + 30;   // at most one attempt per second
            }
        }
    }
    // path finished (follower switched to mode 1): hold position until the flag flips
    if (b->enroute && bot_nav(i)->mode == 1) {
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        float fx = flag_pos(b->targetFlag)[0] - me[0], fz = flag_pos(b->targetFlag)[2] - me[2];
        if (fx * fx + fz * fz > (PATROL_RADIUS + 5.0f) * (PATROL_RADIUS + 5.0f)) { b->enroute = 0; b->targetFlag = -1; return; }   // path ended short: re-plan
        b->enroute = 0;
        b->waiting = 1;
        if (g_hasSector[i]) { bot_nav(i)->mode = 1; bot_nav(i)->targetYaw = g_sectorYaw[i]; }   // watch our sector
        if (defend && b->targetFlag == sq) {
            if (!b->crouched) { bot_crouch(i); b->crouched = 1; }   // guard crouched (up again before the patrol moves on)
            b->stepAt = H->frame + (20 + rand() % 11) * 30;   // stand guard 20-30 s, then patrol to another spot
        } else {
            if (!b->crouched) { bot_crouch(i); b->crouched = 1; }   // crouch on the flag
            bot_log("brain: %s holding flag %d (owner %d)", name, b->targetFlag, flag_owner(b->targetFlag));
        }
        place_class_gadget(i, soldier);
    }
    if (b->waiting && b->targetFlag >= 0 && flag_owner(b->targetFlag) == team && !(defend && b->targetFlag == sq)) b->waiting = 0;
}
static float bot_distance_to(int i, const float *p)
{
    char *s = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
    if (!s) return 1e30f;
    float *me = (float *)(s + OFF_SOLDIER_POSITION), dx = p[0] - me[0], dz = p[2] - me[2];
    return sqrtf(dx * dx + dz * dz);
}

// Loadout slot of the first weapon whose asset name contains any of the given keywords (-1 = not carried).
static int bot_gadget_slot(int i, const char *k1, const char *k2)
{
    LoadoutLib *lib = loadout_lib();
    int idx = g_botLoadout[i];
    if (idx < 0 || idx >= lib->count) return -1;
    Loadout *L = &lib->l[idx];
    for (int w = 0; w < L->nWeapons; w++) {
        const char *n = asset_name(*(void **)(L->weaponMsg[w] + 0x3c));
        if (strstr(n, k1) || (k2 && strstr(n, k2))) return *(int *)(L->weaponMsg[w] + 0x38);
    }
    return -1;
}

// ---- Vehicles ----
static char *player_vehicle(char *pl) { return *(char **)(pl + OFF_PLAYER_VEHICLE); }

// Where a human is: the soldier transform keeps following the seat while in a vehicle (the simulated position freezes).
static float *human_pos(char *pl)
{
    char *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    return s ? (float *)(s + OFF_SOLDIER_SPAWN_POSITION) : NULL;
}

// Light-vehicle driving state (the driving code is with the tickets, further down)
#define DRIVE_MAX_PTS          128
typedef struct DriveState {
    int phase;                          // 0 none, 1 walking to the vehicle, 2 planning, 3 driving,
                                        // 5 squad boarding (walking up), 6 at the wheel waiting for the squad, 7 riding
    char *veh;
    unsigned at, checkAt, reverseUntil;
    int stuckCount, n, next, flag, manual;   // manual: a console driveto (runs with the brain off)
    unsigned boardAt;                   // "Get in" called at this frame (waiting for riders)
    int reroutes;                       // stuck re-routes this drive (survives re-planning)
    unsigned backOutUntil, sensorLogAt;            // a boat leaving its slip: straight back first
    unsigned pickupUntil, pickupAskAt;
    unsigned boxedSince;                // nosed up against something since (quick back-off)
    float sideL, sideR, midNear;        // nearest thing the left / middle / right front sensors see (m)
    unsigned char awayOn;               // backing off: swing the nose to awayHeading (away from the blocked side)
    float awayHeading;
    int nudges;                         // quick back-offs at about the same spot (2nd: strike it off and re-route)
    float nudgePos[3];
    float frontNear;                    // nearest thing the front sensors see (m)  // stopped to pick up bots by the road (until), last look for some
    unsigned char backedOut;
    unsigned char taxi;                 // a Pilot's jeep taxi: roads only, 3 m clear of objects (the taxi masks)
    float goal[3], start[3], lastPos[3], reverseSteer, speed, spPos[3];
    unsigned spAt;
    float pts[DRIVE_MAX_PTS * 3];
} DriveState;
static DriveState g_drive[MAX_BOTS];
static int g_driveOff;                  // console: drive off / on
static void tree_apply_drive(void);
// Spots struck off a drive mask where a vehicle got stuck come back after DRIVE_STRIKE_FRAMES (they used to last the
// whole round: later routes went wide round a "wall" that was only a stuck moment), restored from a copy of the mask
// taken before its first strike.
#define DRIVE_STRIKE_FRAMES (5 * 60 * 30)
#define MAX_DRIVE_STRIKES 64
static struct { unsigned char *mask; float x, z, r; unsigned until; } g_strikes[MAX_DRIVE_STRIKES];
static int g_strikeNext;
#define STRIKE_SLOTS 5
static unsigned char *g_strikeBase[STRIKE_SLOTS], *g_strikeBaseOf[STRIKE_SLOTS];   // drive, boat, tank and the two taxi masks
static int strike_slot_of(const unsigned char *mask) { for (int s = 0; s < STRIKE_SLOTS; s++) if (mask && g_strikeBaseOf[s] == mask) return s; return -1; }
static void strike_forget(const unsigned char *mask)   // a mask about to be freed: no strikes / base copy left pointing at it
{
    for (int k = 0; k < MAX_DRIVE_STRIKES; k++) if (g_strikes[k].mask == mask) g_strikes[k].mask = NULL;
    int s = strike_slot_of(mask);
    if (s >= 0) { HeapFree(GetProcessHeap(), 0, g_strikeBase[s]); g_strikeBase[s] = NULL; g_strikeBaseOf[s] = NULL; }
}
static void drive_strikes_reset(void)   // a mask rebuilt (new map): its strikes and copies are gone too
{
    memset(g_strikes, 0, sizeof g_strikes);
    for (int s = 0; s < STRIKE_SLOTS; s++) { if (g_strikeBase[s]) HeapFree(GetProcessHeap(), 0, g_strikeBase[s]); g_strikeBase[s] = NULL; g_strikeBaseOf[s] = NULL; }
}
static int drive_strike(unsigned char *mask, float x, float z, float r)
{
    NavGrid *g = *nav_slot();
    if (!g || !mask) return 0;
    size_t n = (size_t)g->w * g->h * NAV_LAYERS;
    int slot = strike_slot_of(mask);
    if (slot < 0) {   // first strike on this mask: keep it as it is now (a free slot, else the first)
        slot = 0;
        for (int s = STRIKE_SLOTS - 1; s >= 0; s--) if (!g_strikeBaseOf[s]) slot = s;
        if (g_strikeBase[slot]) HeapFree(GetProcessHeap(), 0, g_strikeBase[slot]);
        g_strikeBase[slot] = HeapAlloc(GetProcessHeap(), 0, n);
        g_strikeBaseOf[slot] = g_strikeBase[slot] ? mask : NULL;
        if (g_strikeBase[slot]) memcpy(g_strikeBase[slot], mask, n);
    }
    int k = g_strikeNext++ % MAX_DRIVE_STRIKES;
    g_strikes[k].mask = mask; g_strikes[k].x = x; g_strikes[k].z = z; g_strikes[k].r = r; g_strikes[k].until = H->frame + DRIVE_STRIKE_FRAMES;
    return nav_mask_block(g, mask, x, z, r);
}
static void drive_strikes_tick(void)
{
    NavGrid *g = *nav_slot();
    if (!g || H->frame % 30 != 11) return;
    for (int k = 0; k < MAX_DRIVE_STRIKES; k++) {
        if (!g_strikes[k].mask || H->frame < g_strikes[k].until) continue;
        int slot = strike_slot_of(g_strikes[k].mask);
        if (slot >= 0) {
            int n = nav_mask_restore(g, g_strikes[k].mask, g_strikeBase[slot], g_strikes[k].x, g_strikes[k].z, g_strikes[k].r);
            bot_log("drive: (%.0f, %.0f) is back on the drivable map (%d node(s), struck 5 minutes ago)", g_strikes[k].x, g_strikes[k].z, n);
        }
        g_strikes[k].mask = NULL;
    }
}
static int g_paveCursor = -1, g_paveCount;   // paving survey of the drive mask (-1 = done / none)
// Vehicle type name ("Growler", "VDVBuggy", "M1A2", "T90", "Ka-60"...): VehicleEntityData +0x1a0.
#define OFF_VEHDATA_NAME 0x1a0
static const char *vehicle_name(char *veh)
{
    char *data = veh && !IsBadReadPtr(veh + 0x14, 4) ? *(char **)(veh + 0x14) : NULL;
    const char *n = data && !IsBadReadPtr(data + OFF_VEHDATA_NAME, 4) ? *(const char **)(data + OFF_VEHDATA_NAME) : NULL;
    return n && (unsigned)n > 0x10000 && !IsBadStringPtrA(n, 64) ? n : "?";
}
// A vehicle's seats (entry components): their count, 0 when the list does not look like one - some vehicle-type
// objects keep something else there, and reading through it crashed the server.
static unsigned char g_driving[MAX_BOTS];
// Flying (research, 2026-10-12): a bot in an aircraft's pilot seat. Its controls reach the aircraft through the same
// vehicle input as driving: throttle = botMove[0] (EIA 0), rudder = botMove[2] (EIA 4), roll = g_seatYaw (EIA 6),
// pitch = g_seatPitch (EIA 5). Console "flytest" holds given inputs and logs the attitude to learn their signs.
static unsigned char g_flying[MAX_BOTS];
static float g_flyIn[MAX_BOTS][4];          // roll, pitch, throttle, yaw
static unsigned g_flyUntil[MAX_BOTS], g_flyLogAt[MAX_BOTS];
// Autopilot (console "flyto"): bank-and-pull towards a point at an altitude. Signs measured live (2026-10-12, Su-35):
// roll input + raises the measured bank (atan2(right.y, up.y)); pitch input + pushes the nose DOWN (pull = negative);
// pushing at + bank turned the heading up, so pulling turns it towards the low wing: at + bank the heading goes down.
static unsigned char g_flyAuto[MAX_BOTS];
static float g_flyGoal[MAX_BOTS][3];       // x, z, altitude
static float g_flyTurnSign = 1.0f;         // console: flysign -1 if turns go the wrong way
static float g_flyAgl[MAX_BOTS];           // height above the ground (ray), refreshed every half second
static unsigned g_flyAglAt[MAX_BOTS];
static float g_flyAhead[MAX_BOTS];
static float g_flyEscape[MAX_BOTS];        // terrain ahead and a climb will not clear it: bank this way (-1 / +1), 0 = climb         // distance to terrain along the flight path (ray ~3 s ahead), 1e9 = clear
static unsigned char g_flyCircuit[MAX_BOTS];
static signed char g_flyOrbit[MAX_BOTS] = {[0 ... MAX_BOTS - 1] = -1};   // console "flyorbit": circle this flag, passive
static float g_flyOrbitR[MAX_BOTS], g_flyOrbitUp[MAX_BOTS];   // radius; height above the flag (250 hit the ceiling on Nebandan)
// Heat-seeker "seek" mode (console flyseek): chase with the seekers selected (EIA 17, as the tank's coax), no guns, nose
// straight at the target (no lead) from 200-400 m behind; BotMod's own lock (the game's is client-side): within
// g_seekRange m and g_seekCone degrees off the nose for g_seekTime s. With fire on, a locked bot presses g_seekFireEia.
static unsigned char g_flySeek[MAX_BOTS], g_seekFire[MAX_BOTS];
static unsigned g_seekSince[MAX_BOTS], g_seekSelAt[MAX_BOTS], g_seekFiredAt[MAX_BOTS];
static float g_seekDist[MAX_BOTS], g_seekAim[MAX_BOTS][3];   // range; the lead point the nose tracks
static float g_seekRange = 700.0f, g_seekCone = 10.0f, g_seekTime = 4.0f;
// Two seekers per load (the user): after 2 launches the cannon until the missiles are back (g_seekReload s, a guess -
// console "seekreload <s>"), then the seekers again
static unsigned g_seekShots[MAX_BOTS], g_seekGunsUntil[MAX_BOTS], g_seekNoLockSince[MAX_BOTS];
static unsigned char g_seekTurnFight[MAX_BOTS];   // (+ close in with no lock for 8 s: guns)
static float g_seekReload = 15.0f;
static int g_seekFireEia = 7;    // EIAFire with the seekers selected (verified 2026-10-13; needs JetHeatseekerStance in the kit)
static int g_mslDebug;                          // console "msldebug 1": dump every new missile-class object
static int g_flareEia = 10;                     // EIAFireCountermeasure (VU's enum; console "flare <bot#> [n] [eia]")
static char *g_flyChase[MAX_BOTS];             // console "flychase": the player whose aircraft it hunts
static float g_flyTgtPos[MAX_BOTS][3], g_flyTgtVel[MAX_BOTS][3];
static unsigned g_flyTgtAt[MAX_BOTS];
// The dive (ground targets, the user's design 2026-10-12): 0 set up (1.3 km out towards the middle of the map, 400+ m up),
// 1 attack (in towards it; within 1000 m the nose straight at it, cannon on it), 2 pull out (at 200 m from it or 200 m
// above the ground: wings level, hard pull) - then reassess and set up again.
static unsigned char g_strafeRun[MAX_BOTS], g_flyDive[MAX_BOTS], g_flyPull[MAX_BOTS];
static unsigned g_strafeAt[MAX_BOTS];
#define DIVE_START_DIST   1000.0f
#define DIVE_PULL_DIST    200.0f
#define DIVE_MIN_AGL      100.0f    // (the user: 100, with buildings on the way pulling it out too)
static float g_flyVel[MAX_BOTS][3];       // the aircraft's own velocity (m/s), for where its rounds really go
static float g_flyAheadHit[MAX_BOTS][3];  // where the look-ahead ray hit
#define FLY_GUN_SPEED   900.0f    // m/s (jet cannon - to be read from the weapon's data)
#define FLY_GUN_RANGE   800.0f
#define FLY_GUN_TTL     0.67f     // s a jet cannon round lives (projectile data +0x78, read live 2026-10-13: rifle 1.5, seeker 3.0,
                                  // flare 3.5, M67 3.6) - ~680 m: from 900-1000 m the tracers died just short of the user
#define FLY_MSL_SPEED   300.0f    // m/s (the heat-seeker weapon's projectile speed, read live 2026-10-13)
static float g_diveAimUp = 3.0f;    // (10 went long over a parked jet on Nebandan; 3 hit it, 2026-10-13)            // m above a ground target the dive aims (the rounds drop short)   // console "flycircuit": fly from flag to flag, circling each until it is ours
static int g_flyFlag[MAX_BOTS];
static unsigned g_flyFlagUntil[MAX_BOTS];
static float g_flyCeiling = 1e9f;         // the level's altitude ceiling, once a jet has stalled against it (reset per level)
static char g_flyCeilingLevel[64];
static float g_gunMaxLevel = 1.0f, g_gunFeedFwd = 1.5f;   // console: gunmax <level>, gunff <level per rad/s> (a driver's main gun traverse)
// Pilots: tank / jet crews that never get out - they hold, fight and move on from inside the vehicle and leave it only by
// dying with it. A bot is a Pilot when its name says so, or when switched on at the console ("pilot <bot#> 1").
static unsigned char g_pilotOn[MAX_BOTS], g_pilotForceExit;
static int is_pilot(int i)
{
    if (i < 0 || i >= H->botCount) return 0;
    return g_pilotOn[i] || strstr((char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, "Pilot") != NULL;
}   // a bot put in a driver's seat to drive (its inputs go to the vehicle)
static int vehicle_seats(char *veh, char ***out)
{
    if (!veh || IsBadReadPtr(veh + OFF_ENTITY_ENTRIES, 8)) return 0;
    char **b = *(char ***)(veh + OFF_ENTITY_ENTRIES), **e = *(char ***)(veh + OFF_ENTITY_ENTRIES + 4);
    int n = (int)(e - b);
    if (!b || n <= 0 || n > 16 || IsBadReadPtr(b, n * 4)) return 0;
    for (int k = 0; k < n; k++) if (!b[k] || IsBadReadPtr(b[k] + 0x4c, 4)) return 0;
    *out = b;
    return n;
}
// Free seat index (passenger seats first, driver last), -1 if full.
static int free_seat(char *veh)
{
    char **b;
    int n = vehicle_seats(veh, &b);
    if (!n) return -1;
    for (int k = 1; k < n; k++) if (!*(void **)(b[k] + 0x4c)) return k;
    return !*(void **)(b[0] + 0x4c) ? 0 : -1;
}

static int free_seat_count(char *veh)
{
    char **b;
    int n = vehicle_seats(veh, &b), free = 0;
    for (int k = 0; k < n; k++) if (!*(void **)(b[k] + 0x4c)) free++;
    return free;
}
// "Get in": a bot that has not boarded within this time, or finds the vehicle full, goes back to its own business
#define GETIN_BOARD_FRAMES     (12 * 30)
static unsigned g_boardUntil[MAX_BOTS];

typedef char (__attribute__((thiscall)) *EnterVehicle_t)(void *player, void *vehicle, int seat);
static int vehicle_ok(char *veh);
// Someone just died in this vehicle (a player still listed in it with no soldier): the vehicle is often being destroyed
// right now and the engine frees it moments later - boarding it then leaves the bot's player pointing at freed memory
// (2026-10-08 crash). Hold off until that has cleared, plus 3 s.
#define BOARD_SETTLE_FRAMES (3 * 30)
static struct { char *veh; unsigned until; } g_boardHold[16];
static int vehicle_board_safe(char *veh)
{
    if (!vehicle_ok(veh)) return 0;
    char *pm = player_manager();
    int dying = 0;
    if (pm) {
        void **arr = *(void ***)(pm + 0xac);
        unsigned count = *(unsigned *)(pm + 8);
        for (unsigned k = 0; k < count && k < 128; k++) {
            char *pl = arr[k];
            if (pl && *(char **)(pl + OFF_PLAYER_VEHICLE) == veh && !*(char **)(pl + OFF_PLAYER_SOLDIER)) { dying = 1; break; }
        }
    }
    int slot = -1, freeSlot = -1;
    for (int k = 0; k < 16; k++) {
        if (g_boardHold[k].veh == veh) slot = k;
        else if (freeSlot < 0 && (!g_boardHold[k].veh || H->frame >= g_boardHold[k].until)) freeSlot = k;
    }
    if (dying) {
        if (slot < 0) slot = freeSlot >= 0 ? freeSlot : 0;
        g_boardHold[slot].veh = veh;
        g_boardHold[slot].until = H->frame + BOARD_SETTLE_FRAMES;
        return 0;
    }
    return slot < 0 || H->frame >= g_boardHold[slot].until;
}
// Every bot boarding goes through here.
static char enter_vehicle(void *pl, char *veh, int seat)
{
    if (!vehicle_board_safe(veh)) {
        bot_log("vehicle: %s waits - someone just died in the %p", (char *)pl + OFF_PLAYER_ONLINE_NAME, veh);
        return 0;
    }
    return ((EnterVehicle_t)ADDR_PLAYER_ENTER_VEHICLE)(pl, veh, seat);
}
static int bot_enter_vehicle(int i, char *veh)
{
    int seat = free_seat(veh);
    if (seat < 0) { bot_log("vehicle: %s finds no free seat", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME); return 0; }
    bot_nav(i)->mode = 1;
    H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
    enter_vehicle(H->bots[i], veh, seat);
    bot_log("vehicle: %s enters seat %d (now in %p)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, seat, player_vehicle(H->bots[i]));
    return player_vehicle(H->bots[i]) != NULL;
}

static int vehicle_seats(char *veh, char ***entries);
static void stranded_in_vehicle_tick(int i)
{
    char *pl = H->bots[i], *veh = player_vehicle(pl);
    if (!veh || g_drive[i].phase || H->frame % 15) { if (!veh || g_drive[i].phase) g_strandedSince[i] = 0; return; }
    char **b;
    int n = vehicle_seats(veh, &b), cared = 0;
    for (int k = 0; k < n && !cared; k++) {
        char *occ = *(char **)(b[k] + 0x4c);
        if (!occ || occ == pl) continue;
        if (!is_bot(occ)) cared = 1;   // a human aboard: riding with them is the point
        else { int j = bot_index(occ); if (j >= 0 && g_drive[j].phase && g_drive[j].veh == veh) cared = 1; }   // a bot driving it
    }
    if (cared) { g_strandedSince[i] = 0; return; }
    if (!g_strandedSince[i]) { g_strandedSince[i] = H->frame ? H->frame : 1; return; }
    if (H->frame - g_strandedSince[i] < 3 * 30) return;
    g_strandedSince[i] = 0;
    g_driving[i] = 0;
    bot_log("vehicle: %s is in a %s nobody drives for it - gets out", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh));
    bot_exit_vehicle(i, 1);
}
static void bot_exit_vehicle(int i, int parachute)
{
    if (!player_vehicle(H->bots[i])) return;
    if (is_pilot(i) && !g_pilotForceExit) {   // a Pilot stays in (the console "exit" still works)
        static unsigned loggedAt[MAX_BOTS];
        if (H->frame - loggedAt[i] > 30 * 30) { loggedAt[i] = H->frame; bot_log("pilot: %s stays in its vehicle", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME); }
        return;
    }
    typedef void (__attribute__((thiscall)) *ExitVehicle_t)(void *player, int a, int b);
    ((ExitVehicle_t)ADDR_PLAYER_EXIT_VEHICLE)(H->bots[i], 0, 1);   // same call as the exit button
    // (a fall after the jump is handled by fall_tick: the chute opens once the bot is really falling)
    bot_log("vehicle: %s gets out%s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, parachute ? " (parachute ready)" : "");
}

// ---- Class gadgets placed on arrival (flag / defend post) ----
// Uses per life, reset on spawn: claymores (Support) up to 2, T-UGS (Recon) 1 (counters declared near the brain).

static void place_gadget_now(int i, int slot)
{
    schedule_press(i, SLOT_ACTION(slot), H->frame + 1);   // select the gadget
    schedule_press(i, EIA_FIRE, H->frame + 31);           // place it
    schedule_press(i, SLOT_ACTION(0), H->frame + 66);     // back to the primary
}

static void place_class_gadget(int i, char *soldier)
{
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float dx = me[0] - g_lastGadgetPos[i][0], dz = me[2] - g_lastGadgetPos[i][1];
    if (dx * dx + dz * dz < 15.0f * 15.0f) return;   // already placed something around here
    int slot;
    const char *what = NULL;
    if ((slot = bot_gadget_slot(i, "Claymore", NULL)) >= 0 && g_claymores[i] < 2) { g_claymores[i]++; what = "claymore"; }
    else if ((slot = bot_gadget_slot(i, "UGS", NULL)) >= 0 && g_tugs[i] < 1) { g_tugs[i]++; what = "T-UGS"; }
    else if ((slot = bot_gadget_slot(i, "M15", NULL)) >= 0 && g_mines[i] < 2) {   // AT mine: only where vehicles drive
        const unsigned char *dm = drive_mask();
        NavGrid *g = *nav_slot();
        float rp[3];
        if (dm && g && nav_mask_nearest(g, dm, me[0], me[2], 3.0f, rp)) { g_mines[i]++; what = "AT mine"; }
    }
    if (!what) return;
    g_lastGadgetPos[i][0] = me[0];
    g_lastGadgetPos[i][1] = me[2];
    place_gadget_now(i, slot);
    bot_log("brain: %s places a %s (slot %d) at (%.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, what, slot, me[0], me[2]);
}

static void end_task(int i, BotBrain *b, const char *why)
{
    if (!strcmp(why, "GoGoGo from commander") || !strcmp(why, "their player died")) bot_say_later(i, ORDER_SORRY, SAY_DELAY_FRAMES);
    bot_log("brain: %s ends %s task (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, kTaskNames[b->task], why);
    b->task = TASK_NONE;
    b->commander = b->serviceTarget = NULL;
    b->targetFlag = -1;
    b->enroute = b->waiting = 0;
    bot_nav(i)->mode = 0;
    H->botMove[i][0] = H->botMove[i][2] = 0;
}

// Plans a path to (x, z); if the grid has no route (gaps near objectives), walks straight there instead.
static void go_towards(int i, char *soldier, float x, float y, float z)
{
    if (plan_path(i, soldier, x, y, z)) { bot_nav(i)->mode = 3; return; }
    if (g_planRefused) { if (bot_nav(i)->mode == 2) bot_nav(i)->mode = 1; return; }   // planning time spent: wait a tick, ask again - never
                                                                                      // "walk straight" for that (into walls, 2026-10-13)
    g_straightAt[i] = H->frame;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    if (H->frame % 150 == 0)
        bot_log("  %s: no grid route from (%.1f, %.1f) to (%.1f, %.1f), walking straight", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                me[0], me[2], x, z);
    bot_nav(i)->mode = 2;
    bot_nav(i)->x = x;
    bot_nav(i)->z = z;
}

// Walks towards the human, stopping `standoff` metres short. Returns the current distance (1e30 if they have no soldier).
static float approach(int i, BotBrain *b, char *soldier, char *human, float standoff)
{
    float *hp = human_pos(human);
    if (!hp) return 1e30f;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float dx = hp[0] - me[0], dz = hp[2] - me[2], d = sqrtf(dx * dx + dz * dz);
    if (d <= standoff + 1.0f) {
        bot_nav(i)->mode = 1;
        bot_nav(i)->targetYaw = yaw_towards(dx, dz);   // stand and face them
    } else if (H->frame >= b->replanAt) {
        b->replanAt = H->frame + 60;   // check every 2 s; re-plan only if the human moved > 3 m or we are not moving
        float mx = hp[0] - b->postPos[0], mz = hp[2] - b->postPos[2];
        if (mx * mx + mz * mz > 9.0f || bot_nav(i)->mode == 1) {
            b->postPos[0] = hp[0]; b->postPos[2] = hp[2];   // (postPos doubles as "last planned target" while following)
            go_towards(i, soldier, hp[0] - dx / d * standoff, hp[1], hp[2] - dz / d * standoff);
        }
    }
    return d;
}

static int player_exists(char *pl)
{
    char *pm = player_manager();
    if (!pm || !pl) return 0;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count; k++) if (arr[k] == pl) return 1;
    return 0;
}

// ---- Cover: a bot that is hit looks for a spot the shooter cannot see (blocked at chest and head height by
// something close to the spot), runs there, and holds until it has not been hit for a few seconds ----
#define OFF_SOLDIER_HEALTH     0x128
#define COVER_THREAT_RANGE     150.0f

#define COVER_HOLD_FRAMES      (6 * 30)
#define COVER_MAX_FRAMES       (25 * 30)
#define COVER_HUG_DIST         3.0f      // the blocker must be this close to the cover spot
static float g_lastHealth[MAX_BOTS];
static unsigned g_hitAt[MAX_BOTS];
static char *g_threat[MAX_BOTS];
// The player who last damaged each bot, straight from the server's damage handler (hook_soldier_on_damage).
static char *g_attacker[MAX_BOTS];
static unsigned g_attackedAt[MAX_BOTS];
static unsigned g_attackerNoted;
static int is_spotted(char *pl);
static int spotted_cached(char *pl);

static int ray_blocked_char(const float *a, const float *b, float *hit)   // world geometry only (soldiers do not block)
{
    unsigned saved = g_rayFlags;
    g_rayFlags = RAY_DONT_CHECK_CHARACTER;
    int r = ray_cast(a, b, hit, NULL);
    g_rayFlags = saved;
    return r;
}

// Blocked by something that also stops bullets: the ray from the shooter hits, and the ray back from the target hits
// the other side more than COVER_MIN_THICKNESS away (chain-link fences, sheet metal, glass, foliage are thinner).
#define COVER_MIN_THICKNESS    0.15f
static int solid_between(const float *from, const float *to, float *hitOut)
{
    float h1[3], h2[3];
    if (!ray_blocked_char(from, to, h1)) return 0;
    if (hitOut) memcpy(hitOut, h1, 12);
    if (!ray_blocked_char(to, from, h2)) return 0;   // one-sided surface
    float dx = h1[0] - h2[0], dy = h1[1] - h2[1], dz = h1[2] - h2[2];
    return dx * dx + dy * dy + dz * dz > COVER_MIN_THICKNESS * COVER_MIN_THICKNESS;
}

// Nearest enemy with a clear line to the bot: the presumed shooter.
static int fired_recently(char *pl);
static char *likely_shooter(int i, char *soldier, float *eyeOut)
{
    char *pl = H->bots[i], *pm = player_manager();
    if (!pm) return NULL;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), best = COVER_THREAT_RANGE;
    char *who = NULL;
    {   // the server told us who it was: no guessing, any range
        char *a = g_attacker[i], *as;
        if (a && H->frame - g_attackedAt[i] < 30 && player_exists(a) && player_team(a) != player_team(pl) && (as = *(char **)(a + OFF_PLAYER_SOLDIER))) {
            float *tp = (float *)(as + OFF_SOLDIER_SPAWN_POSITION);
            if (eyeOut) { eyeOut[0] = tp[0]; eyeOut[1] = tp[1] + 1.6f; eyeOut[2] = tp[2]; }
            return a;
        }
    }
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    // pass 0: enemies that fired in the last 1.5 s; pass 1: any enemy with line of sight
    for (int pass = 0; pass < 2 && !who; pass++)
    for (unsigned k = 0; k < count; k++) {
        char *t = arr[k], *ts;
        if (!t || player_team(t) == player_team(pl) || !(ts = *(char **)(t + OFF_PLAYER_SOLDIER))) continue;
        if (pass == 0 && !fired_recently(t)) continue;
        float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
        float d = sqrtf(dist2(tp, me) + (tp[1] - me[1]) * (tp[1] - me[1]));
        if (d >= best) continue;
        float eye[3] = {tp[0], tp[1] + 1.6f, tp[2]}, chest[3] = {me[0], me[1] + 1.2f, me[2]}, hit[3];
        if (pass == 1 && solid_between(eye, chest, hit)) continue;   // someone who just fired needs no line-of-sight test
        best = d; who = t;
        if (eyeOut) memcpy(eyeOut, eye, 12);
    }
    return who;
}

// Best cover spot from a shooter's eye position: walkable, hidden at chest and head height behind something close
// to it, not towards the shooter, reachable. Returns 1 with the spot in out.
// ---- Taking cover - one routine for every threat (a soldier shooting, a vehicle): get COVER_MIN_DIST from the threat
// and behind something solid within COVER_SEARCH of the bot (never the threat's own hull); no cover that close: open
// ground at least COVER_MIN_DIST from it, lying prone. In cover the bot faces the threat and stays until it is dead /
// gone (vehicles: also beyond range or out of sight for 10 s), or - a soldier - until 6 s pass without a hit, then
// re-engages it if it is still there on foot; otherwise back to what it was doing. ----
#define COVER_MIN_DIST         25.0f     // from the threat
#define COVER_SEARCH           30.0f     // from the bot
static float g_coverThreatPos[3];        // set by take_cover for find_cover
static int g_coverThreatIsVeh, g_coverFoundOpen;
static float hdist(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }
static int find_cover(int i, char *soldier, const float *eye, float *out)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return 0;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float myThreatD = sqrtf(dist2(me, eye));
    float cand[80][3], score[80], open[80][3], openScore[80];
    int n = 0, no = 0;
    static const float radii[7] = {0.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f, COVER_SEARCH};
    for (int r = 0; r < 7; r++)
        for (int a = 0; a < (r ? 12 : 1); a++) {
            float ang = a * 0.5235988f + r * 0.26f, pt[3];
            if (!r) memcpy(pt, me, 12);
            else if (!nav_snap(g, me[0] + sinf(ang) * radii[r], me[1], me[2] + cosf(ang) * radii[r], pt)) continue;
            if (fabsf(pt[1] - me[1]) > 3.0f) continue;
            if (hdist(pt, g_coverThreatPos) < COVER_MIN_DIST) continue;   // too close to the threat
            float threatD = sqrtf(dist2(pt, eye));
            if (r && threatD < myThreatD - 2.0f) continue;                 // do not run towards it
            float hit[3], chest[3] = {pt[0], pt[1] + 1.2f, pt[2]}, head[3] = {pt[0], pt[1] + 1.7f, pt[2]};
            int covered = solid_between(eye, chest, hit) && sqrtf(dist2(hit, pt)) <= COVER_HUG_DIST && solid_between(eye, head, NULL);
            if (covered && g_coverThreatIsVeh && hdist(hit, g_coverThreatPos) < 6.0f) covered = 0;   // (the vehicle's own hull)
            if (covered && n < 80) { memcpy(cand[n], pt, 12); score[n] = radii[r] + (myThreatD - threatD) * 0.5f; n++; }
            else if (!covered && no < 80) { memcpy(open[no], pt, 12); openScore[no] = radii[r]; no++; }   // nearest open ground
        }
    for (int pass = 0; pass < 2; pass++) {   // cover first; else open ground (lie prone)
        float (*cs)[3] = pass ? open : cand, *sc = pass ? openScore : score;
        int cn = pass ? no : n;
        for (int tries = 0; tries < 3 && cn > 0; tries++) {
            int best = 0;
            for (int k = 1; k < cn; k++) if (sc[k] < sc[best]) best = k;
            if (dist2(cs[best], me) < 1.5f * 1.5f) { memcpy(out, cs[best], 12); g_coverFoundOpen = pass; bot_nav(i)->mode = 1; return 1; }   // right here
            if (!plan_budget_left()) return 0;
            if (plan_path(i, soldier, cs[best][0], cs[best][1], cs[best][2])) { memcpy(out, cs[best], 12); g_coverFoundOpen = pass; bot_nav(i)->mode = 3; return 1; }
            cs[best][0] = cs[cn - 1][0]; cs[best][1] = cs[cn - 1][1]; cs[best][2] = cs[cn - 1][2]; sc[best] = sc[cn - 1];
            cn--;
        }
    }
    return 0;
}

// ---- Returning fire: on a hit the bot turns to the shooter and spots it. Vehicle shooter: engineers with rockets
// fire back (any health), everyone else takes cover. Infantry shooter: 65%+ health fight back; below that one burst,
// cover, then re-engage; hit twice in cover within 4 s -> leave cover and fight it out ----
#define FIGHT_HEALTH           65.0f
#define FIGHT_MAX_FRAMES       (20 * 30)
#define FIGHT_LOST_FRAMES      (3 * 30)
#define FIGHT_BURST_FRAMES     12        // 0.4 s trigger held
#define FIGHT_PAUSE_FRAMES     10        // 0.35 s between bursts
#define FIGHT_AIM_TOLERANCE    0.05f     // rad (~3 deg): widest allowed error, close in
#define FIGHT_AIM_MARGIN       0.35f     // m: fire once the aim is this close to the aim point
#define RECON_AIM_MARGIN       0.12f     // m: recon (snipers) wait for a much steadier aim
#define RECON_SHOT_FRAMES      3         // single shot
#define RECON_PAUSE_FRAMES     20        // ~0.75 s a shot at most (3 + 20 frames): faster spoils the aim
#define FIGHT_RANGE            120.0f
#define RECON_RANGE_FACTOR     1.5f      // recon see, engage and keep fighting infantry this much further out
static int is_recon(int i) { return strstr((char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, "Recon") != NULL; }
#define RECON_SPOTTED_RANGE    250.0f    // recon take on infantry a teammate has spotted out to here
// How far a bot keeps fighting a soldier: recon go further, much further for a spotted one.
static float fight_range(int i, char *t)
{
    if (!is_recon(i)) return FIGHT_RANGE;
    return spotted_cached(t) ? RECON_SPOTTED_RANGE + 20.0f : FIGHT_RANGE * RECON_RANGE_FACTOR;
}
#define FIGHT_AIM_HEIGHT       0.55f     // above the target's feet (0.9 and 1.2 still went high: the real camera sits above our 1.6 m eye)
// ...by the target's stance (soldier +0x2d0: 0 standing, 1 crouched, 2 prone): the same ~0.65 m the shots land high, taken off
// a crouched / prone body - bots could not hit the user lying prone (2026-10-13)
static float fight_aim_height(char *ts)
{
    int pose = ts && !IsBadReadPtr(ts + OFF_SOLDIER_POSE, 4) ? *(int *)(ts + OFF_SOLDIER_POSE) : 0;
    return pose == 2 ? -0.30f : pose == 1 ? 0.20f : FIGHT_AIM_HEIGHT;
}
#define FIGHT_CHASE_FRAMES     (6 * 30)
#define ENGAGE_COOLDOWN_FRAMES (10 * 30)  // before re-engaging the target just lost / killed
#define ENGAGE_BREATHER_FRAMES (2 * 30)   // before engaging anyone else
static char *g_cooldownWho[MAX_BOTS];  // advancing bots look for a target that went out of sight this long
#define FIGHT_ADVANCE_STOP     15.0f     // advancing bots stop closing in at this distance
static unsigned char g_fightAdvance[MAX_BOTS], g_fightChase[MAX_BOTS], g_fightSlot[MAX_BOTS];
unsigned char g_fineAim[MAX_BOTS];   // recon fighting: finer pitch control
static unsigned g_fightMoveUntil[MAX_BOTS], g_engageCooldown[MAX_BOTS];
static float g_fightLast[MAX_BOTS][3], g_fightSpot[MAX_BOTS][3];
static int soldier_ammo2(char *soldier, int slot, int *loaded, int *spare);
static int primary_empty(char *soldier) { int l, s; return soldier_ammo2(soldier, 0, &l, &s) && l + s == 0; }
static char *g_fightTarget[MAX_BOTS];
static unsigned char g_fightRetreat[MAX_BOTS];
static unsigned g_fightSeenAt[MAX_BOTS];
static int g_fightBursts[MAX_BOTS];
static unsigned g_coverHitAt[MAX_BOTS], g_coverHits[MAX_BOTS];
static int launcher_slot(int i);
static void start_rocket(int i, char *veh, int slot);
static int bot_spot(char *spotter, char *target);

// Leading moving targets: velocity per bot's current target, smoothed; aim where the target will be when the bullet
// arrives, plus a little for the bot's own turning lag.
#define BULLET_SPEED           600.0f
#define AIM_LAG_SECONDS        0.15f
static float g_leadPos[MAX_BOTS][3], g_leadVel[MAX_BOTS][3];
static void *g_leadWho[MAX_BOTS];
static unsigned g_leadAt[MAX_BOTS];
static void lead_point(int i, void *who, const float *tp, float dist, float *out)
{
    unsigned now = H->frame;
    if (g_leadWho[i] != who || now - g_leadAt[i] > 45) {
        g_leadWho[i] = who;
        memcpy(g_leadPos[i], tp, 12);
        memset(g_leadVel[i], 0, sizeof g_leadVel[i]);
        g_leadAt[i] = now;
    } else if (now != g_leadAt[i]) {
        // a player's position on the server moves in steps (network updates every few frames, not every frame): measure
        // between actual changes over the time they took - frame-to-frame it read 0 m/s, then a "teleport" jump
        float mx = tp[0] - g_leadPos[i][0], mz = tp[2] - g_leadPos[i][2];
        float dt = (now - g_leadAt[i]) / 30.0f;
        if (mx * mx + mz * mz > 0.0004f) {
            for (int k = 0; k < 3; k += 2) {   // horizontal only (jumps / terrain bumps would make it bob)
                float v = (tp[k] - g_leadPos[i][k]) / dt;
                if (fabsf(v) > 45.0f) v = g_leadVel[i][k];   // teleport / respawn
                g_leadVel[i][k] = 0.5f * g_leadVel[i][k] + 0.5f * v;
            }
            memcpy(g_leadPos[i], tp, 12);
            g_leadAt[i] = now;
        } else if (dt > 1.0f) {   // really standing still
            g_leadVel[i][0] = g_leadVel[i][2] = 0;
            g_leadAt[i] = now;
        }
    }
    float t = dist / BULLET_SPEED + AIM_LAG_SECONDS;
    out[0] = tp[0] + g_leadVel[i][0] * t;
    out[1] = tp[1];
    out[2] = tp[2] + g_leadVel[i][2] * t;
}

// A spot near the bot where its eyes see the target but something low in front hides its body from it.
static int find_firing_spot(int i, char *soldier, const float *teye, const float *taim, float *out)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return 0;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    int plans = 0;
    static const float radii[3] = {3.0f, 6.0f, 10.0f};
    for (int r = 0; r < 3; r++)
        for (int a = 0; a < 12; a++) {
            float ang = a * 0.5235988f + r * 0.26f, pt[3], hit[3];
            if (!nav_snap(g, me[0] + sinf(ang) * radii[r], me[1], me[2] + cosf(ang) * radii[r], pt)) continue;
            if (fabsf(pt[1] - me[1]) > 3.0f) continue;
            float eye[3] = {pt[0], pt[1] + 1.6f, pt[2]}, body[3] = {pt[0], pt[1] + 0.9f, pt[2]};
            if (!solid_between(teye, body, hit) || dist2(hit, pt) > 2.5f * 2.5f) continue;   // no low cover in front
            if (solid_between(eye, taim, NULL)) continue;                                   // cannot see it from there
            if (!plan_budget_left()) return 0;
            if (!plan_path(i, soldier, pt[0], pt[1], pt[2])) { if (++plans >= 3) return 0; continue; }   // A* is costly: 3 tries
            memcpy(out, pt, 12);
            return 1;
        }
    return 0;
}

static void start_fight(int i, char *target, int retreat, const char *why)
{
    BotBrain *b = bot_brain(i);
    if (b->task == TASK_REPAIR) { b->serviceTarget = NULL; b->task = b->prevTask; }   // drop the repair
    if (b->task == TASK_NONE || b->task == TASK_DEFEND || b->task == TASK_FOLLOW) { b->prevTask = b->task; b->prevCommander = b->commander; }
    b->task = TASK_FIGHT;
    b->step = 0;
    b->stepAt = 0;
    b->waiting = 0;
    b->taskUntil = H->frame + FIGHT_MAX_FRAMES;
    g_fightTarget[i] = target;
    g_fightRetreat[i] = (unsigned char)retreat;
    g_fightSeenAt[i] = H->frame;
    g_fightBursts[i] = 0;
    g_fightAdvance[i] = g_fightChase[i] = 0;
    g_fightMoveUntil[i] = 0;
    bot_nav(i)->mode = 1;
    bot_log("fight: %s fights %s (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, target + OFF_PLAYER_ONLINE_NAME, why);
}

// Take cover from `shooter` (its eye at `eye`) - or from the vehicle `veh` it is in. Returns 1 if the bot is on its way.
static float *vehicle_pos(char *veh, int *teamOut);
static const char *vehicle_name(char *veh);
static int take_cover(int i, char *soldier, char *shooter, char *veh, const float *eye, const char *why)
{
    BotBrain *b = bot_brain(i);
    float spot[3];
    if (veh) { float *vp = vehicle_pos(veh, NULL); if (!vp) return 0; memcpy(g_coverThreatPos, vp, 12); }
    else { g_coverThreatPos[0] = eye[0]; g_coverThreatPos[1] = eye[1] - 1.6f; g_coverThreatPos[2] = eye[2]; }
    g_coverThreatIsVeh = veh != NULL;
    if (!find_cover(i, soldier, eye, spot)) return 0;
    if (b->task == TASK_REPAIR) { press_button(i, SLOT_ACTION(0), 3); b->serviceTarget = NULL; b->task = b->prevTask; }
    if (b->task == TASK_NONE || b->task == TASK_DEFEND || b->task == TASK_FOLLOW) { b->prevTask = b->task; b->prevCommander = b->commander; }
    b->task = TASK_COVER;
    g_threat[i] = shooter;
    g_threatVeh[i] = veh;
    g_threatVehHidden[i] = 0;
    g_coverOpen[i] = (unsigned char)g_coverFoundOpen;
    g_coverProne[i] = 0;
    memcpy(b->supplyPos, spot, 12);
    b->step = 0;
    b->waiting = 0;
    b->enroute = 0;
    b->taskUntil = H->frame + COVER_MAX_FRAMES;
    g_coverHits[i] = 0;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    bot_log("cover: %s %s %.0f m away, %.0f m from %s%s%s%s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
            g_coverFoundOpen ? "gets clear (no cover near) - goes prone" : "takes cover", sqrtf(dist2(spot, me)), hdist(spot, g_coverThreatPos),
            veh ? "an enemy " : "", veh ? vehicle_name(veh) : shooter + OFF_PLAYER_ONLINE_NAME, why ? " - " : "", why ? why : "");
    return 1;
}
static int go_to_cover(int i, char *soldier, char *shooter, const float *eye) { return take_cover(i, soldier, shooter, NULL, eye, NULL); }

// ---- Taking cover after shooting back: when the exchange is going badly ----
#define COVER_BELOW_HEALTH     40.0f
static int g_fightHitsTaken[MAX_BOTS];        // hits taken since we last hurt our target
static float g_fightTgtHp[MAX_BOTS];
static void fight_note_target_health(int i)
{
    char *t = g_fightTarget[i], *ts = t && player_exists(t) ? *(char **)(t + OFF_PLAYER_SOLDIER) : NULL;
    g_fightTgtHp[i] = ts ? *(float *)(ts + OFF_SOLDIER_HEALTH) : 0;
}
static const char *cover_reason(int i, char *soldier, char *shooter)
{
    float h = *(float *)(soldier + OFF_SOLDIER_HEALTH);
    if (h < COVER_BELOW_HEALTH) return "badly hurt";
    {   // losing the exchange: hit again and again while the target is not getting hurt
        char *t = g_fightTarget[i], *ts = t && player_exists(t) ? *(char **)(t + OFF_PLAYER_SOLDIER) : NULL;
        float th = ts ? *(float *)(ts + OFF_SOLDIER_HEALTH) : 0;
        if (ts && th < g_fightTgtHp[i] - 0.5f) { g_fightTgtHp[i] = th; g_fightHitsTaken[i] = 0; }
        else if (g_fightHitsTaken[i] >= 3) return "losing the exchange";
    }
    {   // outnumbered: more than one enemy has been shooting from where it can see us
        char *pm = player_manager();
        void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
        unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION), chest[3] = {me[0], me[1] + 1.2f, me[2]};
        int shooters = 0;
        for (unsigned k = 0; k < count; k++) {
            char *e = arr[k], *es;
            if (!e || player_team(e) == player_team(H->bots[i]) || !(es = *(char **)(e + OFF_PLAYER_SOLDIER)) || !fired_recently(e)) continue;
            float *ep = (float *)(es + OFF_SOLDIER_SPAWN_POSITION), eye[3] = {ep[0], ep[1] + 1.6f, ep[2]};
            if (dist2(ep, me) > 100.0f * 100.0f || solid_between(eye, chest, NULL)) continue;
            if (++shooters >= 2) return "outnumbered";
        }
    }
    {   // magazine empty while being shot
        int l, s;
        if (soldier_ammo2(soldier, 0, &l, &s) && l == 0 && s > 0) return "reloading under fire";
    }
    (void)shooter;
    return NULL;
}

static void cover_react(int i, char *soldier)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i];
    float h = *(float *)(soldier + OFF_SOLDIER_HEALTH);
    int hit = g_lastHealth[i] > 0 && h < g_lastHealth[i] - 0.5f;
    g_lastHealth[i] = h;
    if (!hit || player_vehicle(pl)) return;
    g_hitAt[i] = H->frame;
    if (b->task == TASK_SUPPLY || b->task == TASK_REVIVE) return;   // finish those first
    float eye[3];
    char *shooter = likely_shooter(i, soldier, eye);
    if (!shooter) { bot_log("cover: %s hit (%.0f hp) but sees no shooter", pl + OFF_PLAYER_ONLINE_NAME, h); return; }
    // look that way and spot it
    {
        char *ss = *(char **)(shooter + OFF_PLAYER_SOLDIER);
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        if (ss) { float *tp = (float *)(ss + OFF_SOLDIER_SPAWN_POSITION); bot_nav(i)->targetYaw = yaw_towards(tp[0] - me[0], tp[2] - me[2]); }
        static unsigned spottedAt[MAX_BOTS];
        if (H->frame - spottedAt[i] > 90) { spottedAt[i] = H->frame; bot_spot(pl, shooter); }
    }
    char *sveh = player_vehicle(shooter);
    if (sveh) {   // shot by a vehicle
        int slot = launcher_slot(i);
        if (slot >= 0 && !(bot_ammo(i, slot) == 0 && g_rocketsThisLife[i] > 0)) {
            if (b->task != TASK_ROCKET) start_rocket(i, sveh, slot);
        } else if (b->task != TASK_COVER) go_to_cover(i, soldier, shooter, eye);
        return;
    }
    if (b->task == TASK_ROCKET) return;   // busy with a vehicle
    {
        char *ss = *(char **)(shooter + OFF_PLAYER_SOLDIER);
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        if (ss) {
            float *tp = (float *)(ss + OFF_SOLDIER_SPAWN_POSITION), dx = tp[0] - me[0], dz = tp[2] - me[2], d = sqrtf(dx * dx + dz * dz);
            if (d > fight_range(i, shooter)) {
                if (b->task != TASK_COVER && b->task != TASK_FIGHT && go_to_cover(i, soldier, shooter, eye))
                    bot_log("cover: %s hit by %s from %.0f m - out of reach, takes cover", pl + OFF_PLAYER_ONLINE_NAME, shooter + OFF_PLAYER_ONLINE_NAME, d);
                return;
            }
        }
    }
    if (b->task == TASK_COVER) {
        if (b->step == 0) return;   // still running for it
        if (H->frame - g_coverHitAt[i] > 4 * 30) g_coverHits[i] = 0;
        g_coverHitAt[i] = H->frame;
        if (++g_coverHits[i] >= 2) start_fight(i, shooter, 0, "cover is not working");
        return;
    }
    if (b->task != TASK_FIGHT) {   // always shoot back first
        start_fight(i, shooter, 0, "returning fire");
        g_fightHitsTaken[i] = 1;
        fight_note_target_health(i);
        return;
    }
    if (g_fightTarget[i] != shooter) {   // already fighting: turn on whoever is shooting us now
        g_fightTarget[i] = shooter; g_fightSeenAt[i] = H->frame; g_fightHitsTaken[i] = 0;
        fight_note_target_health(i);
    }
    g_fightHitsTaken[i]++;
    const char *why = cover_reason(i, soldier, shooter);
    if (why) {
        float eye[3];
        char *ss = *(char **)(shooter + OFF_PLAYER_SOLDIER);
        if (ss) {
            float *sp = (float *)(ss + OFF_SOLDIER_SPAWN_POSITION);
            eye[0] = sp[0]; eye[1] = sp[1] + 1.6f; eye[2] = sp[2];
            if (go_to_cover(i, soldier, shooter, eye)) bot_log("cover: %s breaks for cover (%s)", pl + OFF_PLAYER_ONLINE_NAME, why);
        }
    }
}

static void end_cover(int i, BotBrain *b, const char *why)
{
    char *t = g_threat[i];
    if (g_threatVeh[i]) { g_threatVeh[i] = NULL; g_vehThreatCooldown[i] = H->frame + 5 * 30; }
    g_coverProne[i] = 0;   // (prone in the open: the stance check jumps it back up)
    bot_stand(i);          // (crouched behind cover)
    g_coverOpen[i] = 0;
    b->step = 0;
    b->waiting = 0;
    g_threat[i] = NULL;
    if (t && player_exists(t) && *(char **)(t + OFF_PLAYER_SOLDIER) && !player_vehicle(t) && player_team(t) != player_team(H->bots[i])) {
        char *s = *(char **)(H->bots[i] + OFF_PLAYER_SOLDIER);
        if (s) {   // quiet now: re-engage the one who drove us here (one burst and back to cover again if still hurt)
            float h = *(float *)(s + OFF_SOLDIER_HEALTH);
            (void)h;
            start_fight(i, t, 0, "re-engaging from cover");
            return;
        }
    }
    if (b->prevTask == TASK_DEFEND || (b->prevTask == TASK_FOLLOW && b->prevCommander)) {
        bot_log("cover: %s leaves cover (%s), back to %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why, kTaskNames[b->prevTask]);
        b->task = b->prevTask;
        b->commander = b->prevCommander;
        b->prevTask = TASK_NONE;
        b->prevCommander = NULL;
        b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
        b->replanAt = 0;
        bot_nav(i)->mode = 0;
        return;
    }
    bot_log("cover: %s leaves cover (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    end_task(i, b, why);
}

static void end_fight(int i, BotBrain *b, const char *why)
{
    char *lastTarget = g_fightTarget[i];
    g_fightTarget[i] = NULL;
    g_fightMoveUntil[i] = 0;
    g_cooldownWho[i] = lastTarget;   // cool off on this one; others after a short breather
    g_engageCooldown[i] = H->frame + ENGAGE_COOLDOWN_FRAMES;
    b->step = 0;
    if (b->prevTask == TASK_DEFEND || (b->prevTask == TASK_FOLLOW && b->prevCommander)) {
        bot_log("fight: %s stops (%s), back to %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why, kTaskNames[b->prevTask]);
        b->task = b->prevTask;
        b->commander = b->prevCommander;
        b->prevTask = TASK_NONE;
        b->prevCommander = NULL;
        b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
        b->replanAt = 0;
        bot_nav(i)->mode = 0;
        return;
    }
    bot_log("fight: %s stops (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    end_task(i, b, why);
}

// step 0: rifle out; 1: aiming; 2: burst (trigger held by botmod_tick); 3: pause
static int fight_tick(int i, BotBrain *b, char *soldier)
{
    char *t = g_fightTarget[i];
    if (H->frame > b->taskUntil) { end_fight(i, b, "long enough"); return 0; }
    if (!t || !player_exists(t)) { end_fight(i, b, "target left"); return 0; }
    char *ts = *(char **)(t + OFF_PLAYER_SOLDIER);
    if (!ts || *(float *)(ts + OFF_SOLDIER_HEALTH) <= 0) { end_fight(i, b, "target down"); return 0; }
    if (player_vehicle(t)) { end_fight(i, b, "target got in a vehicle"); return 0; }
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
    float ah = fight_aim_height(ts);
    float eye[3] = {me[0], me[1] + 1.6f, me[2]}, aim[3] = {tp[0], tp[1] + ah, tp[2]}, hit[3];
    float vis[3] = {tp[0], tp[1] + fmaxf(ah, 0.35f), tp[2]};   // (the sight test on the body, not the low aim point in the dirt)
    float dx = aim[0] - eye[0], dz = aim[2] - eye[2], dh = sqrtf(dx * dx + dz * dz);
    if (dh > fight_range(i, t)) { end_fight(i, b, "out of range"); return 0; }
    int visible = !solid_between(eye, vis, hit) || (g_testVictim && t == g_testVictim);   // test: fire regardless
    if (visible) { g_fightSeenAt[i] = H->frame; memcpy(g_fightLast[i], tp, 12); g_fightChase[i] = 0; }
    else if (H->frame - g_fightSeenAt[i] > (g_fightAdvance[i] ? FIGHT_CHASE_FRAMES : FIGHT_LOST_FRAMES)) { end_fight(i, b, "lost sight"); return 0; }
    {   // lead a moving target
        float lp[3];
        lead_point(i, t, tp, dh, lp);
        aim[0] = lp[0]; aim[2] = lp[2];
        float tf = dh / BULLET_SPEED;
        aim[1] += 0.5f * 15.0f * tf * tf;   // bullet drop: BF3 small-arms rounds fall at 15 m/s^2 (read from the weapons' data), ~1.3 m at 250 m
        dx = aim[0] - eye[0]; dz = aim[2] - eye[2]; dh = sqrtf(dx * dx + dz * dz);
    }
    BotNav *nav = bot_nav(i);
    if (g_fightMoveUntil[i]) {   // recon: into the firing position first
        if (sqrtf(dist2(me, g_fightSpot[i])) < 1.2f || nav->mode == 1 || H->frame > g_fightMoveUntil[i]) {
            g_fightMoveUntil[i] = 0;
            bot_log("fight: %s in its firing position", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
        } else return 1;
    }
    if (visible || !g_fightAdvance[i]) {
        if (g_fightAdvance[i] && dh > FIGHT_ADVANCE_STOP) { nav->mode = 4; nav->x = tp[0]; nav->z = tp[2]; }   // close in, facing it
        else nav->mode = 1;
        nav->targetYaw = yaw_towards(dx, dz);
    } else if (!g_fightChase[i]) {   // advancing and it went out of sight: go to where it was last seen
        g_fightChase[i] = 1;
        go_towards(i, soldier, g_fightLast[i][0], g_fightLast[i][1], g_fightLast[i][2]);
        bot_log("fight: %s lost sight of %s, going after it", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, t + OFF_PLAYER_ONLINE_NAME);
    }
    g_aimPitch[i] = atan2f(aim[1] - eye[1], dh);
    if (b->step == 0) {
        g_fightSlot[i] = primary_empty(soldier) ? 1 : 0;   // sidearm when the primary is dry
        press_button(i, SLOT_ACTION(g_fightSlot[i]), 3); b->step = 1; b->stepAt = H->frame + 15; return 1;
    }
    if (H->frame - g_hitAt[i] < 3 * 30 && !g_fightMoveUntil[i]) {
        int l, s;
        if (soldier_ammo2(soldier, 0, &l, &s) && l == 0 && s > 0) {
            float teye[3] = {tp[0], tp[1] + 1.6f, tp[2]};
            if (go_to_cover(i, soldier, t, teye)) { bot_log("cover: %s breaks for cover (reloading under fire)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME); return 1; }
        }
    }
    if (H->frame < b->stepAt) return 1;
    if (b->step == 2) {   // burst done
        g_fightBursts[i]++;
        if (g_fightSlot[i] == 0 && primary_empty(soldier)) {
            g_fightSlot[i] = 1;
            press_button(i, SLOT_ACTION(1), 3);
            bot_log("fight: %s is out of primary ammo, switching to its sidearm", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
            b->step = 3; b->stepAt = H->frame + 20;
            return 1;
        }
        if (g_fightRetreat[i]) {   // hurt: that was the burst - now cover (fight on if there is none)
            float teye[3] = {tp[0], tp[1] + 1.6f, tp[2]};
            g_fightRetreat[i] = 0;
            b->task = b->prevTask == TASK_FIGHT ? TASK_NONE : b->task;
            if (go_to_cover(i, soldier, t, teye)) return 1;
            b->task = TASK_FIGHT;
        }
        b->step = 3;
        b->stepAt = H->frame + (strstr((char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, "Recon") ? RECON_PAUSE_FRAMES : FIGHT_PAUSE_FRAMES);
        return 1;
    }
    if (b->step == 3) b->step = 1;
    // aiming: burst once on target and visible
    float yaw = *(float *)(soldier + OFF_SOLDIER_YAW), ty = fmodf(bot_nav(i)->targetYaw, 6.2831853f);
    if (ty > 0) ty -= 6.2831853f;
    float yerr = fabsf(ty - yaw);
    if (yerr > 3.14159265f) yerr = 6.2831853f - yerr;
    float perr = fabsf(g_aimPitch[i] - *(float *)(soldier + OFF_SOLDIER_PITCH));
    if (g_testVictim && H->frame % 30 == 0)
        bot_log("fight: %s aim yaw err %.3f pitch err %.3f (want %.3f have %.3f) visible %d dist %.1f step %d", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                yerr, perr, g_aimPitch[i], *(float *)(soldier + OFF_SOLDIER_PITCH), visible, dh, b->step);
    int recon = strstr((char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, "Recon") != NULL;
    float tol = atanf((recon ? RECON_AIM_MARGIN : FIGHT_AIM_MARGIN) / fmaxf(dh, 1.0f));
    tol = fminf(tol, recon ? 0.03f : FIGHT_AIM_TOLERANCE);
    tol = fmaxf(tol, recon ? 0.003f : 0.008f);   // what the turn / pitch control can hold
    g_fineAim[i] = recon;
    if (visible && yerr < tol && perr < tol) {
        b->step = 2;
        b->stepAt = H->frame + (recon ? RECON_SHOT_FRAMES : FIGHT_BURST_FRAMES);
    }
    return 1;
}

static int cover_tick(int i, BotBrain *b, char *soldier)
{
    if (H->frame > b->taskUntil) { end_cover(i, b, "took too long"); return 0; }
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float d = sqrtf(dist2(me, b->supplyPos));
    if (b->step == 0) {
        g_sprintUntil[i] = H->frame + 4;   // sprint (the input flag bits) while running for cover
        if (H->frame % 30 == 0)
            bot_log("cover: %s running, %.1f m to go, nav mode %d, move %.1f/%.1f", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, d,
                    bot_nav(i)->mode, H->botMove[i][0], H->botMove[i][2]);
        if (d < 1.2f || bot_nav(i)->mode == 1) {
            b->step = 1;
            if (g_coverOpen[i] && !g_coverProne[i]) { press_button(i, EIA_PRONE, 3); g_coverProne[i] = 1; }   // no cover here: get low
            else if (!g_coverOpen[i]) bot_crouch(i);                                                         // behind cover: crouch
            bot_log("cover: %s in %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, g_coverOpen[i] ? "the open, prone" : "cover");
        }
        return 1;
    }
    // in cover: face the threat, stay until quiet
    char *t = g_threat[i];
    char *ts = t && player_exists(t) ? *(char **)(t + OFF_PLAYER_SOLDIER) : NULL;
    bot_nav(i)->mode = 1;
    if (ts) { float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION); bot_nav(i)->targetYaw = yaw_towards(tp[0] - me[0], tp[2] - me[2]); }
    if (d > 3.0f && !g_coverProne[i]) { b->step = 0; go_towards(i, soldier, b->supplyPos[0], b->supplyPos[1], b->supplyPos[2]); }   // pushed out
    if (g_threatVeh[i]) {   // from a vehicle: until it is destroyed / empty / beyond range / out of sight for 10 s
        const char *gone = vehicle_threat_gone(i, me);
        if (gone) { end_cover(i, b, gone); return 0; }
        return 1;
    }
    if (!ts || *(float *)(ts + OFF_SOLDIER_HEALTH) <= 0) { end_cover(i, b, "threat down"); return 0; }
    if (H->frame - g_hitAt[i] > COVER_HOLD_FRAMES) { end_cover(i, b, "quiet"); return 0; }
    return 1;
}

// ---- Medics revive downed teammates (last 10 s, within ~10 s walk): defib out, look down, hold the trigger ----
static void revive_scan(int i, char *soldier)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i];
    if ((b->task != TASK_NONE && b->task != TASK_DEFEND && b->task != TASK_FOLLOW) || player_vehicle(pl)) return;
    int slot = bot_gadget_slot(i, "Defib", NULL);
    if (slot < 0) return;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), bestD = REVIVE_RANGE;
    Downed *best = NULL;
    for (int d = 0; d < 64; d++) {
        Downed *e = &g_downed[d];
        if (!e->player || e->player == pl || e->team != player_team(pl) || e->medic || H->frame - e->at > DOWNED_WINDOW_FRAMES - 60) continue;
        float dx = e->pos[0] - me[0], dz = e->pos[2] - me[2], dist = sqrtf(dx * dx + dz * dz);
        if (dist < bestD) { bestD = dist; best = e; }
    }
    if (!best) return;
    best->medic = i + 1;
    b->prevTask = b->task;
    b->prevCommander = b->commander;
    b->task = TASK_REVIVE;
    b->serviceTarget = best->player;
    memcpy(b->supplyPos, best->pos, 12);
    b->supplySlot = slot;
    b->step = 0;
    b->stepAt = 0;
    b->replanAt = 0;
    b->waiting = 0;
    b->taskUntil = best->at + DOWNED_WINDOW_FRAMES + 30;
    bot_log("brain: %s runs to revive %s (%.0f m, down %.1f s ago)", pl + OFF_PLAYER_ONLINE_NAME, best->player + OFF_PLAYER_ONLINE_NAME,
            bestD, (H->frame - best->at) / 30.0f);
}

static void end_revive(int i, BotBrain *b, const char *why)
{
    Downed *e = b->serviceTarget && player_exists(b->serviceTarget) ? downed_of(b->serviceTarget) : NULL;
    if (e && e->medic == i + 1) e->medic = 0;
    b->step = 0;
    b->waiting = 0;
    b->stepAt = 0;
    press_button(i, SLOT_ACTION(0), 3);   // back to the primary weapon
    b->serviceTarget = NULL;
    if (b->prevTask == TASK_DEFEND || (b->prevTask == TASK_FOLLOW && b->prevCommander)) {
        bot_log("brain: %s stops reviving (%s), back to %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why, kTaskNames[b->prevTask]);
        b->task = b->prevTask;
        b->commander = b->prevCommander;
        b->prevTask = TASK_NONE;
        b->prevCommander = NULL;
        b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
        b->replanAt = 0;
        b->crouched = 0;
        bot_nav(i)->mode = 0;
        return;
    }
    end_task(i, b, why);
}

// step: 0 walking, 1 defib coming out, 2 trigger held (charging), 3 released (shock), back to 2 until the window ends
static int revive_tick(int i, BotBrain *b, char *soldier)
{
    char *target = b->serviceTarget;
    if (!target || !player_exists(target)) { b->serviceTarget = NULL; end_revive(i, b, "player left"); return 0; }
    char *ts = *(char **)(target + OFF_PLAYER_SOLDIER);
    if (ts) {   // back up: revived here, or redeployed somewhere else
        int revived = came_back_by_revive(target, ts, b->supplyPos);
        bot_log("brain: %s: %s %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, target + OFF_PLAYER_ONLINE_NAME, revived ? "revived!" : "redeployed");
        end_revive(i, b, revived ? "revived" : "they redeployed");
        return 0;
    }
    Downed *dn = downed_of(target);
    if (H->frame > b->taskUntil || !dn) { end_revive(i, b, "too late"); return 0; }
    memcpy(b->supplyPos, dn->pos, 12);   // the ragdoll may still be sliding
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float dx = b->supplyPos[0] - me[0], dz = b->supplyPos[2] - me[2], d = sqrtf(dx * dx + dz * dz);
    b->waiting = d <= REVIVE_REACH + 0.7f;
    if (!b->waiting) {
        if (d > 4.0f) {
            if (H->frame >= b->replanAt || bot_nav(i)->mode == 1 || bot_nav(i)->mode == 4) {
                b->replanAt = H->frame + 60;
                go_towards(i, soldier, b->supplyPos[0], b->supplyPos[1], b->supplyPos[2]);
            }
        } else { bot_nav(i)->mode = 4; bot_nav(i)->x = b->supplyPos[0]; bot_nav(i)->z = b->supplyPos[2]; }   // last metres: straight in
        return 1;
    }
    bot_nav(i)->mode = 1;
    bot_nav(i)->targetYaw = yaw_towards(dx, dz);
    if (b->step == 0) {
        press_button(i, SLOT_ACTION(b->supplySlot), 3);   // defibrillator out
        b->step = 1;
        b->stepAt = H->frame + 35;
        bot_log("brain: %s at %s's body, defib out", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, target + OFF_PLAYER_ONLINE_NAME);
    } else if (H->frame >= b->stepAt) {
        if (b->step == 1 || b->step == 3) { b->step = 2; b->stepAt = H->frame + 60; }   // hold ~2 s (charge)
        else { b->step = 3; b->stepAt = H->frame + 20; }                                  // release: shock
    }
    return 1;
}

// ---- Engineers repair damaged, stationary friendly vehicles they can see ----
// Vehicles are found through the players sitting in them (player+0x3d8); position = an occupant's seat transform.
#define OFF_VEHICLE_HEALTH         0x128       // float, current health (tank 1000)
#define OFF_VEHDATA_MAX_HEALTH     0x11c       // float in the vehicle's data (vehicle+0x14)
#define REPAIR_SCAN_RANGE          25.0f       // every engineer this close (with line of sight) helps
#define REPAIR_STANDOFF            2.5f
#define REPAIR_MAX_FRAMES          (60 * 30)
#define REPAIR_CONTACT_DIST        6.0f        // from the vehicle centre: push in and hold the trigger
#define REPAIR_PITCH               -0.44f      // look ~25 degrees down at the hull
typedef struct VehTrack { char *veh; float pos[3]; unsigned at; float speed; float maxSeen; } VehTrack;
static VehTrack g_vehTrack[32];
static unsigned g_repairVt[MAX_BOTS];

static int vehicle_alive(char *veh, unsigned vt)
{
    return veh && !IsBadReadPtr(veh, OFF_VEHICLE_HEALTH + 4) && *(unsigned *)veh == vt;
}

// Vehicle body position (vehicle+0x230 -> body entity, translation at +0x100) and owning team (vehicle+0xd8:
// 1 US, 2 RU, 0 never used). NULL when the vehicle is not placed in the world.
#define OFF_VEHICLE_BODY           0x230
#define OFF_VEHBODY_POSITION       0x100
#define OFF_VEHICLE_TEAM           0xd8
// Seat position of someone inside the vehicle (follows the turret), NULL if it is empty.
static float *vehicle_occupant_pos(char *veh)
{
    char *pm = player_manager();
    if (!pm) return NULL;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count; k++) {
        char *pl = arr[k], *s;
        if (pl && *(char **)(pl + OFF_PLAYER_VEHICLE) == veh && (s = *(char **)(pl + OFF_PLAYER_SOLDIER)))
            return (float *)(s + OFF_SOLDIER_SPAWN_POSITION);
    }
    return NULL;
}

// Body position when it is trustworthy (it does not swing with the turret); for some vehicles that field is
// elsewhere, so an occupied vehicle whose body point is > 10 m from its crew uses the crew's seat instead.
static float *vehicle_pos(char *veh, int *team)
{
    if (!veh || IsBadReadPtr(veh, OFF_VEHICLE_BODY + 4)) return NULL;
    if (team) *team = *(int *)(veh + OFF_VEHICLE_TEAM);
    {   // the physics entity's transform (+0x30 -> +0x88, translation +0x30): live every frame, where the body position
        // (+0x230 -> +0x100) only updates every second or two while driving (2026-10-12: a T-90 read as standing still at
        // full throttle, its stuck logic reversing blind into the same pylon)
        char *phys = !IsBadReadPtr(veh + 0x30, 4) ? *(char **)(veh + 0x30) : NULL;
        char *xf = phys && !IsBadReadPtr(phys + 0x88, 4) ? *(char **)(phys + 0x88) : NULL;
        if (xf && !IsBadReadPtr(xf + 0x30, 12)) {
            float *t = (float *)(xf + 0x30);
            if ((t[0] != 0 || t[2] != 0) && isfinite(t[0]) && isfinite(t[1]) && isfinite(t[2]) && fabsf(t[0]) < 20000.0f && fabsf(t[2]) < 20000.0f) return t;
        }
    }
    float *seat = vehicle_occupant_pos(veh), *p = NULL;
    char *body = *(char **)(veh + OFF_VEHICLE_BODY);
    if (body && !IsBadReadPtr(body + OFF_VEHBODY_POSITION, 12)) {
        p = (float *)(body + OFF_VEHBODY_POSITION);
        if (p[0] == 0 && p[1] == 0 && p[2] == 0) p = NULL;
    }
    if (seat && (!p || (seat[0] - p[0]) * (seat[0] - p[0]) + (seat[2] - p[2]) * (seat[2] - p[2]) > 10.0f * 10.0f)) return seat;
    if (!p) {   // parked (nobody in it, body not simulated): the physics entity's transform (+0x30 -> +0x88 -> translation)
        char *phys = !IsBadReadPtr(veh + 0x30, 4) ? *(char **)(veh + 0x30) : NULL;
        char *xf = phys && !IsBadReadPtr(phys + 0x88, 4) ? *(char **)(phys + 0x88) : NULL;
        if (xf && !IsBadReadPtr(xf + 0x30, 12)) {
            float *t = (float *)(xf + 0x30);
            if (t[0] != 0 || t[2] != 0) p = t;
        }
    }
    return p;
}

static VehTrack *vehicle_track(char *veh)
{
    VehTrack *free = NULL;
    for (int k = 0; k < 32; k++) {
        if (g_vehTrack[k].veh == veh) return &g_vehTrack[k];
        if (!free && (!g_vehTrack[k].veh || H->frame - g_vehTrack[k].at > 300)) free = &g_vehTrack[k];
    }
    if (free) { memset(free, 0, sizeof *free); free->veh = veh; }
    return free;
}

// Samples speed (m/s, over >= 0.5 s) and max health. Returns the track or NULL.
static VehTrack *vehicle_sample(char *veh)
{
    VehTrack *t = vehicle_track(veh);
    float *p = vehicle_pos(veh, NULL);
    if (!t || !p) return t;
    float h = *(float *)(veh + OFF_VEHICLE_HEALTH);
    char *data = *(char **)(veh + 0x14);
    float mx = data && !IsBadReadPtr(data + OFF_VEHDATA_MAX_HEALTH, 4) ? *(float *)(data + OFF_VEHDATA_MAX_HEALTH) : 0;
    if (mx < 1.0f || mx > 100000.0f) mx = 0;
    if (mx > t->maxSeen) t->maxSeen = mx;
    if (h > t->maxSeen) t->maxSeen = h;
    if (!t->at) { memcpy(t->pos, p, 12); t->at = H->frame; t->speed = 0; }
    else if (H->frame - t->at >= 15) {
        float dx = p[0] - t->pos[0], dy = p[1] - t->pos[1], dz = p[2] - t->pos[2];
        t->speed = sqrtf(dx * dx + dy * dy + dz * dz) / ((H->frame - t->at) / 30.0f);
        memcpy(t->pos, p, 12);
        t->at = H->frame;
    }
    return t;
}

// Sends engineer i to repair veh with the repair tool in `slot`; its current order (defend / follow) resumes afterwards.
static void start_repair(int i, char *veh, int slot)
{
    BotBrain *b = bot_brain(i);
    float *vp = vehicle_pos(veh, NULL);
    if (!vp) return;
    b->prevTask = b->task;            // resume this afterwards (like a supply run)
    b->prevCommander = b->commander;
    b->task = TASK_REPAIR;
    b->serviceTarget = veh;
    memcpy(b->supplyPos, vp, 12);
    b->supplySlot = slot;
    b->step = 0;
    b->stepAt = 0;
    b->replanAt = 0;
    b->taskUntil = H->frame + REPAIR_MAX_FRAMES;
    b->enroute = b->waiting = 0;
    g_repairVt[i] = *(unsigned *)veh;
    VehTrack *t = vehicle_track(veh);
    bot_log("brain: %s goes to repair a vehicle %.0f m away (health %.0f / %.0f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
            bot_distance_to(i, vp), *(float *)(veh + OFF_VEHICLE_HEALTH), t ? t->maxSeen : 0.0f);
}

static void repair_scan(int i, char *soldier)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i];
    // free engineers, and ones holding a post / following someone (they go back to it afterwards)
    if ((b->task != TASK_NONE && b->task != TASK_DEFEND && b->task != TASK_FOLLOW) || player_vehicle(pl)) return;
    int slot = bot_gadget_slot(i, "Repairtool", "RepairTool");
    if (slot < 0) return;
    char **vehs;
    int nveh = vehicle_list(&vehs);
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), bestD = REPAIR_SCAN_RANGE;
    char *best = NULL;
    for (int k = 0; k < nveh; k++) {
        char *veh = vehs[k];
        int vteam = 0;
        if (IsBadReadPtr(veh, 4) || *(unsigned *)veh != 0x025bf8e8) continue;   // still a ServerVehicleEntity
        float *vp = vehicle_pos(veh, &vteam);
        if (!vp || vteam != player_team(pl)) continue;
        VehTrack *t = vehicle_sample(veh);
        if (!t || !t->maxSeen) continue;
        float h = *(float *)(veh + OFF_VEHICLE_HEALTH);
        if (h <= 0 || h >= 0.99f * t->maxSeen) continue;
        float dx = vp[0] - me[0], dy = vp[1] - me[1], dz = vp[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        // damaged friendly vehicle: say why it is skipped (at most every 5 s per engineer)
        const char *why = t->speed > 0.5f ? "moving" : d >= REPAIR_SCAN_RANGE ? "too far" : NULL;
        static unsigned whyAt[MAX_BOTS];
        if (why) {
            if (H->frame - whyAt[i] > 150 && d < 2 * REPAIR_SCAN_RANGE) {
                whyAt[i] = H->frame;
                bot_log("repair: %s skips vehicle %p (%.0f / %.0f, %.0f m, %.1f m/s): %s", pl + OFF_PLAYER_ONLINE_NAME, veh, h, t->maxSeen, d,
                        t->speed, why);
            }
            continue;
        }
        if (d >= bestD) continue;
        float eye[3] = {me[0], me[1] + 1.6f, me[2]}, aim[3] = {vp[0], vp[1] + 0.5f, vp[2]}, hit[3];
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;
        int blocked = ray_cast(eye, aim, hit, NULL);
        g_rayFlags = saved;
        if (blocked) {   // the ray may stop on the vehicle's own hull
            float hx = hit[0] - aim[0], hy = hit[1] - aim[1], hz = hit[2] - aim[2];
            if (hx * hx + hy * hy + hz * hz > 6.0f * 6.0f) {
                static unsigned losAt[MAX_BOTS];
                if (H->frame - losAt[i] > 150) {
                    losAt[i] = H->frame;
                    bot_log("repair: %s skips vehicle %p (%.0f m): no line of sight (ray stopped %.1f m from it)", pl + OFF_PLAYER_ONLINE_NAME,
                            veh, d, sqrtf(hx * hx + hy * hy + hz * hz));
                }
                continue;
            }
        }
        bestD = d;
        best = veh;
    }
    if (!best) return;
    start_repair(i, best, slot);
}

static void end_repair(int i, BotBrain *b, const char *why)
{
    b->step = 0;
    b->waiting = 0;
    b->stepAt = 0;
    press_button(i, SLOT_ACTION(0), 3);   // back to the primary weapon
    b->serviceTarget = NULL;
    if (b->prevTask == TASK_DEFEND || (b->prevTask == TASK_FOLLOW && b->prevCommander)) {
        bot_log("brain: %s stops repairing (%s), back to %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why, kTaskNames[b->prevTask]);
        b->task = b->prevTask;
        b->commander = b->prevCommander;
        b->prevTask = TASK_NONE;
        b->prevCommander = NULL;
        b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
        b->replanAt = 0;
        b->crouched = 0;
        bot_nav(i)->mode = 0;
        return;
    }
    end_task(i, b, why);
}

// ---- Engineers fire rockets at enemy ground vehicles they can see (while they have rockets) ----
#define ROCKET_RANGE           150.0f
#define ROCKET_SPEED           200.0f    // m/s, for leading moving targets
#define ROCKET_MAX_SHOTS       3
#define ROCKET_AIM_TOLERANCE   0.035f    // rad (~2 deg) yaw and pitch before firing
#define ROCKET_RELOAD_FRAMES   (4 * 30)
#define ROCKET_HULL_HEIGHT     0.7f      // aim this high above the ground under the vehicle (1.0 hit BMP turrets)
#define ROCKET_DROP_START      80.0f     // m: beyond this, aim a little higher for rocket drop
static float g_aimPitch[MAX_BOTS];
static int g_rocketShots[MAX_BOTS];
static unsigned g_rocketVt[MAX_BOTS];

static int vehicle_occupied(char *veh)
{
    char *pm = player_manager();
    if (!pm) return 0;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count; k++) if (arr[k] && *(char **)((char *)arr[k] + OFF_PLAYER_VEHICLE) == veh) return 1;
    return 0;
}

// On the ground (a walkable grid node within 3 m under it): not a flying helicopter or jet.
static int vehicle_on_ground(const float *vp)
{
    NavGrid *g = *nav_slot();
    float gp[3];
    return !g || g->phase != 2 || nav_snap(g, vp[0], vp[1], vp[2], gp);
}

static int launcher_slot(int i)
{
    int s = bot_gadget_slot(i, "SMAW", "RPG7");
    return s;
}

static unsigned g_rocketCooldown[MAX_BOTS];
static int g_rocketsThisLife[MAX_BOTS];
// ---- Bots ask for help themselves: "Need ammo" when out of rockets, "Need medic" under half health ----
#define SELF_REQUEST_COOLDOWN  (30 * 30)
#define SELF_HEAL_BELOW        50.0f
static void order_reaction(char *sender, int type, char *target);
static void self_requests(int i, char *soldier)
{
    static unsigned ammoAt[MAX_BOTS], healAt[MAX_BOTS];
    char *pl = H->bots[i];
    if (player_vehicle(pl)) return;
    int slot = launcher_slot(i), pl0, ps0;
    if (soldier_ammo2(soldier, 0, &pl0, &ps0) && ps0 == 0 && g_spareMax[i] > 0 && H->frame - ammoAt[i] > SELF_REQUEST_COOLDOWN) {
        ammoAt[i] = H->frame;
        bot_log("brain: %s has no spare magazines (%d in the gun) - Need ammo", pl + OFF_PLAYER_ONLINE_NAME, pl0);
        bot_say(i, ORDER_REQUESTAMMO);
        order_reaction(pl, ORDER_REQUESTAMMO, NULL);
    }
    if (slot >= 0 && g_rocketsThisLife[i] > 0 && bot_ammo(i, slot) == 0 && H->frame - ammoAt[i] > SELF_REQUEST_COOLDOWN) {
        ammoAt[i] = H->frame;
        bot_log("brain: %s is out of rockets - Need ammo", pl + OFF_PLAYER_ONLINE_NAME);
        bot_say(i, ORDER_REQUESTAMMO);
        order_reaction(pl, ORDER_REQUESTAMMO, NULL);   // other bots answer like they do for players
    }
    float h = *(float *)(soldier + OFF_SOLDIER_HEALTH);
    if (h > 0 && h < SELF_HEAL_BELOW && H->frame - healAt[i] > SELF_REQUEST_COOLDOWN) {
        healAt[i] = H->frame;
        bot_log("brain: %s is hurt (%.0f hp) - Need medic", pl + OFF_PLAYER_ONLINE_NAME, h);
        bot_say(i, ORDER_REQUESTHEAL);
        order_reaction(pl, ORDER_REQUESTHEAL, NULL);
    }
}

static int is_entity_spotted(char *ent);
static void rocket_scan(int i, char *soldier)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i];
    if ((b->task != TASK_NONE && b->task != TASK_DEFEND && b->task != TASK_FOLLOW) || player_vehicle(pl)) return;
    if (H->frame < g_rocketCooldown[i]) return;
    int slot = launcher_slot(i);
    if (slot < 0 || (bot_ammo(i, slot) == 0 && g_rocketsThisLife[i] > 0)) return;   // 0 is only trusted once it has fired
    char **vehs;
    int nveh = vehicle_list(&vehs);
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), bestD = ROCKET_RANGE;
    char *best = NULL;
    for (int k = 0; k < nveh; k++) {
        char *veh = vehs[k];
        int vteam = 0;
        if (IsBadReadPtr(veh, 4) || *(unsigned *)veh != 0x025bf8e8) continue;
        float *vp = vehicle_pos(veh, &vteam);
        if (!vp || !vteam || vteam == player_team(pl) || *(float *)(veh + OFF_VEHICLE_HEALTH) <= 0 || !vehicle_occupied(veh)) continue;
        float d = sqrtf(dist2(vp, me));
        if (d >= bestD || !vehicle_on_ground(vp) || !is_entity_spotted(veh)) continue;   // identified (spotted) first
        float eye[3] = {me[0], me[1] + 1.6f, me[2]}, aim[3] = {vp[0], vp[1] + 1.0f, vp[2]}, hit[3];
        if (ray_blocked_char(eye, aim, hit) && dist2(hit, aim) > 4.0f * 4.0f) continue;   // something in the way (the hull itself is fine)
        bestD = d;
        best = veh;
    }
    if (!best) return;
    start_rocket(i, best, slot);
}

// Engage `veh` with the launcher in `slot` (current order resumes afterwards).
static void start_rocket(int i, char *veh, int slot)
{
    BotBrain *b = bot_brain(i);
    if (b->task == TASK_NONE || b->task == TASK_DEFEND || b->task == TASK_FOLLOW) { b->prevTask = b->task; b->prevCommander = b->commander; }
    b->task = TASK_ROCKET;
    b->serviceTarget = veh;
    b->supplySlot = slot;
    b->step = 0;
    b->stepAt = 0;
    b->taskUntil = H->frame + 30 * 30;
    g_rocketShots[i] = 0;
    g_rocketVt[i] = *(unsigned *)veh;
    g_rocketGrenade[i] = 0;
    bot_nav(i)->mode = 1;
    bot_log("rocket: %s engages an enemy vehicle (%d rocket(s))", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, bot_ammo(i, slot));
}

static unsigned g_rocketCooldown[MAX_BOTS];
#define ROCKET_COOLDOWN_FRAMES (6 * 30)
static void end_rocket(int i, BotBrain *b, const char *why)
{
    g_rocketCooldown[i] = H->frame + ROCKET_COOLDOWN_FRAMES;
    press_button(i, SLOT_ACTION(0), 3);   // rifle back out
    b->step = 0;
    b->serviceTarget = NULL;
    if (b->prevTask == TASK_DEFEND || (b->prevTask == TASK_FOLLOW && b->prevCommander)) {
        bot_log("rocket: %s stops (%s), back to %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why, kTaskNames[b->prevTask]);
        b->task = b->prevTask;
        b->commander = b->prevCommander;
        b->prevTask = TASK_NONE;
        b->prevCommander = NULL;
        b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
        b->replanAt = 0;
        bot_nav(i)->mode = 0;
        return;
    }
    bot_log("rocket: %s stops (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    end_task(i, b, why);
}

static int grenade_tick(int i, BotBrain *b, char *soldier, char *veh, float *vp);
// step 0: launcher out; 1: aiming (fire once on target); 2: reloading
static int rocket_tick(int i, BotBrain *b, char *soldier)
{
    char *veh = b->serviceTarget;
    if (H->frame > b->taskUntil) { end_rocket(i, b, "took too long"); return 0; }
    if (IsBadReadPtr(veh, OFF_VEHICLE_HEALTH + 4) || *(unsigned *)veh != g_rocketVt[i]) { end_rocket(i, b, "vehicle gone"); return 0; }
    int vteam = 0;
    float *vp = vehicle_pos(veh, &vteam);
    if (!vp || *(float *)(veh + OFF_VEHICLE_HEALTH) <= 0) { end_rocket(i, b, "vehicle destroyed"); return 0; }
    if (vteam == player_team(H->bots[i]) || !vehicle_occupied(veh)) { end_rocket(i, b, "no longer a threat"); return 0; }
    if (g_rocketGrenade[i]) return grenade_tick(i, b, soldier, veh, vp);
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    float eye[3] = {me[0], me[1] + 1.6f, me[2]};
    // lead the target by its speed over the rocket's flight time
    VehTrack *t = vehicle_sample(veh);
    // aim at the hull: 1 m above the ground under the vehicle (the crew seat / body point can sit high up in a turret)
    float aim[3] = {vp[0], vp[1] + ROCKET_HULL_HEIGHT, vp[2]};
    {
        NavGrid *g = *nav_slot();
        float gp[3];
        if (g && g->phase == 2 && nav_snap(g, vp[0], vp[1] - 1.5f, vp[2], gp) && gp[1] <= vp[1] + 0.5f) aim[1] = gp[1] + ROCKET_HULL_HEIGHT;
    }
    float d = sqrtf(dist2(aim, eye));
    if (d > ROCKET_RANGE * 1.2f) { end_rocket(i, b, "out of range"); return 0; }
    if (t && t->speed > 0.5f && t->at) {
        static float lastPos[MAX_BOTS][3];
        static unsigned lastAt[MAX_BOTS];
        if (lastAt[i] && H->frame > lastAt[i]) {
            float dt = (H->frame - lastAt[i]) / 30.0f, fly = d / ROCKET_SPEED;
            for (int a = 0; a < 3; a += 2) aim[a] += (vp[a] - lastPos[i][a]) / dt * fly;
        }
        memcpy(lastPos[i], vp, 12);
        lastAt[i] = H->frame;
    }
    float dx = aim[0] - eye[0], dz = aim[2] - eye[2], dh = sqrtf(dx * dx + dz * dz);
    bot_nav(i)->mode = 1;
    bot_nav(i)->targetYaw = yaw_towards(dx, dz);
    g_aimPitch[i] = atan2f(aim[1] - eye[1], dh) + (dh > ROCKET_DROP_START ? (dh - ROCKET_DROP_START) * 0.0004f : 0.0f);   // lift only for long shots
    if (b->step == 0) {
        press_button(i, SLOT_ACTION(b->supplySlot), 3);
        b->step = 1;
        b->stepAt = H->frame + 36;   // launcher up
        return 1;
    }
    if (H->frame < b->stepAt) return 1;
    if (b->step == 2) {   // reloaded
        int left = bot_ammo(i, b->supplySlot);
        if (left == 0 || g_rocketShots[i] >= ROCKET_MAX_SHOTS) { end_rocket(i, b, left == 0 ? "out of rockets" : "fired enough"); return 0; }   // (it has fired: 0 is real)
        float hit[3];
        if (ray_blocked_char(eye, aim, hit) && dist2(hit, aim) > 4.0f * 4.0f) { end_rocket(i, b, "lost sight"); return 0; }
        b->step = 1;
    }
    // fire when yaw and pitch are on target
    float yaw = *(float *)(soldier + OFF_SOLDIER_YAW), ty = fmodf(bot_nav(i)->targetYaw, 6.2831853f);
    if (ty > 0) ty -= 6.2831853f;
    float yerr = fabsf(ty - yaw);
    if (yerr > 3.14159265f) yerr = 6.2831853f - yerr;
    float perr = fabsf(g_aimPitch[i] - *(float *)(soldier + OFF_SOLDIER_PITCH));
    if (yerr < ROCKET_AIM_TOLERANCE && perr < ROCKET_AIM_TOLERANCE) {
        press_button(i, EIA_FIRE, 3);
        g_rocketShots[i]++;
        g_rocketsThisLife[i]++;
        b->step = 2;
        b->stepAt = H->frame + ROCKET_RELOAD_FRAMES;
        bot_log("rocket: %s fires rocket %d at %.0f m", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, g_rocketShots[i], d);
    }
    return 1;
}

// ---- Hand grenades and cover against enemy ground vehicles ----
// Why the vehicle a bot is in cover from no longer counts (NULL: still a threat).
static const char *vehicle_threat_gone(int i, const float *me)
{
    char *v = g_threatVeh[i];
    float *vp = !IsBadReadPtr(v, OFF_VEHICLE_HEALTH + 4) && *(unsigned *)v == 0x025bf8e8 && *(float *)(v + OFF_VEHICLE_HEALTH) > 0 &&
                vehicle_occupied(v) ? vehicle_pos(v, NULL) : NULL;
    if (!vp) return "vehicle gone";
    if (dist2(vp, me) > VEH_THREAT_RANGE * VEH_THREAT_RANGE) return "vehicle moved off";
    float eye[3] = {me[0], me[1] + 1.6f, me[2]}, a[3] = {vp[0], vp[1] + 1.0f, vp[2]}, hit[3];
    if (ray_blocked_char(eye, a, hit) && dist2(hit, a) > 4.0f * 4.0f) {
        if (!g_threatVehHidden[i]) g_threatVehHidden[i] = H->frame ? H->frame : 1;
        else if (H->frame - g_threatVehHidden[i] > 10 * 30) return "vehicle out of sight";
    } else g_threatVehHidden[i] = 0;
    return NULL;
}
static char *vehicle_occupant(char *veh);
static int cover_from_vehicle(int i, char *soldier, char *veh, const char *why)
{
    char *occ = vehicle_occupant(veh);
    float *vp = vehicle_pos(veh, NULL);
    if (!occ || !vp) return 0;
    float eye[3] = {vp[0], vp[1] + 2.0f, vp[2]};   // (a gunner's eye)
    return take_cover(i, soldier, occ, veh, eye, why);
}
static void start_grenade(int i, char *veh, int slot)
{
    start_rocket(i, veh, slot);
    g_rocketGrenade[i] = 1;
    bot_brain(i)->taskUntil = H->frame + 8 * 30;
    bot_log("grenade: %s goes for a grenade at an enemy %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh));
}
// Low throwing arc to a point dh away horizontally and dy up: pitch (rad), 0 if out of reach.
static int grenade_pitch(float dh, float dy, float *pitch)
{
    const float v = GRENADE_SPEED, g = 9.81f;
    float disc = v * v * v * v - g * (g * dh * dh + 2.0f * dy * v * v);
    if (disc < 0 || dh < 0.5f) return 0;
    *pitch = atan2f(v * v - sqrtf(disc), g * dh);
    return 1;
}
// step 0: grenade out; 1: aim on the arc and throw; 2: rifle back, then cover
static int grenade_tick(int i, BotBrain *b, char *soldier, char *veh, float *vp)
{
    const char *name = (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME;
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), eye[3] = {me[0], me[1] + 1.6f, me[2]};
    float aim[3] = {vp[0], vp[1] + 0.3f, vp[2]};
    {
        NavGrid *g = *nav_slot();
        float gp[3];
        if (g && g->phase == 2 && nav_snap(g, vp[0], vp[1] - 1.5f, vp[2], gp) && gp[1] <= vp[1] + 0.5f) aim[1] = gp[1] + 0.3f;
    }
    float dx = aim[0] - eye[0], dz = aim[2] - eye[2], dh = sqrtf(dx * dx + dz * dz), pitch = 0;
    VehTrack *t = vehicle_sample(veh);
    int fast = t && t->speed > VEH_SLOW_SPEED;
    if (b->step < 2 && (fast || dh < GRENADE_MIN_RANGE - 1.0f || !grenade_pitch(dh, aim[1] - eye[1], &pitch))) {   // no throw after all
        press_button(i, SLOT_ACTION(0), 3);
        const char *why = fast ? "it sped up" : dh < GRENADE_MIN_RANGE - 1.0f ? "too close" : "out of reach";
        bot_log("grenade: %s does not throw (%s)", name, why);
        if (!cover_from_vehicle(i, soldier, veh, why)) end_rocket(i, b, why);
        return 0;
    }
    bot_nav(i)->mode = 1;
    bot_nav(i)->targetYaw = yaw_towards(dx, dz);
    g_aimPitch[i] = pitch;
    if (b->step == 0) {
        press_button(i, SLOT_ACTION(b->supplySlot), 3);
        b->step = 1;
        b->stepAt = H->frame + 25;   // grenade in hand
        return 1;
    }
    if (H->frame < b->stepAt) return 1;
    if (b->step == 1) {
        float yaw = *(float *)(soldier + OFF_SOLDIER_YAW), ty = fmodf(bot_nav(i)->targetYaw, 6.2831853f);
        if (ty > 0) ty -= 6.2831853f;
        float yerr = fabsf(ty - yaw);
        if (yerr > 3.14159265f) yerr = 6.2831853f - yerr;
        float perr = fabsf(pitch - *(float *)(soldier + OFF_SOLDIER_PITCH));
        if (yerr < 0.06f && perr < 0.06f) {
            press_button(i, EIA_FIRE, 3);
            g_grenadesThisLife[i]++;
            b->step = 2;
            b->stepAt = H->frame + 35;   // throwing animation
            bot_log("grenade: %s throws at the %s %.0f m away (%.0f deg up, it moves %.1f m/s)", name, vehicle_name(veh), dh,
                    pitch * 57.2958f, t ? t->speed : 0.0f);
        }
        return 1;
    }
    press_button(i, SLOT_ACTION(0), 3);   // rifle back out, and into cover
    if (!cover_from_vehicle(i, soldier, veh, "grenade thrown")) end_rocket(i, b, "grenade thrown, no cover near");
    return 0;
}
// Every half second: an enemy ground vehicle close by (bots with a launcher leave it to rocket_scan).
static void vehicle_threat_scan(int i, char *soldier)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i];
    if (player_vehicle(pl) || H->frame < g_vehThreatCooldown[i]) return;
    if (b->task != TASK_NONE && b->task != TASK_DEFEND && b->task != TASK_FOLLOW && b->task != TASK_FIGHT) return;
    int ls = launcher_slot(i);
    if (ls >= 0 && !(bot_ammo(i, ls) == 0 && g_rocketsThisLife[i] > 0)) return;
    char **vehs;
    int nveh = vehicle_list(&vehs);
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION), bestD = VEH_THREAT_RANGE;
    char *best = NULL;
    for (int k = 0; k < nveh; k++) {
        char *veh = vehs[k];
        int vteam = 0;
        if (IsBadReadPtr(veh, OFF_VEHICLE_HEALTH + 4) || *(unsigned *)veh != 0x025bf8e8) continue;
        float *vp = vehicle_pos(veh, &vteam);
        if (!vp || !vteam || vteam == player_team(pl) || *(float *)(veh + OFF_VEHICLE_HEALTH) <= 0 || !vehicle_occupied(veh)) continue;
        float d = sqrtf(dist2(vp, me));
        if (d >= bestD || !vehicle_on_ground(vp)) continue;
        float eye[3] = {me[0], me[1] + 1.6f, me[2]}, aim[3] = {vp[0], vp[1] + 1.0f, vp[2]}, hit[3];
        if (ray_blocked_char(eye, aim, hit) && dist2(hit, aim) > 4.0f * 4.0f) continue;   // cannot see it
        bestD = d;
        best = veh;
    }
    if (!best) return;
    VehTrack *t = vehicle_sample(best);
    float speed = t ? t->speed : 0.0f;
    int gs = bot_gadget_slot(i, "M67", NULL);
    int nade = gs >= 0 && !(bot_ammo(i, gs) == 0 && g_grenadesThisLife[i] > 0) && g_grenadesThisLife[i] < 2;
    if (speed <= VEH_SLOW_SPEED && nade && bestD >= GRENADE_MIN_RANGE && bestD <= GRENADE_MAX_RANGE) { start_grenade(i, best, gs); return; }
    const char *why = speed > VEH_SLOW_SPEED ? "moving fast" : !nade ? "no grenade" : "out of throwing range";
    if (!cover_from_vehicle(i, soldier, best, why)) g_vehThreatCooldown[i] = H->frame + 3 * 30;
}

// Returns 1 while a task controls the bot (the flag loop is skipped).
static int task_tick(int i, BotBrain *b, char *soldier)
{
    if (b->task == TASK_REVIVE) return revive_tick(i, b, soldier);
    if (b->task == TASK_COVER) return cover_tick(i, b, soldier);
    if (b->task == TASK_ROCKET) return rocket_tick(i, b, soldier);
    if (b->task == TASK_FIGHT) return fight_tick(i, b, soldier);
    if (b->task == TASK_REPAIR) {
        char *veh = b->serviceTarget;
        if (H->frame > b->taskUntil) { end_repair(i, b, "repair took too long"); return 0; }
        if (!vehicle_alive(veh, g_repairVt[i])) { end_repair(i, b, "vehicle gone"); return 0; }
        VehTrack *t = vehicle_sample(veh);
        float h = *(float *)(veh + OFF_VEHICLE_HEALTH), mx = t && t->maxSeen ? t->maxSeen : 1000.0f;
        if (h <= 0) { end_repair(i, b, "vehicle destroyed"); return 0; }
        if (h >= 0.99f * mx) { end_repair(i, b, "vehicle repaired"); return 0; }
        float *vp = vehicle_pos(veh, NULL);
        if (vp) {
            float mx2 = vp[0] - b->supplyPos[0], mz2 = vp[2] - b->supplyPos[2];
            if (mx2 * mx2 + mz2 * mz2 > 3.0f * 3.0f) { end_repair(i, b, "vehicle drove off"); return 0; }
        }
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        float *cp = vp ? vp : b->supplyPos;   // vehicle centre (body position)
        float dx = cp[0] - me[0], dz = cp[2] - me[2], d = sqrtf(dx * dx + dz * dz);
        b->waiting = d <= REPAIR_CONTACT_DIST;   // (waiting = in contact while repairing)
        if (!b->waiting) {
            if (d > REPAIR_CONTACT_DIST + 2.0f && (H->frame >= b->replanAt || bot_nav(i)->mode == 1 || bot_nav(i)->mode == 4)) {
                b->replanAt = H->frame + 90;
                go_towards(i, soldier, cp[0] - dx / d * REPAIR_STANDOFF, cp[1], cp[2] - dz / d * REPAIR_STANDOFF);
            } else if (d <= REPAIR_CONTACT_DIST + 2.0f) {
                bot_nav(i)->mode = 4; bot_nav(i)->x = cp[0]; bot_nav(i)->z = cp[2];
            }
            return 1;
        }
        // in contact: walk into the vehicle's centre so the tool stays on the hull
        bot_nav(i)->mode = 4;
        bot_nav(i)->x = cp[0];
        bot_nav(i)->z = cp[2];
        if (b->step == 0) {
            press_button(i, SLOT_ACTION(b->supplySlot), 3);   // repair tool out (once)
            b->step = 1;
            b->stepAt = H->frame + 45;   // let the tool come out before the trigger goes down
            bot_log("brain: %s repairing (health %.0f / %.0f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, h, mx);
        } else if (b->step == 1 && H->frame >= b->stepAt) {
            b->step = 2;   // tool is out: botmod_tick holds the trigger every frame from now on
        }
        return 1;
    }
    if (H->frame > b->taskUntil) { end_task(i, b, "timed out"); return 0; }
    if (b->task == TASK_FOLLOW && (!b->commander || !*(char **)(b->commander + OFF_PLAYER_SOLDIER))) { end_task(i, b, "their player died"); return 0; }
    if (b->task == TASK_FOLLOW) {
        char *cveh = player_vehicle(b->commander), *myveh = player_vehicle(H->bots[i]);
        if (myveh) {                                   // riding: stay while the commander is in the same vehicle
            g_boardUntil[i] = 0;
            if (cveh != myveh) { bot_exit_vehicle(i, 1); }
            return 1;
        }
        if (g_boardUntil[i] && (!cveh || H->frame > g_boardUntil[i] || free_seat(cveh) < 0)) {   // "Get in" that did not work out
            const char *why = !cveh ? "driver got out" : H->frame > g_boardUntil[i] ? "could not board in time" : "vehicle is full";
            g_boardUntil[i] = 0;
            end_task(i, b, why);
            choose_local_objective(i, "missed the ride");
            return 0;
        }
        if (cveh) {                                    // commander is in a vehicle: board it when close, else walk to it
            float *hp = human_pos(b->commander), *me = (float *)(soldier + OFF_SOLDIER_POSITION);
            float dx = hp[0] - me[0], dz = hp[2] - me[2];
            if (dx * dx + dz * dz < VEHICLE_BOARD_DIST * VEHICLE_BOARD_DIST && free_seat(cveh) >= 0) { bot_enter_vehicle(i, cveh); return 1; }
        }
        approach(i, b, soldier, b->commander, 4.0f);
        return 1;
    }
    if (b->task == TASK_DEFEND) {
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        float dx = b->postPos[0] - me[0], dz = b->postPos[2] - me[2], d = sqrtf(dx * dx + dz * dz);
        // close enough: within 2.5 m, or within 4 m and not getting closer for 2 s (teammates / cover in the way)
        if (d < 4.0f && !b->crouched && H->frame - b->stepAt > 60 && b->stepAt) d = 0;
        if (d > 2.5f && d < 4.0f && !b->stepAt) b->stepAt = H->frame;
        if (d > 4.0f) b->stepAt = 0;
        if (d > 2.5f) {
            if (b->crouched) { bot_stand(i); b->crouched = 0; }   // stand to move
            if (H->frame >= b->replanAt || bot_nav(i)->mode == 1) {
                b->replanAt = H->frame + 90;
                go_towards(i, soldier, b->postPos[0], b->postPos[1], b->postPos[2]);
            }
        } else {
            bot_nav(i)->mode = 1;
            bot_nav(i)->targetYaw = b->postFacing;   // cover outward
            if (!b->crouched) {
                bot_crouch(i);
                b->crouched = 1;
                bot_log("brain: %s holding post at (%.1f, %.1f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, b->postPos[0], b->postPos[2]);
                if (!pilot_heading_for_ride(i)) place_class_gadget(i, soldier);   // (a Pilot at its ride: no mines - Pilot US 2 knelt by one)
            }
        }
        return 1;
    }    if (b->task == TASK_SUPPLY) {
        float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
        if (b->step == 0 && b->serviceTarget && player_exists(b->serviceTarget)) {   // follow the caller, not the spot they called from
            float *hp = human_pos(b->serviceTarget);
            if (hp) {
                if (dist2(hp, b->supplyPos) > 2.0f * 2.0f) b->replanAt = 0;
                memcpy(b->supplyPos, hp, sizeof b->supplyPos);
            }
        }
        float dx = b->supplyPos[0] - me[0], dz = b->supplyPos[2] - me[2], d = sqrtf(dx * dx + dz * dz);
        if (b->step == 0) {
            if (d > 3.0f) {
                int pathDone = bot_nav(i)->mode == 1 && b->replanAt;   // walked and still not there
                if (pathDone && d < 6.0f) goto drop;                    // as close as we can get (e.g. the caller blocks the spot)
                if (H->frame - b->stepAt > SUPPLY_GIVEUP_FRAMES) {       // stepAt = when the run started
                    bot_log("brain: %s cannot reach the spot (%.1f m away), giving up", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, d);
                    bot_say_later(i, ORDER_SORRY, 0);
                    b->task = b->prevTask;
                    b->commander = b->prevCommander;
                    b->serviceTarget = NULL;
                    b->taskUntil = H->frame + FOLLOW_FRAMES;
                    if (b->task == TASK_NONE) end_task(i, b, "supply spot unreachable");
                    return 0;
                }
                if (H->frame >= b->replanAt || bot_nav(i)->mode == 1) {
                    b->replanAt = H->frame + 90;
                    go_towards(i, soldier, b->supplyPos[0] - dx / d * 1.5f, b->supplyPos[1], b->supplyPos[2] - dz / d * 1.5f);
                }
                return 1;
            }
        drop:
            bot_nav(i)->mode = 1;
            bot_nav(i)->targetYaw = yaw_towards(dx, dz);   // face the spot, drop it in front of us
            b->step = 1;
            b->stepAt = H->frame + 15;
        }
        if (H->frame >= b->stepAt) {
            switch (b->step++) {
            case 1: press_button(i, SLOT_ACTION(b->supplySlot), 3);           b->stepAt = H->frame + 30; break;   // gadget
            case 2: press_button(i, EIA_FIRE, 6);                            b->stepAt = H->frame + 35; break;   // drop it
            case 3: press_button(i, SLOT_ACTION(0), 3);                     b->stepAt = H->frame + 10; break;   // primary
            default:
                bot_log("brain: %s dropped slot %d gadget for %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, b->supplySlot,
                        b->serviceTarget ? b->serviceTarget + OFF_PLAYER_ONLINE_NAME : "?");
                b->task = b->prevTask;
                b->commander = b->prevCommander;
                b->serviceTarget = NULL;
                b->step = 0;
                b->taskUntil = b->task == TASK_DEFEND ? NEVER : H->frame + FOLLOW_FRAMES;
                b->replanAt = 0;
                if (b->task == TASK_NONE) end_task(i, b, "supply delivered");
            }
        }
        return 1;
    }
    return 0;
}
static void assign_task(int i, int task, char *commander)
{
    BotBrain *b = bot_brain(i);
    if (b->crouched) { bot_stand(i); b->crouched = 0; }   // stand up for the new order
    b->task = task;
    b->commander = commander;
    b->taskUntil = H->frame + FOLLOW_FRAMES;
    b->replanAt = 0;
    b->enroute = b->waiting = 0;
    b->targetFlag = -1;
}

// Closest living, brain-enabled bot of the team; skip_busy ignores bots already serving a human.
static int closest_bot(int team, const float *p, int skipBusy, float *dist)
{
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; i < H->botCount; i++) {
        BotBrain *b = bot_brain(i);
        if (!b->enabled || player_team(H->bots[i]) != team) continue;
        if (skipBusy && b->task != TASK_NONE) continue;
        float d = bot_distance_to(i, p);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (dist) *dist = bestD;
    return bestD < 1e29f ? best : -1;
}

// target: the bot a quick-Q request was aimed at (NULL for the command wheel) - it answers first.
static void order_reaction(char *sender, int type, char *target)
{
    int ti = target ? bot_index(target) : -1;
    float *sp = human_pos(sender);
    if (!sp) return;
    int team = player_team(sender);
    const char *who = sender + OFF_PLAYER_ONLINE_NAME;
    char *sveh = player_vehicle(sender);
    if (type == ORDER_GETIN && sveh) {
        int seats = free_seat_count(sveh);
        for (int n = 0; n < seats; n++) {   // only as many bots as there are free seats, nearest first
            int i = -1;
            float bestD = GETIN_RADIUS;
            for (int j = 0; j < H->botCount; j++) {
                BotBrain *o = bot_brain(j);
                if (!o->enabled || player_team(H->bots[j]) != team || player_vehicle(H->bots[j]) || !*(char **)(H->bots[j] + OFF_PLAYER_SOLDIER)) continue;
                if (o->task == TASK_FOLLOW && o->commander == sender) continue;   // already coming
                if (g_drive[j].phase) continue;                                    // off to drive its own
                if (is_pilot(j) && is_bot(sender)) continue;                       // a Pilot rides with no bot (the user): it drives its own
                float d = bot_distance_to(j, sp);
                if (d < bestD) { bestD = d; i = j; }
            }
            if (i < 0) break;
            assign_task(i, TASK_FOLLOW, sender);   // riding along = following the driver
            g_boardUntil[i] = H->frame + GETIN_BOARD_FRAMES;
            bot_say_later(i, ORDER_ROGERTHAT, SAY_DELAY_FRAMES);
            bot_log("brain: %s gets in %s's vehicle", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, who);
        }
        return;
    }
    if (type == ORDER_GETOUT && sveh) {
        for (int i = 0; i < H->botCount; i++) {
            if (player_vehicle(H->bots[i]) != sveh || H->bots[i] == sender) continue;   // (a bot driver gets out by itself)
            int byHuman = sender && !is_bot(sender);   // a human aboard says "Get out": a Pilot gives up its seat too - the human
            if (byHuman && is_pilot(i)) {              // takes the vehicle, the Pilot finds another ride (the user, 2026-10-13)
                g_pilotForceExit = 1;
                bot_log("pilot: %s bails out for %s and looks for another ride", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, sender + OFF_PLAYER_ONLINE_NAME);
            }
            bot_exit_vehicle(i, 1);
            g_pilotForceExit = 0;
            if (byHuman && is_pilot(i)) { g_drive[i].phase = 0; g_drive[i].veh = NULL; g_driving[i] = 0; g_flying[i] = 0; g_flyAuto[i] = 0; vehicle_mark_bad(sveh, 120); }
            bot_say_later(i, ORDER_THANKYOU, SAY_DELAY_FRAMES);   // thanks for the ride
            end_task(i, bot_brain(i), "bailed out");
            choose_local_objective(i, "bailed out");   // nearest flag from where they jump out
        }
        return;
    }
    if (type == ORDER_REQUESTREPAIR) {
        // the caller's vehicle, or the nearest friendly vehicle next to them
        char *veh = sveh;
        if (!veh) {
            char **vehs;
            int n = vehicle_list(&vehs);
            float bestD = REPAIR_CALL_VEHICLE_DIST;
            for (int k = 0; k < n; k++) {
                int vt = 0;
                float *vp = !IsBadReadPtr(vehs[k], 4) && *(unsigned *)vehs[k] == 0x025bf8e8 ? vehicle_pos(vehs[k], &vt) : NULL;
                if (!vp || vt != team) continue;
                float dx = vp[0] - sp[0], dz = vp[2] - sp[2], d = sqrtf(dx * dx + dz * dz);
                if (d < bestD) { bestD = d; veh = vehs[k]; }
            }
        }
        VehTrack *t = veh ? vehicle_sample(veh) : NULL;
        float h = veh ? *(float *)(veh + OFF_VEHICLE_HEALTH) : 0, mx = t && t->maxSeen ? t->maxSeen : 1000.0f;
        int sent = 0, nearest = -1;
        float nearD = 1e30f;
        for (int i = 0; veh && h > 0 && h < 0.99f * mx && i < H->botCount; i++) {
            BotBrain *b = bot_brain(i);
            char *pl = H->bots[i];
            if (!b->enabled || player_team(pl) != team || player_vehicle(pl) || !*(char **)(pl + OFF_PLAYER_SOLDIER)) continue;
            float d = bot_distance_to(i, sp);
            if (d < nearD) { nearD = d; nearest = i; }
            if (d > REPAIR_CALL_RADIUS && i != ti) continue;   // the engineer the player looked at comes from anywhere
            if (b->task == TASK_REPAIR && b->serviceTarget == veh) { sent++; continue; }   // already on it
            if (b->task != TASK_NONE && b->task != TASK_DEFEND && b->task != TASK_FOLLOW) continue;
            int slot = bot_gadget_slot(i, "Repairtool", "RepairTool");
            if (slot < 0) continue;
            start_repair(i, veh, slot);
            bot_say_later(i, ORDER_ROGERTHAT, SAY_DELAY_FRAMES);
            sent++;
        }
        if (!sent) {
            if (nearest < 0)   // any bot nearby to answer
                for (int i = 0; i < H->botCount; i++) {
                    if (player_team(H->bots[i]) != team || !*(char **)(H->bots[i] + OFF_PLAYER_SOLDIER)) continue;
                    float d = bot_distance_to(i, sp);
                    if (d < nearD) { nearD = d; nearest = i; }
                }
            bot_log("brain: %s needs a repair but %s", who, !veh ? "there is no friendly vehicle next to them" :
                    h >= 0.99f * mx ? "the vehicle is not damaged" : "no engineer is free within range");
            if (nearest >= 0 && nearD < REPAIR_CALL_RADIUS) {
                bot_say_later(nearest, ORDER_NEGATIVE, SAY_DELAY_FRAMES);
                bot_say_later(nearest, ORDER_SORRY, SAY_DELAY_FRAMES + 3 * 30);
            }
        } else bot_log("brain: %d engineer(s) answer %s's repair call", sent, who);
        return;
    }
    if (type == ORDER_GOGOGO) {
        // releases only the bots this player commands (following / backing up)
        for (int i = 0; i < H->botCount; i++) {
            BotBrain *b = bot_brain(i);
            if (b->task == TASK_FOLLOW && b->commander == sender) { end_task(i, b, "GoGoGo from commander"); choose_local_objective(i, "GoGoGo"); }
            else if (b->task == TASK_DEFEND && bot_distance_to(i, sp) <= DEFEND_RELEASE_RADIUS) {
                if (b->crouched) bot_stand(i);
                b->crouched = 0;
                end_task(i, b, "GoGoGo from commander");   // same reply (Sorry) and back to objectives
                choose_local_objective(i, "GoGoGo");
            }
            if (b->task == TASK_SUPPLY && b->prevCommander == sender) { b->prevTask = TASK_NONE; b->prevCommander = NULL; bot_say_later(i, ORDER_SORRY, SAY_DELAY_FRAMES); }
        }
    } else if (type == ORDER_FOLLOWME) {
        for (int i = 0; i < H->botCount; i++) {
            BotBrain *b = bot_brain(i);
            if (!b->enabled || player_team(H->bots[i]) != team || bot_distance_to(i, sp) > FOLLOW_RADIUS) continue;
            if (b->task == TASK_SUPPLY) { b->prevTask = TASK_FOLLOW; b->prevCommander = sender; bot_say_later(i, ORDER_ROGERTHAT, SAY_DELAY_FRAMES); continue; }
            assign_task(i, TASK_FOLLOW, sender);
            bot_say_later(i, ORDER_ROGERTHAT, SAY_DELAY_FRAMES);
            bot_log("brain: %s follows %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, who);
        }
    } else if (type == ORDER_NEEDBACKUP) {
        for (int n = 0; n < BACKUP_BOTS; n++) {
            float d;
            int i = closest_bot(team, sp, 1, &d);
            if (i < 0) break;
            BotBrain *b = bot_brain(i);
            assign_task(i, TASK_DEFEND, sender);
            b->taskUntil = NEVER;
            b->stepAt = 0;
            // spread evenly around the spot among every bot already defending near it
            int k = 0;
            for (int j = 0; j < H->botCount; j++) {
                BotBrain *o = bot_brain(j);
                if (j == i || o->task != TASK_DEFEND) continue;
                float ox = o->postPos[0] - sp[0], oz = o->postPos[2] - sp[2];
                if (ox * ox + oz * oz < 8.0f * 8.0f) k++;
            }
            float ang = 6.2831853f * k / 4.0f + 0.4f;   // quarter turns: up to 4 bots cover all sides
            b->postPos[0] = sp[0] + sinf(ang) * DEFEND_SPREAD;
            b->postPos[1] = sp[1];
            b->postPos[2] = sp[2] + cosf(ang) * DEFEND_SPREAD;
            b->postFacing = ang;   // yaw_towards(sin, cos) == ang: face away from the spot
            b->crouched = 0;
            bot_say_later(i, ORDER_ROGERTHAT, SAY_DELAY_FRAMES);
            bot_log("brain: %s will defend %s's position (%.0f m away)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, who, d);
        }    } else if (type == ORDER_REQUESTAMMO || type == ORDER_REQUESTHEAL || type == ORDER_REQUESTRIDE) {
        int heal = type == ORDER_REQUESTHEAL, ride = type == ORDER_REQUESTRIDE;
        const char *what = heal ? "medkit" : ride ? "radio beacon" : "ammo";
        // closest bot that carries the gadget; if none does, the closest bot declines
        int best = -1, fallback = -1, slot = -1;
        float bestD = 1e30f, fbD = 1e30f;
        for (int i = 0; i < H->botCount; i++) {
            BotBrain *b = bot_brain(i);
            if (!b->enabled || player_team(H->bots[i]) != team || b->task == TASK_SUPPLY || H->bots[i] == sender) continue;
            float d = bot_distance_to(i, sp);
            int s = heal ? bot_gadget_slot(i, "Medkit", "Medicbag") : ride ? bot_gadget_slot(i, "RadioBeacon", "Beacon")
                                                                  : bot_gadget_slot(i, "Ammobag", "AmmoBag");
            if (s >= 0 && bot_ammo(i, s) == 0) {   // carries it but has none left to drop
                bot_log("brain: %s is out of %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, what);
                s = -1;
            }
            if (s >= 0 && i == ti) { bestD = -1; best = i; slot = s; }   // the bot the player looked at
            if (s >= 0 && d < bestD) { bestD = d; best = i; slot = s; }
            if (d < fbD) { fbD = d; fallback = i; }
        }
        if (best < 0) {
            if (fallback < 0) return;
            BotBrain *b = bot_brain(fallback);
            bot_log("brain: %s has no %s for %s", (char *)H->bots[fallback] + OFF_PLAYER_ONLINE_NAME, what, who);
            bot_say_later(fallback, ORDER_NEGATIVE, SAY_DELAY_FRAMES);
            bot_say_later(fallback, ORDER_SORRY, SAY_DELAY_FRAMES + 3 * 30);
            return;
        }
        BotBrain *b = bot_brain(best);
        if (b->crouched) bot_stand(best);   // stand up for the supply run
        b->crouched = 0;
        b->prevTask = b->task;
        b->prevCommander = b->commander;
        b->task = TASK_SUPPLY;
        b->serviceTarget = sender;
        b->supplySlot = slot;
        memcpy(b->supplyPos, sp, sizeof b->supplyPos);   // where they stood when they called
        b->step = 0;
        b->stepAt = H->frame;   // run start (give-up timer)
        b->taskUntil = H->frame + 90 * 30;
        b->replanAt = 0;
        b->enroute = b->waiting = 0;
        bot_say_later(best, ORDER_ROGERTHAT, SAY_DELAY_FRAMES);
        bot_log("brain: %s brings %s (slot %d) to %s at (%.1f, %.1f), %.0f m away", (char *)H->bots[best] + OFF_PLAYER_ONLINE_NAME, what, slot, who,
                sp[0], sp[2], bestD);
    }
}
// ---- Radio: a bot "says" an order by setting it on its own soldier's order component (replicated to clients) ----
#define ADDR_ORDER_LISTENER_VTABLE 0x02687c3c  // message-listener sub-object at component + 0x20
#define ADDR_ORDERCOMP_SET_ORDER   0x012c22f0  // OrderComponent::setOrder(ServerPlayer* from, int type), thiscall
typedef void (__attribute__((thiscall)) *SetOrder_t)(void *comp, void *fromPlayer, int type);
#define ADDR_ORDER_EVENT_CTOR      0x00ddb270  // (target player, sender player, event hash, 7 x 0), ret 0x28
#define ADDR_ENTITY_FIRE_EVENT     0x008427c0  // component->fireEvent(event*) on its owning entity
#define ADDR_ORDER_EVENT_DTOR      0x00d72be0
typedef void (__attribute__((thiscall)) *OrderEventCtor_t)(void *ev, void *target, void *sender, unsigned hash, int, int, int, int, int, int, int);
typedef void (__attribute__((thiscall)) *FireEvent_t)(void *comp, void *ev);
// Event hash per order type (jump table in the order handler at 0x12c3200).
static const unsigned kOrderEventHash[17] = {0, 0x0142d075, 0x2fe260b1, 0x013f0a5b, 0xf0042302, 0x75a6e7ac, 0x32fac868, 0x013912cf,
    0x5d391f0c, 0x7f84e01d, 0x44daab47, 0xfc7719a1, 0x8e0b907f, 0x87757e00, 0x3f13e920, 0xfb0c89f5, 0x5c9dbedc};
static char *g_orderComp[MAX_BOTS];
static char *g_orderCompSoldier[MAX_BOTS];

// A component (order / spotting, listener at +0x20) that still belongs to `ent`. Cached component pointers go stale
// when a soldier dies and a new one reuses its address: using a freed component crashed the server (0x8427a3).
static int comp_owned_by(char *comp, char *ent)
{
    if (!comp || !ent || IsBadReadPtr(comp, 0x34) || *(unsigned *)(comp + 0x20) != ADDR_ORDER_LISTENER_VTABLE) return 0;
    void **ref = *(void ***)(comp + 8);
    return ref && !IsBadReadPtr(ref, 4) && *ref == ent;
}

static char *find_entity_component(char *ent);
// The soldier's order component, from the scan worker's cache (a memory scan here cost ~80 ms on the game thread).
static char *find_order_component(char *soldier) { return find_entity_component(soldier); }

static void bot_say(int i, int orderType)
{
    char *pl = H->bots[i], *soldier = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!soldier) return;
    if (g_orderCompSoldier[i] != soldier || !comp_owned_by(g_orderComp[i], soldier)) {
        g_orderComp[i] = find_order_component(soldier);
        g_orderCompSoldier[i] = soldier;
    }
    if (!g_orderComp[i]) { bot_log("say: no order component for %s", pl + OFF_PLAYER_ONLINE_NAME); return; }
    ((SetOrder_t)ADDR_ORDERCOMP_SET_ORDER)(g_orderComp[i], pl, orderType);
    // The voice line / animation comes from an entity event fired on the soldier (what the order handler does next).
    if (orderType >= 1 && orderType <= 16) {
        unsigned char ev[0x80] __attribute__((aligned(16))) = {0};
        ((OrderEventCtor_t)ADDR_ORDER_EVENT_CTOR)(ev, pl, pl, kOrderEventHash[orderType], 0, 0, 0, 0, 0, 0, 0);
        ((FireEvent_t)ADDR_ENTITY_FIRE_EVENT)(g_orderComp[i], ev);
        ((ThisVoid_t)ADDR_ORDER_EVENT_DTOR)(ev);
    }
    bot_log("say: %s -> %s", pl + OFF_PLAYER_ONLINE_NAME, orderType >= 0 && orderType < 18 ? kOrderNames[orderType] : "?");
}

// ---- Spotting: VeniceNetwork.Spotting (client, ~1/s + on Q) carries the spotter (+0x3c) and up to 8 targets
// (+0x40..+0x5c, weak refs); the component of every target soldier/vehicle handles it: mode 1 (+0x80) ->
// markSpotted(comp, spotterPlayer) which stores the spotter and starts the spot timer (comp+0x30).
#define ADDR_COMP_MARK_SPOTTED     0x012c32a0  // thiscall (component, ServerPlayer* spotter), ret 4
typedef void (__attribute__((thiscall)) *MarkSpotted_t)(void *comp, void *spotter);

// Component whose owner entity is `ent` (soldier or vehicle): same listener sub-object as the order component.
// A worker thread rescans them every 2 s into a double buffer, so the main thread only does a lookup.
#define MAX_COMPS 512
typedef struct CompCache {
    int count; char *comp[MAX_COMPS]; char *owner[MAX_COMPS];
    int wcount; char *wcomp[MAX_COMPS]; char *wsoldier[MAX_COMPS];   // soldier weapon components (ammo)
    int vcount; char *veh[64];                                          // every ServerVehicleEntity (occupied or not)
    int bcount; char *body[128]; char *bodySoldier[128];                // soldier hit bodies (vt 0x0247ac10, +0x30 soldier)
    int pcount; char *pack[64];                                         // deployed ammo boxes / medkits (ServerSupplySphereEntity)
    int gcount; char *gun[256];                                         // vehicle / turret weapons (ServerWeapon)
} CompCache;
#define ADDR_SUPPLY_SPHERE_VTABLE  0x02696b68  // ServerSupplySphereEntity (type info 0x02c23a08)
#define ADDR_VEHICLE_WEAPON_VTABLE 0x025baba0  // ServerWeapon on vehicles: world matrix +0x30 (forward row +0x50, position +0x60)
#define ADDR_HITBODY_VTABLE        0x0247ac10
#define ADDR_VEHICLE_VTABLE        0x025bf8e8  // ServerVehicleEntity (tanks, LAVs, jeeps, ...)
#define ADDR_WEAPONS_COMP_VTABLE   0x025c2f60  // soldier weapons component: +0x14 weapon per slot [8], +0x44 soldier
static double perf_now(void);
static volatile double g_scanMs, g_scanMaxMs;
static volatile LONG g_scanPasses;
static CompCache g_compCache[2];
static unsigned char g_vaimOn[MAX_BOTS];
static int g_gunDebug;
static int g_vehInput = 1;   // feed bots' input to their vehicle seat (console: vinput 0/1)
#define EIA_SEAT_YAW 6          // turret traverse in a vehicle seat (EIAYaw 4 does nothing there)
static float g_seatYaw[MAX_BOTS], g_seatPitch[MAX_BOTS];
static int vehicle_seat_of(char *veh, char *pl)
{
    char **b;
    int n = vehicle_seats(veh, &b);
    for (int k = 0; k < n; k++) if (*(char **)(b[k] + 0x4c) == pl) return k;
    return -1;
}
static float g_vaimYaw[MAX_BOTS], g_vaimPitch[MAX_BOTS];
static volatile const char *g_where;      // what the game-thread tick is doing right now (NULL = not in our tick)
static volatile int g_whereBot = -1;
static void stall_check(void)
{
    static unsigned lastFrame, sameFor;
    static int reported;
    const char *w = (const char *)g_where;
    if (!w || H->frame != lastFrame) { lastFrame = H->frame; sameFor = 0; reported = 0; return; }
    sameFor += 2;
    if (sameFor >= 4 && !reported) {
        reported = 1;
        int b = g_whereBot;
        bot_log("STALL: game thread stuck %u s in %s (bot %d %s)", sameFor, w, b,
                b >= 0 && b < H->botCount ? (char *)H->bots[b] + OFF_PLAYER_ONLINE_NAME : "");
    }
}
static volatile LONG g_compCur;
static volatile LONG g_compStop;
static HANDLE g_compThread;
static DWORD WINAPI comp_worker(LPVOID unused)
{
    (void)unused;
    while (!g_compStop) {
        if (*wanted_bots() == 0 && H->botCount == 0) {   // no bots: no memory scans either
            for (int t = 0; t < 20 && !g_compStop; t++) Sleep(100);
            continue;
        }
        double scanStart = perf_now();
        static char *hListen[MAX_COMPS], *hWeap[MAX_COMPS], *hVeh[64], *hGun[256], *hPack[64], *hBody[MAX_COMPS];
        ScanType st[6] = {
            {ADDR_ORDER_LISTENER_VTABLE, 8, hListen, MAX_COMPS, 0}, {ADDR_WEAPONS_COMP_VTABLE, 4, hWeap, MAX_COMPS, 0},
            {ADDR_VEHICLE_VTABLE, 8, hVeh, 64, 0}, {ADDR_VEHICLE_WEAPON_VTABLE, 8, hGun, 256, 0},
            {ADDR_SUPPLY_SPHERE_VTABLE, 4, hPack, 64, 0}, {ADDR_HITBODY_VTABLE, 4, hBody, MAX_COMPS, 0}};
        scan_objects_multi(st, 6);
        CompCache *c = &g_compCache[!g_compCur];
        c->count = 0;
        for (int k = 0; k < st[0].count; k++) {
            char *comp = hListen[k] - 0x20, *owner = NULL;
            void ***ref = (void ***)(comp + 8);
            if (!IsBadReadPtr(ref, 4) && *ref && !IsBadReadPtr(*ref, 4)) owner = **ref;
            if (owner) { c->comp[c->count] = comp; c->owner[c->count] = owner; c->count++; }
        }
        c->wcount = 0;
        for (int k = 0; k < st[1].count; k++) {
            char *s = !IsBadReadPtr(hWeap[k] + 0x44, 4) ? *(char **)(hWeap[k] + 0x44) : NULL;
            if (s) { c->wcomp[c->wcount] = hWeap[k]; c->wsoldier[c->wcount] = s; c->wcount++; }
        }
        c->vcount = 0;
        for (int k = 0; k < st[2].count; k++) c->veh[c->vcount++] = hVeh[k];
        c->gcount = 0;
        for (int k = 0; k < st[3].count; k++) c->gun[c->gcount++] = hGun[k];
        c->pcount = 0;
        for (int k = 0; k < st[4].count; k++) c->pack[c->pcount++] = hPack[k];
        char **hits = hBody;
        int n = st[5].count;
        c->bcount = 0;
        for (int k = 0; k < n && c->bcount < 128; k++) {
            char *s = !IsBadReadPtr(hits[k] + 0x30, 4) ? *(char **)(hits[k] + 0x30) : NULL;
            if (s && !IsBadReadPtr(s, 4) && *(unsigned *)s == ADDR_SOLDIER_VTABLE) { c->body[c->bcount] = hits[k]; c->bodySoldier[c->bcount] = s; c->bcount++; }
        }
        InterlockedExchange(&g_compCur, !g_compCur);
        {
            double ms = perf_now() - scanStart;
            g_scanMs += ms; if (ms > g_scanMaxMs) g_scanMaxMs = ms;
            InterlockedIncrement(&g_scanPasses);
        }
        stall_check();
        for (int t = 0; t < 40 && !g_compStop; t++) Sleep(100);
    }
    return 0;
}
static char *soldier_hit_body(char *soldier)
{
    CompCache *c = &g_compCache[g_compCur];
    for (int k = 0; k < c->bcount; k++)
        if (c->bodySoldier[k] == soldier && !IsBadReadPtr(c->body[k], 0x34) && *(unsigned *)c->body[k] == ADDR_HITBODY_VTABLE &&
            *(char **)(c->body[k] + 0x30) == soldier) return c->body[k];
    return NULL;
}

// ---- Supply packs (ammo boxes / medkits): a bot passing within 8 m of a friendly one it needs walks onto it and
// stands there a second. Kind: learned per pack data from packs our bots dropped (class tells) ----
#define PACK_AMMO              1
#define PACK_HEAL              2
#define PACK_DETOUR_DIST       8.0f
#define OFF_PACK_OWNER_REF     0x84        // weak ref -> owner player + 8
#define OFF_PACK_POSITION      0xb0
static char *g_packData[16];
static int g_packKind[16];
static char *pack_owner(char *pack)
{
    void **ref = *(void ***)(pack + OFF_PACK_OWNER_REF);
    if (!ref || IsBadReadPtr(ref, 4) || !*ref) return NULL;
    char *pl = (char *)*ref - 8;
    return player_exists(pl) ? pl : NULL;
}
static int pack_kind(char *pack, char *owner)
{
    char *data = *(char **)(pack + 0x14);
    for (int k = 0; k < 16; k++) if (g_packData[k] == data) return g_packKind[k];
    if (!owner || !is_bot(owner)) return 0;
    const char *n = owner + OFF_PLAYER_ONLINE_NAME;
    int kind = strstr(n, "Medic") ? PACK_HEAL : strstr(n, "Support") ? PACK_AMMO : 0;
    if (kind) {
        static int next;
        g_packData[next] = data; g_packKind[next] = kind; next = (next + 1) % 16;
        bot_log("packs: data %p is %s", data, kind == PACK_HEAL ? "a medkit" : "an ammo box");
    }
    return kind;
}
static int pack_valid(char *pack) { return pack && !IsBadReadPtr(pack, 0x100) && *(unsigned *)pack == ADDR_SUPPLY_SPHERE_VTABLE; }
static int bot_needs(int i, char *soldier, int kind)
{
    if (kind == PACK_HEAL) { float h = *(float *)(soldier + OFF_SOLDIER_HEALTH); return h > 0 && h < 99.5f; }
    int l, s;
    if (soldier_ammo2(soldier, 0, &l, &s) && s < g_spareMax[i]) return 1;
    int ls = launcher_slot(i);
    return ls >= 0 && g_launcherMax[i] > 0 && bot_ammo(i, ls) >= 0 && bot_ammo(i, ls) < g_launcherMax[i];
}
static char *g_pickPack[MAX_BOTS];
static unsigned g_pickUntil[MAX_BOTS], g_pickStandUntil[MAX_BOTS];
static char *g_pickDone[MAX_BOTS][4];
static unsigned g_pickDoneAt[MAX_BOTS][4];
static void end_pickup(int i, BotBrain *b, const char *why)
{
    for (int k = 0; k < 4; k++)
        if (!g_pickDone[i][k] || H->frame - g_pickDoneAt[i][k] > 20 * 30) { g_pickDone[i][k] = g_pickPack[i]; g_pickDoneAt[i][k] = H->frame; break; }
    bot_log("packs: %s leaves the pack (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    g_pickPack[i] = NULL;
    g_pickStandUntil[i] = 0;
    bot_nav(i)->mode = 1;
    b->replanAt = 0;
    if (b->task == TASK_NONE) b->enroute = b->waiting = 0;   // re-plan the way to the objective from here
}
static int pickup_tick(int i, BotBrain *b, char *soldier)
{
    char *pl = H->bots[i];
    int free = (b->task == TASK_NONE || b->task == TASK_DEFEND || b->task == TASK_FOLLOW) && !player_vehicle(pl);
    float *me = (float *)(soldier + OFF_SOLDIER_POSITION);
    if (g_pickPack[i]) {
        char *pack = g_pickPack[i];
        if (!free) { g_pickPack[i] = NULL; g_pickStandUntil[i] = 0; return 0; }   // something more important came up
        if (!pack_valid(pack)) { end_pickup(i, b, "pack gone"); return 0; }
        if (H->frame > g_pickUntil[i]) { end_pickup(i, b, "could not get onto it"); return 0; }
        float *pp = (float *)(pack + OFF_PACK_POSITION);
        BotNav *nav = bot_nav(i);
        if (g_pickStandUntil[i]) {
            nav->mode = 1;
            if (H->frame >= g_pickStandUntil[i]) { end_pickup(i, b, "stood on it"); return 0; }
            return 1;
        }
        if (sqrtf(dist2(me, pp)) < 0.7f) { g_pickStandUntil[i] = H->frame + 30; nav->mode = 1; return 1; }
        nav->mode = 4; nav->x = pp[0]; nav->z = pp[2];
        return 1;
    }
    if (!free || H->frame % 15 != (unsigned)(i % 15)) return 0;
    CompCache *cc = &g_compCache[g_compCur];
    char *best = NULL;
    float bestD = PACK_DETOUR_DIST;
    for (int k = 0; k < cc->pcount; k++) {
        char *pack = cc->pack[k];
        if (!pack_valid(pack)) continue;
        char *owner = pack_owner(pack);
        if (!owner || player_team(owner) != player_team(pl)) continue;
        float *pp = (float *)(pack + OFF_PACK_POSITION);
        float d = sqrtf(dist2(me, pp));
        if (d >= bestD || fabsf(pp[1] - me[1]) > 2.5f) continue;
        int kind = pack_kind(pack, owner), done = 0;
        if (!kind || !bot_needs(i, soldier, kind)) continue;
        for (int m = 0; m < 4; m++) if (g_pickDone[i][m] == pack && H->frame - g_pickDoneAt[i][m] < 20 * 30) done = 1;
        if (done) continue;
        float a[3] = {me[0], me[1] + 0.5f, me[2]}, z[3] = {pp[0], pp[1] + 0.5f, pp[2]};
        if (solid_between(a, z, NULL)) continue;
        bestD = d; best = pack;
    }
    if (!best) return 0;
    g_pickPack[i] = best;
    g_pickUntil[i] = H->frame + 6 * 30;
    g_pickStandUntil[i] = 0;
    bot_log("packs: %s steps onto %s's %s %.0f m away", pl + OFF_PLAYER_ONLINE_NAME, pack_owner(best) + OFF_PLAYER_ONLINE_NAME,
            pack_kind(best, NULL) == PACK_HEAL ? "medkit" : "ammo box", bestD);
    return 1;
}

static int vehicle_list(char ***out)
{
    CompCache *c = &g_compCache[g_compCur];
    *out = c->veh;
    return c->vcount;
}

static int comp_owned_by(char *comp, char *ent);
static char *find_entity_component(char *ent)
{
    CompCache *c = &g_compCache[g_compCur];
    for (int k = 0; k < c->count; k++) if (c->owner[k] == ent && comp_owned_by(c->comp[k], ent)) return c->comp[k];
    return NULL;
}

// Rounds left for a weapon slot of a soldier: loaded (+0x2a4) + spare (+0x2a8). -1 = unknown (no data yet / odd layout).
// Launchers: 1 + spares; medkit / ammo box / radio beacon: 1 while one can be deployed.
#define OFF_WEAPON_AMMO_LOADED     0x2a4
#define OFF_WEAPON_AMMO_SPARE      0x2a8
// The weapon entity layout differs per weapon class (primaries keep their ammo 0x120 further in than pistols and
// gadgets), but the ammo pair always sits 0x110 after a block that reads {.., +0x14 = 24, +0x18 = 344}.
static int soldier_ammo2(char *soldier, int slot, int *loaded, int *spare)
{
    // the soldier's own weapon slots: [soldier+0x53c]+0x30 -> weapon entity per slot
    if (!soldier || slot < 0 || slot > 7 || IsBadReadPtr(soldier + 0x53c, 4)) return 0;
    char *q = *(char **)(soldier + 0x53c);
    if (!q || IsBadReadPtr(q + 0x30, 4)) return 0;
    char **slots = *(char ***)(q + 0x30);
    if (!slots || IsBadReadPtr(slots, 32)) return 0;
    char *w = slots[slot];
    if (!w || IsBadReadPtr(w, 0x800)) return 0;
    for (int o = 0x100; o + 0x118 <= 0x800; o += 4) {
        if (*(int *)(w + o + 0x14) != 24 || *(int *)(w + o + 0x18) != 344) continue;
        int l = *(int *)(w + o + 0x110), s = *(int *)(w + o + 0x114);
        if (l < 0 || l > 500 || s < 0 || s > 2000) return 0;
        *loaded = l; *spare = s;
        return 1;
    }
    return 0;
}
static int soldier_ammo(char *soldier, int slot)
{
    int l, s;
    return soldier_ammo2(soldier, slot, &l, &s) ? l + s : -1;
}
static int bot_ammo(int i, int slot) { return soldier_ammo(*(char **)(H->bots[i] + OFF_PLAYER_SOLDIER), slot); }

// Spotted right now (by anyone, humans included): the entity's spotting component runs a countdown at +0x30.
static int is_entity_spotted(char *ent)
{
    char *comp = ent ? find_entity_component(ent) : NULL;
    return comp && !IsBadReadPtr(comp + 0x30, 4) && *(float *)(comp + 0x30) > 0;
}
static int is_spotted(char *pl)
{
    char *ent = *(char **)(pl + OFF_PLAYER_VEHICLE);
    return is_entity_spotted(ent ? ent : *(char **)(pl + OFF_PLAYER_SOLDIER));
}
// is_spotted, re-read at most every 1/3 s per player (it walks the component cache)
static int spotted_cached(char *pl)
{
    static char *who[64];
    static unsigned at[64];
    static unsigned char val[64];
    int k, oldest = 0;
    for (k = 0; k < 64; k++) {
        if (who[k] == pl) break;
        if (at[k] < at[oldest]) oldest = k;
    }
    if (k == 64) { k = oldest; who[k] = pl; at[k] = 0; }
    if (!at[k] || H->frame - at[k] >= 10) { val[k] = (unsigned char)is_spotted(pl); at[k] = H->frame ? H->frame : 1; }
    return val[k];
}

// "Enemy over there!": when a bot spots a soldier, teammate bots close enough to hear it that are not busy turn and
// engage it from where they stand (no charging off); if they cannot see it they drop it after a few seconds.
#define ALERT_RADIUS           30.0f
#define ENGAGE_RANGE           80.0f
static void alert_nearby(char *spotter, char *target)
{
    char *ss = *(char **)(spotter + OFF_PLAYER_SOLDIER), *ts = *(char **)(target + OFF_PLAYER_SOLDIER);
    if (!ss || !ts) return;
    float *sp = (float *)(ss + OFF_SOLDIER_POSITION), *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
    int team = player_team(spotter), told = 0;
    for (int j = 0; j < H->botCount; j++) {
        char *pl = H->bots[j], *os = *(char **)(pl + OFF_PLAYER_SOLDIER);
        BotBrain *o = bot_brain(j);
        if (pl == spotter || !os || !o->enabled || player_team(pl) != team || player_vehicle(pl)) continue;
        if (o->task != TASK_NONE && o->task != TASK_DEFEND && o->task != TASK_FOLLOW) continue;   // fighting / supplying / ...
        float *op = (float *)(os + OFF_SOLDIER_POSITION);
        if (dist2(op, sp) > ALERT_RADIUS * ALERT_RADIUS) continue;
        float range = ENGAGE_RANGE * (is_recon(j) ? RECON_RANGE_FACTOR : 1.0f);
        if (dist2(op, tp) > range * range) continue;
        if (H->frame < g_engageCooldown[j] && g_cooldownWho[j] == target) continue;
        {   // only a teammate that can see it from where it stands: the rest keep going (they used to all stop and face an
            // enemy only the spotter saw - a squad stood 30 m short of an enemy flag for minutes, fighting each new spawn)
            float eye[3] = {op[0], op[1] + 1.6f, op[2]}, chest[3] = {tp[0], tp[1] + 1.2f, tp[2]};
            if (solid_between(eye, chest, NULL)) continue;
        }
        start_fight(j, target, 0, "heard it called out");
        g_fightAdvance[j] = 0;
        told++;
    }
    if (told) bot_log("alert: %s called out %s - %d teammate(s) turn to engage", spotter + OFF_PLAYER_ONLINE_NAME, target + OFF_PLAYER_ONLINE_NAME, told);
}

static int bot_spot(char *spotter, char *target)
{
    char *ent = *(char **)(target + OFF_PLAYER_VEHICLE);
    if (!ent) ent = *(char **)(target + OFF_PLAYER_SOLDIER);
    if (!ent) return 0;
    char *comp = find_entity_component(ent);
    if (!comp) { bot_log("spot: no component for %s's %s", target + OFF_PLAYER_ONLINE_NAME, ent == *(char **)(target + OFF_PLAYER_SOLDIER) ? "soldier" : "vehicle"); return 0; }
    ((MarkSpotted_t)ADDR_COMP_MARK_SPOTTED)(comp, spotter);
    bot_log("spot: %s spotted %s (%s, comp %p, timer %.1f)", spotter + OFF_PLAYER_ONLINE_NAME, target + OFF_PLAYER_ONLINE_NAME,
            *(char **)(target + OFF_PLAYER_VEHICLE) ? "vehicle" : "soldier", comp, *(float *)(comp + 0x30));
    if (is_bot(spotter) && !*(char **)(target + OFF_PLAYER_VEHICLE)) alert_nearby(spotter, target);
    return 1;
}

// ---- Detection: every 0.5 s each bot looks for enemies in a 120 deg cone with line of sight and spots them ----
#define DETECT_RANGE_INFANTRY  90.0f
#define DETECT_RANGE_VEHICLE   300.0f
#define DETECT_COS_HALF_FOV    0.22f     // ~77 deg either side of the heading
#define DETECT_ALL_AROUND      15.0f     // closer than this: noticed in any direction (heard)
#define SPOT_COOLDOWN_FRAMES   (8 * 30)
static unsigned g_spottedAt[64];          // by player slot: frame of the last bot spot
static char *g_spottedWho[64];

// ---- Engaging what a bot has seen and spotted: armour is left to engineers with rockets (rocket_scan), everyone
// else carries on. Infantry: medics, support and engineers advance on it while firing; recon fires from low cover.
// After a fight (target down / out of sight / enough) a bot does not start another one on its own for a while. ----
#define ENGAGE_RANGE           80.0f
static int find_firing_spot(int i, char *soldier, const float *teye, const float *taim, float *out);
static void engage_seen(int i, char *t, char *veh, float d)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i], *soldier = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!soldier || veh || d > (is_recon(i) ? RECON_SPOTTED_RANGE : ENGAGE_RANGE)) return;   // vehicles: rocket_scan (engineers only)
    int free = b->task == TASK_NONE || b->task == TASK_DEFEND || b->task == TASK_FOLLOW;
    if (!free && !(b->task == TASK_SUPPLY && d < 20.0f)) return;   // a supply run gives way to an enemy this close
    if (!is_spotted(t)) return;
    if (H->frame < g_engageCooldown[i] && (t == g_cooldownWho[i] || H->frame + ENGAGE_COOLDOWN_FRAMES - ENGAGE_BREATHER_FRAMES < g_engageCooldown[i])) return;
    if (b->task == TASK_SUPPLY) {
        bot_log("brain: %s drops its supply run - enemy %s at %.0f m", pl + OFF_PLAYER_ONLINE_NAME, t + OFF_PLAYER_ONLINE_NAME, d);
        b->task = b->prevTask; b->commander = b->prevCommander; b->serviceTarget = NULL; b->step = 0;
    }
    int recon = strstr(pl + OFF_PLAYER_ONLINE_NAME, "Recon") != NULL;
    int atFlag = b->targetFlag >= 0 && b->waiting && dist2((float *)(soldier + OFF_SOLDIER_POSITION), flag_pos(b->targetFlag)) < 20.0f * 20.0f;
    int advance = !recon && !atFlag && b->task == TASK_NONE;   // guards, followers and flag holders fight from where they are
    char *ts = *(char **)(t + OFF_PLAYER_SOLDIER);
    float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
    float teye[3] = {tp[0], tp[1] + 1.6f, tp[2]}, taim[3] = {tp[0], tp[1] + 1.0f, tp[2]}, spot[3];
    int moving = recon && !atFlag && find_firing_spot(i, soldier, teye, taim, spot);   // (plans the path)
    start_fight(i, t, 0, recon ? (moving ? "spotted - moving to low cover" : "spotted - firing from here") : advance ? "spotted - advancing" : "spotted");
    g_fightAdvance[i] = (unsigned char)advance;
    if (atFlag)   // the others holding this flag who can see it engage it too
        for (int j = 0; j < H->botCount; j++) {
            BotBrain *o = bot_brain(j);
            char *os = *(char **)(H->bots[j] + OFF_PLAYER_SOLDIER);
            if (j == i || !os || !o->enabled || o->task != TASK_NONE || o->targetFlag != b->targetFlag || !o->waiting) continue;
            if (player_team(H->bots[j]) != player_team(pl) || player_vehicle(H->bots[j])) continue;
            float *op = (float *)(os + OFF_SOLDIER_POSITION), oeye[3] = {op[0], op[1] + 1.6f, op[2]};
            if (dist2(op, tp) > ENGAGE_RANGE * ENGAGE_RANGE || solid_between(oeye, taim, NULL)) continue;
            start_fight(j, t, 0, "helping hold the flag");
        }
    if (moving) {
        memcpy(g_fightSpot[i], spot, 12);
        g_fightMoveUntil[i] = H->frame + 8 * 30;
        bot_nav(i)->mode = 3;
    }
}

static void detect_tick(int i)
{
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!s || player_vehicle(pl) || !bot_brain(i)->enabled) return;   // (brain off by hand: driven from the console)
    char *pm = player_manager();
    if (!pm) return;
    float *me = (float *)(s + OFF_SOLDIER_POSITION), yaw = *(float *)(s + OFF_SOLDIER_YAW);
    float fx = sinf(yaw), fz = cosf(yaw);
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    char *best = NULL, *bestVeh = NULL;
    float bestD = 1e30f;
    unsigned bestK = 0;
    for (unsigned k = 0; k < count && k < 64; k++) {
        char *t = arr[k];
        if (!t || t == pl || player_team(t) == player_team(pl)) continue;
        char *ts = *(char **)(t + OFF_PLAYER_SOLDIER), *veh = *(char **)(t + OFF_PLAYER_VEHICLE);
        if (!ts || *(float *)(ts + OFF_SOLDIER_HEALTH) <= 0) continue;
        float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);   // transform: follows the seat in vehicles
        float dx = tp[0] - me[0], dy = tp[1] - me[1], dz = tp[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < 0.5f) continue;
        int own = d <= (veh ? DETECT_RANGE_VEHICLE : DETECT_RANGE_INFANTRY * (is_recon(i) ? RECON_RANGE_FACTOR : 1.0f)) &&
                  (d <= DETECT_ALL_AROUND || (dx * fx + dz * fz) / sqrtf(dx * dx + dz * dz + 1e-6f) >= DETECT_COS_HALF_FOV);
        // not in view: a soldier a teammate has spotted shows on the map / 3D marker - look for it there
        if (!own && (veh || d > (is_recon(i) ? RECON_SPOTTED_RANGE : ENGAGE_RANGE) || !spotted_cached(t))) continue;
        float eye[3] = {me[0], me[1] + 1.6f, me[2]}, aim[3] = {tp[0], tp[1] + (veh ? 1.5f : 1.2f), tp[2]}, hit[3];
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;   // soldiers never block the view; walls, terrain and vehicles do
        int blocked = ray_cast(eye, aim, hit, NULL);
        g_rayFlags = saved;
        if (blocked) {
            float hx = hit[0] - aim[0], hy = hit[1] - aim[1], hz = hit[2] - aim[2];
            if (!veh || hx * hx + hy * hy + hz * hz > 5.0f * 5.0f) continue;   // hit something else than the target vehicle
        }
        if (d < bestD) { bestD = d; best = t; bestVeh = veh; bestK = k; }
    }
    if (!best) return;
    // identify first: spot it unless it was spotted by a bot just now
    if (!(g_spottedWho[bestK] == best && H->frame - g_spottedAt[bestK] < SPOT_COOLDOWN_FRAMES) && bot_spot(pl, best)) {
        g_spottedAt[bestK] = H->frame;
        g_spottedWho[bestK] = best;
        bot_log("detect: %s sees %s%s at %.0f m", pl + OFF_PLAYER_ONLINE_NAME, best + OFF_PLAYER_ONLINE_NAME, bestVeh ? " (vehicle)" : "", bestD);
    }
    engage_seen(i, best, bestVeh, bestD);
}

// Replies are delayed so they do not drown under the human's own voice line.
#define SAY_QUEUE 4
typedef struct SayItem { int type; unsigned at; } SayItem;
static SayItem g_sayQueue[MAX_BOTS][SAY_QUEUE];

static void bot_say_later(int i, int type, unsigned delayFrames)
{
    for (int k = 0; k < SAY_QUEUE; k++)
        if (!g_sayQueue[i][k].type) { g_sayQueue[i][k].type = type; g_sayQueue[i][k].at = H->frame + delayFrames; return; }
}

static void say_tick(int i)
{
    for (int k = 0; k < SAY_QUEUE; k++)
        if (g_sayQueue[i][k].type && H->frame >= g_sayQueue[i][k].at) { bot_say(i, g_sayQueue[i][k].type); g_sayQueue[i][k].type = 0; }
}

static void kill_bot(char *pl)
{
    void *s = *(void **)(pl + OFF_PLAYER_SOLDIER);
    if (!s) return;
    typedef void (__attribute__((thiscall)) *Kill_t)(void *soldier, int arg);
    ((Kill_t)(*(void ***)s)[0x170 / 4])(s, 0);   // same call as NetworkSuicideMessage
}

// ---- Button presses (digital actions: setLevel != 0 sets the button bit) ----
#define MAX_PRESSES 8
typedef struct BotPress { int action; int frames; } BotPress;
static BotPress g_press[MAX_BOTS][MAX_PRESSES];

static void press_button(int i, int action, int frames)
{
    // already held: hold it longer (a fresh press every frame overlapped the old ones, and each expiring one let the
    // button go - a held sprint mostly read as off)
    for (int k = 0; k < MAX_PRESSES; k++)
        if (g_press[i][k].frames > 0 && g_press[i][k].action == action) { if (frames > g_press[i][k].frames) g_press[i][k].frames = frames; return; }
    for (int k = 0; k < MAX_PRESSES; k++)
        if (g_press[i][k].frames <= 0) { g_press[i][k].action = action; g_press[i][k].frames = frames; return; }
}

#define MAX_SCHEDULED 8
typedef struct ScheduledPress { int action; unsigned at; } ScheduledPress;
static ScheduledPress g_sched[MAX_BOTS][MAX_SCHEDULED];
void schedule_press(int i, int action, unsigned at)
{
    for (int k = 0; k < MAX_SCHEDULED; k++)
        if (!g_sched[i][k].at) { g_sched[i][k].action = action; g_sched[i][k].at = at; return; }
}

static void apply_presses(int i, void *input)
{
    for (int k = 0; k < MAX_SCHEDULED; k++)
        if (g_sched[i][k].at && H->frame >= g_sched[i][k].at) { press_button(i, g_sched[i][k].action, 3); g_sched[i][k].at = 0; }
    for (int k = 0; k < MAX_PRESSES; k++) {
        BotPress *p = &g_press[i][k];
        if (p->frames > 0) {
            ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(input, p->action, 1.0f);
            if (--p->frames == 0) ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(input, p->action, 0.0f);
        }
    }
}

// ---- Message trace: log every distinct message type dispatched through the MessageManager ----
#define ADDR_MSGMGR_DISPATCH       0x005495e0  // MessageManager::dispatch(Message*)
static const unsigned char kMsgDispatchPrologue[10] = {0x83, 0xEC, 0x18, 0x80, 0x3D, 0xA8, 0xA0, 0xAC, 0x02, 0x00}; // sub esp,0x18; cmp byte [0x2aca0a8],0
static volatile int g_traceMessages;
static const char *g_seenMsg[512];
static int g_seenMsgCount;

static const char *message_name(void *msg)
{
    unsigned char *fn = (*(unsigned char ***)msg)[2];   // slot 2: "mov eax, <name>; ret"
    return fn[0] == 0xB8 ? *(const char **)(fn + 1) : NULL;
}

#define ADDR_MSG_GET_CONNECTION    0x01a089e0  // NetworkMessage -> ServerConnection* (thiscall)
#define ADDR_CONNECTION_GET_PLAYER 0x00e3b0e0  // ServerConnection -> ServerPlayer* (thiscall)
typedef void *(__attribute__((thiscall)) *GetPtr_t)(void *self);
static volatile int g_traceOrders = 1;

static char *message_sender(void *msg)
{
    void *conn = ((GetPtr_t)ADDR_MSG_GET_CONNECTION)(msg);
    return conn ? ((GetPtr_t)ADDR_CONNECTION_GET_PLAYER)(conn) : NULL;
}

static void order_reaction(char *sender, int type, char *target);

static const char *describe_ptr(void *p, char *buf, int n);
static void on_order_message(unsigned char *msg)
{
    char *pl = message_sender(msg);
    int type = *(int *)(msg + OFF_ORDER_TYPE);
    bot_log("order from %s: %s (%d)", pl ? pl + OFF_PLAYER_ONLINE_NAME : "?", type >= 0 && type < 18 ? kOrderNames[type] : "?", type);
    if (g_traceMessages) {   // research: every field, with player / soldier / vehicle pointers named
        char line[600], d[64];
        int n = snprintf(line, sizeof line, "  order raw:");
        for (int off = 0x28; off < 0x60; off += 4) {
            unsigned v = *(unsigned *)(msg + off);
            const char *w = describe_ptr((void *)v, d, sizeof d);
            if (*w) n += snprintf(line + n, sizeof line - n, " +%02x=%s", off, w);
            else n += snprintf(line + n, sizeof line - n, " +%02x=%08x", off, v);
            if (n > (int)sizeof line - 40) break;
        }
        bot_log("%s", line);
    }
    // the same order is dispatched several times (one per listener): react once per sender+type within 15 frames
    static char *lastSender; static int lastType; static unsigned lastFrame;
    if (pl == lastSender && type == lastType && H->frame - lastFrame < 15) return;
    lastSender = pl; lastType = type; lastFrame = H->frame;
    // quick-Q requests (look at a teammate + Q) name that teammate: +0x44 is a weak ref to the player (+8)
    char *target = NULL;
    void **ref = *(void ***)(msg + 0x44);
    if (ref && !IsBadReadPtr(ref, 4) && *ref) target = (char *)*ref - 8;
    if (target && !is_bot(target)) target = NULL;
    if (target) bot_log("  (quick request to %s)", target + OFF_PLAYER_ONLINE_NAME);
    if (pl && !is_bot(pl)) order_reaction(pl, type, target);
}

// Names a pointer if it is a player, a player's soldier or a player's vehicle ("" otherwise).
static const char *describe_ptr(void *p, char *buf, int n)
{
    buf[0] = 0;
    char *pm = player_manager();
    if (!pm || !p) return buf;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned i = 0; i < count; i++) {
        char *pl = arr[i];
        if (!pl) continue;
        const char *nm = pl + OFF_PLAYER_ONLINE_NAME;
        if (p == pl) snprintf(buf, n, "player %s", nm);
        else if (p == *(void **)(pl + OFF_PLAYER_SOLDIER)) snprintf(buf, n, "soldier of %s", nm);
        else if (p == *(void **)(pl + OFF_PLAYER_VEHICLE)) snprintf(buf, n, "vehicle of %s", nm);
        else continue;
        return buf;
    }
    return buf;
}

// Spotting: dump the message and name every pointer (directly or inside an fb array) that is a player/soldier/vehicle.
static void on_spotting_message(unsigned char *msg)
{
    char *pl = message_sender(msg);
    bot_log("spotting from %s:", pl ? pl + OFF_PLAYER_ONLINE_NAME : "?");
    char d[64];
    for (int off = 0; off < 0x80; off += 4) {
        unsigned v = *(unsigned *)(msg + off);
        const char *what = describe_ptr((void *)v, d, sizeof d);
        if (*what) { bot_log("  +%02x %08x = %s", off, v, what); continue; }
        if (v >= 0x10000 && !IsBadReadPtr((void *)(v - 4), 8)) {   // fb array: count at ptr-4
            unsigned cnt = *(unsigned *)(v - 4);
            if (cnt > 0 && cnt < 16 && !IsBadReadPtr((void *)v, cnt * 4))
                for (unsigned k = 0; k < cnt; k++) {
                    char d2[64];
                    const char *w2 = describe_ptr(((void **)v)[k], d2, sizeof d2);
                    if (*w2) bot_log("  +%02x[%u] %08x = %s", off, k, ((unsigned *)v)[k], w2);
                }
        }
    }
    char line[400];
    int n = 0;
    for (int off = 0; off < 0x80; off += 4) n += snprintf(line + n, sizeof line - n, "%s%08x", off % 32 ? " " : " | ", *(unsigned *)(msg + off));
    bot_log("  raw%s", line);
}

// Vehicle damage (research): dump the message, name player/vehicle pointers and print floats.

// Who fired recently: ServerSoldier.Firing carries the shooter's soldier (or player) somewhere in its fields.
#define MAX_SHOOTERS 64
static char *g_firedBy[MAX_SHOOTERS];
static unsigned g_firedAt[MAX_SHOOTERS];
static void on_firing_message(unsigned char *msg)
{
    char *pm = player_manager();
    if (!pm) return;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (int off = 0x28; off < 0x60; off += 4) {
        void *v = *(void **)(msg + off);
        if (!v) continue;
        for (unsigned k = 0; k < count && k < MAX_SHOOTERS; k++) {
            char *pl = arr[k];
            if (!pl || (v != pl && v != *(void **)(pl + OFF_PLAYER_SOLDIER))) continue;
            g_firedBy[k] = pl;
            g_firedAt[k] = H->frame;
            return;
        }
    }
}
static int fired_recently(char *pl)
{
    for (int k = 0; k < MAX_SHOOTERS; k++) if (g_firedBy[k] == pl && H->frame - g_firedAt[k] < 45) return 1;
    return 0;
}
static unsigned g_botFiredAt[MAX_BOTS];   // a bot's trigger last held by BotMod (shots without a Firing message too)
static int fired_within(char *pl, unsigned frames)
{
    for (int k = 0; k < MAX_SHOOTERS; k++) if (g_firedBy[k] == pl && H->frame - g_firedAt[k] < frames) return 1;
    int i = bot_index(pl);
    return i >= 0 && g_botFiredAt[i] && H->frame - g_botFiredAt[i] < frames;
}

// Projectile impacts (research): the server sees every bullet hit, bots' included - dump the fields, naming pointers
// (one level deep too) to find shooter, victim and damage.
// Where in a bullet object is its shooter? Look for weak refs (cell -> player + 8) and for the soldier itself.


// ServerSoldierEntity::onDamage (soldier vtable +0x110, thiscall, 3 args, ret 0xc) - research: log real calls.
#define ADDR_SOLDIER_ON_DAMAGE     0x00e08000
typedef void (__attribute__((thiscall)) *OnDamage_t)(void *soldier, void *a1, void *a2, void *a3);
static void dump_named(const char *tag, unsigned char *p, int len)
{
    char d[64], line[900];
    int n = snprintf(line, sizeof line, "%s %p:", tag, p);
    if (!p || IsBadReadPtr(p, len)) { bot_log("%s (unreadable)", line); return; }
    for (int off = 0; off < len && n < (int)sizeof line - 60; off += 4) {
        unsigned v = *(unsigned *)(p + off);
        const char *what = describe_ptr((void *)v, d, sizeof d);
        float f = *(float *)(p + off);
        if (*what) n += snprintf(line + n, sizeof line - n, " +%02x=%s", off, what);
        else if (v && (f > 0.001f || f < -0.001f) && f > -1e6f && f < 1e6f) n += snprintf(line + n, sizeof line - n, " +%02x=%.2f", off, f);
        else n += snprintf(line + n, sizeof line - n, " +%02x=%08x", off, v);
    }
    bot_log("%s", line);
}
int __cdecl bot_damage_giver(void *giver);
static void note_attacker(void *soldier, void *giver)
{
    if (!giver || IsBadReadPtr(giver, 0x30)) return;
    for (int j = 0; j < H->botCount; j++) {
        char *bot = H->bots[j];
        if (*(void **)(bot + OFF_PLAYER_SOLDIER) != soldier) continue;
        typedef char *(__attribute__((thiscall)) *GetPlayer_t)(void *);
        char *pl = ((GetPlayer_t)0x00dd1de0)(giver);
        if (pl && pl != bot && player_exists(pl) && player_team(pl) != player_team(bot)) {
            g_attacker[j] = pl;
            g_attackedAt[j] = H->frame;
            g_attackerNoted++;
        }
        return;
    }
}
void __attribute__((thiscall)) hook_soldier_on_damage(void *soldier, void *a1, void *a2, void *a3)
{
    note_attacker(soldier, a2);
    // With client-side hit detection on, damage from a server-simulated bullet (a1+0x58 bit 0x10) is ignored - the
    // shooter's client is expected to report the hit. Bots have no client: their bullets count as reported.
    if (a1 && !IsBadWritePtr((char *)a1 + 0x58, 1) && (*((unsigned char *)a1 + 0x58) & 0x10) && bot_damage_giver(a2))
        *((unsigned char *)a1 + 0x58) &= ~0x10;
    ((OnDamage_t)ADDR_SOLDIER_ON_DAMAGE)(soldier, a1, a2, a3);
}

// Bullet hit handler (research): projectile::processHit(hit record 0x40, ...) at 0xe72040 - dump real hit records.
#define ADDR_PROJECTILE_PROCESS_HIT 0x00e72040
static const unsigned char kProcessHitPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};   // push ebp; mov ebp,esp; and esp,-16
static char *g_fightTarget[MAX_BOTS];
static char *soldier_hit_body(char *soldier);
static int solid_between(const float *from, const float *to, float *hitOut);



static int g_gunDebug;
static char *g_gunTarget[MAX_BOTS];
static int patch_jump(unsigned addr, const unsigned char *expect, int len, void *stub, const char *what);
// ---- Health component damage (0x1180190): a soldier whose state wants client-confirmed hits only accepts damage
// records of type 1 (a2+0x28); bot bullets arrive as type 0 and were dropped at 0x11801c5. Let a bot shooter's through. ----
unsigned g_hcBlocked, g_hcBotPass;
int __cdecl bot_damage_giver(void *giver)
{
    if (!giver || IsBadReadPtr(giver, 0x30)) return 0;
    typedef char *(__attribute__((thiscall)) *GetPlayer_t)(void *);
    char *pl = ((GetPlayer_t)0x00dd1de0)(giver);
    int r = pl && is_bot(pl);
    if (r) g_hcBotPass++; else g_hcBlocked++;
    return r;
}
extern void hc_stub(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hc_stub\n"
    "_hc_stub:\n"
    "  je 3f\n"                           /* type 1: accepted as before */
    "  pushad\n"
    "  push dword ptr [ebp + 0xc]\n"      /* damage giver */
    "  call _bot_damage_giver\n"
    "  add esp, 4\n"
    "  mov dword ptr [_g_gateResult], eax\n"
    "  popad\n"
    "  cmp dword ptr [_g_gateResult], 0\n"
    "  jne 3f\n"
    "  push 0x011803f8\n"                 /* rejected */
    "  ret\n"
    "3:\n"
    "  push 0x011801cb\n"
    "  ret\n"
    ".att_syntax prefix\n");

void __cdecl on_process_hit(char *projectile, unsigned char *hit, unsigned *args)
{
    (void)args;
    if (!g_gunDebug) return;   // aim measurement (console: gundebug 1): where a rifle bot's bullets land near its target
    void **ref = !IsBadReadPtr(projectile + 0x84, 4) ? *(void ***)(projectile + 0x84) : NULL;
    char *shooter = ref && !IsBadReadPtr(ref, 4) && *ref ? (char *)*ref - 8 : NULL;
    int i = shooter ? bot_index(shooter) : -1;
    if (i < 0 || player_vehicle(shooter) || !g_fightTarget[i] || !player_exists(g_fightTarget[i])) return;
    char *ts = *(char **)(g_fightTarget[i] + OFF_PLAYER_SOLDIER), *me = *(char **)(shooter + OFF_PLAYER_SOLDIER), *ent = *(char **)(hit + 0x20), d[64];
    if (!ts || !me) return;
    float *hp = (float *)hit, *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION), *mp = (float *)(me + OFF_SOLDIER_POSITION);
    float dx = tp[0] - mp[0], dz = tp[2] - mp[2], dist = sqrtf(dx * dx + dz * dz) + 1e-6f;
    float rx = hp[0] - tp[0], rz = hp[2] - tp[2], side = (-rx * dz + rz * dx) / dist, along = (rx * dx + rz * dz) / dist;
    if (fabsf(side) > 3.0f || fabsf(along) > 4.0f) return;
    int onTarget = ent && !IsBadReadPtr(ent, 0x34) && *(unsigned *)ent == ADDR_HITBODY_VTABLE && *(char **)(ent + 0x30) == ts;
    bot_log("aim: %s -> %s %.0f m: %.2f m above feet, %+.2f side, %+.1f along, hit %s", shooter + OFF_PLAYER_ONLINE_NAME,
            g_fightTarget[i] + OFF_PLAYER_ONLINE_NAME, dist, hp[1] - tp[1], side, along, onTarget ? "TARGET" : describe_ptr(ent, d, sizeof d));
}
extern void hook_process_hit(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_process_hit\n"
    "_hook_process_hit:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"      /* eax -> [ret][hit][a2][a3][a4] */
    "  push eax\n"
    "  push dword ptr [eax + 4]\n" /* hit record */
    "  push ecx\n"                 /* projectile (this) */
    "  call _on_process_hit\n"
    "  add esp, 12\n"
    "  popfd\n"
    "  popad\n"
    "  push ebp\n"
    "  mov ebp, esp\n"
    "  and esp, 0xfffffff0\n"
    "  push 0x00e72046\n"
    "  ret\n"
    ".att_syntax prefix\n");

int g_gateResult;   // scratch for the health-component stub below

static int patch_jump(unsigned addr, const unsigned char *expect, int len, void *stub, const char *what)
{
    if (memcmp((void *)addr, expect, len) != 0) { bot_log("ERROR: %s bytes mismatch - wrong server build?", what); return 0; }
    unsigned char code[16];
    code[0] = 0xE9;
    *(DWORD *)(code + 1) = (DWORD)((unsigned char *)stub - (unsigned char *)(addr + 5));
    for (int k = 5; k < len; k++) code[k] = 0x90;
    patch_bytes((void *)addr, code, len);
    bot_log("patched %s @ %08x", what, addr);
    return 1;
}

// Soldier damage (research): dump the fields, naming players / soldiers / vehicles.

// console "msgdump <part of a message name>": dump each such message (research) - its dwords, naming assets and the
// assets in any fb array it points to
static char g_msgDump[64];
static void msg_dump(const char *name, unsigned char *msg)
{
    char line[900]; int n = 0;
    char *pl = NULL;
    bot_log("msgdump: %s", name);
    for (int off = 0; off < 0xa0; off += 4) {
        unsigned v = *(unsigned *)(msg + off);
        // only heap / image addresses: probing a thread's stack (06xxxxxx here) can hit its guard page and kill the
        // server later (crashed it 2026-10-12 on Network.SpawnVehicleCustomization)
        int safe = v >= 0xf0000000u || (v >= 0x00400000u && v < 0x03000000u);
        const char *an = safe ? asset_name((void *)v) : "?";
        n += snprintf(line + n, sizeof line - n, " +%02x=%08x%s%s", off, v, an[0] != '?' ? ":" : "", an[0] != '?' ? an : "");
        unsigned cnt = safe ? fb_count((void *)v) : 0;
        if (cnt && !IsBadReadPtr((void *)v, cnt * 4)) {
            n += snprintf(line + n, sizeof line - n, "[%u:", cnt);
            for (unsigned k = 0; k < cnt && n < (int)sizeof line - 120; k++) {
                const char *e = asset_name(((void **)v)[k]), *s = strrchr(e, '/');
                n += snprintf(line + n, sizeof line - n, " %s", s ? s + 1 : e);
            }
            n += snprintf(line + n, sizeof line - n, "]");
        }
        if (n > (int)sizeof line - 160) { bot_log("%s", line); n = 0; }
    }
    if (n) bot_log("%s", line);
    (void)pl;
}

void __cdecl on_msg_dispatch(unsigned *stack)
{
    void *msg = (void *)stack[1];
    const char *name = msg ? message_name(msg) : NULL;
    if (!name) return;
    if (g_msgDump[0] && strstr(name, g_msgDump)) msg_dump(name, msg);
    if (g_traceOrders && !strcmp(name, "VeniceNetwork.Order")) on_order_message(msg);
    if (!strcmp(name, "VeniceNetwork.Spotting") && g_traceMessages) on_spotting_message(msg);
    if (!strcmp(name, "ServerSoldier.Firing")) on_firing_message(msg);
    if (!g_traceMessages) return;
    for (int i = 0; i < g_seenMsgCount; i++) if (g_seenMsg[i] == name) return;
    if (g_seenMsgCount < 512) g_seenMsg[g_seenMsgCount++] = name;
    bot_log("  msg: %s (vt %08x)", name, *(unsigned *)msg);
}

extern void hook_msg_dispatch(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_msg_dispatch\n"
    "_hook_msg_dispatch:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  call _on_msg_dispatch\n"
    "  add esp, 4\n"
    "  popfd\n"
    "  popad\n"
    "  sub esp, 0x18\n"
    "  cmp byte ptr [0x02aca0a8], 0\n"
    "  push 0x005495ea\n"   /* push/ret leave the flags from cmp intact for the je that follows */
    "  ret\n"
    ".att_syntax prefix\n");

// ---- Human spawn requests: the server calls spawn target vslot 0x6c (player, -1, arg, 0) on whatever the client picked
// (HQ / flag CharacterSpawnEntity, radio beacon, squad mate). Logged so new spawn target classes can be identified.
#define ADDR_SPAWN_REQ_CALL        0x0118fab3  // mov edx,[edx+0x6c]; push 0; push eax; push -1; push esi; call edx
static const unsigned char kSpawnReqCall[11] = {0x8B, 0x52, 0x6C, 0x6A, 0x00, 0x50, 0x6A, 0xFF, 0x56, 0xFF, 0xD2};
// Jet spawner groups (Air Superiority): a human spawning on a CharacterSpawnEntity tells us its group (data pointer
// +0x14) belongs to their team - the spawners report no team before the round (all bots took one side's jets).
static unsigned g_cseGroup[4];
static int g_cseGroupTeam[4];
static int cse_group_team(void *e)   // 1 / 2 known, -1 = the other team's, 0 = unknown
{
    unsigned grp = !IsBadReadPtr((char *)e + 0x14, 4) ? *(unsigned *)((char *)e + 0x14) : 0;
    for (int k = 0; k < 4; k++) if (g_cseGroup[k] && g_cseGroup[k] == grp) return g_cseGroupTeam[k];
    for (int k = 0; k < 4; k++) if (g_cseGroup[k]) return 3 - g_cseGroupTeam[k];   // two sides: not the known one's
    return 0;
}
static void cse_group_learn(void *e, int team)
{
    unsigned grp = !IsBadReadPtr((char *)e + 0x14, 4) ? *(unsigned *)((char *)e + 0x14) : 0;
    if (!grp || (team != 1 && team != 2)) return;
    int slot = -1;
    for (int k = 0; k < 4; k++) if (g_cseGroup[k] == grp) slot = k;
    for (int k = 0; k < 4 && slot < 0; k++) if (!g_cseGroup[k]) slot = k;
    if (slot < 0 || (g_cseGroup[slot] == grp && g_cseGroupTeam[slot] == team)) return;
    g_cseGroup[slot] = grp; g_cseGroupTeam[slot] = team;
    bot_log("air: spawner group %08x belongs to team %d", grp, team);
}
void __cdecl on_spawn_request(unsigned *target, char *player, unsigned arg)
{
    if (target && player && *target == 0x025bbbc0u && !is_bot(player)) cse_group_learn(target, player_team(player));
    unsigned vt = target ? target[0] : 0;
    bot_log("spawnreq: %s -> target %p vt %08x (slot6c %08x) arg %08x", player ? player + OFF_PLAYER_ONLINE_NAME : "?", target, vt,
            vt ? *(unsigned *)(vt + 0x6c) : 0, arg);
    if (target && vt != 0x025bbbc0)
        for (int k = 0; k < 0x100 / 4; k += 8)
            bot_log("  +%03x %08x %08x %08x %08x %08x %08x %08x %08x", k * 4, target[k], target[k + 1], target[k + 2], target[k + 3],
                    target[k + 4], target[k + 5], target[k + 6], target[k + 7]);
}
extern void hook_spawn_request(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_spawn_request\n"
    "_hook_spawn_request:\n"
    "  pushad\n"
    "  pushfd\n"
    "  push eax\n"
    "  push esi\n"
    "  push ecx\n"
    "  call _on_spawn_request\n"
    "  add esp, 12\n"
    "  popfd\n"
    "  popad\n"
    "  mov edx, dword ptr [edx + 0x6c]\n"
    "  push 0\n"
    "  push eax\n"
    "  push -1\n"
    "  push esi\n"
    "  call edx\n"
    "  push 0x0118fabe\n"
    "  ret\n"
    ".att_syntax prefix\n");

// Soldier constructor: logs the code addresses on the stack (a poor man's call stack) to map spawn paths
// (e.g. radio beacon spawns do not go through the spawn-request call above).
#define ADDR_SOLDIER_CTOR          0x00e104b0
static const unsigned char kSoldierCtorPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};   // push ebp; mov ebp,esp; and esp,-16
void __cdecl on_soldier_ctor(unsigned *stack)
{
    if (!g_traceMessages) return;
    char line[256];
    int n = snprintf(line, sizeof line, "soldier ctor: stack");
    for (int k = 0, found = 0; k < 0x600 / 4 && found < 14 && n < (int)sizeof line - 10; k++)
        if (stack[k] >= 0x00401000 && stack[k] < 0x02028000) { n += snprintf(line + n, sizeof line - n, " %08x", stack[k]); found++; }
    bot_log("%s", line);
}
extern void hook_soldier_ctor(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_soldier_ctor\n"
    "_hook_soldier_ctor:\n"
    "  pushad\n"
    "  pushfd\n"
    "  lea eax, [esp + 36]\n"
    "  push eax\n"
    "  call _on_soldier_ctor\n"
    "  add esp, 4\n"
    "  popfd\n"
    "  popad\n"
    "  push ebp\n"
    "  mov ebp, esp\n"
    "  and esp, 0xfffffff0\n"
    "  push 0x00e104b6\n"
    "  ret\n"
    ".att_syntax prefix\n");

// ---- Unlock ownership: bots have no persistence stats, so the engine refuses every weapon/gadget they select ----
#define ADDR_PLAYER_VTABLE         0x025c778c  // ServerPlayer
#define ADDR_PLAYER_HAS_UNLOCK     0x00e145a0  // ServerPlayer::hasUnlock(asset) - vtable slot 0x18, thiscall, ret 4
int __cdecl core_is_bot_player(void *player) { return is_bot(player); }

extern void hook_has_unlock(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _hook_has_unlock\n"
    "_hook_has_unlock:\n"
    "  push ecx\n"
    "  push ecx\n"
    "  call _core_is_bot_player\n"
    "  add esp, 4\n"
    "  pop ecx\n"
    "  test eax, eax\n"
    "  jnz 1f\n"
    "  push 0x00e145a0\n"
    "  ret\n"
    "1:\n"
    "  mov eax, 1\n"
    "  ret 4\n"
    ".att_syntax prefix\n");

static int g_navDirty;   // learned link removals not yet written to the map's nav file
static int g_autoRepair;  // console: navautorepair 0/1 - steep-slope check where bots get stuck (experimental)
static void on_bot_stuck(int i, char *soldier)
{
    float *pos = (float *)(soldier + OFF_SOLDIER_POSITION);
    BotNav *nav = bot_nav(i);
    // (off by default: underground its ground samples hit handrails / low ceilings and cut real stairs - MP_Subway)
    int repaired = g_autoRepair && *nav_slot() && (*nav_slot())->phase == 2 ? nav_repair_around(*nav_slot(), ray_cast, pos[0], pos[1], pos[2], 4.0f, nav->x - pos[0], nav->z - pos[2]) : 0;
    if (repaired) { g_navDirty = 1; bot_log("nav: %d link(s) up a slope too steep to walk removed around (%.1f, %.1f) - saved with the map", repaired, pos[0], pos[2]); }
    int blocked = *nav_slot() ? nav_block_towards(*nav_slot(), pos[0], pos[1], pos[2], nav->x, nav->z) : 0;
    bot_log("path: %s stuck at (%.1f, %.1f) heading (%.1f, %.1f): backing off, blocked %d link(s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
            pos[0], pos[2], nav->x, nav->z, blocked);
    BotBrain *b = bot_brain(i);
    b->replanAt = 0;                                   // tasks re-plan on the next frame
    if (nav->mode == 3) {
        BotPath *p = &g_paths[i];
        float wx = p->pts[p->next * 3] - pos[0], wz = p->pts[p->next * 3 + 2] - pos[2];
        int ladderNext = p->next < p->count && (p->link[p->next] >= 0 || (p->next + 1 < p->count && p->link[p->next + 1] >= 0));
        if (p->next < p->count && !ladderNext && wx * wx + wz * wz < 2.0f * 2.0f) { p->next++; return; }   // blocked right at a corner: count it as reached
        if (!plan_path(i, soldier, p->goalX, p->goalY, p->goalZ)) nav->mode = 1;                         // otherwise re-plan to the same goal
    }
}

// ---- Level changes ----
// Level name lives at [[ctx+0x10]+0x6c] ("Levels/MP_003/MP_003"). On a change everything tied to the old level is
// dropped, and once the new level's capture points exist, HQ spawns / nav grid are rediscovered automatically.
static char g_level[64];
static int g_levelReady;           // flags + HQ spawns found for g_level
static int g_hqLess;               // g_level has no HQ spawns at all (Air Superiority): ready without them
static unsigned g_levelCheckAt;
static int g_announceLevel;           // a real level change happened (not just a reload): tell the players when ready

static const char *current_level(void)
{
    char *ctx = *(char **)ADDR_SERVER_GAME_CONTEXT;
    char *lvl = ctx ? *(char **)(ctx + 0x10) : NULL;
    const char *name = lvl && !IsBadReadPtr(lvl + 0x6c, 4) ? *(const char **)(lvl + 0x6c) : NULL;
    if (!name || IsBadStringPtrA(name, 64)) return NULL;
    const char *s = strrchr(name, '/');
    return s ? s + 1 : name;
}

// HQ spawn per team: HQ capture points (not takeable) carry the owning team and share their owner object (+0x10)
// with that team's HQ CharacterSpawnEntity.
// Spawn points tied to no capture point: some are HQs the capture points do not show (Wake's US island HQ), others
// belong to other game modes. Bots try each once (cmd_spawn) and keep the one that serves their team best.
#define MAX_HQ_CAND 16
static void *g_hqCand[MAX_HQ_CAND];
static int g_hqCandCount;
static unsigned char g_hqCandTried[3][MAX_HQ_CAND];
static float g_hqCpPos[3][3];
static int g_hqCpKnown[3];
static int find_hq_spawns(float *bmin, float *bmax)
{
    char *cps[MAX_FLAGS * 2], *spawns[128];
    int ncp = scan_objects(ADDR_CAPTURE_POINT_VTABLE, cps, MAX_FLAGS * 2);
    int nsp = scan_objects(0x025bbbc0, spawns, 128);
    {
        int n = 0;
        for (int s = 0; s < nsp && n < MAX_HQ_CAND; s++) {
            void *owner = *(void **)(spawns[s] + OFF_CP_OWNER_OBJECT);
            int tied = 0;
            for (int i = 0; i < ncp && !tied; i++) if (*(void **)(cps[i] + OFF_CP_OWNER_OBJECT) == owner) tied = 1;
            if (!tied) g_hqCand[n++] = spawns[s];
        }
        g_hqCandCount = n;
        memset(g_hqCandTried, 0, sizeof g_hqCandTried);
        g_hqCpKnown[1] = g_hqCpKnown[2] = 0;
    }
    int found = 0;
    float bestFront[3] = {1e30f, 1e30f, 1e30f};   // per team: distance of the chosen HQ to the nearest takeable flag
    for (int i = 0; i < ncp; i++) {
        char *data = *(char **)(cps[i] + OFF_CP_DATA);
        float *pos = (float *)(cps[i] + OFF_CP_POSITION);
        if (pos[0] == 0 && pos[2] == 0) continue;
        for (int a = 0; a < 3; a += 2) { if (pos[a] < bmin[a]) bmin[a] = pos[a]; if (pos[a] > bmax[a]) bmax[a] = pos[a]; }
        if (!data || IsBadReadPtr(data + OFF_CPDATA_TAKEABLE, 1) || data[OFF_CPDATA_TAKEABLE]) continue;   // takeable flag
        int team = *(int *)(cps[i] + OFF_CP_OWNER_TEAM);
        if (team != 1 && team != 2) continue;
        // a team with several HQs (Wake: island and carrier lower deck) uses the one closest to the fighting
        float front = 1e30f;
        for (int k = 0; k < ncp; k++) {
            char *kd = *(char **)(cps[k] + OFF_CP_DATA);
            float *kp = (float *)(cps[k] + OFF_CP_POSITION);
            if (k == i || !kd || IsBadReadPtr(kd + OFF_CPDATA_TAKEABLE, 1) || !kd[OFF_CPDATA_TAKEABLE] || (kp[0] == 0 && kp[2] == 0)) continue;
            float d = dist2(kp, pos);
            if (d < front) front = d;
        }
        memcpy(g_hqCpPos[team], pos, 12);
        g_hqCpKnown[team] = 1;
        if (front >= bestFront[team]) continue;
        memcpy(g_hqPos[team], pos, sizeof g_hqPos[team]);
        void *owner = *(void **)(cps[i] + OFF_CP_OWNER_OBJECT);
        for (int s = 0; s < nsp; s++)
            if (*(void **)(spawns[s] + OFF_CP_OWNER_OBJECT) == owner) {
                H->teamSpawn[team].entity = spawns[s];
                H->teamSpawn[team].a2 = 0xffffffff; H->teamSpawn[team].a3 = 1; H->teamSpawn[team].a4 = 0;
                bestFront[team] = front;
                found |= team;
                break;
            }
    }
    return found;
}

// Level discovery (loadout asset lookup, flags, HQ spawns, soldier blueprint) scans process memory and can take
// seconds - long enough for the server's stall watchdog to kill it during a level load. It runs on a worker thread;
// the main thread only starts it and applies the results. Bots are paused (level not ready) meanwhile.
static volatile LONG g_jobState;     // 0 idle, 1 running, 2 done
static int g_jobDoLoadouts;
static int g_needLoadouts;   // a level change: look the kit assets up once the new level has loaded (not while the old one
                             // is still in memory - its copies are about to be freed; a bot handed one crashed the server)
static char g_jobLevel[64];
static int g_jobHq;
static float g_jobMin[3], g_jobMax[3];
static void *g_jobBlueprint;
static HANDLE g_jobThread;

// ---- Ladders: every ServerLadderComponent sits in the component list of the object it belongs to (a ladder model,
// a ServerStaticModelEntity). The object's world transform (+0x160: right, up, forward, position) and model bounds
// (+0x70 min, +0x80 max, local) give the ladder's foot and top. ----
#define ADDR_LADDER_COMPONENT_VTABLE   0x025c0228
#define ADDR_STATIC_MODEL_ENTITY_VTABLE 0x025b6d50
#define OFF_STATIC_MODEL_TRANSFORM     0x160
#define OFF_STATIC_MODEL_BOUNDS        0x70
#define MAX_LADDERS 256
typedef struct Ladder { float foot[3], top[3], fwd[3], height; } Ladder;
static Ladder g_ladders[MAX_LADDERS];
static int g_ladderCount;
static void find_ladders(void)
{
    char *comps[MAX_LADDERS];
    int n = scan_objects(ADDR_LADDER_COMPONENT_VTABLE, comps, MAX_LADDERS), kept = 0, odd = 0;
    for (int k = 0; k < n; k++) {
        char *list = *(char **)(comps[k] + 8), *ent;
        if (!list || IsBadReadPtr(list, 4) || !(ent = *(char **)list) || IsBadReadPtr(ent, 0x1a0)) { odd++; continue; }
        if (*(unsigned *)ent != ADDR_STATIC_MODEL_ENTITY_VTABLE) {
            if (odd++ < 4) bot_log("ladders: component %p belongs to an object of vtable %08x - skipped", comps[k], *(unsigned *)ent);
            continue;
        }
        float *m = (float *)(ent + OFF_STATIC_MODEL_TRANSFORM), *bb = (float *)(ent + OFF_STATIC_MODEL_BOUNDS);
        float h = bb[5] - bb[1], w = bb[4] - bb[0], d = bb[6] - bb[2];
        if (!(h > 1.0f && h < 80.0f) || w > 3.0f || d > 3.0f) {   // not a plain ladder model
            if (odd++ < 4) bot_log("ladders: object %p bounds %.1f x %.1f x %.1f - skipped", ent, w, h, d);
            continue;
        }
        Ladder *L = &g_ladders[kept++];
        for (int a = 0; a < 3; a++) {
            float cx = (bb[0] + bb[4]) * 0.5f, cz = (bb[2] + bb[6]) * 0.5f;
            L->foot[a] = m[12 + a] + m[a] * cx + m[4 + a] * bb[1] + m[8 + a] * cz;
            L->top[a] = m[12 + a] + m[a] * cx + m[4 + a] * bb[5] + m[8 + a] * cz;
            L->fwd[a] = m[8 + a];
        }
        L->height = h;
        float best = 1e30f;
        int bf = -1;
        for (int f = 0; f < g_flagCount; f++) { float dd = dist2(flag_pos(f), L->foot); if (dd < best) { best = dd; bf = f; } }
        bot_log("ladders: #%d foot (%.1f, %.1f, %.1f) %.1f m tall, facing (%.2f, %.2f); nearest flag %d at %.0f m", kept - 1,
                L->foot[0], L->foot[1], L->foot[2], h, L->fwd[0], L->fwd[2], bf, bf >= 0 ? sqrtf(best) : -1.0f);
        if (kept == MAX_LADDERS) break;
    }
    g_ladderCount = kept;
    bot_log("ladders: %d ladder(s) on this level (%d component(s) found)", kept, n);
}

static const float *ladder_foot(int k) { return g_ladders[k].foot; }
static float ladder_top_y(int k) { return g_ladders[k].top[1]; }

// Ground straight below (x, fromY, z) down to toY: 1 with its height.
static int ground_below(float x, float z, float fromY, float toY, float *y)
{
    float a[3] = {x, fromY, z}, b[3] = {x, toY, z}, h[3];
    unsigned saved = g_rayFlags;
    g_rayFlags = RAY_DONT_CHECK_CHARACTER;
    int r = ray_cast(a, b, h, NULL);
    g_rayFlags = saved;
    if (r) *y = h[1];
    return r;
}
// Links every ladder into the nav grid: foot on the side with ground at the ladder's base, exit on the far side at
// the top (where the soldier steps off).
static void ladders_link(void)
{
    NavGrid *g = *nav_slot();
    nav_clear_links();
    if (!g || g->phase != 2) return;
    int made = 0;
    for (int k = 0; k < g_ladderCount; k++) {
        Ladder *L = &g_ladders[k];
        float fl = sqrtf(L->fwd[0] * L->fwd[0] + L->fwd[2] * L->fwd[2]);
        if (fl < 0.5f) continue;
        float f[3] = {L->fwd[0] / fl, 0, L->fwd[2] / fl};
        int done = 0;
        for (int s = 1; s >= -1 && !done; s -= 2) {
            float n[3] = {f[0] * s, 0, f[2] * s}, gy, ty;
            float ax = L->foot[0] + n[0] * LADDER_STAND_OUT, az = L->foot[2] + n[2] * LADDER_STAND_OUT;
            float bx = L->foot[0] - n[0] * 1.6f, bz = L->foot[2] - n[2] * 1.6f;   // over the top, onto the roof / deck / quay
            if (!ground_below(bx, bz, L->top[1] + 1.5f, L->top[1] - 2.0f, &ty) || ty < L->top[1] - 1.5f) continue;
            float b[3] = {bx, ty, bz}, snapB[3];
            if (!nav_snap(g, b[0], b[1] + 0.3f, b[2], snapB) || fabsf(snapB[1] - ty) > 1.0f) continue;
            // the floor (or water) in front of it, wherever the ladder passes through it - as long as it is below the
            // top exit (ladders often reach down past a deck, or out of the water up to a barge)
            if (!ground_below(ax, az, L->top[1] + 0.3f, L->foot[1] - 1.0f, &gy) || gy > ty - 0.5f) continue;
            float a[3] = {ax, gy, az}, snapA[3];
            if (!nav_snap(g, a[0], a[1] + 0.3f, a[2], snapA) || fabsf(snapA[1] - gy) > 1.0f) continue;
            int id = nav_add_link(g, a, b, 6.0f + (ty - gy) * 3.0f);
            if (id < 0 || id >= 256) continue;
            g_ladderLink[id].ladder = k;
            g_ladderLink[id].groundY = gy;
            memcpy(g_ladderLink[id].n, n, 12);
            bot_log("ladders: #%d linked - foot (%.1f, %.1f, %.1f) -> top (%.1f, %.1f, %.1f)", k, a[0], a[1], a[2], b[0], b[1], b[2]);
            made++;
            done = 1;
        }
        if (!done) bot_log("ladders: #%d at (%.1f, %.1f) not linked (no walkable ground at its foot or top)", k, L->foot[0], L->foot[2]);
    }
    bot_log("ladders: %d of %d ladder(s) linked into the nav grid", made, g_ladderCount);
}

static DWORD WINAPI discovery_worker(LPVOID doLoadouts)
{
    if (doLoadouts) load_loadouts();   // passed in: the main thread clears g_jobDoLoadouts as soon as the thread starts
    g_jobMin[0] = g_jobMin[2] = 1e9f; g_jobMax[0] = g_jobMax[2] = -1e9f;
    g_jobHq = find_hq_spawns(g_jobMin, g_jobMax);
    find_flags();
    find_ladders();
    g_jobBlueprint = NULL;
    char *objs[16];
    int nb = scan_objects(0x02500e38, objs, 16);   // SoldierBlueprint instances
    for (int k = 0; k < nb && !g_jobBlueprint; k++) if (!strcmp(asset_name(objs[k]), "Characters/Soldiers/MpSoldier")) g_jobBlueprint = objs[k];
    InterlockedExchange(&g_jobState, 2);
    return 0;
}

static void tickets_reset(void);
static void tickets_find(void);
static void hq_memory_apply(void);
static int near_origin(const float *p) { return fabsf(p[0]) < 12.0f && fabsf(p[2]) < 12.0f; }
static void hq_memory_save(int team, const float *pos, unsigned a2, unsigned a3, unsigned a4);
static float *spawn_data_pos(void *spawn);
static unsigned g_swimSeconds, g_swimSecondsLevel;
static void level_tick(void)
{
    const char *lvl = current_level();   // name checked every frame so no bot spawns on the previous level's HQ
    if (!lvl) { g_levelReady = 0; return; }
    // a new level object with the same name = the same map loaded again (round restart / !map to the current map):
    // everything found on the old instance (spawn points, flags, assets) is gone just the same
    // Signals: the level object changed, or RCON reported a level load. After a round is over bots wait (no spawns on
    // the old instance) until the level has loaded again - or 60 s, whichever comes first.
    char *ctx = *(char **)ADDR_SERVER_GAME_CONTEXT;
    void *lvlObj = ctx ? *(void **)(ctx + 0x10) : NULL;
    static void *lastLvlObj;
    static long lastLoads = -1, lastRounds = -1;
    static unsigned roundOverAt, changedAt;
    long loads, rounds;
    chat_level_events(&loads, &rounds);
    if (lastLoads < 0) { lastLoads = loads; lastRounds = rounds; }
    if (loads != lastLoads) bot_log("level: RCON event - a level loaded (#%ld)", loads);   // (a missed reset crashed server 2, 2026-10-13)
    if (lastLvlObj && lvlObj != lastLvlObj) bot_log("level: level object %p -> %p", lastLvlObj, lvlObj);
    if (rounds != lastRounds) {
        lastRounds = rounds;
        roundOverAt = H->frame ? H->frame : 1;
        g_levelReady = 0;
        bot_log("level: round over - bots wait for the next level");
    }
    int reloaded = !strcmp(lvl, g_level) && ((lastLvlObj && lvlObj != lastLvlObj) || loads != lastLoads ||
                                            (roundOverAt && H->frame - roundOverAt > 180 * 30));
    if (reloaded && changedAt && H->frame - changedAt < 45 * 30) reloaded = 0;   // (the change's own load event)
    if (loads != lastLoads || reloaded) roundOverAt = 0;
    lastLvlObj = lvlObj;
    lastLoads = loads;
    if (roundOverAt && !strcmp(lvl, g_level)) { g_levelReady = 0; return; }   // waiting for the reload
    if (strcmp(lvl, g_level) || reloaded) {
        bot_log("level: %s -> %s%s, resetting bots", g_level[0] ? g_level : "(none)", lvl, reloaded ? " (same map loaded again)" : "");
        if (g_level[0]) bot_log("swim: %u bot-second(s) in the water on %s", g_swimSecondsLevel, g_level);
        g_swimSecondsLevel = 0;
        changedAt = H->frame ? H->frame : 1;
        tickets_reset();
        strncpy(g_level, lvl, sizeof g_level - 1);
        g_levelReady = 0;
        g_flagCount = 0;
        memset(H->teamSpawn, 0, sizeof H->teamSpawn);
        for (int i = 0; i < H->botCount; i++) {
            BotBrain *b = bot_brain(i);
            int en = b->enabled;
            memset(b, 0, sizeof *b);
            b->enabled = en; b->targetFlag = -1; b->avoidFlag = -1;
            bot_nav(i)->mode = 0;
            H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
            g_orderCompSoldier[i] = NULL;
        }
        g_needLoadouts = 1;    // re-verify asset pointers against this level (in the worker, once it has loaded)
        g_hqLess = 0;
        load_kitskip();
        g_pendingHq.at = 0;    // a deploy seen before the reset belongs to the old instance
        memset(g_hqProbes, 0, sizeof g_hqProbes);
        g_announceLevel = 1;
        memset(g_squadTarget, 0, sizeof g_squadTarget);   // flag indices belong to the old level
    }
    if (g_levelReady) return;
    if (H->frame < g_levelCheckAt) return;
    g_levelCheckAt = H->frame + 30;
    if (g_jobState == 1) return;                     // discovery running in the background
    if (g_jobState == 0) {
        strncpy(g_jobLevel, g_level, sizeof g_jobLevel - 1);
        InterlockedExchange(&g_jobState, 1);
        if (g_jobThread) CloseHandle(g_jobThread);
        g_jobThread = CreateThread(NULL, 0, discovery_worker, (LPVOID)(INT_PTR)g_jobDoLoadouts, 0, NULL);
        g_jobDoLoadouts = 0;
        return;
    }
    // job done: apply if it is for this level and complete, otherwise try again shortly (level still loading)
    InterlockedExchange(&g_jobState, 0);
    if (strcmp(g_jobLevel, g_level)) return;
    // a level with flags and a soldier blueprint but no HQ spawn at all after several passes: Air Superiority (everyone
    // spawns straight into a jet) - ready without HQs, so its kits are looked up (a kit left pointing at the previous
    // level's freed assets crashed the server when a bot was spawned into a jet, 2026-10-12)
    static char noHqLevel[64];
    static int noHqPasses;
    int blueprintOk = g_jobBlueprint && *(unsigned *)g_jobBlueprint == 0x02500e38;
    if (g_flagCount && blueprintOk && !g_jobHq) {
        if (strcmp(noHqLevel, g_level)) { strncpy(noHqLevel, g_level, sizeof noHqLevel - 1); noHqPasses = 0; }
        if (++noHqPasses >= 5 && !g_hqLess) { g_hqLess = 1; bot_log("level: %s has no HQ spawns (Air Superiority?) - ready without them", g_level); }
    }
    if (!g_flagCount || (!g_jobHq && !g_hqLess) || !blueprintOk) {
        bot_log("level: %s not ready yet (%d flags, hq mask %d, blueprint %p)", g_level, g_flagCount, g_jobHq, g_jobBlueprint);
        g_levelCheckAt = H->frame + 60;
        return;
    }
    if (g_needLoadouts) {   // loaded: now the kit assets (one more discovery pass, with the lookup)
        g_needLoadouts = 0;
        g_jobDoLoadouts = 1;
        bot_log("level: %s has loaded - looking up the bot kits' assets", g_level);
        return;
    }
    static char retriedFor[64];
    if (!loadout_lib()->count && strcmp(retriedFor, g_level)) {   // library didn't load: rerun discovery once with loading on
        strncpy(retriedFor, g_level, sizeof retriedFor - 1);
        bot_log("level: loadout library empty, loading it");
        g_jobDoLoadouts = 1; g_jobState = 0;
        return;
    }
    H->teamBlueprint[1] = H->teamBlueprint[2] = g_jobBlueprint;
    g_levelReady = 1;
    for (int t = 1; t <= 2; t++)
        if (!(g_jobHq & t)) bot_log("level: team %d has no HQ here (Conquest Assault defenders) - it spawns on its flags", t);
    if (H->botCount && g_announceLevel) chat_say("[BotMod] bots ready on %s (%d flags)", g_level, g_flagCount);
    g_announceLevel = 0;
    bot_log("level: %s ready - %d flags, HQ spawns US %p RU %p, blueprint %p", g_level, g_flagCount, H->teamSpawn[1].entity,
            H->teamSpawn[2].entity, g_jobBlueprint);
    tickets_reset();
    tickets_find();
    hq_memory_apply();
    teams_fix_all();
    NavGrid *g = *nav_slot();
    char gm[64];
    chat_game_mode(gm, sizeof gm);
    if (!_strnicmp(gm, "AirSuperiority", 14)) {   // everyone flies: no walking / driving grid needed (building one cost CPU for nothing)
        if (g) { if (!*nav_building()) nav_save_if_dirty(); nav_free(g); *nav_slot() = NULL; }   // (a half-built grid is never saved)
        *nav_building() = 0;
        bot_log("level: %s is Air Superiority - no nav grid", g_level);
    } else if (!g || strcmp(g->level, g_level)) {
        char path[MAX_PATH + 64];
        nav_file(path, sizeof path, g_level);
        nav_save_if_dirty();
        NavGrid *loaded = nav_load(path);
        nav_free(*nav_slot());
        if (loaded) { *nav_slot() = loaded; *nav_building() = 0; bot_log("level: loaded nav grid %s", path); ladders_link(); }
        else {
            float m = 200.0f;
            *nav_slot() = nav_create(g_level, g_jobMin[0] - m, g_jobMin[2] - m, g_jobMax[0] + m, g_jobMax[2] + m, 2.0f);
            *nav_building() = 1;
            bot_log("level: no nav grid for %s, building (%.0f..%.0f, %.0f..%.0f)", g_level, g_jobMin[0] - m, g_jobMax[0] + m,
                    g_jobMin[2] - m, g_jobMax[2] + m);
        }
    }
}
static int level_ready(void) { return g_levelReady; }

// ---- Commands ----
// ---- Squad spawn rules (stricter than the game's own): a squadmate can be spawned on unless it is in combat - an
// enemy soldier within 25 m, hit / shot at / fired in the last 15 s, or (a bot) fighting / in cover / firing rockets - and
// at most one bot every 15 s on the same squadmate (no hydra). In a vehicle only with a free seat (the bot rides). ----
#define SQUAD_SPAWN_ENEMY_CLEAR 25.0f
static char *vehicle_occupant(char *veh);
#define SQUAD_SPAWN_EACH_FRAMES (15 * 30)
static char *g_squadSpawnOn[16];
static unsigned g_squadSpawnOnAt[16];
static int squadmate_in_combat(char *m, char *ms)
{
    int mi = bot_index(m);
    if (mi >= 0) {
        BotBrain *mb = bot_brain(mi);
        if (mb->task == TASK_FIGHT || mb->task == TASK_COVER || mb->task == TASK_ROCKET) return 1;
        if (g_hitAt[mi] && H->frame - g_hitAt[mi] < 15 * 30) return 1;
        if (g_attackedAt[mi] && H->frame - g_attackedAt[mi] < 15 * 30) return 1;
    }
    if (fired_within(m, 15 * 30)) return 1;   // has been shooting
    char *pm = player_manager();
    void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
    unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
    float *mp = (float *)(ms + OFF_SOLDIER_SPAWN_POSITION);
    for (unsigned k = 0; k < count && k < 64; k++) {   // an enemy close by
        char *e = arr[k], *es;
        if (!e || player_team(e) == player_team(m) || !(es = *(char **)(e + OFF_PLAYER_SOLDIER)) || *(float *)(es + OFF_SOLDIER_HEALTH) <= 0) continue;
        if (dist2((float *)(es + OFF_SOLDIER_SPAWN_POSITION), mp) < SQUAD_SPAWN_ENEMY_CLEAR * SQUAD_SPAWN_ENEMY_CLEAR) return 1;
    }
    return 0;
}
static int squadmate_spawned_on_recently(char *m)
{
    for (int k = 0; k < 16; k++) if (g_squadSpawnOn[k] == m && H->frame - g_squadSpawnOnAt[k] < SQUAD_SPAWN_EACH_FRAMES) return 1;
    return 0;
}
static void squadmate_note_spawn(char *m)
{
    int slot = 0;
    for (int k = 0; k < 16; k++) if (H->frame - g_squadSpawnOnAt[k] >= SQUAD_SPAWN_EACH_FRAMES) { slot = k; break; }
    g_squadSpawnOn[slot] = m; g_squadSpawnOnAt[slot] = H->frame ? H->frame : 1;
}
// After a squad spawn into a vehicle: ride with a bot driver (as a passenger of its drive) or follow a human one.
static void squadmate_ride(int i, char *veh)
{
    for (int j = 0; j < H->botCount; j++)
        if (g_drive[j].phase && g_drive[j].veh == veh && j != i) {
            DriveState *d = &g_drive[i];
            memset(d, 0, sizeof *d);
            d->phase = 7; d->veh = veh; d->at = H->frame;
            return;
        }
    char *b0 = vehicle_occupant(veh);
    if (b0 && !is_bot(b0)) assign_task(i, TASK_FOLLOW, b0);   // a player's vehicle: ride along with them
}

// A bot without a squad (round restarts empty them) is moved into one with its teammates - the bot squad with room
// first, else a new one - via RCON while it is dead. Returns 1 if a move was just requested (spawn a moment later).
static unsigned g_squadJoinAt[MAX_BOTS];
static int squad_join(char *bot, int team)
{
    int i = bot_index(bot);
    if (i < 0 || H->frame - g_squadJoinAt[i] < 5 * 30) return 0;   // asked recently: spawn as it is
    g_squadJoinAt[i] = H->frame ? H->frame : 1;
    int members[33] = {0}, bots[33] = {0};
    char *pm = player_manager();
    void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
    unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
    for (unsigned k = 0; k < count && k < 64; k++) {
        char *m = arr[k];
        int s = m ? player_squad(m) : 0;
        if (!m || player_team(m) != team || s < 1 || s > 32) continue;
        members[s]++;
        if (is_bot(m)) bots[s]++;
    }
    int pick = 0;
    for (int s = 1; s <= 32 && !pick; s++) if (bots[s] && members[s] < 4) pick = s;   // a bot squad with room
    for (int s = 1; s <= 32 && !pick; s++) if (!members[s]) pick = s;                  // else a new one
    if (!pick) return 0;
    chat_rcon("admin.movePlayer\t%s\t%d\t%d\tfalse", bot + OFF_PLAYER_ONLINE_NAME, team, pick);
    bot_log("squad: %s joins %s squad %d", bot + OFF_PLAYER_ONLINE_NAME, team == 1 ? "US" : "RU", pick);
    return 1;
}
// A squadmate a bot may spawn on: on solid ground - not swimming, not in mid-air (the game's squad spawn crashed on
// Seine, where bots end up in the river; players cannot spawn on a swimming mate either).
static int squadmate_spawnable(char *mate, char *ms)
{
    if (player_vehicle(mate)) return 1;   // a free seat (checked by the callers)
    NavGrid *g = *nav_slot();
    float *p = (float *)(ms + OFF_SOLDIER_POSITION), gp[3];
    if (!g || g->phase != 2) return 0;
    if (nav_is_water(g, p[0], p[1], p[2])) return 0;
    return nav_snap(g, p[0], p[1], p[2], gp) && fabsf(gp[1] - p[1]) < 1.5f;
}
static int cse_valid(void *e)
{
    if (!e || IsBadReadPtr(e, 0x18) || *(unsigned *)e != 0x025bbbc0) return 0;
    unsigned *data = *(unsigned **)((char *)e + 0x14);
    return data && !IsBadReadPtr(data, 0x50) && data[0] >= 0x00400000u && data[0] < 0x03000000u;
}
// The side a bot belongs on by its name ("... US" team 1, "... RU" team 2) - whatever the game's round swap says.
static int named_team(char *pl)
{
    const char *n = pl + OFF_PLAYER_ONLINE_NAME;
    return strstr(n, " US") ? 1 : strstr(n, " RU") ? 2 : (player_team(pl) == 2 ? 2 : 1);
}
// Map loaded (teams swapped by the game): every dead bot back on its named side in one go, before anyone spawns -
// nobody dies for it (bots used to be corrected at spawn, moved back by the squad code, spawned on the wrong side
// and killed to be corrected again: 156 deaths on one 60-bot map change).
static void teams_fix_all(void)
{
    int moved = 0;
    for (int i = 0; i < H->botCount; i++) {
        char *pl = H->bots[i];
        int want = named_team(pl);
        if (*(void **)(pl + OFF_PLAYER_SOLDIER) || player_team(pl) == want) continue;
        if (((AddToTeam_t)ADDR_ADD_TO_TEAM)(pl, want, 0, 1)) moved++;
    }
    if (moved) bot_log("level: %d bot(s) moved back to their named team before spawning", moved);
}
static void cmd_spawn(const char *name)
{
    char *bot = find_player(name);
    if (!bot) { bot_log("spawn: no player named '%s'", name); return; }
    if (*(void **)(bot + OFF_PLAYER_SOLDIER)) { bot_log("spawn: '%s' is already alive", name); return; }
    if (!g_levelReady) { bot_log("spawn: '%s' waits - level not ready", name); return; }
    if (!player_squad(bot) && squad_request_recent(bot)) return;   // squad move on its way (only works while dead)
    // The game swaps teams between rounds: keep "... US" bots on team 1 and "... RU" bots on team 2.
    {
        int want = strstr(name, " US") ? 1 : strstr(name, " RU") ? 2 : 0;
        if (want && player_team(bot) != want) {
            char ok = ((AddToTeam_t)ADDR_ADD_TO_TEAM)(bot, want, 0, 1);
            bot_log("spawn: moved '%s' back to team %d (%s)", name, want, ok ? "ok" : "FAILED");
        }
        if (want && player_team(bot) != want) return;   // still on the wrong side: wait (never spawn there to be killed later)
    }
    int team = player_team(bot) == 2 ? 2 : 1;
    if (!*(void **)(bot + OFF_PLAYER_BLUEPRINT)) {
        if (!H->teamBlueprint[team]) { bot_log("spawn: no SoldierBlueprint known for team %d yet (a human on that team must deploy once)", team); return; }
        ((SetPtr_t)ADDR_PLAYER_SET_BLUEPRINT)(bot, H->teamBlueprint[team]);
    }
    int pilotBot = bot_index(bot) >= 0 && is_pilot(bot_index(bot));   // Pilots belong to no squad (the user, 2026-10-13)
    if (!pilotBot && player_squad(bot) == 0 && squad_join(bot, team)) return;   // joins a squad first; spawns on the next try
    if (pilotBot && player_squad(bot) > 0) {   // a Pilot still in one (from before): out of it while it is dead
        static unsigned outAt[MAX_BOTS];
        int bi = bot_index(bot);
        if (H->frame - outAt[bi] > 10 * 30) {
            outAt[bi] = H->frame;
            chat_rcon("admin.movePlayer	%s	%d	0	false", name, team);
            bot_log("squad: %s (a Pilot) leaves squad %d", name, player_squad(bot));
            return;   // spawns on the next try, squadless
        }
    }
    // the team's HQ spawn point gone: the level is being torn down (next map / restart) - nobody spawns, and no kit is
    // handed out (its assets are being freed: that crashed the server during map changes)
    if (H->teamSpawn[team].entity && !cse_valid(H->teamSpawn[team].entity)) {
        H->teamSpawn[team].entity = NULL;
        g_levelReady = 0;
        bot_log("level: team %d's HQ spawn point is gone - the level is unloading, bots wait", team);
        return;
    }
    {   // Air Superiority: Pilots spawn in the air, in a jet of their side
        char m[64];
        chat_game_mode(m, sizeof m);
        int bi = bot_index(bot);
        if (!_strnicmp(m, "AirSuperiority", 14) && bi >= 0 && is_pilot(bi)) { air_spawn(bi); return; }
        // any other mode: no jet of ours up for 60 s - this Pilot goes straight into a free one of ours on its first tick
        // alive (the parked jets' spawn points hold only a template - Wake's carrier jets never flew); the clock restarts
        // so the next Pilot does not take the other jet too
        if (bi >= 0 && is_pilot(bi) && (team == 1 || team == 2) && H->frame - g_jetUpAt[team] > 60 * 30) {
            g_jetUpAt[team] = H->frame;
            g_intoJet[bi] = H->frame + 15 * 30;
            bot_log("pilot: '%s' - no jet of ours up for a minute: into one straight after spawning", name);
        }
    }
    BotTeamSpawn *ts = &H->teamSpawn[team];
    BotTeamSpawn flagArgs = {0};
    int noHq = !ts->entity;   // Conquest Assault defenders: flags only
    if (noHq) { flagArgs.a2 = 0xffffffff; flagArgs.a3 = 1; flagArgs.a4 = 0; ts = &flagArgs; }
    // Front-line spawn: of HQ and the flags the team holds (not being taken by the enemy), the one closest to a flag
    // the team still has to capture - so bots on long maps / carrier starts get into the fight quickly.
    void *ent = ts->entity;
    const char *where = "HQ";
    int best = -1;
    float bestD = 1e30f;
    {   // a defending squad spawns at the flag it guards (if it is ours and not being taken)
        int sqf = squad_objective(team, player_squad(bot));
        if (sqf >= 0 && squad_defending(team, player_squad(bot)) && flag_owner(sqf) == team && g_flagSpawn[sqf][team]) {
            int ctl = *(int *)(g_flags[sqf] + OFF_CP_CONTROL_TEAM);
            if (!ctl || ctl == team) { best = sqf; bestD = -1; }
        }
    }
    int sqObj = squad_objective(team, player_squad(bot));   // attacking squad: get near its flag
    if (sqObj >= 0 && squad_defending(team, player_squad(bot))) sqObj = -1;
    for (int s = noHq ? 0 : -1; s < g_flagCount && bestD >= 0; s++) {
        const float *sp = s < 0 ? g_hqPos[team] : flag_pos(s);
        if (s >= 0) {
            int ctl = *(int *)(g_flags[s] + OFF_CP_CONTROL_TEAM);
            if (flag_owner(s) != team || !g_flagSpawn[s][team] || (ctl && ctl != team)) continue;
        }
        float d = 1e30f;
        if (sqObj >= 0) d = dist2(flag_pos(sqObj), sp);   // the squad's flag
        else
            for (int f = 0; f < g_flagCount; f++) {
                if (flag_owner(f) == team) continue;
                float dx = flag_pos(f)[0] - sp[0], dz = flag_pos(f)[2] - sp[2], dd = dx * dx + dz * dz;
                if (dd < d) d = dd;
            }
        if (s < 0 && d > 1e29f) break;   // team holds every flag: HQ
        if (d < bestD || (noHq && best < 0)) { bestD = d; best = s; }
    }
    {   // a Pilot (with tanks / jets for its team on this map) spawns at its HQ only - that is where they stand
        int bi = bot_index(bot);
        if (bi >= 0 && is_pilot(bi) && (noHq || !pilot_has_rides(bi))) bot_log("pilot-hq: '%s' spawns as infantry (%s)", name, noHq ? "no HQ" : "no rides of ours");
        if (bi >= 0 && !noHq && pilot_has_rides(bi)) {
            best = -1; bestD = -1; where = "HQ (pilot)";
            char **vs;   // a free ride of ours near one of our flags: spawn there (the tanks waited at the taken flags)
            int nv = vehicle_list(&vs), bf = -1;
            float bfd = 150.0f * 150.0f;
            for (int k = 0; k < nv; k++) {
                char *v = vs[k];
                if (!vehicle_ok(v) || !pilot_ride_kind(vehicle_kind(v)) || !driver_seat_free(v) || !pilot_ride_ours(v, team)) continue;
                float *vp = vehicle_pos(v, NULL);
                if (!vp || (g_hqPos[team][0] != 0 && dist2(vp, g_hqPos[team]) < 250.0f * 250.0f)) continue;   // one at the HQ: HQ it is
                for (int f = 0; f < g_flagCount; f++) {
                    if (flag_owner(f) != team || !g_flagSpawn[f][team]) continue;
                    int ctl = *(int *)(g_flags[f] + OFF_CP_CONTROL_TEAM);
                    if (ctl && ctl != team) continue;
                    float d = dist2(flag_pos(f), vp);
                    if (d < bfd) { bfd = d; bf = f; }
                }
            }
            if (bf >= 0) { best = bf; bestD = -1; where = "flag (pilot - a ride there)"; }
        }
    }
    if (sqObj < 0 && bestD >= 0 && player_squad(bot) > 0) {   // in a player's squad: on the human leader (alive, on foot, not shooting)
        char *pm = player_manager(), *lead = NULL;
        void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
        unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
        for (unsigned k = 0; k < count && k < 64 && !lead; k++) {
            char *m = arr[k], *ms;
            if (!m || is_bot(m) || player_team(m) != team || player_squad(m) != player_squad(bot) || !m[0x788]) continue;
            if (!(ms = *(char **)(m + OFF_PLAYER_SOLDIER)) || *(float *)(ms + OFF_SOLDIER_HEALTH) <= 0) continue;
            if (player_vehicle(m) && !free_seat_count(player_vehicle(m))) continue;
            if (squadmate_spawned_on_recently(m) || squadmate_in_combat(m, ms) || !squadmate_spawnable(m, ms)) continue;
            lead = m;
        }
        if (lead) {
            typedef char (__attribute__((thiscall)) *SpawnOnSquad_t)(void *extent, void *mate);
            apply_kit(bot);   // the bot's class loadout from the library (only right before it really spawns)
            {
                float *lp = (float *)(*(char **)(lead + OFF_PLAYER_SOLDIER) + OFF_SOLDIER_POSITION);
                bot_log("spawn: '%s' tries its squad leader %s at (%.0f, %.1f, %.0f)%s", name, lead + OFF_PLAYER_ONLINE_NAME, lp[0], lp[1], lp[2],
                        player_vehicle(lead) ? " (in a vehicle)" : "");
            }
            char ok = ((SpawnOnSquad_t)0x01185b50)(bot + *(int *)0x0285bad8, lead);
            char *s = *(char **)(bot + OFF_PLAYER_SOLDIER);
            if (ok && s) {
                squadmate_note_spawn(lead);
                bot_log("spawn: '%s' team=%d on its squad leader %s%s -> soldier=%p", name, team, lead + OFF_PLAYER_ONLINE_NAME,
                        player_vehicle(bot) ? " into a vehicle seat" : "", s);
                int k = bot_index(bot);
                if (k >= 0) { g_lastSpawnedBot[k] = bot; g_lastSpawnedAt[k] = H->frame; if (player_vehicle(bot)) squadmate_ride(k, player_vehicle(bot)); }
                return;
            }
        }
    }
    if (sqObj >= 0 && bestD > 0) {   // ...or on the squadmate nearest it, like players do (not one in a vehicle or a fight)
        char *pm = player_manager(), *mate = NULL;
        float mateD = bestD;
        int squad = player_squad(bot);
        void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
        unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
        for (unsigned k = 0; k < count && k < 64; k++) {
            char *m = arr[k], *ms;
            if (!m || m == bot || player_team(m) != team || player_squad(m) != squad) continue;
            if (!(ms = *(char **)(m + OFF_PLAYER_SOLDIER)) || *(float *)(ms + OFF_SOLDIER_HEALTH) <= 0) continue;
            if (player_vehicle(m) && !free_seat_count(player_vehicle(m))) continue;   // a full vehicle
            if (squadmate_spawned_on_recently(m) || squadmate_in_combat(m, ms) || !squadmate_spawnable(m, ms)) continue;
            float d = dist2((float *)(ms + OFF_SOLDIER_SPAWN_POSITION), flag_pos(sqObj));
            if (d < mateD) { mateD = d; mate = m; }
        }
        if (mate) {
            typedef char (__attribute__((thiscall)) *SpawnOnSquad_t)(void *extent, void *mate);
            apply_kit(bot);
            {
                float *mp = (float *)(*(char **)(mate + OFF_PLAYER_SOLDIER) + OFF_SOLDIER_POSITION);
                bot_log("spawn: '%s' tries squadmate %s at (%.0f, %.1f, %.0f)%s", name, mate + OFF_PLAYER_ONLINE_NAME, mp[0], mp[1], mp[2],
                        player_vehicle(mate) ? " (in a vehicle)" : "");
            }
            char ok = ((SpawnOnSquad_t)0x01185b50)(bot + *(int *)0x0285bad8, mate);   // VeniceServerPlayerExtent::spawnOnSquad
            char *s = *(char **)(bot + OFF_PLAYER_SOLDIER);
            if (ok && s) {
                squadmate_note_spawn(mate);
                bot_log("spawn: '%s' team=%d on squadmate %s (%.0f m from flag %d)%s -> soldier=%p", name, team, mate + OFF_PLAYER_ONLINE_NAME,
                        sqrtf(mateD), sqObj, player_vehicle(bot) ? " into a vehicle seat" : "", s);
                int k = bot_index(bot);
                if (k >= 0) { g_lastSpawnedBot[k] = bot; g_lastSpawnedAt[k] = H->frame; if (player_vehicle(bot)) squadmate_ride(k, player_vehicle(bot)); }
                return;
            }
            bot_log("spawn: '%s' could not spawn on %s - using a spawn point", name, mate + OFF_PLAYER_ONLINE_NAME);
        }
    }
    if (noHq && best < 0) { bot_log("spawn: '%s' waits - its team has no HQ and no flag to spawn on", name); return; }
    if (best >= 0) {
        static char flagName[16];
        snprintf(flagName, sizeof flagName, "flag %d", best);
        ent = g_flagSpawn[best][team];
        where = flagName;
    }
    int cand = -1;
    int slot = -1;
    for (int q = 0; q < MAX_HQ_PROBES && slot < 0; q++) if (!g_hqProbes[q].bot) slot = q;
    if (best < 0 && !noHq && g_hqCpKnown[team] && slot >= 0)   // spawning at HQ: is there a closer HQ to discover?
        for (int k = 0; k < g_hqCandCount; k++)
            if (!g_hqCandTried[team][k] && g_hqCand[k] != ts->entity && spawn_team(g_hqCand[k]) != 3 - team) {   // not the other team's
                typedef void *(__attribute__((thiscall)) *GetVeh_t)(void *);
                if (IsBadReadPtr(g_hqCand[k], 0x100) || *(unsigned *)g_hqCand[k] != 0x025bbbc0u || !cse_valid(g_hqCand[k])) { g_hqCandTried[team][k] = 1; continue; }   // (an old map's: freed -
                if (((GetVeh_t)0x00dd7140)(g_hqCand[k])) { g_hqCandTried[team][k] = 1; continue; }   // calling into it crashed server 1 on a map change) a vehicle
                cand = k; break;   // spawner (a jet): no HQ - and a parked one has no team yet, so RU bots took US jets (2026-10-13)
            }
    if (cand >= 0) { g_hqCandTried[team][cand] = 1; ent = g_hqCand[cand]; where = "HQ candidate (probe)"; }
    if (cand < 0 && best < 0 && !noHq && !strcmp(where, "HQ (pilot)")) {   // a Pilot: the HQ spawn nearest our jets (Wake: the carrier's
        // lower deck, not the island - the user, 2026-10-13); only when clearly nearer them than the usual HQ
        char **vs;
        int nv = vehicle_list(&vs), nj = 0;
        float jp[16][3];
        for (int k = 0; k < nv && nj < 16; k++) {
            char *v = vs[k];
            if (!vehicle_ok(v) || vehicle_kind(v) != VK_JET || !pilot_ride_ours(v, team)) continue;
            float *vp = vehicle_pos(v, NULL);
            if (vp) { memcpy(jp[nj], vp, 12); nj++; }
        }
        BotTeamSpawn *oh = &g_origHq[team];
        if (oh->entity && oh->entity != ts->entity && cse_valid(oh->entity)) {   // the level's own HQ (where its jets are - at a round's
                                                                                  //  start they have not spawned yet: always)
            static BotTeamSpawn pts;
            pts = *oh; ts = &pts; ent = oh->entity; where = "HQ (pilot - the level's own, by the jets)";
            nj = 0;   // (no need to look further)
        }
        if (nj) {
            float hqD = 1e30f, bd2 = 150.0f * 150.0f;
            void *pick = NULL;
            for (int j = 0; j < nj; j++) { float q = dist2(jp[j], g_hqPos[team]); if (q < hqD) hqD = q; }
            if (strcmp(g_cseLevel, g_level) || !g_cseCount) cse_scan();   // every character spawn point (Wake's lower deck is no HQ candidate)
            for (int k = 0; k < g_hqCandCount + g_cseCount; k++) {
                void *c = k < g_hqCandCount ? g_hqCand[k] : g_cse[k - g_hqCandCount];
                typedef void *(__attribute__((thiscall)) *GetVeh_t)(void *);
                if (c == ts->entity || IsBadReadPtr(c, 0x100) || *(unsigned *)c != 0x025bbbc0u || !cse_valid(c) || spawn_team(c) == 3 - team) continue;
                if (((GetVeh_t)0x00dd7140)(c)) continue;   // (a vehicle spawner)
                float *cp = spawn_data_pos(c);
                if (!cp || near_origin(cp)) continue;
                for (int j = 0; j < nj; j++) { float q = dist2(jp[j], cp); if (q < bd2 && sqrtf(q) < sqrtf(hqD) - 100.0f) { bd2 = q; pick = c; } }
            }
            if (pick) { ent = pick; where = "HQ (pilot - by the jets)"; }
            static unsigned dbgAt;
            if (!pick && H->frame - dbgAt > 120 * 30) {   // (what the HQ candidates are, once in 2 min)
                dbgAt = H->frame;
                bot_log("pilot-hq: team %d, %d jet(s) (first at %.0f, %.0f), HQ %.0f m from them; %d + %d spawn point(s):", team, nj, jp[0][0], jp[0][2], sqrtf(hqD), g_hqCandCount, g_cseCount);
                for (int k = 0; k < g_hqCandCount + g_cseCount; k++) {
                    void *c = k < g_hqCandCount ? g_hqCand[k] : g_cse[k - g_hqCandCount];
                    typedef void *(__attribute__((thiscall)) *GetVeh_t)(void *);
                    int okc = !IsBadReadPtr(c, 0x100) && *(unsigned *)c == 0x025bbbc0u && cse_valid(c);
                    float *cp = okc ? spawn_data_pos(c) : NULL;
                    bot_log("pilot-hq:   %p %s team %d at (%.0f, %.0f, %.0f)%s", c, okc ? "ok" : "bad", okc ? spawn_team(c) : -1, cp ? cp[0] : 0, cp ? cp[1] : 0, cp ? cp[2] : 0,
                            okc && ((GetVeh_t)0x00dd7140)(c) ? " (vehicle spawner)" : "");
                }
            }
        }
    }
    if (!cse_valid(ent)) {   // a spawn point that is gone (or never was one): back to HQ, else wait
        bot_log("spawn: '%s' - spawn point %p (%s) is not valid", name, ent, where);
        if (cand >= 0) cand = -1;
        if (ent == ts->entity || noHq || !cse_valid(ts->entity)) { if (ent == ts->entity && !noHq) ts->entity = NULL; return; }
        ent = ts->entity; where = "HQ";
    }
    apply_kit(bot);
    void *r = ((CseSpawn_t)ADDR_CSE_SPAWN)(ent, bot, ts->a2, ts->a3, ts->a4);
    if (cand >= 0 && r) { HqProbe *q = &g_hqProbes[slot]; q->bot = bot; q->team = team; q->cand = cand; q->at = H->frame ? H->frame : 1; }
    if (cand >= 0 && !r) bot_log("spawn: HQ candidate %p refused team %d - dropped", ent, team);
    if (!r && ent != ts->entity && !noHq && cse_valid(ts->entity)) {   // flag spawn refused: fall back to HQ
        where = "HQ (flag spawn refused)";
        ent = ts->entity;
        r = ((CseSpawn_t)ADDR_CSE_SPAWN)(ent, bot, ts->a2, ts->a3, ts->a4);
    }
    bot_log("spawn: '%s' team=%d at %s via entity=%p -> soldier=%p", name, team, where, ent, r);
    {
        int k = bot_index(bot);
        if (k >= 0) { g_lastSpawnedBot[k] = bot; g_lastSpawnedAt[k] = H->frame; }
    }
}

static void cmd_pos(const char *name)
{
    char *pl = find_player(name);
    char *soldier = pl ? *(char **)(pl + OFF_PLAYER_SOLDIER) : NULL;
    if (!soldier) { bot_log("pos: '%s' has no soldier", name); return; }
    float *v = (float *)(soldier + OFF_SOLDIER_POSITION);
    float *s = (float *)(soldier + OFF_SOLDIER_SPAWN_POSITION);
    bot_log("pos: %s soldier=%p sim=(%.2f, %.2f, %.2f) transform=(%.2f, %.2f, %.2f) yaw=%.1fdeg", name, soldier, v[0], v[1], v[2], s[0], s[1], s[2],
            *(float *)(soldier + OFF_SOLDIER_YAW) * 180.0f / 3.14159265f);
}

__declspec(dllexport) void botmod_command(char *line)
{
    bot_log("> %s", line);
    // add / addh: a player clients list on the scoreboard (Tab); addai: an AI player (clients hide those - 2026-10-13 the
    // user wanted the console's test bots in the Tab list too, so plain "add" is no longer AI)
    int isAddH = !strncmp(line, "addh", 4) && (line[4] == 0 || line[4] == ' ');
    int isAddAI = !strncmp(line, "addai", 5) && (line[5] == 0 || line[5] == ' ');
    if (isAddH || isAddAI || (!strncmp(line, "add", 3) && (line[3] == 0 || line[3] == ' '))) {
        char first[64] = "";
        int team = 0;
        sscanf(line + (isAddAI ? 5 : isAddH ? 4 : 3), "%63s %d", first, &team);
        int n = atoi(first);
        if (n > 0) { for (int i = 0; i < n; i++) cmd_add(NULL, team, isAddAI); }
        else cmd_add(first, team, isAddAI);
    } else if (!strncmp(line, "remove ", 7)) {   // remove <bot#|name>: kill its soldier, then deletePlayer (the roster's removal)
        int k = -1;
        if (sscanf(line + 7, "%d", &k) != 1 || line[7] < '0' || line[7] > '9') k = bot_index(find_player(line + 7));
        if (k < 0 || k >= H->botCount) { bot_log("usage: remove <bot#|name>"); return; }
        g_flying[k] = 0; g_flyChase[k] = NULL; g_flySeek[k] = 0;
        queue_bot_removal(k);
    } else if (!strncmp(line, "setai ", 6)) {   // setai <bot#> <0|1>: ServerPlayer::setIsAIPlayer (0 = on the clients' scoreboard)
        int k = -1, v = 0;
        if (sscanf(line + 6, "%d %d", &k, &v) < 2 || k < 0 || k >= H->botCount) { bot_log("usage: setai <bot#> <0|1>"); return; }
        ((Player_SetAI_t)ADDR_PLAYER_SET_AI)(H->bots[k], v);
        bot_log("setai: %s ai=%d (flag now %d)", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, v, ((char *)H->bots[k])[OFF_PLAYER_IS_AI]);
    } else if (!strncmp(line, "spawn ", 6)) {
        char name[64] = "";
        sscanf(line + 6, "%63s", name);
        cmd_spawn(name);
    } else if (!strncmp(line, "move ", 5)) {
        char name[64] = "";
        float t = 0, s = 0, y = 0;
        int idx = -1;
        if (sscanf(line + 5, "%d %f %f %f", &idx, &t, &s, &y) == 4 && idx >= 0 && idx < H->botCount) snprintf(name, sizeof name, "%s", (char *)H->bots[idx] + OFF_PLAYER_ONLINE_NAME);
        else sscanf(line + 5, "%63s %f %f %f", name, &t, &s, &y);   // move <bot# | name without spaces> t s y
        int i = bot_index(find_player(name));
        if (i < 0) { bot_log("move: '%s' is not a bot", name); return; }
        bot_nav(i)->mode = 0;
        H->botMove[i][0] = t; H->botMove[i][1] = s; H->botMove[i][2] = y;
        bot_log("move: %s throttle=%.2f strafe=%.2f yaw=%.2f", name, t, s, y);
    } else if (!strncmp(line, "face ", 5)) {
        char name[64] = "";
        float yawDeg = 0, pitchDeg = 0;
        {   // face <name with spaces> <yawDeg> <pitchDeg>
            const char *e = line + strlen(line);
            int words = 0;
            while (e > line + 5 && words < 2) { e--; if (*e == ' ' && e[1] != ' ') words++; }
            snprintf(name, sizeof name, "%.*s", (int)(e - (line + 5)), line + 5);
            sscanf(e, "%f %f", &yawDeg, &pitchDeg);
        }
        int i = bot_index(find_player(name));
        if (i < 0) { bot_log("face: '%s' is not a bot", name); return; }
        bot_nav(i)->mode = 1;
        bot_nav(i)->targetYaw = wrap_pi(yawDeg * 3.14159265f / 180.0f);
        bot_aim(i)[1] = pitchDeg * 3.14159265f / 180.0f;
        bot_log("face: %s yaw=%.1f pitch=%.1f deg", name, yawDeg, pitchDeg);
    } else if (!strncmp(line, "aimmode ", 8)) {
        *aim_mode() = atoi(line + 8);
        for (int i = 0; i < H->botCount; i++)   // clear what we wrote so the engine's own values apply
            if (!*aim_mode()) *(int *)((char *)H->botInput[i] + OFF_INPUT_AIM_VALID) = 0;
        bot_log("aimmode %d", *aim_mode());
    } else if (!strncmp(line, "aim ", 4)) {
        char name[64] = "";
        float yawDeg = 0, pitchDeg = 0;
        sscanf(line + 4, "%63s %f %f", name, &yawDeg, &pitchDeg);
        int i = bot_index(find_player(name));
        if (i < 0) { bot_log("aim: '%s' is not a bot", name); return; }
        bot_nav(i)->mode = 0;
        H->botMove[i][2] = 0;
        bot_aim(i)[0] = yawDeg * 3.14159265f / 180.0f;
        bot_aim(i)[1] = pitchDeg * 3.14159265f / 180.0f;
        bot_log("aim: %s authoritative yaw=%.1f pitch=%.1f deg", name, yawDeg, pitchDeg);
    } else if (!strncmp(line, "goto ", 5)) {
        char name[64] = "";
        float x = 0, z = 0;
        {   // goto <name with spaces> <x> <z>
            const char *e = line + strlen(line);
            int words = 0;
            while (e > line + 5 && words < 2) { e--; if (*e == ' ' && e[1] != ' ') words++; }
            snprintf(name, sizeof name, "%.*s", (int)(e - (line + 5)), line + 5);
            sscanf(e, "%f %f", &x, &z);
        }
        int i = bot_index(find_player(name));
        if (i < 0) { bot_log("goto: '%s' is not a bot", name); return; }
        bot_nav(i)->mode = 2;
        bot_nav(i)->x = x;
        bot_nav(i)->z = z;
        bot_log("goto: %s -> (%.1f, %.1f)", name, x, z);
    } else if (!strncmp(line, "navbuild ", 9)) {
        char level[32] = "";
        float x0, z0, x1, z1, cell = 2.0f;
        if (sscanf(line + 9, "%31s %f %f %f %f %f", level, &x0, &z0, &x1, &z1, &cell) < 5) { bot_log("usage: navbuild <level> <x0> <z0> <x1> <z1> [cell]"); return; }
        nav_free(*nav_slot());
        *nav_slot() = nav_create(level, x0, z0, x1, z1, cell);
        *nav_building() = 1;
        bot_log("navbuild: %s %dx%d cells of %.1fm", level, (*nav_slot())->w, (*nav_slot())->h, cell);
    } else if (!strncmp(line, "navload ", 8)) {
        char path[MAX_PATH + 64];
        nav_file(path, sizeof path, line + 8);
        NavGrid *g = nav_load(path);
        if (!g) { bot_log("navload: cannot load %s", path); return; }
        nav_free(*nav_slot());
        *nav_slot() = g;
        *nav_building() = 0;
        bot_log("navload: %s %dx%d cells", g->level, g->w, g->h);
    } else if (!strcmp(line, "navrebuild")) {   // throw away this level's grid (memory + file); it is rebuilt now
        NavGrid *g = *nav_slot();
        if (!g || g->phase != 2) { bot_log("navrebuild: no finished grid loaded"); return; }
        char path[MAX_PATH + 64];
        nav_file(path, sizeof path, g->level);
        DeleteFileA(path);
        float x0 = g->x0 - g->cell * 0.5f, z0 = g->z0 - g->cell * 0.5f, x1 = x0 + g->w * g->cell, z1 = z0 + g->h * g->cell, cell = g->cell;
        char lvl[32];
        snprintf(lvl, sizeof lvl, "%s", g->level);
        *nav_slot() = nav_create(lvl, x0, z0, x1, z1, cell);
        *nav_building() = 1;
        nav_clear_links();   // (ladders re-link when the build is done)
        nav_free(g);
        bot_log("navrebuild: rebuilding %s (%.0f..%.0f x %.0f..%.0f, %.1f m cells)", lvl, x0, x1, z0, z1, cell);
    } else if (!strcmp(line, "navstatus")) {
        NavGrid *g = *nav_slot();
        if (!g) bot_log("nav: none");
        else {
            int water = 0, fences = 0, doors = 0;
            if (g->phase == 2)
                for (int k = 0; k < g->w * g->h * NAV_LAYERS; k++) {
                    water += (g->flags[k] & NAV_WATER) != 0;
                    for (int d = 0; d < 8; d++) { if (d & 1) doors += (g->fences[k] >> d) & 1; else fences += (g->fences[k] >> d) & 1; }
                }
            bot_log("nav: %s %dx%d phase=%d building=%d rays=%u, %d water node(s), %d knifeable fence crossing(s), %d doorway(s)", g->level, g->w,
                    g->h, g->phase, *nav_building(), g->raysCast, water, fences / 2, doors / 2);
        }
    } else if (!strncmp(line, "path ", 5)) {
        char name[64] = "";
        float x = 0, z = 0, y = NAN;
        {   // path <name with spaces> <x> <z>   |   path <name with spaces> <x> <z> y<height>
            const char *e = line + strlen(line), *ys = strrchr(line, ' ');
            int words = 0, want = ys && ys[1] == 'y' ? 3 : 2;
            if (want == 3) y = (float)atof(ys + 2);
            while (e > line + 5 && words < want) { e--; if (*e == ' ' && e[1] != ' ') words++; }
            snprintf(name, sizeof name, "%.*s", (int)(e - (line + 5)), line + 5);
            sscanf(e, "%f %f", &x, &z);
        }
        int i = bot_index(find_player(name));
        char *soldier = i >= 0 ? *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER) : NULL;
        if (!soldier) { bot_log("path: '%s' is not a living bot", name); return; }
        int n = plan_path(i, soldier, x, isnan(y) ? ((float *)(soldier + OFF_SOLDIER_POSITION))[1] : y, z);
        if (!n) { bot_log("path: no route for %s to (%.1f, %.1f): %s", name, x, z, nav_last_error()); return; }
        bot_nav(i)->mode = 3;
        bot_log("path: %s -> (%.1f, %.1f) via %d waypoint(s)", name, x, z, n);
        for (int k = 0; k < n && k < 12; k++) bot_log("    wp%d (%.1f, %.1f, %.1f)", k, g_paths[i].pts[k * 3], g_paths[i].pts[k * 3 + 1], g_paths[i].pts[k * 3 + 2]);
    } else if (!strcmp(line, "flags") || !strcmp(line, "flags rescan")) {
        if (!g_flagCount || line[5]) find_flags();
        for (int f = 0; f < g_flagCount; f++) bot_log("  flag %d at (%.1f, %.1f, %.1f) owner %d, in zone %d", f, flag_pos(f)[0], flag_pos(f)[1], flag_pos(f)[2], flag_owner(f),
                                          *(int *)(g_flags[f] + OFF_CP_CONTROL_TEAM));
    } else if (!strncmp(line, "brain ", 6)) {
        char name[64] = "", onoff[8] = "on";
        {   // brain <name with spaces | all> [on|off]
            const char *e = strrchr(line + 6, ' ');
            if (e && (!strcmp(e + 1, "on") || !strcmp(e + 1, "off"))) { snprintf(name, sizeof name, "%.*s", (int)(e - (line + 6)), line + 6); snprintf(onoff, sizeof onoff, "%s", e + 1); }
            else snprintf(name, sizeof name, "%s", line + 6);
        }
        int on = strcmp(onoff, "off") != 0;
        if (!g_flagCount) find_flags();
        for (int i = 0; i < H->botCount; i++) {
            if (_stricmp(name, "all") && _stricmp(name, (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME)) continue;
            memset(bot_brain(i), 0, sizeof(BotBrain));
            bot_brain(i)->enabled = on;
            g_brainHeld[i] = (unsigned char)!on;
            bot_brain(i)->targetFlag = -1;
            bot_brain(i)->avoidFlag = -1;
            if (!on) { bot_nav(i)->mode = 0; H->botMove[i][0] = H->botMove[i][2] = 0; }
            bot_log("brain: %s %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, on ? "on" : "off");
        }
    } else if (!strncmp(line, "sprintbits ", 11)) {   // sprintbits <bot#> <frames>: research - set the sprint flag bits that long
        int i = -1, frames = 90;
        sscanf(line + 11, "%d %d", &i, &frames);
        if (i < 0 || i >= H->botCount) { bot_log("usage: sprintbits <bot#> <frames>"); return; }
        g_sprintUntil[i] = H->frame + frames;
        bot_log("sprintbits: %s for %d frame(s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, frames);
    } else if (!strncmp(line, "setpose ", 8)) {   // setpose <bot#> <0 stand|1 crouch|2 prone>: research - write the wanted stance
        int i = -1, pose = 0;
        sscanf(line + 8, "%d %d", &i, &pose);
        char *s = i >= 0 && i < H->botCount ? *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER) : NULL;
        if (!s || pose < 0 || pose > 2) { bot_log("usage: setpose <bot#> <0|1|2>"); return; }
        bot_log("setpose: %s stance %d (to %d) -> to %d", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, *(int *)(s + OFF_SOLDIER_POSE), *(int *)(s + OFF_SOLDIER_POSE + 4), pose);
        *(int *)(s + OFF_SOLDIER_POSE + 4) = pose;
    } else if (!strncmp(line, "pressi ", 7)) {
        int i = -1, action = -1, frames = 1;
        sscanf(line + 7, "%d %d %d", &i, &action, &frames);
        if (i < 0 || i >= H->botCount || action < 0) { bot_log("usage: pressi <bot#> <action> [frames]"); return; }
        press_button(i, action, frames);
        bot_log("press: %s action %d for %d frame(s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, action, frames);
    } else if (!strncmp(line, "press ", 6)) {
        char name[64] = "";
        int action = -1, frames = 1;
        sscanf(line + 6, "%63s %d %d", name, &action, &frames);
        int i = bot_index(find_player(name));
        if (i < 0 || action < 0) { bot_log("usage: press <bot> <action> [frames]"); return; }
        press_button(i, action, frames);
        bot_log("press: %s action %d for %d frame(s)", name, action, frames);
    } else if (!strcmp(line, "loadouts")) {
        LoadoutLib *lib = loadout_lib();
        bot_log("loadouts: %d stored", lib->count);
        for (int i = 0; i < lib->count; i++) log_loadout(&lib->l[i], NULL);
    } else if (!strcmp(line, "capture on") || !strcmp(line, "capture off")) {
        char dir[MAX_PATH], path[MAX_PATH + 32];
        GetModuleFileNameA(NULL, dir, sizeof dir);
        *strrchr(dir, '\\') = 0;
        snprintf(path, sizeof path, "%s\\BotMod_capture.on", dir);
        if (line[9] == 'n') { FILE *f = fopen(path, "w"); if (f) fclose(f); } else DeleteFileA(path);
        bot_log("loadout capture %s (deploys %s change the bot library)", line[9] == 'n' ? "ON" : "OFF", line[9] == 'n' ? "WILL" : "will NOT");
    } else if (!strncmp(line, "place ", 6)) {   // place <bot name> <slot>: select that slot, fire, back to the rifle (gadget test)
        const char *e = strrchr(line + 6, ' ');
        char name[64] = "";
        if (e) snprintf(name, sizeof name, "%.*s", (int)(e - (line + 6)), line + 6);
        int i = bot_index(find_player(name)), slot = e ? atoi(e + 1) : -1;
        if (i < 0 || slot < 0 || slot > 9) { bot_log("usage: place <bot name> <slot>"); return; }
        place_gadget_now(i, slot);
        bot_log("place: %s uses slot %d", name, slot);
    } else if (!strcmp(line, "navunblock")) {   // every link blocked at stuck spots: open again now
        bot_log("navunblock: %d link(s) reopened", *nav_slot() ? nav_unblock_expired(*nav_slot(), 1) : 0);
    } else if (!strcmp(line, "loadouts load")) {   // reloads on the discovery worker (the asset search takes ~40 s: never on the game thread)
        g_jobDoLoadouts = 1;
        g_levelReady = 0;
        bot_log("loadouts: reloading in the background");
    } else if (!strcmp(line, "roster")) {
        // 4 bots per team named by role; creates whichever are missing.
        static const char *roles[4] = {"Medic", "Engineer", "Support", "Recon"};
        static const char *tag[3] = {"", "US", "RU"};
        char *pm = player_manager();
        for (int team = 1; team <= 2; team++) {
            for (int r = 0; r < 4; r++) {
                char name[32];
                snprintf(name, sizeof name, "[BOT] %s %s", roles[r], tag[team]);
                if (find_player(name)) continue;
                // (renaming an existing bot re-registers its network ghost and trips an engine assert: only create)
                {
                    cmd_add(name, team, 0);
                    bot_brain(H->botCount - 1)->enabled = 1;
                    bot_brain(H->botCount - 1)->targetFlag = -1;
                    bot_brain(H->botCount - 1)->avoidFlag = -1;
                }
            }
        }
    } else if (!strncmp(line, "vehicles", 8)) {   // vehicles [player]: every vehicle - type, position, health, free seats
        char *ref = line[8] == ' ' ? find_player(line + 9) : NULL;
        float *rp = ref ? human_pos(ref) : NULL;
        char **vehs;
        int nv = vehicle_list(&vehs);
        for (int k = 0; k < nv; k++) {
            char *v = vehs[k];
            if (IsBadReadPtr(v, 0x200) || *(unsigned *)v != ADDR_VEHICLE_VTABLE) continue;   // (stale cache entries)
            int vt = 0;
            float *vp = vehicle_pos(v, &vt);
            bot_log("  veh #%d %p %s team %d at (%.0f, %.0f, %.0f) hp %.0f, %d free seat(s)%s", k, v, vehicle_name(v), vt,
                    vp ? vp[0] : 0, vp ? vp[1] : 0, vp ? vp[2] : 0, *(float *)(v + OFF_VEHICLE_HEALTH), free_seat_count(v),
                    rp && vp ? (dist2(rp, vp) < 60.0f * 60.0f ? "  <- near" : "") : "");
        }
        bot_log("%d vehicle(s)", nv);
    } else if (!strcmp(line, "drive off") || !strcmp(line, "drive on")) {
        g_driveOff = line[7] == 'f';
        bot_log("drive: bots %s light vehicles", g_driveOff ? "do not take" : "may take");
    } else if (!strncmp(line, "driveto ", 8)) {   // driveto <bot#> <x> <z>: a bot at the wheel drives there
        int i = -1; float x = 0, z = 0;
        if (sscanf(line + 8, "%d %f %f", &i, &x, &z) != 3 || i < 0 || i >= H->botCount || !player_vehicle(H->bots[i])) { bot_log("usage: driveto <bot# at a wheel> <x> <z>"); return; }
        DriveState *d = &g_drive[i];
        memset(d, 0, sizeof *d);
        d->veh = player_vehicle(H->bots[i]); d->phase = 2; d->at = H->frame; d->flag = -1; d->manual = 1;
        d->goal[0] = x; d->goal[2] = z; d->goal[1] = NAN;
        for (int f = 0; f < g_flagCount; f++)   // at a flag: its height
            if ((flag_pos(f)[0] - x) * (flag_pos(f)[0] - x) + (flag_pos(f)[2] - z) * (flag_pos(f)[2] - z) < 60.0f * 60.0f) d->goal[1] = flag_pos(f)[1];
        float *vp = vehicle_pos(d->veh, NULL);
        if (vp) memcpy(d->start, vp, 12);
        g_driving[i] = 1;
        bot_log("driveto: %s -> (%.0f, %.0f)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, x, z);
    } else if (!strncmp(line, "drive ", 6)) {   // drive <bot#> <veh#>: put the bot in the driver's seat (research)
        int i = -1, k = -1;
        char **vehs;
        int nv = vehicle_list(&vehs);
        if (sscanf(line + 6, "%d %d", &i, &k) != 2 || i < 0 || i >= H->botCount || k < 0 || k >= nv) { bot_log("usage: drive <bot#> <veh#>"); return; }
        char **b;
        if (IsBadReadPtr(vehs[k], 0x200) || *(unsigned *)vehs[k] != ADDR_VEHICLE_VTABLE || !vehicle_seats(vehs[k], &b) || *(void **)(b[0] + 0x4c)) {
            bot_log("drive: no free driver's seat");
            return;
        }
        bot_nav(i)->mode = 0;
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        enter_vehicle(H->bots[i], vehs[k], 0);
        g_driving[i] = 1;
        bot_log("drive: %s -> driver of %p (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, vehs[k], player_vehicle(H->bots[i]) ? "in" : "FAILED");
    } else if (!strncmp(line, "enter ", 6)) {
        char name[64] = "", human[64] = "";
        sscanf(line + 6, "%63s %63s", name, human);
        int i = bot_index(find_player(name));
        char *h = find_player(human);
        if (i < 0 || !h || !player_vehicle(h)) { bot_log("usage: enter <bot> <player-in-a-vehicle>"); return; }
        bot_enter_vehicle(i, player_vehicle(h));
    } else if (!strncmp(line, "exit ", 5)) {
        int i = bot_index(find_player(line + 5));
        g_pilotForceExit = 1;
        if (i >= 0) bot_exit_vehicle(i, 1);
        g_pilotForceExit = 0;
    } else if (!strncmp(line, "spawnvia ", 9)) {   // spawnvia <bot> <target hex> [arg hex]: replay a human's spawn request
        char name[64] = ""; unsigned tgt = 0, arg = 1;     // (vslot 0x6c (player, -1, arg, 0) on the target - e.g. a jet spawner)
        if (sscanf(line + 9, "%63s %x %x", name, &tgt, &arg) < 2) { bot_log("usage: spawnvia <bot> <target hex> [arg hex]"); return; }
        char *bot = find_player(name);
        if (!bot || !is_bot(bot)) { bot_log("spawnvia: '%s' is not a bot", name); return; }
        if (*(void **)(bot + OFF_PLAYER_SOLDIER)) { bot_log("spawnvia: '%s' is alive", name); return; }
        if (!g_levelReady) { bot_log("spawnvia: waits - level not ready (its kits are not looked up yet)"); return; }
        if (!*(void **)(bot + OFF_PLAYER_BLUEPRINT)) {   // no SoldierBlueprint (a bot never spawned by "spawn"): the game does
            int team = player_team(bot) == 2 ? 2 : 1;     // not create the soldier - every jet spawner returned NULL (2026-10-13)
            if (!H->teamBlueprint[team]) { bot_log("spawnvia: no SoldierBlueprint known for team %d yet", team); return; }
            ((SetPtr_t)ADDR_PLAYER_SET_BLUEPRINT)(bot, H->teamBlueprint[team]);
            bot_log("spawnvia: '%s' given the team %d SoldierBlueprint", name, team);
        }
        if (!tgt || IsBadReadPtr((void *)tgt, 4) || IsBadReadPtr(*(void **)tgt, 0x70)) { bot_log("spawnvia: %08x is not an object", tgt); return; }
        unsigned fn = *(unsigned *)(*(unsigned *)tgt + 0x6c);
        if (fn < 0x401000 || fn >= 0x02028000) { bot_log("spawnvia: %08x's slot 0x6c (%08x) is not code", tgt, fn); return; }
        apply_kit(bot);
        void *r = ((CseSpawn_t)fn)((void *)tgt, bot, 0xffffffffu, arg, 0);
        bot_log("spawnvia: '%s' via %08x (vt %08x, fn %08x, arg %x) -> %p, soldier %p, vehicle %p", name, tgt, *(unsigned *)tgt, fn, arg, r,
                *(void **)(bot + OFF_PLAYER_SOLDIER), player_vehicle(bot));
    } else if (!strncmp(line, "spawnchk ", 9)) {   // spawnchk <bot> <spawn entity hex>: the entity's own spawn checks for the bot
        char name[64]; unsigned tgt = 0;
        if (sscanf(line + 9, "%63s %x", name, &tgt) < 2) { bot_log("usage: spawnchk <bot> <spawn entity hex>"); return; }
        char *bot = find_player(name);
        if (!bot || !tgt || IsBadReadPtr((void *)tgt, 0x100)) { bot_log("spawnchk: bad bot / entity"); return; }
        typedef void *(__attribute__((thiscall)) *Get_t)(void *);
        typedef char (__attribute__((thiscall)) *Chk_t)(void *);
        typedef char (__attribute__((thiscall)) *ChkP_t)(void *, void *);
        void *veh = ((Get_t)0x00dd7140)((void *)tgt);
        char en = ((Chk_t)0x00e44ea0)((void *)tgt);
        char ok = veh ? ((ChkP_t)0x00dd8f90)((void *)tgt, bot) : -1;
        void *own = veh ? ((Get_t)0x00e3e1d0)(veh) : NULL;
        bot_log("spawnchk: %s at %08x: vehicle %p (owner %p, its team %d) enabled %d, allowed %d; bot team %d, ai %d, timer %.2f",
                name, tgt, veh, own, own ? *(int *)((char *)own + 0xd8) : -1, en, ok, *(int *)(bot + 0x31c), bot[0x318], *(float *)(tgt + 0x88));
    } else if (!strncmp(line, "flyto ", 6)) {   // flyto <bot#> <x> <z> [alt]: autopilot towards a point (it circles back over it)
        int k = -1; float x = 0, z = 0, a = 400;
        if (sscanf(line + 6, "%d %f %f %f", &k, &x, &z, &a) < 3 || k < 0 || k >= H->botCount || !player_vehicle(H->bots[k])) {
            bot_log("usage: flyto <bot# in an aircraft> <x> <z> [altitude]"); return; }
        g_flying[k] = 1; g_flyAuto[k] = 1; g_flyCircuit[k] = 0; g_flyOrbit[k] = -1; g_flyGoal[k][0] = x; g_flyGoal[k][1] = z; g_flyGoal[k][2] = a; g_flyLogAt[k] = 0; g_flyAglAt[k] = 0;
        bot_log("flyto: %s -> (%.0f, %.0f) at %.0f m", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, x, z, a);
    } else if (!strncmp(line, "flyorbit ", 9)) {   // flyorbit <bot#> <flag#> [radius] [height]: circle that flag wide, passive (no guns)
        int k = -1, f = -1; float r = 600, up = 100;
        if (sscanf(line + 9, "%d %d %f %f", &k, &f, &r, &up) < 2 || k < 0 || k >= H->botCount || !player_vehicle(H->bots[k]) || f < 0 || f >= g_flagCount) {
            bot_log("usage: flyorbit <bot# in an aircraft> <flag# 0..%d> [radius]", g_flagCount - 1); return; }
        char *b = H->bots[k];
        g_flying[k] = 1; g_flyAuto[k] = 1; g_flyCircuit[k] = 0; g_flyChase[k] = NULL; g_flyOrbit[k] = (signed char)f; g_flyOrbitR[k] = fmaxf(250.0f, r); g_flyOrbitUp[k] = up;
        g_flyLogAt[k] = 0; g_flyAglAt[k] = 0;
        bot_log("flyorbit: %s circles flag %d at %.0f m, passive (player %p soldier %p vehicle %p)", b + OFF_PLAYER_ONLINE_NAME, f, g_flyOrbitR[k],
                b, *(void **)(b + OFF_PLAYER_SOLDIER), player_vehicle(b));
    } else if (!strncmp(line, "msldebug ", 9)) {   // msldebug 0|1: log each new missile / flare and its pointers to players
        g_mslDebug = atoi(line + 9);
        bot_log("msldebug: %s", g_mslDebug ? "on" : "off");
    } else if (!strncmp(line, "flare ", 6)) {   // flare <bot#> [count] [eia]: countermeasures, 1 s apart
        int k = -1, n = 3, e = g_flareEia;
        if (sscanf(line + 6, "%d %d %d", &k, &n, &e) < 1 || k < 0 || k >= H->botCount) { bot_log("usage: flare <bot#> [count] [eia]"); return; }
        g_flareEia = e;
        for (int q = 0; q < n && q < 8; q++) schedule_press(k, e, H->frame + 1 + q * 30);
        bot_log("flare: %s - %d x EIA %d, 1 s apart", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, n, e);
    } else if (!strncmp(line, "flycircuit ", 11)) {   // flycircuit <bot#>: fly the Air Superiority flag circuit
        int k = atoi(line + 11);
        if (k < 0 || k >= H->botCount || !player_vehicle(H->bots[k])) { bot_log("usage: flycircuit <bot# in an aircraft>"); return; }
        g_flying[k] = 1; g_flyAuto[k] = 1; g_flyCircuit[k] = 1; g_flyOrbit[k] = -1; g_flyFlag[k] = -1; g_flyLogAt[k] = 0; g_flyAglAt[k] = 0;
        bot_log("flycircuit: %s flies the flags", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME);
    } else if (!strncmp(line, "flychase ", 9)) {   // flychase <bot#> <player> | flychase <bot#> off: hunt that player's aircraft
        int k = -1; char who[64] = "";
        if (sscanf(line + 9, "%d %63s", &k, who) < 2 || k < 0 || k >= H->botCount || !player_vehicle(H->bots[k])) { bot_log("usage: flychase <bot# in an aircraft> <player|off>"); return; }
        if (!strcmp(who, "off")) { g_flyChase[k] = NULL; g_flySeek[k] = 0; bot_log("flychase: %s off", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME); return; }
        char *t = find_player(who);
        if (!t) { bot_log("flychase: no player '%s'", who); return; }
        g_flying[k] = 1; g_flyAuto[k] = 1; g_flyOrbit[k] = -1; g_flyChase[k] = t; g_flyTgtAt[k] = 0; memset(g_flyTgtVel[k], 0, 12); g_flyLogAt[k] = 0; g_flyAglAt[k] = 0;
        g_flySeek[k] = 0;
        bot_log("flychase: %s hunts %s", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, t + OFF_PLAYER_ONLINE_NAME);
    } else if (!strncmp(line, "flyseek ", 8)) {   // flyseek <bot#> <player|off> [fire]: heat-seeker chase, BotMod's own lock
        int k = -1, f = 0; char who[64] = "";
        if (sscanf(line + 8, "%d %63s %d", &k, who, &f) < 2 || k < 0 || k >= H->botCount || !player_vehicle(H->bots[k])) { bot_log("usage: flyseek <bot# in an aircraft> <player|off> [fire 0/1]"); return; }
        if (!strcmp(who, "off")) { g_flyChase[k] = NULL; g_flySeek[k] = 0; bot_log("flyseek: %s off", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME); return; }
        char *t = find_player(who);
        if (!t) { bot_log("flyseek: no player '%s'", who); return; }
        g_flying[k] = 1; g_flyAuto[k] = 1; g_flyOrbit[k] = -1; g_flyCircuit[k] = 0; g_flyChase[k] = t; g_flyTgtAt[k] = 0; memset(g_flyTgtVel[k], 0, 12); g_flyLogAt[k] = 0; g_flyAglAt[k] = 0;
        g_flySeek[k] = 1; g_seekFire[k] = (unsigned char)f; g_seekSince[k] = 0; g_seekSelAt[k] = 0; g_seekFiredAt[k] = 0;
        g_seekShots[k] = 0; g_seekGunsUntil[k] = 0; g_seekNoLockSince[k] = 0;
        bot_log("flyseek: %s hunts %s with heat-seekers (lock: %.0f m, %.1f deg, %.1f s; fire %s, EIA %d)", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME,
                t + OFF_PLAYER_ONLINE_NAME, g_seekRange, g_seekCone, g_seekTime, f ? "on" : "off", g_seekFireEia);
    } else if (!strncmp(line, "seekreload ", 11)) {   // seekreload <s>: how long the cannon is used after 2 missiles
        g_seekReload = (float)atof(line + 11); bot_log("seekreload: cannon for %.0f s after 2 missiles", g_seekReload);
    } else if (!strncmp(line, "seeklock ", 9)) {   // seeklock <range m> <cone deg> <secs> [fire eia]
        sscanf(line + 9, "%f %f %f %d", &g_seekRange, &g_seekCone, &g_seekTime, &g_seekFireEia);
        bot_log("seeklock: %.0f m, %.1f deg, %.1f s, fire EIA %d", g_seekRange, g_seekCone, g_seekTime, g_seekFireEia);
    } else if (!strncmp(line, "diveaim ", 8)) {   // diveaim <m>: how far above a ground target the dive aims
        g_diveAimUp = (float)atof(line + 8); bot_log("diveaim: %.1f m above the target", g_diveAimUp);
    } else if (!strncmp(line, "vguns ", 6)) {   // vguns <bot#>: the weapons on the bot's vehicle - projectile speed / gravity / fx
        int k = atoi(line + 6);
        char *veh = k >= 0 && k < H->botCount ? player_vehicle(H->bots[k]) : NULL;
        float *vp = veh ? vehicle_xf(veh) : NULL;
        if (!vp) { bot_log("usage: vguns <bot# in a vehicle>"); return; }
        CompCache *cc = &g_compCache[g_compCur];
        int n = 0;
        for (int q = 0; q < cc->gcount; q++) {
            char *w = cc->gun[q];
            if (!gun_ok(w)) continue;
            float *wp = (float *)(w + 0x60), dx = wp[0] - vp[12], dy = wp[1] - vp[13], dz = wp[2] - vp[14];
            if (dx * dx + dy * dy + dz * dz > 15.0f * 15.0f) continue;
            float sp, gr; const char *fx;
            vweapon_ballistics(w, &sp, &gr, &fx);
            float *f = (float *)(w + 0x50);
            bot_log("vguns: %p %.0f m/s gravity %.2f fwd (%.2f %.2f %.2f) vs nose (%.2f %.2f %.2f) %s", w, sp, gr, f[0], f[1], f[2], vp[8], vp[9], vp[10], fx);
            n++;
        }
        bot_log("vguns: %d weapon(s) near the %s", n, vehicle_name(veh));
    } else if (!strncmp(line, "flysign ", 8)) {
        g_flyTurnSign = (float)atof(line + 8) < 0 ? -1.0f : 1.0f;
        bot_log("fly: turn sign %.0f", g_flyTurnSign);
    } else if (!strncmp(line, "flytest ", 8)) {
        int k = -1, fr = 30; float r = 0, pt = 0, th = 0, yw = 0; char off[8] = "";
        if (sscanf(line + 8, "%d %7s", &k, off) == 2 && !strcmp(off, "off") && k >= 0 && k < H->botCount) {
            g_flying[k] = 0; g_flyAuto[k] = 0; g_driving[k] = 0; bot_log("flytest: %s off", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME); return; }
        if (sscanf(line + 8, "%d %f %f %f %f %d", &k, &r, &pt, &th, &yw, &fr) < 5 || k < 0 || k >= H->botCount || !player_vehicle(H->bots[k])) {
            bot_log("usage: flytest <bot# in an aircraft> <roll> <pitch> <throttle> <yaw> [frames] | flytest <bot#> off"); return; }
        g_flying[k] = 1; g_flyAuto[k] = 0; g_flyIn[k][0] = r; g_flyIn[k][1] = pt; g_flyIn[k][2] = th; g_flyIn[k][3] = yw;
        g_flyUntil[k] = H->frame + (unsigned)fr; g_flyLogAt[k] = 0;
        bot_log("flytest: %s roll %.2f pitch %.2f throttle %.2f yaw %.2f for %d frames", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, r, pt, th, yw, fr);
    } else if (!strncmp(line, "gunmax ", 7)) {   // gunmax <level>: the most traverse input a driver's main gun may use
        g_gunMaxLevel = (float)atof(line + 7);
        bot_log("gunner: main gun traverse input capped at %.2f", g_gunMaxLevel);
    } else if (!strncmp(line, "gunff ", 6)) {    // gunff <level per rad/s>: main gun feed-forward
        g_gunFeedFwd = (float)atof(line + 6);
        bot_log("gunner: main gun feed-forward %.2f", g_gunFeedFwd);
    } else if (!strncmp(line, "pilot ", 6)) {   // pilot <bot#> <0|1>: treat a bot as a Pilot (never leaves its vehicle)
        int i = -1, on = 1;
        if (sscanf(line + 6, "%d %d", &i, &on) < 1 || i < 0 || i >= H->botCount) { bot_log("usage: pilot <bot#> [0|1]"); return; }
        g_pilotOn[i] = (unsigned char)(on != 0);
        bot_log("pilot: %s is %sa Pilot", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, is_pilot(i) ? "" : "not ");
    } else if (!strncmp(line, "crouchaction ", 13)) {
        g_crouchAction = atoi(line + 13);
        bot_log("crouch action = %d", g_crouchAction);
    } else if (!strncmp(line, "say ", 4)) {
        char name[64] = "";
        int type = 0;
        sscanf(line + 4, "%63s %d", name, &type);
        int i = bot_index(find_player(name));
        if (i < 0) { bot_log("usage: say <bot> <orderType>"); return; }
        bot_say(i, type);
    } else if (!strncmp(line, "kill ", 5)) {
        char *pl = find_player(line + 5);
        if (!is_bot(pl)) bot_log("kill: '%s' is not a bot", line + 5); else { kill_bot(pl); bot_log("kill: %s", line + 5); }
    } else if (!strncmp(line, "rayflags ", 9)) {
        g_rayFlags = strtoul(line + 9, NULL, 0);
        bot_log("rayflags 0x%x", g_rayFlags);
    } else if (!strncmp(line, "navprobe ", 9)) {   // navprobe x0 z0 x1 z1 fromY: add ground missed under a bridge / roof
        float x0, z0, x1, z1, fy;
        NavGrid *g = *nav_slot();
        if (sscanf(line + 9, "%f %f %f %f %f", &x0, &z0, &x1, &z1, &fy) != 5 || !g || g->phase != 2) { bot_log("usage: navprobe x0 z0 x1 z1 fromY (nav grid ready)"); return; }
        int n = nav_reprobe_area(g, ray_cast, x0, z0, x1, z1, fy);
        bot_log("navprobe: %d ground node(s) added in (%.0f..%.0f, %.0f..%.0f) from y %.1f", n, x0, x1, z0, z1, fy);
        g_navDirty = 1;
        nav_save_if_dirty();
    } else if (!strncmp(line, "navautorepair ", 14)) {
        g_autoRepair = atoi(line + 14);
        bot_log("navautorepair %d", g_autoRepair);
    } else if (!strncmp(line, "botpath ", 8)) {   // botpath <bot#>: the route a bot is following (goal, next waypoint, all points)
        int i = atoi(line + 8);
        if (i < 0 || i >= H->botCount) { bot_log("usage: botpath <bot#>"); return; }
        BotPath *p = &g_paths[i];
        BotBrain *b = bot_brain(i);
        char out[1000];
        int k = snprintf(out, sizeof out, "botpath: %s nav mode %d task %s flag %d - goal (%.0f,%.1f,%.0f), next %d of %d:", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                         bot_nav(i)->mode, kTaskNames[b->task], b->targetFlag, p->goalX, p->goalY, p->goalZ, p->next, p->count);
        for (int q = 0; q < p->count && k < (int)sizeof out - 40; q++)
            k += snprintf(out + k, sizeof out - k, "%s(%.0f,%.1f,%.0f)%s", q == p->next ? " >" : " ", p->pts[q * 3], p->pts[q * 3 + 1], p->pts[q * 3 + 2], p->link[q] >= 0 ? "L" : "");
        bot_log("%s", out);
    } else if (!strncmp(line, "edgetest ", 9)) {   // edgetest x y z d: why the link from there in direction d (0..7) is / is not there
        float x, y, z;
        int d = 0;
        NavGrid *g = *nav_slot();
        if (sscanf(line + 9, "%f %f %f %d", &x, &y, &z, &d) != 4 || d < 0 || d > 7 || !g || g->phase != 2) { bot_log("usage: edgetest x y z dir(0..7: +x,+x+z,+z,-x+z,-x,-x-z,-z,+x-z)"); return; }
        char out[700];
        nav_debug_edge(g, ray_cast, x, y, z, d, out, sizeof out);
        bot_log("edgetest: %s", out);
    } else if (!strncmp(line, "navroute ", 9)) {   // navroute x y z tx ty tz: the soldier route between two points (waypoints)
        float s[3], t[3], pts[128 * 3];
        NavGrid *g = *nav_slot();
        if (sscanf(line + 9, "%f %f %f %f %f %f", &s[0], &s[1], &s[2], &t[0], &t[1], &t[2]) != 6 || !g || g->phase != 2) { bot_log("usage: navroute x y z tx ty tz"); return; }
        int n = nav_find_path(g, ray_cast, s[0], s[1], s[2], t[0], t[1], t[2], pts, 128);
        if (!n) { bot_log("navroute: no route (%s)", nav_last_error()); return; }
        char out[900];
        int k = snprintf(out, sizeof out, "navroute: %d point(s):", n);
        for (int q = 0; q < n && k < (int)sizeof out - 40; q++) k += snprintf(out + k, sizeof out - k, " (%.0f,%.1f,%.0f)", pts[q * 3], pts[q * 3 + 1], pts[q * 3 + 2]);
        bot_log("%s", out);
    } else if (!strncmp(line, "navtest ", 8)) {   // navtest x y z: reachable nodes from there on the live grid
        float x, y, z, np[3];
        NavGrid *g = *nav_slot();
        if (sscanf(line + 8, "%f %f %f", &x, &y, &z) != 3 || !g || g->phase != 2) { bot_log("usage: navtest x y z (nav grid ready)"); return; }
        int n = nav_reach(g, x, y, z, np);
        bot_log("navtest: from (%.1f, %.1f, %.1f) node (%.1f, %.1f, %.1f) reaches %d nodes", x, y, z, np[0], np[1], np[2], n);
    } else if (!strncmp(line, "navrepair ", 10)) {   // navrepair <x> <z> [radius]: fine slope check of the links around a spot
        float x, z, r = 8.0f;
        NavGrid *g = *nav_slot();
        if (sscanf(line + 10, "%f %f %f", &x, &z, &r) < 2 || !g || g->phase != 2) { bot_log("usage: navrepair x z [radius] (nav grid ready)"); return; }
        float top[3] = {x, 400.0f, z}, bot[3] = {x, -100.0f, z}, gp[3];   // the real ground there (not the lowest nav layer)
        if (!ray_cast(top, bot, gp, NULL)) { bot_log("navrepair: no ground at (%.1f, %.1f)", x, z); return; }
        int n = nav_repair_around(g, ray_cast, x, gp[1], z, r, 0, 0);
        if (n) g_navDirty = 1;
        bot_log("navrepair: %d link(s) removed around (%.1f, %.1f), r %.0f m", n, x, z, r);
        nav_save_if_dirty();
    } else if (!strncmp(line, "knife ", 6)) {   // knife <bot name>: the fence-cutting sequence, whatever is in front
        int i = bot_index(find_player(line + 6));
        char *s = i >= 0 ? *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER) : NULL;
        if (!s) { bot_log("knife: '%s' is not a living bot", line + 6); return; }
        g_fenceAt[i] = 0;
        const FenceKind *fk = fence_ahead(i, s);
        bot_log("knife: ahead of %s: %s (material %08x)", line + 6, fk ? fk->name : "nothing catalogued", hit_material());
        unsigned f = H->frame;
        schedule_press(i, SLOT_ACTION(7), f + 1);
        schedule_press(i, EIA_FIRE_KNIFE, f + 25);
        schedule_press(i, EIA_FIRE_KNIFE, f + 45);
        schedule_press(i, EIA_FIRE_KNIFE, f + 65);
        schedule_press(i, SLOT_ACTION(0), f + 90);
    } else if (!strncmp(line, "roadwatch ", 10)) {   // roadwatch <player|off>: every 2 s, what the taxi maps make of their spot (log + chat)
        extern char g_roadWatch[64];
        snprintf(g_roadWatch, sizeof g_roadWatch, "%s", strcmp(line + 10, "off") ? line + 10 : "");
        bot_log("roadwatch: %s", g_roadWatch[0] ? g_roadWatch : "off");
    } else if (!strncmp(line, "ground ", 7)) {   // ground <player>: the surface material under them and 3 m around
        char *pl = find_player(line + 7);
        float *hp = pl ? human_pos(pl) : NULL;
        if (!hp) { bot_log("ground: '%s' not found / not spawned", line + 7); return; }
        static const float ring[9][2] = {{0, 0}, {3, 0}, {-3, 0}, {0, 3}, {0, -3}, {2.1f, 2.1f}, {-2.1f, 2.1f}, {2.1f, -2.1f}, {-2.1f, -2.1f}};
        char out[600];
        int n = snprintf(out, sizeof out, "ground under %s (%.0f, %.0f):", line + 7, hp[0], hp[2]);
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;
        for (int k = 0; k < 9; k++) {
            float a[3] = {hp[0] + ring[k][0], hp[1] + 3.0f, hp[2] + ring[k][1]}, b[3] = {a[0], hp[1] - 5.0f, a[2]}, h[3];
            if (!ray_cast(a, b, h, NULL)) { n += snprintf(out + n, sizeof out - n, " [none]"); continue; }
            char *body = *(char **)g_lastHitRest;
            unsigned bvt = body && !IsBadReadPtr(body, 4) ? *(unsigned *)body : 0;
            n += snprintf(out + n, sizeof out - n, " %s%08x", bvt == 0x02486a40 ? "water:" : "", hit_material());
        }
        g_rayFlags = saved;
        bot_log("%s", out);
    } else if (!strncmp(line, "squadspawn ", 11)) {   // squadspawn <bot#> <player>: spawn a dead bot on a squadmate (research)
        int i = -1, used = 0;
        if (sscanf(line + 11, "%d %n", &i, &used) < 1 || i < 0 || i >= H->botCount) { bot_log("usage: squadspawn <bot#> <squadmate>"); return; }
        char *bot = H->bots[i], *mate = find_player(line + 11 + used);
        if (!mate) { bot_log("squadspawn: no player '%s'", line + 11 + used); return; }
        if (*(void **)(bot + OFF_PLAYER_SOLDIER)) { bot_log("squadspawn: %s is alive - kill it first", bot + OFF_PLAYER_ONLINE_NAME); return; }
        int team = player_team(bot) == 2 ? 2 : 1;
        if (!*(void **)(bot + OFF_PLAYER_BLUEPRINT) && H->teamBlueprint[team]) ((SetPtr_t)ADDR_PLAYER_SET_BLUEPRINT)(bot, H->teamBlueprint[team]);
        apply_kit(bot);
        typedef char (__attribute__((thiscall)) *SpawnOnSquad_t)(void *extent, void *mate);
        int extentOff = *(int *)0x0285bad8;   // ServerPlayer -> its VeniceServerPlayerExtent
        char r = ((SpawnOnSquad_t)0x01185b50)(bot + extentOff, mate);
        char *s = *(char **)(bot + OFF_PLAYER_SOLDIER);
        float *mp = human_pos(mate);
        bot_log("squadspawn: %s on %s (squads %d / %d): %s, soldier %p at (%.0f, %.0f) - mate at (%.0f, %.0f)", bot + OFF_PLAYER_ONLINE_NAME,
                mate + OFF_PLAYER_ONLINE_NAME, player_squad(bot), player_squad(mate), r ? "ok" : "refused", s, s ? ((float *)(s + OFF_SOLDIER_SPAWN_POSITION))[0] : 0,
                s ? ((float *)(s + OFF_SOLDIER_SPAWN_POSITION))[2] : 0, mp ? mp[0] : 0, mp ? mp[2] : 0);
    } else if (!strcmp(line, "ladders")) {   // (re)scan this level's ladders, list and link them
        find_ladders();
        ladders_link();
    } else if (!strncmp(line, "heightmap ", 10)) {   // heightmap <x> <z> <half size m> <step m>: ground heights around a point
        float cx, cz, hs = 3, st = 0.5f, fromY = 400.0f;
        if (sscanf(line + 10, "%f %f %f %f %f", &cx, &cz, &hs, &st, &fromY) < 2) { bot_log("usage: heightmap x z [half] [step] [ray start y]"); return; }
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;
        bot_log("heightmap around (%.1f, %.1f), rows = z from high to low, columns = x from low to high, step %.2f", cx, cz, st);
        for (float z = cz + hs; z >= cz - hs - 0.001f; z -= st) {
            char row[1200];
            int n = snprintf(row, sizeof row, "z %7.2f:", z);
            for (float x = cx - hs; x <= cx + hs + 0.001f && n < (int)sizeof row - 8; x += st) {
                float a[3] = {x, fromY, z}, b[3] = {x, -100.0f, z}, p[3];
                if (ray_cast(a, b, p, NULL)) n += snprintf(row + n, sizeof row - n, " %6.2f", p[1]);
                else n += snprintf(row + n, sizeof row - n, "   ----");
            }
            bot_log("%s", row);
        }
        g_rayFlags = saved;
    } else if (!strncmp(line, "ray ", 4)) {
        float a[3], b[3], p[3] = {0}, n[3] = {0};
        char name[64];
        if (sscanf(line + 4, "%f %f %f %f %f %f", &a[0], &a[1], &a[2], &b[0], &b[1], &b[2]) == 6) {
            int h = ray_cast(a, b, p, n);
            bot_log("ray: hit=%d pos=(%.2f, %.2f, %.2f) normal=(%.2f, %.2f, %.2f) material %08x", h, p[0], p[1], p[2], n[0], n[1], n[2],
                    h ? hit_material() : 0);
            if (h) {
                char line[400];
                int k = snprintf(line, sizeof line, "ray: record");
                for (int o = 0; o < 0x40; o += 4) k += snprintf(line + k, sizeof line - k, " +%02x=%08x", 0x20 + o, *(unsigned *)(g_lastHitRest + o));
                bot_log("%s", line);
            }
        } else if (sscanf(line + 4, "%63s", name) == 1 && find_player(name) && *(char **)((char *)find_player(name) + OFF_PLAYER_SOLDIER)) {
            float *sp = (float *)(*(char **)((char *)find_player(name) + OFF_PLAYER_SOLDIER) + OFF_SOLDIER_POSITION);
            float top[3] = {sp[0], sp[1] + 50, sp[2]}, bot[3] = {sp[0], sp[1] - 50, sp[2]};
            int h = ray_cast(top, bot, p, n);
            bot_log("ray down at %s (%.1f, %.1f, %.1f): hit=%d pos=(%.2f, %.2f, %.2f) normal=(%.2f, %.2f, %.2f)", name, sp[0], sp[1], sp[2],
                    h, p[0], p[1], p[2], n[0], n[1], n[2]);
        }
    } else if (!strncmp(line, "botcount ", 9)) {
        int n = atoi(line + 9);
        *wanted_bots() = n < 0 ? 0 : n > MAX_BOTS ? MAX_BOTS : n;
        g_rosterAuto = 1;
        save_bot_count();
        bot_log("roster: wanted bot count %d", *wanted_bots());
    } else if (!strncmp(line, "lookpitch ", 10)) {   // lookpitch <bot#> <radians | off>  (test the pitch control)
        int i = -1; char v[16] = "";
        sscanf(line + 10, "%d %15s", &i, v);
        if (i < 0 || i >= H->botCount) { bot_log("usage: lookpitch <bot#> <radians|off>"); return; }
        g_pitchTestOn[i] = strcmp(v, "off") != 0;
        g_pitchTest[i] = (float)atof(v);
        char *s = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
        bot_log("lookpitch: bot %d target %s, now %.3f", i, v, s ? *(float *)(s + OFF_SOLDIER_PITCH) : 0.0f);
    } else if (!strncmp(line, "shoot ", 6)) {   // shoot <bot#> <target bot#>: test bot gunfire (logs the target's health)
        int i = -1, t = -1;
        sscanf(line + 6, "%d %d", &i, &t);
        if (i < 0 || t < 0 || i >= H->botCount || t >= H->botCount || !*(char **)(H->bots[i] + OFF_PLAYER_SOLDIER)) { bot_log("usage: shoot <bot#> <target bot#>"); return; }
        start_fight(i, H->bots[t], 0, "test");
        g_testVictim = H->bots[t];
    } else if (!strncmp(line, "cover ", 6)) {   // cover <bot#>: act as if just hit
        int i = atoi(line + 6);
        char *s = i >= 0 && i < H->botCount ? *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER) : NULL;
        if (!s) { bot_log("usage: cover <living bot#>"); return; }
        g_lastHealth[i] = *(float *)(s + OFF_SOLDIER_HEALTH) + 10.0f;
        cover_react(i, s);
    } else if (!strcmp(line, "ammo")) {   // rounds per weapon slot for every bot (loaded + spare)
        for (int i = 0; i < H->botCount; i++) {
            char row[160];
            int n = 0;
            for (int s = 0; s < 8; s++) {
                int a = bot_ammo(i, s);
                if (a >= 0) n += snprintf(row + n, sizeof row - n, " s%d=%d", s, a);
            }
            bot_log("ammo: %s%s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, n ? row : " (unknown yet)");
        }
    } else if (!strncmp(line, "inwatch ", 8)) {   // inwatch <player> [seconds]: log every input level that changes, and stance
        char name[64] = "";
        int secs = 20;
        const char *e = strrchr(line + 8, ' ');
        if (e && atoi(e + 1) > 0) { secs = atoi(e + 1); snprintf(name, sizeof name, "%.*s", (int)(e - (line + 8)), line + 8); }
        else snprintf(name, sizeof name, "%s", line + 8);
        g_inWatchPl = find_player(name);
        g_inWatchUntil = H->frame + secs * 30;
        memset(g_inWatchPrev, 0, sizeof g_inWatchPrev);
        { char *wp = g_inWatchPl, *win = wp ? *(char **)(wp + OFF_PLAYER_INPUT) : NULL; if (win && !IsBadReadPtr(win, 0x400)) memcpy(g_inWatchRaw, win, 0x400); }
        g_inWatchPose = -1;
        bot_log("inwatch: %s for %d s", g_inWatchPl ? name : "(no such player)", secs);
    } else if (!strncmp(line, "inaim ", 6)) {   // inaim <player>: research - a player's input aim fields (humans: their client's)
        char *pl = find_player(line + 6);
        if (!pl) { bot_log("inaim: no player '%s'", line + 6); return; }
        char *in = *(char **)(pl + OFF_PLAYER_INPUT), *veh = player_vehicle(pl), *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
        if (!in) { bot_log("inaim: %s has no input", line + 6); return; }
        float *vp = veh ? vehicle_pos(veh, NULL) : NULL;
        bot_log("inaim: %s valid %d aim yaw %.3f pitch %.3f (+1f0 %.3f) | levels yaw %.2f pitch %.2f fire %.2f | veh %p at (%.1f %.1f %.1f) | soldier yaw %.3f pitch %.3f",
                line + 6, *(int *)(in + OFF_INPUT_AIM_VALID), *(float *)(in + OFF_INPUT_AIM_YAW), *(float *)(in + OFF_INPUT_AIM_PITCH),
                *(float *)(in + 0x1f0), *(float *)(in + 0x10 + 4 * 4), *(float *)(in + 0x10 + 5 * 4), *(float *)(in + 0x10 + 7 * 4),
                veh, vp ? vp[0] : 0, vp ? vp[1] : 0, vp ? vp[2] : 0, s ? *(float *)(s + OFF_SOLDIER_YAW) : 0, s ? *(float *)(s + OFF_SOLDIER_PITCH) : 0);
    } else if (!strncmp(line, "gundebug ", 9)) {
        g_gunDebug = atoi(line + 9);
    } else if (!strncmp(line, "seatlv ", 7)) {   // seatlv <bot#> <yaw level> <pitch level>
        int k = -1; float y = 0, pt = 0;
        if (sscanf(line + 7, "%d %f %f", &k, &y, &pt) == 3 && k >= 0 && k < H->botCount) { g_seatYaw[k] = y; g_seatPitch[k] = pt; bot_log("seatlv %d %.2f %.2f", k, y, pt); }
    } else if (!strncmp(line, "vinput ", 7)) {
        g_vehInput = atoi(line + 7);
        bot_log("vinput %d", g_vehInput);
    } else if (!strncmp(line, "vaim ", 5)) {   // vaim <bot#> <yaw rad> <pitch rad> | vaim <bot#> off  (research: absolute aim in a seat)
        int k = -1; float y = 0, pt = 0; char off[8] = "";
        if (sscanf(line + 5, "%d %7s", &k, off) == 2 && !strcmp(off, "off") && k >= 0 && k < H->botCount) {
            g_vaimOn[k] = 0; *(int *)((char *)H->botInput[k] + OFF_INPUT_AIM_VALID) = 0; bot_log("vaim %d off", k); return;
        }
        if (sscanf(line + 5, "%d %f %f", &k, &y, &pt) != 3 || k < 0 || k >= H->botCount) { bot_log("usage: vaim <bot#> <yaw> <pitch> | off"); return; }
        g_vaimOn[k] = 1; g_vaimYaw[k] = y; g_vaimPitch[k] = pt;
        bot_log("vaim: %s aim yaw %.3f pitch %.3f", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, y, pt);
    } else if (!strncmp(line, "vfire ", 6)) {   // vfire <bot#> <frames>: hold fire
        int k = -1, f = 30;
        sscanf(line + 6, "%d %d", &k, &f);
        if (k >= 0 && k < H->botCount) { press_button(k, EIA_FIRE, f); bot_log("vfire: %s %d frames", (char *)H->bots[k] + OFF_PLAYER_ONLINE_NAME, f); }
    } else if (!strncmp(line, "kitwatch ", 9)) {
        g_kitWatch = atoi(line + 9);
        bot_log("kitwatch %d", g_kitWatch);
    } else if (!strcmp(line, "kitdump")) {   // what the bots are handed, per loadout and item
        LoadoutLib *lib = loadout_lib();
        for (int i = 0; i < lib->count; i++) {
            char who[96];
            for (int w = 0; w < lib->l[i].nWeapons; w++) {
                snprintf(who, sizeof who, "LIBRARY team %d %s", lib->l[i].team, kit_class(lib->l[i].kit));
                log_weapon_msg(who, lib->l[i].weaponMsg[w]);
            }
        }
    } else if (!strcmp(line, "kitskip")) {   // re-read BotMod\kitskip.txt
        load_kitskip();
        bot_log("kitskip: %d rule(s)", g_kitSkipCount);
        for (int k = 0; k < g_kitSkipCount; k++) bot_log("  %s %s", g_kitSkipMap[k], g_kitSkipItem[k]);
    } else if (!strncmp(line, "packs", 5)) {   // research: dump deployed supply packs
        CompCache *cc = &g_compCache[g_compCur];
        bot_log("packs: %d", cc->pcount);
        for (int k = 0; k < cc->pcount; k++) {
            char tag[32];
            snprintf(tag, sizeof tag, "  pack%d", k);
            dump_named(tag, (unsigned char *)cc->pack[k], 0x100);
            dump_named("   +100", (unsigned char *)cc->pack[k] + 0x100, 0x100);
            dump_named("   +200", (unsigned char *)cc->pack[k] + 0x200, 0x100);
        }
    } else if (!strncmp(line, "request ", 8)) {   // request <ammo|heal> <player>: act as if they asked on the radio
        const char *who = line + 8;
        int heal = !strncmp(who, "heal ", 5);
        if (heal || !strncmp(who, "ammo ", 5)) who += 5;
        char *pl = find_player(who);
        if (!pl) { bot_log("request: unknown player '%s'", who); return; }
        order_reaction(pl, heal ? ORDER_REQUESTHEAL : ORDER_REQUESTAMMO, NULL);
    } else if (!strncmp(line, "spot ", 5)) {   // spot <bot name> | <target player>   (the bot spots the target)
        char a[64] = "", b[64] = "";
        const char *bar = strchr(line, '|');
        if (!bar) { bot_log("usage: spot <spotter> | <target>"); return; }
        snprintf(a, sizeof a, "%.*s", (int)(bar - (line + 5)), line + 5);
        snprintf(b, sizeof b, "%s", bar + 1);
        for (char *s = a + strlen(a); s > a && s[-1] == ' '; ) *--s = 0;
        char *bs = b; while (*bs == ' ') bs++;
        char *sp = find_player(a), *tg = find_player(bs);
        if (!sp || !tg) { bot_log("spot: unknown player '%s' or '%s'", a, bs); return; }
        bot_spot(sp, tg);
    } else if (!strncmp(line, "tp ", 3)) {   // tp <bot name> <x> <y> <z>  (experiment: move a soldier by writing its position)
        char name[64] = "";
        float v[3] = {0};
        const char *e = line + strlen(line);
        int words = 0;
        while (e > line + 3 && words < 3) { e--; if (*e == ' ' && e[1] != ' ') words++; }
        snprintf(name, sizeof name, "%.*s", (int)(e - (line + 3)), line + 3);
        sscanf(e, "%f %f %f", &v[0], &v[1], &v[2]);
        char *pl = find_player(name);
        char *s = pl ? *(char **)(pl + OFF_PLAYER_SOLDIER) : NULL;
        if (!s) { bot_log("tp: '%s' has no soldier", name); return; }
        memcpy(s + OFF_SOLDIER_POSITION, v, 12);
        memcpy(s + OFF_SOLDIER_SPAWN_POSITION, v, 12);
        bot_log("tp: %s -> (%.1f, %.1f, %.1f)", name, v[0], v[1], v[2]);
    } else if (!strncmp(line, "pos ", 4)) {
        cmd_pos(line + 4);
    } else if (!strncmp(line, "kit ", 4)) {
        char *pl = find_player(line + 4);
        if (!is_bot(pl)) bot_log("kit: '%s' is not a bot", line + 4); else apply_kit(pl);
    } else if (!strncmp(line, "step ", 5)) {
        H->stepBots = atoi(line + 5);
        bot_log("step bots: %d", H->stepBots);
    } else if (!strncmp(line, "msgdump", 7)) {   // msgdump <part of a message name> | msgdump (off)
        snprintf(g_msgDump, sizeof g_msgDump, "%s", line[7] == ' ' ? line + 8 : "");
        bot_log("msgdump: %s", g_msgDump[0] ? g_msgDump : "off");
    } else if (!strncmp(line, "trace ", 6)) {
        int v = atoi(line + 6);
        H->traceGhosts = v & 1;
        H->traceInput = (v & 2) != 0;
        g_traceMessages = (v & 4) != 0;
        if (g_traceMessages) g_seenMsgCount = 0;
        bot_log("trace: ghosts=%d input=%d", H->traceGhosts, H->traceInput);
    } else if (!strcmp(line, "list")) {
        bot_log("%d bot(s); team spawns: 1=%p 2=%p; blueprints: 1=%p 2=%p", H->botCount,
                H->teamSpawn[1].entity, H->teamSpawn[2].entity, H->teamBlueprint[1], H->teamBlueprint[2]);
        for (int i = 0; i < H->botCount; i++) {
            char *pl = H->bots[i];
            BotBrain *bb = bot_brain(i);
            bot_log("  #%d %s player=%p team=%d soldier=%p brain=%d task=%s flag=%d enroute=%d waiting=%d squad=%d", i, pl + OFF_PLAYER_ONLINE_NAME, pl,
                    player_team(pl), *(void **)(pl + OFF_PLAYER_SOLDIER), bb->enabled, bb->task >= 0 && bb->task <= TASK_REPAIR ? kTaskNames[bb->task] : "?",
                    bb->targetFlag, bb->enroute, bb->waiting, player_squad(pl));
        }
    } else {
        bot_log("commands: add|addh|addai [count|name] [team], setai <bot#> <0|1>, spawn <name>, move <name> <throttle> <strafe> <yaw>, face <name> <yawDeg> [pitchDeg], goto <name> <x> <z>, path <name> <x> <z>, navbuild <level> <x0> <z0> <x1> <z1> [cell], navload <level>, navstatus, flags, brain <name|all> on|off, kill <name>, loadouts [load], say <name> <order>, pos <name>, kit <name>, "
                "step 0|1, trace <bits: 1=ghosts 2=input>, list, reload, unload");
    }
}

// Bots whose ServerPlayer was destroyed (level change) are dropped and re-added once the new level is ready.
static char g_readdName[MAX_BOTS][32];
static int g_readdTeam[MAX_BOTS], g_readdCount;

// Drops bot i from every per-bot table (later bots move down one index).
static void bot_forget(int i)
{
    #define SHIFT(arr) memmove(&(arr)[i], &(arr)[i + 1], sizeof((arr)[0]) * (size_t)(H->botCount - 1 - i))
    if (i < 0 || i >= H->botCount) return;
    if (i < H->botCount - 1) {
        SHIFT(H->bots); SHIFT(H->botInput); SHIFT(H->botMove);
        SHIFT(g_botLoadout); SHIFT(g_progress); SHIFT(g_paths); SHIFT(g_claymores); SHIFT(g_tugs); SHIFT(g_lastGadgetPos);
        SHIFT(g_orderComp); SHIFT(g_orderCompSoldier); SHIFT(g_sayQueue); SHIFT(g_press); SHIFT(g_sched); SHIFT(g_repairVt);
        SHIFT(g_localFlag); SHIFT(g_localUntil);
        memmove(bot_brain(i), bot_brain(i + 1), sizeof(BotBrain) * (size_t)(H->botCount - 1 - i));
        memmove(bot_nav(i), bot_nav(i + 1), sizeof(BotNav) * (size_t)(H->botCount - 1 - i));
        memmove(bot_aim(i), bot_aim(i + 1), sizeof(float) * 2 * (size_t)(H->botCount - 1 - i));
    }
    unsigned long long m = *kitted_mask(), low = (1ull << i) - 1;
    *kitted_mask() = (m & low) | ((m >> 1) & ~low);
    int last = H->botCount - 1;
    H->bots[last] = NULL; H->botInput[last] = NULL;
    memset(bot_brain(last), 0, sizeof(BotBrain));
    memset(bot_nav(last), 0, sizeof(BotNav));
    memset(&g_paths[last], 0, sizeof g_paths[last]);
    memset(g_sayQueue[last], 0, sizeof g_sayQueue[last]);
    memset(g_press[last], 0, sizeof g_press[last]);
    memset(g_sched[last], 0, sizeof g_sched[last]);
    g_botLoadout[last] = -1; g_orderComp[last] = g_orderCompSoldier[last] = NULL; g_claymores[last] = g_tugs[last] = g_mines[last] = 0;
    H->botCount--;
    #undef SHIFT
}

static void validate_bots(void)
{
    for (int i = 0; i < H->botCount; i++) {
        char *pl = H->bots[i];
        int ok = pl && !IsBadReadPtr(pl, 0x400) && *(unsigned *)pl == ADDR_PLAYER_VTABLE && player_manager();
        if (ok) {
            char *pm = player_manager();
            void **arr = *(void ***)(pm + 0xac);
            unsigned count = *(unsigned *)(pm + 8), k;
            for (k = 0; k < count && arr[k] != pl; k++) {}
            ok = k < count;
        }
        if (ok) continue;
        bot_log("bots: '%s' no longer exists (level change?) - will re-add", g_readdCount < MAX_BOTS ? "?" : "?");
        if (g_readdCount < MAX_BOTS) {
            strncpy(g_readdName[g_readdCount], pl && !IsBadReadPtr(pl + OFF_PLAYER_ONLINE_NAME, 16) ? pl + OFF_PLAYER_ONLINE_NAME : "BOT", 16);
            g_readdTeam[g_readdCount++] = pl && !IsBadReadPtr(pl + OFF_PLAYER_TEAM, 4) ? player_team(pl) : 0;
        }
        bot_forget(i);   // compact all per-bot state
        i--;
    }
}

static void readd_bots(void)
{
    for (int k = 0; k < g_readdCount; k++) {
        cmd_add(g_readdName[k], g_readdTeam[k], 0);
        bot_brain(H->botCount - 1)->enabled = 1;
        bot_brain(H->botCount - 1)->targetFlag = -1;
    }
    g_readdCount = 0;
}

// ---- Roster: the wanted bot count (!bots N), split over the teams and named by role ----
// "[BOT] Medic US", "[BOT] Engineer US", ... then "[BOT] Medic US 2" ... Bots above the count are benched (AI off,
// dead); bots are never removed from the server. host->reserved: 320 wanted count (0 = default 8), 324 paused.
#define DEFAULT_BOT_COUNT 8
static const char *kRoleNames[4] = {"Medic", "Engineer", "Support", "Recon"};
static int *wanted_bots(void) { return (int *)(H->reserved + 320); }
static int *bots_paused(void) { return (int *)(H->reserved + 324); }
static int g_rosterAuto;   // manage the roster (autoroster on, or a count was set)

static void botmod_file(char *out, int n, const char *file)   // ...\Battlefield 3 Server\BotMod\<file>
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    char *up = strrchr(dir, '\\');
    if (up) *up = 0;
    snprintf(out, n, "%s\\BotMod\\%s", dir, file);
}

// BotMod\botcount_<game port>.txt, if there is one, is this server instance's own bot count (e.g. a second instance
// running vehicle-only modes with no bots); otherwise the shared BotMod\botcount.txt.
static void bot_count_file(char *path, int n)
{
    char own[32];
    snprintf(own, sizeof own, "botcount_%d.txt", H->gamePort);
    botmod_file(path, n, own);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) botmod_file(path, n, "botcount.txt");
}
static void save_bot_count(void)
{
    char path[MAX_PATH + 64];
    bot_count_file(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", *wanted_bots()); fclose(f); }
}

static void load_bot_count(void)
{
    if (*wanted_bots()) return;   // already set in this server run
    char path[MAX_PATH + 64];
    bot_count_file(path, sizeof path);
    FILE *f = fopen(path, "r");
    int n = DEFAULT_BOT_COUNT;
    if (f) { if (fscanf(f, "%d", &n) != 1) n = DEFAULT_BOT_COUNT; fclose(f); }
    *wanted_bots() = n < 0 ? 0 : n > MAX_BOTS ? MAX_BOTS : n;
}

// Pilots (2026-10-13, the user's spec): in a vehicle mode (Tank / Air Superiority) every roster bot is "[BOT] Pilot US|RU [n]";
// otherwise roster slots 5 and 10 of each team (k 4 and 9) are "[BOT] Pilot US|RU" and "... 2" - Engineer kit, they take
// tanks / jets where the map has them. The game mode comes from RCON serverInfo (chat_game_mode).
static int roster_all_pilots(void)
{
    char m[64];
    chat_game_mode(m, sizeof m);
    return !_strnicmp(m, "TankSuperiority", 15) || !_strnicmp(m, "AirSuperiority", 14);
}
static int roster_is_pilot(int k) { return roster_all_pilots() || k % 5 == 4; }   // every 5th bot of a team (5, 10, 15 ...; the user)
static int roster_class_bit(int k) { return roster_is_pilot(k) ? 1 << 1 : 1 << (k % 4); }   // (a pilot counts as the squad's Engineer)

static void roster_name(int team, int k, char *out, int n)
{
    const char *side = team == 1 ? "US" : "RU";
    if (roster_is_pilot(k)) {
        int num = roster_all_pilots() ? k + 1 : k / 5 + 1;
        if (num == 1) snprintf(out, n, "[BOT] Pilot %s", side);
        else snprintf(out, n, "[BOT] Pilot %s %d", side, num);
        return;
    }
    if (k < 4) snprintf(out, n, "[BOT] %s %s", kRoleNames[k % 4], side);
    else snprintf(out, n, "[BOT] %s %s %d", kRoleNames[k % 4], side, k / 4 + 1);
}

// Position of a bot in its team's roster (from its name), -1 if it is not a roster bot.
static int roster_ordinal(const char *name)
{
    const char *pp = strstr(name, "Pilot");
    if (pp) {
        const char *t = strstr(pp, " US");
        if (!t) t = strstr(pp, " RU");
        if (!t) return -1;
        int num = atoi(t + 3);
        if (num < 1) num = 1;
        if (roster_all_pilots()) return num - 1;
        return num * 5 - 1;
    }
    for (int r = 0; r < 4; r++) {
        const char *p = strstr(name, kRoleNames[r]);
        if (!p) continue;
        const char *t = strstr(p, " US");
        if (!t) t = strstr(p, " RU");
        if (!t) return -1;
        int num = atoi(t + 3);
        return r + 4 * (num > 1 ? num - 1 : 0);
    }
    return -1;
}

static unsigned char g_brainHeld[MAX_BOTS];   // "brain <bot> off" by hand: the roster leaves it off
static void set_brain(int i, int on)
{
    BotBrain *b = bot_brain(i);
    memset(b, 0, sizeof *b);
    b->enabled = on;
    b->targetFlag = -1;
    b->avoidFlag = -1;
    if (!on) { bot_nav(i)->mode = 0; H->botMove[i][0] = H->botMove[i][2] = 0; }
}

// ---- Removing a bot from the server: kill its soldier, wait until it is gone, then the engine's deletePlayer ----
#define ADDR_PM_DELETE_PLAYER      0x00e67460  // ServerPlayerManager::deletePlayer(ServerPlayer*), thiscall, ret 4
typedef void (__attribute__((thiscall)) *PM_DeletePlayer_t)(void *pm, void *player);
static char *g_removeQueue[MAX_BOTS];
static int g_removeCount;

static int removal_pending(char *pl) { for (int k = 0; k < g_removeCount; k++) if (g_removeQueue[k] == pl) return 1; return 0; }

static void queue_bot_removal(int i)
{
    if (removal_pending(H->bots[i]) || g_removeCount >= MAX_BOTS) return;
    set_brain(i, 0);
    g_removeQueue[g_removeCount++] = H->bots[i];
    bot_log("roster: removing %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
}

static void removal_tick(void)
{
    for (int k = 0; k < g_removeCount; k++) {
        char *pl = g_removeQueue[k];
        int i = bot_index(pl);
        if (i >= 0 && *(void **)(pl + OFF_PLAYER_SOLDIER)) { if (H->frame % 15 == 0) kill_bot(pl); continue; }   // wait for the unspawn
        g_removeQueue[k--] = g_removeQueue[--g_removeCount];
        if (i < 0) continue;
        char name[40];
        snprintf(name, sizeof name, "%s", pl + OFF_PLAYER_ONLINE_NAME);
        void *input = H->botInput[i];
        *(void **)(pl + OFF_PLAYER_INPUT) = NULL;   // ours, not the engine's
        bot_forget(i);
        ((PM_DeletePlayer_t)ADDR_PM_DELETE_PLAYER)(player_manager(), pl);
        if (input) _aligned_free(input);
        bot_log("roster: %s removed from the server", name);
        return;   // one per frame
    }
}

// Players per team the server allows (vars.maxPlayers / 2, from Instance\Admin\Startup.txt; 64 if not found).
#define HUMAN_SLOTS_PER_TEAM 2
// Players per team the server allows right now: its live vars.maxPlayers (RCON, refreshed every 30 s), else the
// -Network.MaxClientCount it was started with. Nothing about the server is assumed.
static int team_capacity(void)
{
    int maxPlayers = chat_max_players();
    if (maxPlayers <= 0) {
        static int fromCmdLine;
        if (!fromCmdLine) {
            const char *p = strstr(GetCommandLineA(), "-Network.MaxClientCount ");
            fromCmdLine = p ? atoi(p + 24) : -1;
        }
        maxPlayers = fromCmdLine;
    }
    if (maxPlayers <= 0) return 0;          // unknown yet: add no bots until the server tells us
    if (maxPlayers > MAX_BOTS + 2 * HUMAN_SLOTS_PER_TEAM) maxPlayers = MAX_BOTS + 2 * HUMAN_SLOTS_PER_TEAM;
    return maxPlayers / 2;
}

// Bots each team should have now: the wanted count split evenly, but always HUMAN_SLOTS_PER_TEAM free slots per team.
static void roster_targets(int *target, int *humans)
{
    humans[1] = humans[2] = 0;
    char *pm = player_manager();
    if (pm) {
        void **arr = *(void ***)(pm + 0xac);
        unsigned count = *(unsigned *)(pm + 8);
        for (unsigned k = 0; k < count; k++) {
            char *pl = arr[k];
            if (!pl || is_bot(pl)) continue;
            int t = player_team(pl);
            if (t == 1 || t == 2) humans[t]++;
        }
    }
    // Bots even the teams out: the largest team size T that both teams can reach with at most `want` bots in total,
    // each team keeping HUMAN_SLOTS_PER_TEAM free slots. A human joining a team takes a bot off it; a human joining
    // the other team brings that bot back.
    int want = *wanted_bots(), room[3];
    for (int t = 1; t <= 2; t++) { room[t] = team_capacity() - humans[t] - HUMAN_SLOTS_PER_TEAM; if (room[t] < 0) room[t] = 0; }
    target[1] = target[2] = 0;
    for (int T = team_capacity(); T >= 0; T--) {
        int b1 = T - humans[1], b2 = T - humans[2];
        if (b1 < 0) b1 = 0;
        if (b2 < 0) b2 = 0;
        if (b1 > room[1]) b1 = room[1];
        if (b2 > room[2]) b2 = room[2];
        if (b1 + b2 <= want) { target[1] = b1; target[2] = b2; break; }
    }
    // a spare bot (odd count) only goes where it makes the teams more even
    if (target[1] + target[2] < want) {
        int s1 = humans[1] + target[1], s2 = humans[2] + target[2];
        if (s1 < s2 && target[1] < room[1]) target[1]++;
        else if (s2 < s1 && target[2] < room[2]) target[2]++;
    }
}

static void roster_tick(void)
{
    removal_tick();
    if (!g_rosterAuto || H->frame % 30 != 7) return;
    if (!team_capacity()) return;   // server's player limit not known yet: leave the roster as it is
    int target[3] = {0}, humans[3] = {0}, have[3] = {0}, lastIdx[3] = {-1, -1, -1}, lastOrd[3] = {-1, -1, -1};
    roster_targets(target, humans);
    for (int i = 0; i < H->botCount; i++) {
        const char *nm = (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME;
        int k = roster_ordinal(nm), team = strstr(nm, " US") ? 1 : 2;
        if (k < 0 || removal_pending(H->bots[i])) continue;
        have[team]++;
        if (k > lastOrd[team]) { lastOrd[team] = k; lastIdx[team] = i; }
    }
    // a roster bot whose name is not its slot's name any more (every 5th is a Pilot now; a vehicle mode makes all Pilots):
    // out, the roster re-adds the slot under the right name - one a second
    for (int i = 0; i < H->botCount; i++) {
        const char *nm = (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME;
        int k = roster_ordinal(nm), team = strstr(nm, " US") ? 1 : 2;
        if (k < 0 || removal_pending(H->bots[i])) continue;
        char want[40];
        roster_name(team, k, want, sizeof want);
        if (strcmp(want, nm)) { bot_log("roster: %s is slot %d - now '%s', re-added", nm, k, want); queue_bot_removal(i); return; }
    }
    // too many (count lowered, or humans joined): remove the most recently added bot from the team furthest over its
    // target - on a tie the team with more players - so the teams stay even while shrinking
    {
        int ex1 = have[1] - target[1], ex2 = have[2] - target[2];
        if (ex1 > 0 || ex2 > 0) {
            int t = ex1 > ex2 ? 1 : ex2 > ex1 ? 2 : (humans[1] + have[1] >= humans[2] + have[2] ? 1 : 2);
            if (lastIdx[t] >= 0) { queue_bot_removal(lastIdx[t]); return; }
        }
    }
    // too few: add one, alternating teams (the team with fewer bots first, US on a tie)
    int t = 0;
    if (have[1] < target[1] && (have[1] <= have[2] || have[2] >= target[2])) t = 1;
    else if (have[2] < target[2]) t = 2;
    if (t && H->botCount < MAX_BOTS) {
        for (int k = 0; k < MAX_BOTS; k++) {
            char name[40];
            roster_name(t, k, name, sizeof name);
            if (find_player(name)) continue;
            cmd_add(name, t, 0);
            if (H->botCount && H->bots[H->botCount - 1] == find_player(name)) set_brain(H->botCount - 1, !*bots_paused());
            return;
        }
    }
    // pause / resume
    for (int i = 0; i < H->botCount; i++) {
        if (roster_ordinal((char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME) < 0 || removal_pending(H->bots[i])) continue;
        int active = !*bots_paused();
        if (!active && bot_brain(i)->enabled) { set_brain(i, 0); kill_bot(H->bots[i]); }
        else if (active && !bot_brain(i)->enabled && !g_brainHeld[i]) set_brain(i, 1);   // (not one switched off by hand)
    }
}

// ---- Squads: bots fill bot-led squads (4 players); a bot that finds none creates the next free squad and leads it ----
// Squad changes go through RCON admin.movePlayer (the engine's own squad logic), which only works while the bot is dead.
#define OFF_PLAYER_SQUAD           0x784       // squad id (0 = none)
#define OFF_PLAYER_SQUAD_LEADER    0x788       // byte
#define SQUAD_SIZE                 4
#define MAX_SQUADS                 32
static char *g_squadAsked[MAX_BOTS];
static unsigned g_squadAskedAt[MAX_BOTS];

static int player_squad(char *pl) { return *(int *)(pl + OFF_PLAYER_SQUAD); }

// Squad a bot may use to wait for its squad before spawning (gives the RCON move a moment).
static int squad_request_recent(char *pl)
{
    for (int k = 0; k < MAX_BOTS; k++) if (g_squadAsked[k] == pl && H->frame - g_squadAskedAt[k] < 3 * 30) return 1;
    return 0;
}

static void squad_tick(void)
{
    if (H->frame % 30 != 17) return;
    char *pm = player_manager();
    if (!pm) return;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    int members[3][MAX_SQUADS + 1], botLed[3][MAX_SQUADS + 1], roles[3][MAX_SQUADS + 1];   // roles: bit per class present
    memset(members, 0, sizeof members);
    memset(botLed, 0, sizeof botLed);
    memset(roles, 0, sizeof roles);
    for (unsigned k = 0; k < count; k++) {
        char *pl = arr[k];
        if (!pl) continue;
        int t = player_team(pl), s = player_squad(pl);
        if ((t != 1 && t != 2) || s <= 0 || s > MAX_SQUADS) continue;
        members[t][s]++;
        if (pl[OFF_PLAYER_SQUAD_LEADER] && is_bot(pl)) botLed[t][s] = 1;
        int k = roster_ordinal(pl + OFF_PLAYER_ONLINE_NAME);
        if (k >= 0) roles[t][s] |= roster_class_bit(k);
    }
    if (H->frame % (30 * 30) == 17)   // keep every bot-led squad private (new rounds, squads made before this)
        for (int t = 1; t <= 2; t++)
            for (int s = 1; s <= MAX_SQUADS; s++)
                if (botLed[t][s]) chat_rcon("squad.private\t%d\t%d\ttrue", t, s);
    for (int i = 0; i < H->botCount; i++) {
        char *pl = H->bots[i];
        if (player_squad(pl) || *(void **)(pl + OFF_PLAYER_SOLDIER) || removal_pending(pl) || !bot_brain(i)->enabled || is_pilot(i)) continue;   // (Pilots: no squad)
        if (squad_request_recent(pl)) continue;
        int t = named_team(pl), pick = 0, k = roster_ordinal(pl + OFF_PLAYER_ONLINE_NAME);   // (named side: the move takes the team too)
        int role = k >= 0 ? roster_class_bit(k) : 0;
        // one of each class per squad where possible: a bot squad missing this class, then any bot squad with room, then a new one
        for (int s = 1; s <= MAX_SQUADS && !pick; s++) if (botLed[t][s] && members[t][s] < SQUAD_SIZE && !(roles[t][s] & role)) pick = s;
        for (int s = 1; s <= MAX_SQUADS && !pick; s++) if (botLed[t][s] && members[t][s] < SQUAD_SIZE) pick = s;
        for (int s = 1; s <= MAX_SQUADS && !pick; s++) if (!members[t][s]) { pick = s; botLed[t][s] = 1; }
        if (!pick) continue;
        members[t][pick]++;
        roles[t][pick] |= role;
        // bot squads are private (players cannot join them by accident); open the squad just for the move
        chat_rcon("squad.private\t%d\t%d\tfalse", t, pick);
        chat_rcon("admin.movePlayer\t%s\t%d\t%d\tfalse", pl + OFF_PLAYER_ONLINE_NAME, t, pick);
        chat_rcon("squad.private\t%d\t%d\ttrue", t, pick);
        for (int k = 0; k < MAX_BOTS; k++)
            if (!g_squadAsked[k] || g_squadAsked[k] == pl || H->frame - g_squadAskedAt[k] > 10 * 30) {
                g_squadAsked[k] = pl; g_squadAskedAt[k] = H->frame; break;
            }
        bot_log("squad: %s -> %s squad %d (%s)", pl + OFF_PLAYER_ONLINE_NAME, t == 1 ? "US" : "RU", pick,
                members[t][pick] == 1 ? "creates it" : "joins");
    }
}

// ---- In-game chat commands (!bots ...) - read through RCON by chat.c ----
// Changing commands need the player's name in BotMod\admins.txt (one per line, # comments); without that file everyone may.
static int chat_is_admin(const char *who)
{
    char dir[MAX_PATH], path[MAX_PATH + 32], line[128];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;                        // ...\Battlefield_3_Server
    char *up = strrchr(dir, '\\');
    if (up) *up = 0;                                // ...\Battlefield 3 Server
    snprintf(path, sizeof path, "%s\\BotMod\\admins.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return 1;
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line; while (*s == ' ') s++;
        for (char *e = s + strlen(s); e > s && e[-1] == ' '; ) *--e = 0;
        if (*s && *s != '#' && !_stricmp(s, who)) ok = 1;
    }
    fclose(f);
    return ok;
}

static void chat_status(void)
{
    int n[3] = {0}, alive[3] = {0}, on = 0;
    for (int i = 0; i < H->botCount; i++) {
        int t = player_team(H->bots[i]) == 2 ? 2 : 1;
        n[t]++;
        if (*(void **)(H->bots[i] + OFF_PLAYER_SOLDIER)) alive[t]++;
        if (bot_brain(i)->enabled) on++;
    }
    chat_say("[BotMod] %d active of %d wanted - US %d (%d alive), RU %d (%d alive) - %s", on, *wanted_bots(), n[1], alive[1], n[2],
             alive[2], *bots_paused() ? "paused (!bots on)" : "running");
}

// Friendly names for !map (vanilla + expansion maps of this build).
static const struct { const char *name, *code; } kMaps[] = {
    {"bazaar", "MP_001"}, {"teheran", "MP_003"}, {"tehran", "MP_003"}, {"caspian", "MP_007"}, {"seine", "MP_011"},
    {"firestorm", "MP_012"}, {"damavand", "MP_013"}, {"canals", "MP_017"}, {"noshahr", "MP_017"}, {"kharg", "MP_018"},
    {"metro", "MP_Subway"}, {"karkand", "XP1_001"}, {"oman", "XP1_002"}, {"sharqi", "XP1_003"}, {"wake", "XP1_004"},
    // The DLC packs (the LAN clients have them: server 2 runs Armored Kill / End Game maps, 2026-10-13)
    {"scrapmetal", "XP2_Factory"}, {"operation925", "XP2_Office"}, {"donya", "XP2_Palace"}, {"ziba", "XP2_Skybar"},
    {"bandar", "XP3_Desert"}, {"alborz", "XP3_Alborz"}, {"shield", "XP3_Shield"}, {"valley", "XP3_Valley"},
    {"markaz", "XP4_Quake"}, {"talah", "XP4_FD"}, {"epicenter", "XP4_Rubble"}, {"azadi", "XP4_Parl"},
    {"riverside", "XP5_001"}, {"nebandan", "XP5_002"}, {"kiasar", "XP5_003"}, {"sabalan", "XP5_004"},
};

static void chat_command(const char *who, const char *text)
{
    bot_log("chat: %s: %s", who, text);
    char cmd[64] = "", arg[64] = "";
    sscanf(text + 1, "%63s %63s", cmd, arg);
    if (!_stricmp(cmd, "help")) {
        chat_say("[BotMod] !bots = status | admins: !bots <0-%d>/fill = bot count !bots off/on = pause/resume !map <name> !pos", MAX_BOTS);
    } else if (!_stricmp(cmd, "bots") && !arg[0]) {
        chat_status();
    } else if (!_stricmp(cmd, "pos")) {   // admins: where am I (for nav work) - position, heading, how much of the nav it reaches
        if (!chat_is_admin(who)) { chat_say("[BotMod] %s: only admins can do that", who); return; }
        char *pl = find_player(who), *s = pl ? *(char **)(pl + OFF_PLAYER_SOLDIER) : NULL;
        if (!s) { chat_say("[BotMod] %s: not spawned", who); return; }
        float *p = (float *)(s + OFF_SOLDIER_SPAWN_POSITION), np[3] = {0, 0, 0};
        float yaw = -*(float *)(s + OFF_SOLDIER_YAW) * 57.2958f;
        while (yaw >= 360.0f) yaw -= 360.0f;
        while (yaw < 0) yaw += 360.0f;
        NavGrid *g = *nav_slot();
        int reach = g && g->phase == 2 ? nav_reach(g, p[0], p[1], p[2], np) : -1;
        chat_say("[BotMod] %s at (%.1f, %.1f, %.1f) facing %.0f - nav reaches %d cells", who, p[0], p[1], p[2], yaw, reach);
        bot_log("pos: %s at (%.2f, %.2f, %.2f) yaw %.0f - nav node (%.1f, %.1f, %.1f) reaches %d", who, p[0], p[1], p[2], yaw, np[0], np[1], np[2], reach);
    } else if (!_stricmp(cmd, "bots") && (!_stricmp(arg, "on") || !_stricmp(arg, "off"))) {
        if (!chat_is_admin(who)) { chat_say("[BotMod] %s: only admins can do that", who); return; }
        int on = !_stricmp(arg, "on");
        *bots_paused() = !on;
        g_rosterAuto = 1;
        chat_say(on ? "[BotMod] bots resumed by %s - respawning" : "[BotMod] bots paused by %s (!bots on to resume)", who);
    } else if (!_stricmp(cmd, "map")) {
        if (!arg[0]) { chat_map_list(); return; }   // this server's own rotation (only those can be switched to)
        if (!chat_is_admin(who)) { chat_say("[BotMod] %s: only admins can do that", who); return; }
        const char *code = NULL;
        for (int k = 0; k < (int)(sizeof kMaps / sizeof kMaps[0]); k++) if (!_stricmp(arg, kMaps[k].name)) code = kMaps[k].code;
        if (!code && (!_strnicmp(arg, "MP_", 3) || !_strnicmp(arg, "XP", 2))) code = arg;   // a raw map code
        if (!code) { chat_say("[BotMod] unknown map '%s' - type !map for the list", arg); return; }
        chat_map(code, who);
    } else if (!_stricmp(cmd, "bots") && ((arg[0] >= '0' && arg[0] <= '9') || !_stricmp(arg, "fill"))) {
        if (!chat_is_admin(who)) { chat_say("[BotMod] %s: only admins can do that", who); return; }
        int n = !_stricmp(arg, "fill") ? MAX_BOTS : atoi(arg);
        if (n > MAX_BOTS) { chat_say("[BotMod] max %d bots - using %d", MAX_BOTS, MAX_BOTS); n = MAX_BOTS; }
        *wanted_bots() = n;
        g_rosterAuto = 1;
        save_bot_count();
        int target[3], humans[3];
        roster_targets(target, humans);
        chat_say("[BotMod] %s set the bot count to %d: US %d, RU %d bots (%d free slots per team kept for players)", who, n,
                 target[1], target[2], HUMAN_SLOTS_PER_TEAM);
    } else {
        chat_say("[BotMod] unknown command %s - try !help", text);
    }
}


// ---- Gunners: a bot in a vehicle seat other than the driver's aims that seat's weapon and shoots - enemy vehicles
// first, then spotted infantry. Seat input: traverse EIA 6 (a negative level turns towards larger atan2(x, z)),
// elevation EIA 5 (positive = up). The seat's weapon is found by nudging the traverse after boarding and seeing
// which nearby vehicle weapon turned; its facing is read back every frame for closed-loop aiming. ----
#define GUN_VEHICLE_RANGE      250.0f
#define GUN_INFANTRY_RANGE     150.0f
#define GUN_BURST_ON           30
#define GUN_BURST_OFF          12
static char *g_gunVeh[MAX_BOTS], *g_gunWeapon[MAX_BOTS], *g_gunTarget[MAX_BOTS], *g_gunTargetVeh[MAX_BOTS];
static unsigned char g_gunFire[MAX_BOTS], g_gunTries[MAX_BOTS], g_gunNudging[MAX_BOTS];
static char *g_gunCand[MAX_BOTS][16];
static char *g_gunBad[MAX_BOTS][4];
static float g_gunCandFwd[MAX_BOTS][16][3];
static int g_gunCandN[MAX_BOTS], g_gunCalibBot = -1;
static unsigned g_gunStepAt[MAX_BOTS], g_gunScanAt[MAX_BOTS], g_gunBurstAt[MAX_BOTS];
static int g_gunDebug;   // console: gundebug 0/1
static float g_gunAimErr[MAX_BOTS], g_gunTgtDist[MAX_BOTS], g_gunYawErr[MAX_BOTS], g_gunTgtBearing[MAX_BOTS];
// |yaw| + |pitch| aim error of the bot's seat weapon (rad), 9 = no target

static int gun_ok(char *w) { return w && !IsBadReadPtr(w, 0x70) && *(unsigned *)w == ADDR_VEHICLE_WEAPON_VTABLE; }
static float g_gunSpeed[MAX_BOTS], g_gunGrav[MAX_BOTS];
// A tank driver's coax (the machine gun beside the barrel): its weapon object, ballistics, and which weapon is selected
static char *g_gunCoax[MAX_BOTS];
static float g_coaxSpeed[MAX_BOTS], g_coaxGrav[MAX_BOTS];
static unsigned char g_gunSlot[MAX_BOTS];   // 0 unknown, 1 main gun, 2 coax
static unsigned g_gunSwitchAt[MAX_BOTS];   // the manned weapon's projectile: m/s, gravity (0 = unknown)
// A vehicle weapon's projectile (2026-10-09): weapon +0xc firing data -> +0x8 shot config: initial velocity vec3 at +0x80
// (forward speed in z), projectile data at +0x98 (bullet vt 0250fdd8): gravity float +0xcc, FX assets around +0xa0..+0xbc
// (FX_Tracer_Tank = a tank's main gun, FX_Tracer_MG_* = machine guns, FX_Trail_ThinSmoke = the smoke launcher).
#define VWEAP_OFF_FIRING      0x0c
#define FIRING_OFF_SHOT       0x08
#define SHOT_OFF_SPEED        0x80
#define SHOT_OFF_PROJECTILE   0x98
#define PROJ_OFF_GRAVITY      0xcc
static int vweapon_ballistics(char *w, float *speed, float *grav, const char **fx)
{
    *speed = 0; *grav = 0; if (fx) *fx = "";
    char *fd = *(char **)(w + VWEAP_OFF_FIRING);
    if (!fd || IsBadReadPtr(fd, 0x10)) return 0;
    char *shot = *(char **)(fd + FIRING_OFF_SHOT);
    if (!shot || IsBadReadPtr(shot, 0xa0)) return 0;
    float *v = (float *)(shot + SHOT_OFF_SPEED);
    *speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    char *proj = *(char **)(shot + SHOT_OFF_PROJECTILE);
    if (!proj || IsBadReadPtr(proj, 0xd0)) return 1;
    *grav = *(float *)(proj + PROJ_OFF_GRAVITY);
    if (fx) for (int off = 0xa0; off < 0xc0; off += 4) {
        const char *n = asset_name(*(void **)(proj + off));
        if (n[0] != '?' && strstr(n, "FX/")) { *fx = n; break; }
    }
    return 1;
}
// A tank's main gun among the weapons on the driver's turret: the tank-shell tracer, else a slow heavy shot
static char * __attribute__((unused)) driver_main_gun(int i, const float *vp)
{
    CompCache *cc = &g_compCache[g_compCur];
    char *best = NULL;
    int bestScore = 0;
    for (int k = 0; k < cc->gcount; k++) {
        char *w = cc->gun[k];
        if (!gun_ok(w)) continue;
        float *wp = (float *)(w + 0x60);
        if (dist2(wp, vp) > 7.0f * 7.0f || fabsf(wp[1] - vp[1]) > 6.0f) continue;
        int taken = 0;
        for (int j = 0; j < H->botCount; j++) if (j != i && g_gunWeapon[j] == w) taken = 1;
        if (taken) continue;
        float sp, gr; const char *fx;
        if (!vweapon_ballistics(w, &sp, &gr, &fx)) continue;
        int score = strstr(fx, "Tracer_Tank") ? 3 : strstr(fx, "Tracer_IFV") ? 2   // (an IFV's autocannon: the BTR-90 took its coax instead)
                  : (sp >= 150.0f && sp <= 350.0f && gr < -4.5f && !strstr(fx, "Smoke")) ? 1 : 0;
        if (score > bestScore) { bestScore = score; best = w; }
    }
    return best;
}
static void gunner_reset(int i)
{
    if (g_gunCalibBot == i) g_gunCalibBot = -1;
    g_gunVeh[i] = g_gunWeapon[i] = g_gunTarget[i] = g_gunTargetVeh[i] = NULL;
    g_gunFire[i] = g_gunTries[i] = g_gunNudging[i] = 0;
    g_seatYaw[i] = g_seatPitch[i] = 0;
    g_gunSpeed[i] = g_gunGrav[i] = 0;
    g_gunCoax[i] = NULL; g_coaxSpeed[i] = g_coaxGrav[i] = 0; g_gunSlot[i] = 0; g_gunSwitchAt[i] = 0;
}
static char *vehicle_occupant(char *veh)
{
    char **b;
    int n = vehicle_seats(veh, &b);
    for (int k = 0; k < n; k++) if (*(char **)(b[k] + 0x4c)) return *(char **)(b[k] + 0x4c);
    return NULL;
}
// clear line from the gun (starting a few metres out, past our own hull) to the aim point
static int gun_can_see(const float *gp, const float *aim, int isVehicle)
{
    float dx = aim[0] - gp[0], dy = aim[1] - gp[1], dz = aim[2] - gp[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < 4.0f) return 1;
    float from[3] = {gp[0] + dx / d * 3.5f, gp[1] + dy / d * 3.5f, gp[2] + dz / d * 3.5f}, hit[3];
    if (!ray_blocked_char(from, aim, hit)) return 1;   // anything blocks: fences / bushes / breakables stop bullets too
    float hx = hit[0] - aim[0], hy = hit[1] - aim[1], hz = hit[2] - aim[2];
    return isVehicle && hx * hx + hy * hy + hz * hz < 4.0f * 4.0f;   // hit the target's hull
}
static int gunner_find_weapon(int i, char *veh, const float *vp)
{
    if (g_gunTries[i] >= 4) return 0;   // this seat has no weapon we can find (e.g. a plain passenger seat)
    if (H->frame < g_gunStepAt[i]) return 0;
    if (!g_gunNudging[i]) {
        if (g_gunCalibBot >= 0 && g_gunCalibBot != i) return 0;   // one nudge at a time, so the answer is unambiguous
        CompCache *cc = &g_compCache[g_compCur];
        int n = 0;
        for (int k = 0; k < cc->gcount && n < 16; k++) {
            char *w = cc->gun[k];
            if (!gun_ok(w)) continue;
            float *wp = (float *)(w + 0x60);
            if (dist2(wp, vp) > 12.0f * 12.0f || fabsf(wp[1] - vp[1]) > 8.0f) continue;
            int taken = 0;
            for (int j = 0; j < H->botCount; j++) if (j != i && g_gunWeapon[j] == w) taken = 1;
            for (int b = 0; b < 4; b++) if (g_gunBad[i][b] == w) taken = 1;
            if (taken) continue;
            g_gunCand[i][n] = w;
            memcpy(g_gunCandFwd[i][n], w + 0x50, 12);
            n++;
        }
        g_gunCandN[i] = n;
        if (!n) { g_gunTries[i]++; g_gunStepAt[i] = H->frame + 60; return 0; }
        g_gunCalibBot = i;
        g_gunNudging[i] = 1;
        g_seatYaw[i] = (g_gunTries[i] & 1) ? 0.8f : -0.8f;
        g_gunStepAt[i] = H->frame + 6;
        return 0;
    }
    g_seatYaw[i] = 0;
    g_gunNudging[i] = 0;
    g_gunCalibBot = -1;
    // of the weapons that really turned with our traverse (the turret's main gun, coax, canister, smoke launchers all do),
    // a driver takes the main gun (tank-shell tracer / a slow heavy shot); otherwise the one that turned most
    char *best = NULL;
    float bestD = 0.002f;
    int bestScore = -1, driver = vehicle_seat_of(veh, H->bots[i]) == 0;
    for (int k = 0; k < g_gunCandN[i]; k++) {
        char *w = g_gunCand[i][k];
        if (!gun_ok(w)) continue;
        float *f = (float *)(w + 0x50), dx = f[0] - g_gunCandFwd[i][k][0], dz = f[2] - g_gunCandFwd[i][k][2], d = dx * dx + dz * dz;
        if (d <= 0.002f) continue;
        int score = 0;
        if (driver) {
            float sp, gr; const char *fx;
            if (vweapon_ballistics(w, &sp, &gr, &fx))
                score = strstr(fx, "Tracer_Tank") ? 3 : strstr(fx, "Tracer_IFV") ? 3 : strstr(fx, "Smoke") ? -1   // (an IFV autocannon: the BTR-90
                      : (sp >= 150.0f && sp <= 350.0f && gr < -4.5f) ? 2 : 0;                                    // took its coax as the main gun)
        }
        if (score > bestScore || (score == bestScore && d > bestD)) { bestScore = score; bestD = d; best = w; }
    }
    g_gunStepAt[i] = H->frame + 15;
    if (!best) {
        if (++g_gunTries[i] >= 4) bot_log("gunner: %s found no weapon on its seat", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME);
        return 0;
    }
    g_gunWeapon[i] = best;
    vweapon_ballistics(best, &g_gunSpeed[i], &g_gunGrav[i], NULL);
    if (driver && bestScore == 3) {
        float *bp = (float *)(best + 0x60), coaxD = 4.0f * 4.0f;
        for (int k = 0; k < g_gunCandN[i]; k++) {
            char *w = g_gunCand[i][k];
            if (w == best || !gun_ok(w)) continue;
            float *f = (float *)(w + 0x50), dx = f[0] - g_gunCandFwd[i][k][0], dz = f[2] - g_gunCandFwd[i][k][2];
            if (dx * dx + dz * dz <= 0.002f) continue;   // did not turn with our turret
            float sp, gr; const char *fx;
            if (!vweapon_ballistics(w, &sp, &gr, &fx) || !strstr(fx, "Tracer_MG") || sp < 500.0f) continue;
            float *wq = (float *)(w + 0x60), ex = wq[0] - bp[0], ey = wq[1] - bp[1], ez = wq[2] - bp[2], d = ex * ex + ey * ey + ez * ez;
            if (d < coaxD) { coaxD = d; g_gunCoax[i] = w; g_coaxSpeed[i] = sp; g_coaxGrav[i] = gr; }
        }
        if (g_gunCoax[i]) bot_log("gunner: %s has a coax %p (%.0f m/s) - for soldiers", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, g_gunCoax[i], g_coaxSpeed[i]);
    }
    bot_log("gunner: %s mans weapon %p (%d candidates, %.0f m/s, gravity %.2f%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, best, g_gunCandN[i],
            g_gunSpeed[i], g_gunGrav[i], bestScore == 3 ? ", the main gun" : "");
    return 1;
}
// Lowest of a few aim heights above a soldier's feet the gun has a clear line to (low walls, sandbags, breakables in
// front of the target), or -1.
static float g_gunAimH[MAX_BOTS];
static float gun_clear_height(const float *gp, const float *tp)
{
    static const float hs[3] = {1.1f, 1.4f, 0.8f};   // torso first (0.55 went between a standing soldier's legs)
    for (int k = 0; k < 3; k++) {
        float aim[3] = {tp[0], tp[1] + hs[k], tp[2]};
        if (gun_can_see(gp, aim, 0)) return hs[k];
    }
    return -1.0f;
}

static unsigned g_gunLostAt[MAX_BOTS];
static char *g_gunBad[MAX_BOTS][4];   // weapons that did not turn for this bot's seat
// current target still worth shooting: alive, and seen within the last 1.5 s
static int gunner_target_ok(int i, const float *gp)
{
    float *tp = NULL;
    int isVeh = 0;
    if (g_gunTargetVeh[i]) {
        char *tv = g_gunTargetVeh[i];
        if (!IsBadReadPtr(tv, 4) && *(unsigned *)tv == ADDR_VEHICLE_VTABLE && *(float *)(tv + OFF_VEHICLE_HEALTH) > 0 && vehicle_occupied(tv)) { tp = vehicle_pos(tv, NULL); isVeh = 1; }
    } else if (g_gunTarget[i] && player_exists(g_gunTarget[i]) && !player_vehicle(g_gunTarget[i])) {
        char *ts = *(char **)(g_gunTarget[i] + OFF_PLAYER_SOLDIER);
        if (ts && *(float *)(ts + OFF_SOLDIER_HEALTH) > 0) tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
    }
    if (!tp) return 0;
    int seen;
    if (isVeh) { float aim[3] = {tp[0], tp[1] + 1.0f, tp[2]}; seen = gun_can_see(gp, aim, 1); }
    else { float h = gun_clear_height(gp, tp); seen = h > 0; if (seen) g_gunAimH[i] = h; }
    if (seen) { g_gunLostAt[i] = 0; return 1; }
    if (!g_gunLostAt[i]) g_gunLostAt[i] = H->frame;
    return H->frame - g_gunLostAt[i] < 45;
}
static void gunner_pick_target(int i, const float *gp)
{
    char *pl = H->bots[i];
    int team = player_team(pl);
    char *bestPl = NULL, *bestVeh = NULL;
    float bestD = GUN_VEHICLE_RANGE;
    char **vehs;
    int nveh = vehicle_list(&vehs), pilot = is_pilot(i), bestKr = 9;
    for (int k = 0; k < nveh; k++) {   // vehicles first
        char *v = vehs[k];
        int vteam = 0;
        if (IsBadReadPtr(v, 4) || *(unsigned *)v != ADDR_VEHICLE_VTABLE || v == g_gunVeh[i]) continue;
        float *tp = vehicle_pos(v, &vteam);
        if (!tp || !vteam || vteam == team || *(float *)(v + OFF_VEHICLE_HEALTH) <= 0 || !vehicle_occupied(v)) continue;
        float d = sqrtf(dist2(tp, gp) + (tp[1] - gp[1]) * (tp[1] - gp[1]));
        float aim[3] = {tp[0], tp[1] + 1.0f, tp[2]};
        // a Pilot (the user's order): tanks, then IFVs / LAVs, then jeeps, then anything else - nearest within a kind
        int kd = vehicle_kind(v), kr = !pilot ? 0 : kd == VK_TANK ? 0 : kd == VK_IFV ? 1 : kd == VK_LIGHT ? 2 : 3;
        if (kr > bestKr || (kr == bestKr && d >= bestD) || d >= GUN_VEHICLE_RANGE || !gun_can_see(gp, aim, 1)) continue;
        bestD = d; bestVeh = v; bestPl = vehicle_occupant(v); bestKr = kr;
    }
    // enemy engineers on foot come first (rockets), then vehicles, then everyone else
    char *bestEng = NULL;
    float engD = GUN_INFANTRY_RANGE, engH = 0;
    {
        char *pm = player_manager();
        void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
        unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
        for (unsigned k = 0; k < count; k++) {
            char *t = arr[k], *ts;
            if (!t || player_team(t) == team || player_vehicle(t) || !(ts = *(char **)(t + OFF_PLAYER_SOLDIER))) continue;
            if (*(float *)(ts + OFF_SOLDIER_HEALTH) <= 0 || !player_is_engineer(t)) continue;
            float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
            float d = sqrtf(dist2(tp, gp) + (tp[1] - gp[1]) * (tp[1] - gp[1]));
            if (d >= engD) continue;
            float h = gun_clear_height(gp, tp);
            if (h < 0) continue;
            engD = d; bestEng = t; engH = h;
        }
    }
    int curRank = g_gunTargetVeh[i] ? 2 : g_gunTarget[i] ? (player_is_engineer(g_gunTarget[i]) ? 3 : 1) : 0;
    int newRank = bestEng ? 3 : bestVeh ? 2 : 0;
    if (pilot) {   // a Pilot: any vehicle before engineers (by kind), engineers before other soldiers
        if (g_gunTargetVeh[i]) { int kd = vehicle_kind(g_gunTargetVeh[i]); curRank = 10 - (kd == VK_TANK ? 0 : kd == VK_IFV ? 1 : kd == VK_LIGHT ? 2 : 3); }
        newRank = bestVeh ? 10 - bestKr : bestEng ? 3 : 0;
        if (bestVeh) bestEng = NULL;
    }
    // stay on the current target unless something of a higher priority is in view
    if (curRank && newRank <= curRank && gunner_target_ok(i, gp)) return;
    if (bestEng) { bestVeh = NULL; bestPl = bestEng; bestD = engD; g_gunAimH[i] = engH; }
    else if (!bestVeh) {   // then infantry (the gunner spots what it engages)
        char *pm = player_manager();
        void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
        unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
        bestD = GUN_INFANTRY_RANGE;
        for (unsigned k = 0; k < count; k++) {
            char *t = arr[k], *ts;
            if (!t || player_team(t) == team || player_vehicle(t) || !(ts = *(char **)(t + OFF_PLAYER_SOLDIER))) continue;
            if (*(float *)(ts + OFF_SOLDIER_HEALTH) <= 0) continue;
            float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
            float d = sqrtf(dist2(tp, gp) + (tp[1] - gp[1]) * (tp[1] - gp[1]));
            if (d >= bestD) continue;
            float h = gun_clear_height(gp, tp);
            if (h < 0) continue;
            bestD = d; bestPl = t; g_gunAimH[i] = h;
        }
    }
    if (bestVeh || bestPl) {
        bot_log("gunner: %s engages %s%s at %.0f m", pl + OFF_PLAYER_ONLINE_NAME, bestPl ? bestPl + OFF_PLAYER_ONLINE_NAME : "a vehicle",
                bestVeh ? " (vehicle)" : "", bestD);
        if (bestPl) bot_spot(pl, bestPl);
    }
    g_gunBurstAt[i] = H->frame;
    g_gunLostAt[i] = 0;
    g_gunTarget[i] = bestPl;
    g_gunTargetVeh[i] = bestVeh;
}
static void gunner_tick(int i)
{
    char *pl = H->bots[i], *veh = player_vehicle(pl);
    int seat = veh && !IsBadReadPtr(veh, 4) && *(unsigned *)veh == ADDR_VEHICLE_VTABLE ? vehicle_seat_of(veh, pl) : -1;
    // seat 0 too while the bot drives: a tank / IFV driver aims and fires the main gun (traverse / elevation / trigger are
    // separate inputs from steering); a jeep's driver seat has no weapon and the search gives up after a few tries
    if (seat < 0 || (seat == 0 && !g_driving[i]) || !g_vehInput || g_flying[i]) { if (g_gunVeh[i]) gunner_reset(i); return; }
    if (veh != g_gunVeh[i]) { gunner_reset(i); g_gunVeh[i] = veh; g_gunStepAt[i] = H->frame + 30; memset(g_gunBad[i], 0, sizeof g_gunBad[i]); }
    float *vp = vehicle_pos(veh, NULL);
    if (!vp) return;
    char *w = g_gunWeapon[i];
    if (w && !gun_ok(w)) { g_gunWeapon[i] = w = NULL; g_gunTries[i] = 0; }
    if (!w) { g_gunFire[i] = 0; gunner_find_weapon(i, veh, vp); return; }
    float *gp = (float *)(w + 0x60), *f = (float *)(w + 0x50);
    {   // the weapon must follow our traverse input; if it does not, it is not this seat's - drop it and look again
        static float lastYaw[MAX_BOTS];
        static unsigned checkAt[MAX_BOTS], stuck[MAX_BOTS];
        if (H->frame >= checkAt[i]) {
            float y = atan2f(f[0], f[2]), moved = fabsf(y - lastYaw[i]);
            if (moved > 3.14159265f) moved = 6.2831853f - moved;
            if (fabsf(g_seatYaw[i]) >= 0.35f && checkAt[i]) stuck[i] = moved < 0.01f ? stuck[i] + 1 : 0;
            else stuck[i] = 0;
            lastYaw[i] = y;
            checkAt[i] = H->frame + 20;
            if (stuck[i] >= 3) {
                stuck[i] = 0;
                for (int b = 0; b < 4; b++) if (!g_gunBad[i][b] || b == 3) { g_gunBad[i][b] = w; break; }
                bot_log("gunner: %s's weapon %p does not turn - looking for its real one", pl + OFF_PLAYER_ONLINE_NAME, w);
                g_gunWeapon[i] = NULL; g_gunTries[i] = 0; g_seatYaw[i] = g_seatPitch[i] = 0; g_gunFire[i] = 0;
                return;
            }
        }
    }
    if (H->frame >= g_gunScanAt[i]) { g_gunScanAt[i] = H->frame + 10; gunner_pick_target(i, gp); }
    float *tp = NULL;
    int isVeh = 0;
    if (g_gunTargetVeh[i]) {
        char *tv = g_gunTargetVeh[i];
        if (!IsBadReadPtr(tv, 4) && *(unsigned *)tv == ADDR_VEHICLE_VTABLE && *(float *)(tv + OFF_VEHICLE_HEALTH) > 0) { tp = vehicle_pos(tv, NULL); isVeh = 1; }
    } else if (g_gunTarget[i] && player_exists(g_gunTarget[i])) {
        char *ts = *(char **)(g_gunTarget[i] + OFF_PLAYER_SOLDIER);
        if (ts && *(float *)(ts + OFF_SOLDIER_HEALTH) > 0 && !player_vehicle(g_gunTarget[i])) tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
    }
    if (!tp) { g_seatYaw[i] = g_seatPitch[i] = 0; g_gunFire[i] = 0; g_gunTarget[i] = g_gunTargetVeh[i] = NULL; g_gunAimErr[i] = 9; g_gunYawErr[i] = 0; return; }
    if (seat == 0 && g_gunCoax[i] && !gun_ok(g_gunCoax[i])) g_gunCoax[i] = NULL;
    if (seat == 0 && g_gunCoax[i]) {   // a tank driver: the main gun for vehicles, the coax for soldiers on foot
        int want = isVeh ? 1 : 2;
        if (g_gunSlot[i] != want) {
            press_button(i, want == 2 ? EIA_SELECT_WEAPON2 : EIA_SELECT_WEAPON1, 8);
            g_gunSlot[i] = (unsigned char)want;
            g_gunSwitchAt[i] = H->frame;
            bot_log("gunner: %s switches to the %s", pl + OFF_PLAYER_ONLINE_NAME, want == 2 ? "coax (soldier)" : "main gun (vehicle)");
        }
    }
    int onCoax = seat == 0 && g_gunSlot[i] == 2 && g_gunCoax[i];
    if (onCoax) { gp = (float *)(g_gunCoax[i] + 0x60); f = (float *)(g_gunCoax[i] + 0x50); }
    float aSpeed = onCoax ? g_coaxSpeed[i] : g_gunSpeed[i], aGrav = onCoax ? g_coaxGrav[i] : g_gunGrav[i];
    float aim[3] = {tp[0], tp[1] + (isVeh ? 1.0f : g_gunAimH[i] > 0 ? g_gunAimH[i] : FIGHT_AIM_HEIGHT), tp[2]};   // infantry: lowest clear point
    {   // lead a moving target by the shot's flight time; a slow shell (a tank's 200 m/s) also needs holding over for its drop
        float lp[3], ddx = tp[0] - gp[0], ddz = tp[2] - gp[2], hd = sqrtf(ddx * ddx + ddz * ddz);
        float sp = aSpeed > 20.0f ? aSpeed : BULLET_SPEED;
        lead_point(i, isVeh ? (void *)g_gunTargetVeh[i] : (void *)g_gunTarget[i], tp, hd * (BULLET_SPEED / sp), lp);
        aim[0] = lp[0]; aim[2] = lp[2];
        if (aGrav < 0 && aSpeed > 20.0f) { float t = hd / sp; aim[1] += 0.5f * -aGrav * t * t; }
    }
    float dx = aim[0] - gp[0], dy = aim[1] - gp[1], dz = aim[2] - gp[2], dh = sqrtf(dx * dx + dz * dz);
    float err = atan2f(dx, dz) - atan2f(f[0], f[2]);
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
    float perr = atan2f(dy, dh) - asinf(fmaxf(-1.0f, fminf(1.0f, f[1])));
    // traverse: negative level raises atan2 yaw; a small floor so the last degrees still move
    // a driver's main gun traverses flat out until nearly on (a target circling close in outruns a gentle turret)
    g_gunYawErr[i] = err;
    g_gunTgtBearing[i] = atan2f(dx, dz);
    if (seat == 0) {
        // a driver's main gun: stiff, plus feed-forward - the target's angular speed around us (from its measured
        // velocity) so the turret turns with it instead of only reacting once it has fallen behind
        float vx = g_leadVel[i][0], vz = g_leadVel[i][2], r2 = dx * dx + dz * dz;
        float omega = r2 > 1.0f ? (dz * vx - dx * vz) / r2 : 0.0f;   // d/dt atan2(dx, dz), rad/s
        float lvl = err * 12.0f + omega * g_gunFeedFwd;               // positive = towards larger atan2
        if (fabsf(err) < 0.004f && fabsf(omega) < 0.02f) lvl = 0;
        lvl = fmaxf(-g_gunMaxLevel, fminf(g_gunMaxLevel, lvl));
        g_seatYaw[i] = -lvl;                                          // (a negative level raises atan2 yaw)
    } else {
        float ya = fabsf(err) < 0.015f ? 0.0f : fmaxf(0.35f, fminf(1.0f, fabsf(err) * 2.0f));
        g_seatYaw[i] = err > 0 ? -ya : ya;
    }
    float pa = fabsf(perr) < 0.01f ? 0.0f : fmaxf(0.25f, fminf(1.0f, fabsf(perr) * 3.0f));
    g_seatPitch[i] = perr > 0 ? pa : -pa;
    // a driver's main gun (one shell per reload): only when the line is within ~1.5 m of the aim point at that range,
    // trigger held while it is; gunner seats (machine guns) keep the wider window and their bursts
    int mainGun = seat == 0 && !onCoax;
    float tolY = mainGun ? fmaxf(0.006f, 1.0f / fmaxf(dh, 1.0f)) : 0.05f, tolP = mainGun ? fmaxf(0.008f, 1.0f / fmaxf(dh, 1.0f)) : 0.06f;
    int onTarget = fabsf(err) < tolY && fabsf(perr) < tolP;
    g_gunAimErr[i] = fabsf(err) + fabsf(perr);
    g_gunTgtDist[i] = dh;
    unsigned ph = (H->frame - g_gunBurstAt[i]) % (GUN_BURST_ON + GUN_BURST_OFF);
    g_gunFire[i] = (unsigned char)(onTarget && (mainGun || ph < GUN_BURST_ON) && H->frame - g_gunSwitchAt[i] > 15);   // (a switch takes a moment)
    if (g_gunDebug && H->frame % 30 == 0)
        bot_log("gunner: %s aim err yaw %.3f pitch %.3f (want %.3f have %.3f) dist %.0f fire %d levels %.2f %.2f | target v (%.1f, %.1f) m/s, lead %.1f m, holdover %.1f m",
                pl + OFF_PLAYER_ONLINE_NAME, err, perr, atan2f(dy, dh), asinf(fmaxf(-1.0f, fminf(1.0f, f[1]))), dh, g_gunFire[i], g_seatYaw[i], g_seatPitch[i],
                g_leadVel[i][0], g_leadVel[i][2], sqrtf((aim[0] - tp[0]) * (aim[0] - tp[0]) + (aim[2] - tp[2]) * (aim[2] - tp[2])), aim[1] - tp[1] - 1.0f);
}

// ---- Performance counters: time spent per part of the tick, logged every 30 s (console: perf 0/1) ----
enum { PF_INPUT, PF_LEVEL, PF_ROSTER, PF_SQUAD, PF_DOWNED, PF_NAV, PF_BRAIN, PF_PATH, PF_DETECT, PF_STEP, PF_TOTAL, PF_COUNT };
static const char *kPerfNames[PF_COUNT] = {"input+steer+gunner", "level", "roster", "squads", "downed", "nav", "brain", "(of which paths)", "detect", "soldier step", "total"};
static double g_perfMs[PF_COUNT], g_perfMax[PF_COUNT];
static volatile double g_scanMs, g_scanMaxMs;
static volatile LONG g_scanPasses;
static int g_perfOn = 1;
static double perf_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void perf_add(int k, double ms) { g_perfMs[k] += ms; if (ms > g_perfMax[k]) g_perfMax[k] = ms; }
static void perf_report(void)
{
    static unsigned lastFrame;
    static double lastWall;
    double now = perf_now();
    if (!lastWall) { lastWall = now; lastFrame = H->frame; return; }
    if (now - lastWall < 30000.0) return;
    unsigned frames = H->frame - lastFrame;
    if (g_perfOn && frames) {
        char line[600];
        int n = snprintf(line, sizeof line, "perf: %u bots, %.1f ticks/s; per tick avg/max ms:", H->botCount, frames * 1000.0 / (now - lastWall));
        for (int k = 0; k < PF_COUNT; k++) n += snprintf(line + n, sizeof line - n, " %s %.2f/%.1f", kPerfNames[k], g_perfMs[k] / frames, g_perfMax[k]);
        n += snprintf(line + n, sizeof line - n, " | %u path plans (%u deferred) | %u hits on bots traced to a shooter | %u bot-s swimming (%u this level)",
                      g_pathPlans, g_planBudgetHits, g_attackerNoted, g_swimSeconds, g_swimSecondsLevel);
        n += snprintf(line + n, sizeof line - n, " | scan worker: %ld passes, avg %.0f ms, max %.0f ms", (long)g_scanPasses,
                      g_scanPasses ? g_scanMs / g_scanPasses : 0.0, g_scanMaxMs);
        bot_log("%s", line);
    }
    memset(g_perfMs, 0, sizeof g_perfMs); memset(g_perfMax, 0, sizeof g_perfMax);
    g_scanMs = 0; g_scanMaxMs = 0; g_scanPasses = 0; g_pathPlans = 0; g_planBudgetHits = 0; g_attackerNoted = 0; g_swimSeconds = 0;
    lastWall = now; lastFrame = H->frame;
}

// ---- Falling: a bot in free fall (bailed out of an aircraft, off a cliff, a tower or a ladder top) pulls its chute.
// Vertical speed from the position every frame; past FALL_CHUTE_SPEED it presses the chute key, and again every
// 0.5 s while still falling that fast - until the descent slows to chute speed, it lands, or it dies. ----
#define FALL_CHUTE_SPEED       20.0f    // m/s down: free fall reaches 40+, an open chute comes down at ~13 (jumps and ledges never get there)
static float g_fallLastY[MAX_BOTS], g_fallVy[MAX_BOTS];
static char *g_fallSoldier[MAX_BOTS];
static unsigned g_fallPressAt[MAX_BOTS];
static float g_fallPressVy[MAX_BOTS];
static int g_fallLandedFrames[MAX_BOTS];
static unsigned char g_chuteOpen[MAX_BOTS];
extern unsigned char g_gliding[MAX_BOTS];
static void fall_tick(int i)
{
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!s || player_vehicle(pl)) { g_fallSoldier[i] = NULL; g_fallPressAt[i] = 0; g_gliding[i] = 0; g_chuteOpen[i] = 0; return; }
    float y = ((float *)(s + OFF_SOLDIER_POSITION))[1];
    if (g_fallSoldier[i] != s) { g_fallSoldier[i] = s; g_fallLastY[i] = y; g_fallVy[i] = 0; return; }
    float vy = (y - g_fallLastY[i]) / BOT_TICK_DT;
    g_fallLastY[i] = y;
    g_fallVy[i] = 0.6f * g_fallVy[i] + 0.4f * vy;   // smoothed (positions arrive in small steps)
    // the key toggles the chute: after a press, only press again if 1.5 s later the bot is not slowing down
    int since = (int)(H->frame - g_fallPressAt[i]);
    if (g_fallPressAt[i] && g_fallVy[i] > g_fallPressVy[i] + 8.0f) g_chuteOpen[i] = 1;   // slowed right down: it is open
    int retry = g_fallPressAt[i] && since >= 45 && g_fallVy[i] <= g_fallPressVy[i] + 1.0f;   // pressed, still not slowing
    int first = !g_fallPressAt[i];
    if (g_chuteOpen[i]) first = retry = 0;   // never toggle an open chute shut
    // in the air: steer towards the nearest flag we do not own, else the nearest of ours, else the map centre
    if (!g_gliding[i] && g_fallVy[i] < -FALL_CHUTE_SPEED) { g_gliding[i] = 1; g_fallLandedFrames[i] = 0; }
    if (g_gliding[i]) {
        g_fallLandedFrames[i] = g_fallVy[i] > -1.0f ? g_fallLandedFrames[i] + 1 : 0;
        if (g_fallLandedFrames[i] >= 15) {   // on the ground again
            g_gliding[i] = 0;
            g_fallPressAt[i] = 0; g_chuteOpen[i] = 0;
            bot_nav(i)->mode = 1;
            bot_log("fall: %s landed - picking an objective from here", pl + OFF_PLAYER_ONLINE_NAME);
            end_task(i, bot_brain(i), "landed");
            choose_local_objective(i, "landed");
        } else if (g_flagCount) {
            float *me = (float *)(s + OFF_SOLDIER_POSITION), best = 1e30f, tx = 0, tz = 0;
            int team = player_team(pl), found = 0;
            for (int pass = 0; pass < 2 && !found; pass++)
                for (int f = 0; f < g_flagCount; f++) {
                    if ((pass == 0) == (flag_owner(f) == team)) continue;   // pass 0: not ours, pass 1: ours
                    float d = dist2(flag_pos(f), me);
                    if (d < best) { best = d; tx = flag_pos(f)[0]; tz = flag_pos(f)[2]; found = 1; }
                }
            if (!found) {
                for (int f = 0; f < g_flagCount; f++) { tx += flag_pos(f)[0]; tz += flag_pos(f)[2]; }
                tx /= g_flagCount; tz /= g_flagCount;
            }
            bot_nav(i)->mode = 4;   // face it and push forward (glides the chute that way)
            bot_nav(i)->x = tx;
            bot_nav(i)->z = tz;
        }
    }
    if (g_fallVy[i] < -FALL_CHUTE_SPEED && (first || retry)) {
        g_fallPressAt[i] = H->frame;
        g_fallPressVy[i] = g_fallVy[i];
        press_button(i, EIA_TOGGLE_PARACHUTE, 3);
        static unsigned logged;
        if (logged++ < 40) bot_log("fall: %s falling at %.0f m/s - parachute", pl + OFF_PLAYER_ONLINE_NAME, -g_fallVy[i]);
    }
}

// Adopt a human's deploy point as the team's HQ when it is not a flag spawn and puts soldiers closer to the takeable
// flags than the HQ the bots use now.
static void kill_bot(char *pl);
// Can soldiers of `team` appearing at sp be an HQ of theirs? Not the origin (spawns of other modes put them there),
// nearer their own HQ than the enemy's and within 900 m of it, and not inside the fighting (150 m from a flag
// they do not hold). Returns NULL if fine, else why not.
static const char *hq_spot_problem(int team, const float *sp)
{
    if (fabsf(sp[0]) < 1.0f && fabsf(sp[2]) < 1.0f) return "at the map origin (not a real spawn here)";
    int other = 3 - team;
    if (g_hqCpKnown[team]) {
        float dOwn = sqrtf(dist2(sp, g_hqCpPos[team])), dEnemy = g_hqCpKnown[other] ? sqrtf(dist2(sp, g_hqCpPos[other])) : 1e30f;
        if (dEnemy < dOwn) return "on the enemy's side";
        if (dOwn > 900.0f) return "too far from the team's HQ";
    }
    for (int f = 0; f < g_flagCount; f++) if (flag_owner(f) != team && dist2(flag_pos(f), sp) < 150.0f * 150.0f) return "inside the fighting";
    return NULL;
}
static void hq_probe_one(HqProbe *q);
static void hq_probe_tick(void) { for (int k = 0; k < MAX_HQ_PROBES; k++) hq_probe_one(&g_hqProbes[k]); }
static void hq_probe_one(HqProbe *q)
{
    if (!q->bot || H->frame - q->at < 20) return;
    char *bot = q->bot, *s = player_exists(bot) ? *(char **)(bot + OFF_PLAYER_SOLDIER) : NULL;
    int team = q->team, other = 3 - team, k = q->cand;
    q->bot = NULL;
    if (!s || k < 0 || k >= g_hqCandCount || !g_flagCount) return;
    if (player_vehicle(bot)) {   // a vehicle spawn point (it put the bot in a seat): no HQ
        char *pv = player_vehicle(bot);
        if (!vehicle_ours_side(pv, team) || vehicle_nation(pv) == other) {   // the other side's (a parked jet has no team): out, respawn properly
            bot_log("spawn: HQ candidate %p put %s in the other side's %s - killed to respawn", g_hqCand[k], bot + OFF_PLAYER_ONLINE_NAME, vehicle_name(pv));
            kill_bot(bot);
            return;
        }
        bot_log("spawn: HQ candidate %p is a vehicle spawn - %s rides along, candidate dropped", g_hqCand[k], bot + OFF_PLAYER_ONLINE_NAME);
        return;
    }
    float *sp = (float *)(s + OFF_SOLDIER_POSITION);
    float dOwn = sqrtf(dist2(sp, g_hqCpPos[team])), dEnemy = g_hqCpKnown[other] ? sqrtf(dist2(sp, g_hqCpPos[other])) : 1e30f;
    float dNew = 1e30f, dOld = 1e30f;
    for (int f = 0; f < g_flagCount; f++) {
        float a = dist2(flag_pos(f), sp), b = dist2(flag_pos(f), g_hqPos[team]);
        if (a < dNew) dNew = a;
        if (b < dOld) dOld = b;
    }
    dNew = sqrtf(dNew); dOld = sqrtf(dOld);
    const char *why = hq_spot_problem(team, sp);
    (void)dOwn; (void)dEnemy;
    if (why) {
        bot_log("spawn: HQ candidate %p put team %d at (%.0f, %.0f) - rejected (%s)", g_hqCand[k], team, sp[0], sp[2], why);
        kill_bot(bot);
        return;
    }
    bot_log("spawn: HQ candidate %p for team %d at (%.0f, %.0f): %.0f m from the nearest flag (current HQ %.0f m)%s", g_hqCand[k], team,
            sp[0], sp[2], dNew, dOld, dNew < dOld ? " - bots spawn there from now on" : "");
    if (dNew < dOld) {
        hq_memory_save(team, spawn_data_pos(g_hqCand[k]) && !near_origin(spawn_data_pos(g_hqCand[k])) ? spawn_data_pos(g_hqCand[k]) : sp, H->teamSpawn[team].a2, H->teamSpawn[team].a3,
                       H->teamSpawn[team].a4);
        float oldHq[3];
        memcpy(oldHq, g_hqPos[team], sizeof oldHq);
        H->teamSpawn[team].entity = g_hqCand[k];
        memcpy(g_hqPos[team], sp, sizeof g_hqPos[team]);
        int moved = 0;
        for (int j = 0; j < H->botCount; j++) {   // whoever is still at the old HQ comes back at the new one
            char *pl = H->bots[j], *os = *(char **)(pl + OFF_PLAYER_SOLDIER);
            if (!os || player_team(pl) != team || player_vehicle(pl)) continue;
            if (dist2((float *)(os + OFF_SOLDIER_POSITION), oldHq) < 120.0f * 120.0f) { kill_bot(pl); moved++; }
        }
        if (moved) bot_log("spawn: %d team %d bot(s) at the old HQ respawn at the new one", moved, team);
    }
}

// ---- HQ memory: an HQ learned on a map (from a player's deploy or a probe) is saved with its position
// (BotMod_hq_<level>.txt: "team x y z") and picked straight away at the start of every later round - the spawn point
// whose data transform (+0x10, position at +0x40) is there. Wake's US island HQ from the first second. ----
#define ADDR_CSE_VTABLE 0x025bbbc0
static void hq_memory_file(char *out, int n)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(out, n, "%s\\BotMod_hq_%s.txt", dir, g_level);
}
static void hq_memory_save(int team, const float *pos, unsigned a2, unsigned a3, unsigned a4)
{
    if (near_origin(pos)) { bot_log("level: not remembering team %d's HQ at the world origin (%.0f, %.0f)", team, pos[0], pos[2]); return; }
    char path[MAX_PATH + 64], lines[3][96] = {"", "", ""};
    hq_memory_file(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) { char l[96]; while (fgets(l, sizeof l, f)) { int t = atoi(l); if (t == 1 || t == 2) snprintf(lines[t], sizeof lines[t], "%s", l); } fclose(f); }
    // with the deploy arguments that spawn point was used with (another spawn's arguments crash the game)
    snprintf(lines[team], sizeof lines[team], "%d %.1f %.1f %.1f %u %u %u\n", team, pos[0], pos[1], pos[2], a2, a3, a4);
    if ((f = fopen(path, "w"))) { fputs(lines[1], f); fputs(lines[2], f); fclose(f); }
    bot_log("level: remembered team %d's HQ on %s at (%.0f, %.0f) [args %08x %u %u]", team, g_level, pos[0], pos[2], a2, a3, a4);
}
static float *spawn_data_pos(void *spawn)   // CharacterSpawnEntity data transform position (NULL if unreadable)
{
    char *data = spawn && !IsBadReadPtr((char *)spawn + 0x14, 4) ? *(char **)((char *)spawn + 0x14) : NULL;
    return data && !IsBadReadPtr(data + 0x40, 12) ? (float *)(data + 0x40) : NULL;
}
static void hq_memory_apply(void)
{
    g_origHq[1] = H->teamSpawn[1]; g_origHq[2] = H->teamSpawn[2];   // (the level's own, for the Pilots)
    char path[MAX_PATH + 64], l[96];
    hq_memory_file(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    float want[3][3];
    unsigned args[3][3];
    int have[3] = {0, 0, 0};
    while (fgets(l, sizeof l, f)) {   // "team x y z a2 a3 a4" - a line without the arguments is not used
        int t; float x, y, z; unsigned a2, a3, a4;
        if (sscanf(l, "%d %f %f %f %u %u %u", &t, &x, &y, &z, &a2, &a3, &a4) != 7 || (t != 1 && t != 2)) continue;
        { float o[3] = {x, y, z}; if (near_origin(o)) { bot_log("level: ignoring a remembered team %d HQ at the world origin", t); continue; } }
        if (a2 != 0xffffffff && a2 >= 0x10000) continue;   // a pointer: only valid in the round it was seen in
        want[t][0] = x; want[t][1] = y; want[t][2] = z; args[t][0] = a2; args[t][1] = a3; args[t][2] = a4; have[t] = 1;
    }
    fclose(f);
    char *sp[160];
    int n = scan_objects(ADDR_CSE_VTABLE, sp, 160);
    for (int t = 1; t <= 2; t++) {
        if (!have[t]) continue;
        for (int k = 0; k < n; k++) {
            float *p = spawn_data_pos(sp[k]);
            if (!p || dist2(p, want[t]) > 15.0f * 15.0f || spawn_team(sp[k]) == 3 - t) continue;   // (HQ spawns carry no 1/2 team)
            H->teamSpawn[t].entity = sp[k];
            H->teamSpawn[t].a2 = args[t][0]; H->teamSpawn[t].a3 = args[t][1]; H->teamSpawn[t].a4 = args[t][2];
            memcpy(g_hqPos[t], want[t], 12);
            memset(g_hqCandTried[t], 1, sizeof g_hqCandTried[t]);   // already the best one found: nothing left to probe
            bot_log("level: team %d spawns at its remembered HQ (%.0f, %.0f)", t, want[t][0], want[t][2]);
            break;
        }
    }
}

static void hq_learn_tick(void)
{
    hq_probe_tick();
    PendingHq *q = &g_pendingHq;
    if (!q->at || H->frame - q->at < 30 || !level_ready()) return;
    char *pl = q->player;
    void *ent = q->entity;
    q->at = 0;
    if (!player_exists(pl) || !g_flagCount) return;
    int team = player_team(pl);
    char *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if ((team != 1 && team != 2) || !s || ent == H->teamSpawn[team].entity) return;
    if (IsBadReadPtr(ent, 4) || *(unsigned *)ent != 0x025bbbc0) return;   // a CharacterSpawnEntity (not a beacon / squad mate)
    for (int f = 0; f < g_flagCount; f++) if (g_flagSpawn[f][1] == ent || g_flagSpawn[f][2] == ent) return;   // a flag spawn
    float *sp = (float *)(s + OFF_SOLDIER_POSITION), dNew = 1e30f, dOld = 1e30f;
    const char *why = hq_spot_problem(team, sp);
    if (why) return;
    for (int f = 0; f < g_flagCount; f++) {
        float a = dist2(flag_pos(f), sp), b = dist2(flag_pos(f), g_hqPos[team]);
        if (a < dNew) dNew = a;
        if (b < dOld) dOld = b;
    }
    bot_log("level: %s deployed from spawn %p at (%.0f, %.0f) - %.0f m from the nearest flag (bot HQ: %.0f m)", pl + OFF_PLAYER_ONLINE_NAME,
            ent, sp[0], sp[2], sqrtf(dNew), sqrtf(dOld));
    if (H->teamSpawn[team].entity && dNew >= dOld) return;
    H->teamSpawn[team].entity = ent;
    H->teamSpawn[team].a2 = q->a2; H->teamSpawn[team].a3 = q->a3; H->teamSpawn[team].a4 = q->a4;
    memcpy(g_hqPos[team], sp, sizeof g_hqPos[team]);
    bot_log("level: team %d bots now use that spawn as their HQ (closer to the fighting)", team);
    hq_memory_save(team, spawn_data_pos(ent) && !near_origin(spawn_data_pos(ent)) ? spawn_data_pos(ent) : sp, q->a2, q->a3, q->a4);
}

// ---- Close threat: an enemy soldier within CLOSE_THREAT_RANGE with a line to the bot is dealt with first, whatever
// the bot was doing (fighting someone further away, a supply run, a revive, cover, a rocket...). Checked 5x a second. ----
#define CLOSE_THREAT_RANGE     10.0f
static void close_threat_tick(int i)
{
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER), *pm = player_manager();
    BotBrain *b = bot_brain(i);
    if (!s || !pm || !b->enabled || player_vehicle(pl) || g_fallVy[i] < -8.0f || *(float *)(s + OFF_SOLDIER_HEALTH) <= 0) return;
    float *me = (float *)(s + OFF_SOLDIER_POSITION), eye[3] = {me[0], me[1] + 1.6f, me[2]};
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    char *best = NULL;
    float bestD = CLOSE_THREAT_RANGE;
    for (unsigned k = 0; k < count && k < 64; k++) {
        char *t = arr[k], *ts;
        if (!t || t == pl || player_team(t) == player_team(pl) || player_vehicle(t) || !(ts = *(char **)(t + OFF_PLAYER_SOLDIER))) continue;
        if (*(float *)(ts + OFF_SOLDIER_HEALTH) <= 0) continue;
        float *tp = (float *)(ts + OFF_SOLDIER_SPAWN_POSITION);
        float dx = tp[0] - me[0], dy = tp[1] - me[1], dz = tp[2] - me[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d >= bestD) continue;
        float chest[3] = {tp[0], tp[1] + 1.2f, tp[2]}, head[3] = {tp[0], tp[1] + 1.6f, tp[2]};
        if (solid_between(eye, chest, NULL) && solid_between(eye, head, NULL)) continue;   // behind a wall / floor
        bestD = d; best = t;
    }
    if (!best) return;
    if (b->task == TASK_FIGHT) {
        char *cur = g_fightTarget[i], *cs;
        if (cur == best) return;
        if (cur && player_exists(cur) && (cs = *(char **)(cur + OFF_PLAYER_SOLDIER)) && *(float *)(cs + OFF_SOLDIER_HEALTH) > 0 &&
            dist2((float *)(cs + OFF_SOLDIER_SPAWN_POSITION), me) < CLOSE_THREAT_RANGE * CLOSE_THREAT_RANGE) return;   // already on a close one
    }
    bot_log("close: %s turns on %s %.1f m away (was %s)", pl + OFF_PLAYER_ONLINE_NAME, best + OFF_PLAYER_ONLINE_NAME, bestD, kTaskNames[b->task]);
    if (b->task == TASK_SUPPLY || b->task == TASK_REVIVE || b->task == TASK_COVER || b->task == TASK_ROCKET) {
        b->task = b->prevTask; b->commander = b->prevCommander; b->serviceTarget = NULL;   // drop it, back to what came before
        b->prevTask = TASK_NONE; b->prevCommander = NULL;
    }
    start_fight(i, best, 0, "enemy close");
    g_fightAdvance[i] = 0;
    if (!spotted_cached(best)) bot_spot(pl, best);   // and call it out
}

// ---- Light vehicles: a bot with a long way to go takes a free jeep / buggy / quad of its own side (never armour or
// aircraft, never one a human is near) and drives it on the drivable part of the nav grid (nav_drive_mask: open,
// gently sloped, dry - corridors of 4 m or more), gets out ~30 m short of its flag and walks on. Throttle = EIA 0,
// steering = EIAYaw (positive turns towards a lower heading). ----
static const char *const kLightVehicles[] = {"Growler", "VDVBuggy", "Buggy", "DPV", "HMMWV", "Humvee", "Vodnik", "GAZ", "Quad", "DirtBike", "Dirtbike", "RHIB", NULL};
static int vehicle_is_boat(char *veh) { return strstr(vehicle_name(veh), "RHIB") != NULL; }
static unsigned char *g_boatMask;
static unsigned g_boatMaskFor;   // grid serial
static int *g_landLabels;   // landmass of every dry node (boat landings must be on the flag's)
static const unsigned char *boat_mask(void)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return NULL;
    if (g_boatMaskFor != g->serial) {
        if (g_boatMask) HeapFree(GetProcessHeap(), 0, g_boatMask);
        drive_strikes_reset();
        int n = 0;
        g_boatMask = nav_boat_mask(g, &n);
        g_boatMaskFor = g->serial;
        if (g_landLabels) HeapFree(GetProcessHeap(), 0, g_landLabels);
        g_landLabels = nav_land_labels(g);
        bot_log("drive: %d boat-drivable water node(s) on %s", n, g->level);
    }
    return g_boatMask;
}
#define DRIVE_MIN_TRIP         250.0f   // only for flags at least this far away
#define DRIVE_FIND_RADIUS      80.0f    // a vehicle this close to the bot
#define DRIVE_STOP_SHORT       55.0f
#define PILOT_STOP_SHORT       15.0f    // a Pilot's tank drives into the flag's zone (vehicles capture)    // get out this far from the flag (a short walk; driving into a flag's clutter is slow)
#define DRIVE_WAYPOINT         6.0f
static unsigned g_driveCooldown[MAX_BOTS];
static unsigned char *g_driveMask;
static unsigned char *g_tankMask;   // pilots' looser route map on steep maps (nav_tank_mask); stuck spots struck off it too
static unsigned g_tankMaskFor;
static unsigned char *tank_mask_now(void)   // only the current grid's (an old map's, smaller, crashed server 1 in drive_strike on
{                                           // Caspian, 2026-10-13): a stale one is freed, never used
    NavGrid *g = *nav_slot();
    if (g_tankMask && (!g || g_tankMaskFor != g->serial)) {
        strike_forget(g_tankMask);
        HeapFree(GetProcessHeap(), 0, g_tankMask);
        g_tankMask = NULL;
    }
    return g_tankMask;
}
static unsigned g_driveMaskFor;   // grid serial
static int vehicle_is_light(char *veh)
{
    const char *n = vehicle_name(veh);
    for (int k = 0; kLightVehicles[k]; k++) if (strstr(n, kLightVehicles[k])) return 1;
    return 0;
}
static float *vehicle_xf(char *veh)   // 4x4 transform (right, up, forward, position) of the vehicle's physics
{
    char *phys = !IsBadReadPtr(veh + 0x30, 4) ? *(char **)(veh + 0x30) : NULL;
    char *xf = phys && !IsBadReadPtr(phys + 0x88, 4) ? *(char **)(phys + 0x88) : NULL;
    return xf && !IsBadReadPtr(xf, 64) ? (float *)xf : NULL;
}
static int vehicle_ok(char *veh)
{
    return veh && !IsBadReadPtr(veh, 0x200) && *(unsigned *)veh == ADDR_VEHICLE_VTABLE && *(float *)(veh + OFF_VEHICLE_HEALTH) > 0;
}
static int driver_seat_free(char *veh)
{
    char **b;
    return vehicle_seats(veh, &b) && !*(void **)(b[0] + 0x4c);
}
static int __attribute__((unused)) human_near(const float *p, float r)
{
    char *pm = player_manager();
    if (!pm) return 0;
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count && k < 64; k++) {
        char *pl = arr[k];
        float *hp;
        if (!pl || is_bot(pl) || !(hp = human_pos(pl))) continue;
        if (dist2(hp, p) < r * r) return 1;
    }
    return 0;
}
// The paved drive mask is the same every time a map loads: kept in BotMod_pave_<level>.bin (the paving survey casts up
// to 15000 rays a frame for seconds after every load - a stutter on a slow server).
static void pave_cache_path(char *out, int n, const char *level)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(out, n, "%s\\BotMod_pave_%s.bin", dir, level);
}
static const unsigned char *drive_mask(void)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return NULL;
    if (g_driveMaskFor != g->serial) {
        if (g_driveMask) HeapFree(GetProcessHeap(), 0, g_driveMask);
        drive_strikes_reset();
        int n = 0;
        double t = perf_now();
        nav_set_road_mask(NULL, NULL);
        g_driveMask = nav_drive_mask(g, &n);
        g_driveMaskFor = g->serial;
        nav_set_road_mask(g, g_driveMask);
        g_paveCursor = 0; g_paveCount = 0;
        bot_log("drive: %d drivable node(s) on %s (%.0f ms)", n, g->level, perf_now() - t);
        if (g_driveMask) {
            char path[MAX_PATH + 64];
            pave_cache_path(path, sizeof path, g->level);
            FILE *f = fopen(path, "rb");
            int hdr[3] = {0, 0, 0}, cells = g->w * g->h * NAV_LAYERS;
            if (f) {
                if (fread(hdr, 4, 3, f) == 3 && hdr[0] == g->w && hdr[1] == g->h && hdr[2] == NAV_LAYERS &&
                    fread(g_driveMask, 1, (size_t)cells, f) == (size_t)cells) {
                    g_paveCursor = -1;
                    bot_log("drive: paving loaded from %s", path);
                    tree_apply_drive();
                } else { g_driveMask = nav_drive_mask(g, &n); nav_set_road_mask(g, g_driveMask); }   // (a bad file: start clean)
                fclose(f);
            }
        }
    }
    return g_driveMask;
}
// Ground materials (2026-10-13, per map - the user's Wake Island walk: its dirt roads are a 400c strip between 4013
// verges, grass 4010, beaches 400c too): one ray down at every drivable node, 6000 a frame after the paving survey,
// kept in BotMod_mat_<level>.bin. The streets classifier uses them (a strip of one material, 4-16 m wide, 2x as long).
static unsigned short *g_nodeMat;
static unsigned char *g_nodeObst;   // a prop beside the node at bumper height (mailboxes, news stands - between grid points the nav never
                                    // saw them; Pilot US 5's Humvee clipped one on Seine, 2026-10-13): off the taxi map
static unsigned g_matFor;
static int g_matCursor = -1, g_matDone;
static void mat_cache_path(char *out, int n, const char *level)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    *strrchr(dir, '\\') = 0;
    snprintf(out, n, "%s\\BotMod_mat2_%s.bin", dir, level);   // (mat2: materials + props)
}
static void mat_survey_tick(void)
{
    NavGrid *g = *nav_slot();
    const unsigned char *dm = g_driveMask;
    if (!g || g->phase != 2 || !dm || g_driveMaskFor != g->serial || g_paveCursor >= 0) return;
    int n = g->w * g->h * NAV_LAYERS;
    char path[MAX_PATH + 64];
    if (g_matFor != g->serial) {
        if (g_nodeMat) HeapFree(GetProcessHeap(), 0, g_nodeMat);
        if (g_nodeObst) HeapFree(GetProcessHeap(), 0, g_nodeObst);
        g_nodeMat = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)n * 2);
        g_nodeObst = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)n);
        g_matFor = g->serial; g_matDone = 0; g_matCursor = 0;
        if (!g_nodeMat || !g_nodeObst) { g_matCursor = -1; return; }
        mat_cache_path(path, sizeof path, g->level);
        FILE *f = fopen(path, "rb");
        int hdr[3];
        if (f) {
            if (fread(hdr, 4, 3, f) == 3 && hdr[0] == g->w && hdr[1] == g->h && hdr[2] == NAV_LAYERS && fread(g_nodeMat, 2, (size_t)n, f) == (size_t)n &&
                fread(g_nodeObst, 1, (size_t)n, f) == (size_t)n) {
                g_matCursor = -1; g_matDone = 1;
                bot_log("drive: ground materials loaded from %s", path);
            }
            fclose(f);
        }
        return;
    }
    if (g_matCursor < 0) return;
    unsigned saved = g_rayFlags;
    int rays = 0;
    void *vb[64];   // parked vehicles' bodies: not props (they move - the file is kept)
    int nvb = 0;
    { char **vs; int nv = vehicle_list(&vs); for (int k = 0; k < nv && nvb < 64; k++) if (!IsBadReadPtr(vs[k] + 0x30, 4)) vb[nvb++] = *(void **)(vs[k] + 0x30); }
    for (; g_matCursor < n && rays < 6000; g_matCursor++) {
        if (!dm[g_matCursor]) continue;
        float p[3], a[3], b[3], h[3], nr[3];
        nav_node_pos(g, g_matCursor, p);
        a[0] = b[0] = p[0]; a[2] = b[2] = p[2]; a[1] = p[1] + 1.0f; b[1] = p[1] - 1.0f;
        rays += 5;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;
        if (ray_cast(a, b, h, NULL)) { unsigned m = hit_material(); g_nodeMat[g_matCursor] = (m >> 24) ? 0 : (unsigned short)m; }
        g_rayFlags = RAY_DONT_CHECK_CHARACTER | RAY_CHECK_DETAIL_MESH;   // (as the bumper sees)
        static const float dir4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int k = 0; k < 4 && !g_nodeObst[g_matCursor]; k++) {
            float s0[3] = {p[0], p[1] + 0.8f, p[2]}, s1[3] = {p[0] + dir4[k][0] * 1.4f, p[1] + 0.8f, p[2] + dir4[k][1] * 1.4f};
            if (!ray_cast(s0, s1, h, nr) || nr[1] > 0.65f) continue;   // (rising ground is no prop)
            void *body = *(void **)g_lastHitRest;
            int veh = 0;
            for (int v = 0; v < nvb && !veh; v++) if (vb[v] == body) veh = 1;
            if (!veh && !(body && !IsBadReadPtr(body, 4) && *(unsigned *)body == ADDR_WATER_PHYSICS_VTABLE)) g_nodeObst[g_matCursor] = 1;
        }
    }
    g_rayFlags = saved;
    if (g_matCursor >= n) {
        g_matCursor = -1; g_matDone = 1;
        int kinds = 0;
        { static unsigned char seen[65536 / 8]; memset(seen, 0, sizeof seen);
          for (int k = 0; k < n; k++) if (g_nodeMat[k] && !(seen[g_nodeMat[k] >> 3] & (1 << (g_nodeMat[k] & 7)))) { seen[g_nodeMat[k] >> 3] |= 1 << (g_nodeMat[k] & 7); kinds++; } }
        bot_log("drive: ground material survey done - %d material(s)", kinds);
        mat_cache_path(path, sizeof path, g->level);
        FILE *f = fopen(path, "wb");
        if (f) { int hdr[3] = {g->w, g->h, NAV_LAYERS}; fwrite(hdr, 4, 3, f); fwrite(g_nodeMat, 2, (size_t)n, f); fwrite(g_nodeObst, 1, (size_t)n, f); fclose(f); }
        int no = 0;
        for (int k = 0; k < n; k++) no += g_nodeObst[k];
        bot_log("drive: %d drivable node(s) have a prop beside them - off the taxi map", no);
    }
}
// Taxi masks (the user, 2026-10-13: a Pilot's jeep taxi keeps to the roads and at least 3 m off objects - one kept
// nosing a parked coal truck's corners): drivable nodes with drivable ground 3 m all round, classed by the streets
// rule - street middles cheapest, street edges next, anything else dear. Rebuilt when the paving / material surveys end.
static unsigned char *g_taxiRoad, *g_taxiAny;
static unsigned g_taxiFor;
static int g_taxiPaved;
static void taxi_masks(void)
{
    NavGrid *g = *nav_slot();
    const unsigned char *dm = drive_mask();
    if (!g || !dm) return;
    int state = (g_paveCursor < 0) + 2 * (g_matDone && g_matFor == g->serial);
    if (g_taxiFor == g->serial && g_taxiRoad && g_taxiPaved == state) return;
    if (g_taxiRoad) { strike_forget(g_taxiRoad); HeapFree(GetProcessHeap(), 0, g_taxiRoad); }
    if (g_taxiAny) { strike_forget(g_taxiAny); HeapFree(GetProcessHeap(), 0, g_taxiAny); g_taxiAny = NULL; }
    int nr = 0;
    double t = perf_now();
    g_taxiRoad = nav_mask_clear_of(g, dm, 4, 0.0f, &nr);   // (every drivable node but a tree's: clearance is a cost now, not a filter -
                                                            //  a dirt road between two obstacles fell off the map)
    extern int g_lastStreetNodes;
    if (g_taxiRoad && (state & 2) && g_nodeObst) { int n = g->w * g->h * NAV_LAYERS; for (int k = 0; k < n; k++) if (g_nodeObst[k] && g_taxiRoad[k]) g_taxiRoad[k] = 7; }   // (dear, not gone: a gate between two posts
                                                                                          //  cut Seine's US HQ off the map)
    if (g_taxiRoad) nav_mask_streets(g, g_taxiRoad, state & 2 ? g_nodeMat : NULL);
    g_taxiFor = g->serial; g_taxiPaved = state;
    bot_log("drive: taxi map for %s - %d node(s), %d street node(s) (%.0f ms%s)", g->level, nr, g_lastStreetNodes, perf_now() - t,
            state & 2 ? ", by ground material" : ", before the material survey - kerbs only");
}
// roadwatch (2026-10-13, the user walking Wake Island's roads): every 2 s the watched player's spot - ground material,
// drive-mask class, and the taxi road map's verdict (street middle / street side / other flat ground / not road).
char g_roadWatch[64];
static void roadwatch_tick(void)
{
    if (!g_roadWatch[0] || H->frame % 60) return;
    NavGrid *g = *nav_slot();
    char *pl = find_player(g_roadWatch);
    float *hp = pl ? human_pos(pl) : NULL;
    if (!g || g->phase != 2 || !hp) return;
    taxi_masks();
    const unsigned char *dm = drive_mask();
    int c = nav_cell_index(g, hp[0], hp[2]), node = -1;
    float bd = 3.0f;
    for (int l = 0; c >= 0 && l < NAV_LAYERS; l++) {
        float p[3];
        nav_node_pos(g, c * NAV_LAYERS + l, p);
        if (fabsf(p[1] - hp[1]) < bd) { bd = fabsf(p[1] - hp[1]); node = c * NAV_LAYERS + l; }
    }
    unsigned saved = g_rayFlags, mat = 0;
    g_rayFlags = RAY_DONT_CHECK_CHARACTER;
    float a[3] = {hp[0], hp[1] + 1.5f, hp[2]}, b[3] = {hp[0], hp[1] - 3.0f, hp[2]}, h[3];
    if (ray_cast(a, b, h, NULL)) mat = hit_material();
    g_rayFlags = saved;
    int dc = node >= 0 && dm ? dm[node] : 0, rc = node >= 0 && g_taxiRoad ? g_taxiRoad[node] : 0, ac = node >= 0 && g_taxiAny ? g_taxiAny[node] : 0;
    const char *v = rc == 1 ? "STREET (middle)" : rc == 2 ? "STREET (near an edge)" : rc == 3 ? "STREET (against an edge)" : rc == 4 ? "OFF road" :
                    rc == 6 ? "OFF road (against an edge)" : dc ? "OFF road (by a tree)" : "not drivable";
    (void)ac;
    bot_log("roadwatch: %s at (%.0f, %.0f) mat %08x drive class %d road %d any %d -> %s", g_roadWatch, hp[0], hp[2], mat, dc, rc, ac, v);
    chat_say("[BotMod] %s mat %x", v, mat);
}
static char *g_vehBad[8];          // vehicles whose route failed recently: left alone for a while
static unsigned g_vehBadUntil[8];
static int vehicle_bad(char *v)
{
    for (int k = 0; k < 8; k++) if (g_vehBad[k] == v && H->frame < g_vehBadUntil[k]) return 1;
    return 0;
}
static void vehicle_mark_bad(char *v, int sec)
{
    int slot = 0;
    for (int k = 0; k < 8; k++) if (g_vehBad[k] == v || H->frame >= g_vehBadUntil[k]) { slot = k; break; }
    g_vehBad[slot] = v; g_vehBadUntil[slot] = H->frame + (unsigned)sec * 30;
}
static void drive_stop(int i, const char *why, int cooldownSec)
{
    DriveState *d = &g_drive[i];
    if (!d->phase) return;
    char *pveh = player_vehicle(H->bots[i]);
    if (is_pilot(i) && pveh && *(char **)(H->bots[i] + OFF_PLAYER_SOLDIER)) {   // a Pilot: hold here and keep fighting
        if (d->phase != 8) bot_log("pilot: %s holds in the %s (%s)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, vehicle_name(pveh),
                                   !strncmp(why, "arrived", 7) ? "at its objective" : why);
        d->phase = 8; d->veh = pveh; d->n = 0;
        d->at = H->frame + (unsigned)(cooldownSec > 20 ? 20 : cooldownSec < 6 ? 6 : cooldownSec) * 30;   // next decision
        if (strncmp(why, "arrived", 7)) d->at = H->frame + 4 * 30;   // stopped short of its objective: back at it in 4 s
        { int vk = vehicle_kind(pveh); if (vk == VK_LIGHT || vk == VK_BOAT) d->at = H->frame + 15; }   // a taxi: its next leg now (it sat 20 s
                                                                                                  //  with 3 riders after a pick-up, 2026-10-13)
        g_driving[i] = 1;
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        return;
    }
    int wasIn = player_vehicle(H->bots[i]) != NULL;
    if (wasIn && d->phase != 7) {   // the driver: "Get out!" - riders bail first, then the driver
        bot_say(i, ORDER_GETOUT);
        order_reaction(H->bots[i], ORDER_GETOUT, NULL);
    }
    if (player_vehicle(H->bots[i])) bot_exit_vehicle(i, 0);
    g_driving[i] = 0;
    bot_log("drive: %s %s", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, why);
    d->phase = 0; d->veh = NULL;
    g_driveCooldown[i] = H->frame + (unsigned)cooldownSec * 30;
    H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
    bot_nav(i)->mode = 0;
    bot_brain(i)->replanAt = 0;
    if (wasIn) {
        choose_local_objective(i, "out of the vehicle");   // the flag nearest where it got out, then back to the squad
        g_vehChanceUntil[i] = H->frame + 20 * 30;           // a ground vehicle nearby for the next leg?
        g_driveCooldown[i] = 0;
    }
}
// Paving survey (after the mask is built, ~15000 nodes a frame): a ray down at every drivable node reads the ground's
// material - paved surfaces (asphalt / concrete, material index 0x3fxx) become the cheapest class, smooth unpaved
// ground (terrain soils 0x40xx, dirt roads included) one class dearer than before.
static void drive_pave_tick(void)
{
    NavGrid *g = *nav_slot();
    if (!g || !g_driveMask || g_driveMaskFor != g->serial || g_paveCursor < 0) return;
    int n = g->w * g->h * NAV_LAYERS, end = g_paveCursor + 15000;
    unsigned saved = g_rayFlags;
    g_rayFlags = RAY_DONT_CHECK_CHARACTER;
    for (; g_paveCursor < n && g_paveCursor < end; g_paveCursor++) {
        unsigned char *m = &g_driveMask[g_paveCursor];
        if (!*m) continue;
        float p[3], a[3], b[3], h[3];
        nav_node_pos(g, g_paveCursor, p);
        a[0] = b[0] = p[0]; a[2] = b[2] = p[2]; a[1] = p[1] + 1.0f; b[1] = p[1] - 1.0f;
        if (!ray_cast(a, b, h, NULL)) continue;
        unsigned mat = hit_material();
        if ((mat & 0xff00) == 0x3f00 && (mat >> 24) == 0) { *m = 1; g_paveCount++; }   // paved
        else if (*m == 1) *m = 2;                                                        // smooth, but not paved
    }
    g_rayFlags = saved;
    if (g_paveCursor >= n) {
        bot_log("drive: paving survey done - %d paved node(s)", g_paveCount);
        g_paveCursor = -1;
        char path[MAX_PATH + 64];
        pave_cache_path(path, sizeof path, g->level);
        FILE *f = fopen(path, "wb");
        if (f) { int hdr[3] = {g->w, g->h, NAV_LAYERS}; fwrite(hdr, 4, 3, f); fwrite(g_driveMask, 1, (size_t)n, f); fclose(f); }
        tree_apply_drive();
    }
}

// Tree survey (once per nav grid, ~400 nodes a frame): trees are detail mesh, which the nav build never saw. Rays
// out from every dry walkable node at 1.5 m, 2.5 m long in 8 directions, with detail mesh on; a hit the same ray
// without detail mesh does not have is vegetation. Per node the distance to the nearest trunk (decimetres): soldiers
// keep off nodes within 1.2 m (nav), vehicles get class 5 within 2.5 m - very dear, no straight short cut past it.
static unsigned char *g_treeDist;
static unsigned g_treeFor;   // grid serial
static int g_treeCursor = -1, g_treeCount;
static void tree_apply_drive(void)
{
    NavGrid *g = *nav_slot();
    if (!g || g_treeFor != g->serial || g_treeCursor >= 0 || !g_treeDist || !g_driveMask || g_driveMaskFor != g->serial || g_paveCursor >= 0) return;
    int n = g->w * g->h * NAV_LAYERS, k = 0;
    for (int i = 0; i < n; i++) if (g_driveMask[i] && g_treeDist[i] <= 25) { g_driveMask[i] = 5; k++; }
    bot_log("drive: %d drivable node(s) within 2.5 m of a tree - vehicles keep off them", k);
}
static void tree_tick(void)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2) return;
    int n = g->w * g->h * NAV_LAYERS;
    if (g_treeFor != g->serial) {
        nav_set_tree_dist(NULL, NULL);
        if (g_treeDist) HeapFree(GetProcessHeap(), 0, g_treeDist);
        g_treeDist = HeapAlloc(GetProcessHeap(), 0, (size_t)n);
        g_treeFor = g->serial;
        if (!g_treeDist) { g_treeCursor = -1; return; }
        memset(g_treeDist, 255, (size_t)n);
        g_treeCursor = 0; g_treeCount = 0;
        char path[MAX_PATH + 64], dir[MAX_PATH];   // surveyed before (same grid size): straight from the file
        GetModuleFileNameA(NULL, dir, sizeof dir);
        *strrchr(dir, '\\') = 0;
        snprintf(path, sizeof path, "%s\\BotMod_trees_%s.bin", dir, g->level);
        FILE *f = fopen(path, "rb");
        if (f) {
            int hdr[3] = {0, 0, 0};
            if (fread(hdr, 4, 3, f) == 3 && hdr[0] == g->w && hdr[1] == g->h && hdr[2] == NAV_LAYERS && fread(g_treeDist, 1, (size_t)n, f) == (size_t)n) {
                g_treeCursor = n;   // finished below
                bot_log("trees: loaded %s", path);
            } else memset(g_treeDist, 255, (size_t)n);
            fclose(f);
        }
    }
    if (g_treeCursor < 0) return;
    int end = g_treeCursor + 400, scanned = 0;
    unsigned saved = g_rayFlags;
    for (; g_treeCursor < n && g_treeCursor < end && scanned < 200000; g_treeCursor++, scanned++) {
        int node = g_treeCursor;
        if (!(g->flags[node] & NAV_WALKABLE) || (g->flags[node] & NAV_WATER) || !g->edges[node]) { end++; continue; }   // (empty: free)
        float p[3], h[3], h2[3], nrm[3], best = 99.0f;
        nav_node_pos(g, node, p);
        for (int d = 0; d < 16; d++) {   // (16: a thin palm trunk slips between 8 rays)
            float a[3] = {p[0], p[1] + 1.5f, p[2]}, b[3] = {p[0] + sinf(d * 0.392699f) * 2.5f, p[1] + 1.5f, p[2] + cosf(d * 0.392699f) * 2.5f};
            g_rayFlags = RAY_DONT_CHECK_CHARACTER | RAY_CHECK_DETAIL_MESH;
            if (!ray_cast(a, b, h, nrm) || fabsf(nrm[1]) > 0.6f) continue;   // (a trunk is upright: its normal is level)
            float dh = sqrtf(dist2(h, a));
            if (dh >= best) continue;
            g_rayFlags = RAY_DONT_CHECK_CHARACTER;
            if (ray_cast(a, b, h2, NULL) && sqrtf(dist2(h2, a)) <= dh + 0.1f) continue;   // a wall: the nav knows it
            best = dh;
        }
        if (best < 2.6f) { g_treeDist[node] = (unsigned char)(best * 10.0f); g_treeCount++; }
    }
    g_rayFlags = saved;
    if (g_treeCursor >= n) {
        if (g_treeCount) {   // fresh survey: fill the gaps - a node is at most a neighbour's distance + the step from a trunk
            for (int pass = 0; pass < 2; pass++)
                for (int node = 0; node < n; node++) {
                    if (!(g->flags[node] & NAV_WALKABLE)) continue;
                    for (int d = 0; d < 8; d++) {
                        int nb = nav_step_node(g, node, d);
                        if (nb < 0 || g_treeDist[nb] == 255) continue;
                        int v = g_treeDist[nb] + (int)(((d & 1) ? 1.414f : 1.0f) * g->cell * 10.0f);
                        if (v <= 25 && v < g_treeDist[node]) g_treeDist[node] = (unsigned char)v;
                    }
                }
        }
        int nearCnt = 0;
        for (int i = 0; i < n; i++) if (g_treeDist[i] <= 12) nearCnt++;
        bot_log("trees: survey of %s done - %d node(s) within 2.5 m of a trunk, %d within 1.2 m (soldiers go round)", g->level, g_treeCount, nearCnt);
        if (g_treeCount || nearCnt) {   // (a loaded survey has g_treeCount 0 - only save a fresh one)
            char path[MAX_PATH + 64], dir[MAX_PATH];
            GetModuleFileNameA(NULL, dir, sizeof dir);
            *strrchr(dir, '\\') = 0;
            snprintf(path, sizeof path, "%s\\BotMod_trees_%s.bin", dir, g->level);
            FILE *f = g_treeCount ? fopen(path, "wb") : NULL;
            if (f) { int hdr[3] = {g->w, g->h, NAV_LAYERS}; fwrite(hdr, 4, 3, f); fwrite(g_treeDist, 1, (size_t)n, f); fclose(f); }
        }
        g_treeCursor = -1;
        nav_set_tree_dist(g, g_treeDist);
        tree_apply_drive();
    }
}

// Squads: every 2 s, bot squads with a far objective (farthest first) take a free light vehicle near them if at
// least half their living members (2 or more) are within SQUAD_VEH_RADIUS of it; those members board - first in
// drives - and the rest carry on on foot.
static int plan_to_vehicle(int i, char *soldier, const float *vp);
#define SQUAD_VEH_RADIUS 90.0f   // (vehicles are parked around the edge of an HQ)
// A boat only when the trip needs one (the user, 2026-10-13: a squad on Kharg took a RHIB 7 s along the coast and walked
// 400 m from there): different landmasses AND no walking route from here to there. Cached per landmass pair a minute.
static int need_boat(const float *from, const float *to)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2 || !boat_mask() || g_boatMaskFor != g->serial || !g_landLabels) return 1;
    int a = nav_label_at(g, g_landLabels, from[0], from[1], from[2]), b = nav_label_at(g, g_landLabels, to[0], to[1], to[2]);
    if (a && a == b) return 0;
    static struct { int a, b, res; unsigned serial, until; } cache[32];
    static int next;
    for (int k = 0; k < 32; k++)
        if (cache[k].serial == g->serial && cache[k].a == a && cache[k].b == b && H->frame < cache[k].until && (a || b)) return cache[k].res;
    static float pts[128 * 3];
    int n = nav_find_path(g, ray_cast, from[0], from[1], from[2], to[0], to[1], to[2], pts, 128);
    int res = n <= 0, wet = 0;
    for (int k = 0; k + 1 < n && !wet; k++) {   // a route that swims is no walking route (Wake's US island: every squad swam, 2026-10-13)
        float *a = &pts[k * 3], *b = &pts[(k + 1) * 3], len = sqrtf(dist2(a, b));
        int steps = (int)(len / 3.0f) + 1;
        for (int t = 0; t <= steps && !wet; t++) {
            float u = (float)t / steps;
            if (nav_is_water(g, a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u)) wet = 1;
        }
    }
    if (wet) res = 1;
    int k = next++ % 32;
    cache[k].a = a; cache[k].b = b; cache[k].res = res; cache[k].serial = g->serial; cache[k].until = H->frame + 60 * 30;
    bot_log("boat: (%.0f, %.0f) -> (%.0f, %.0f): landmass %d -> %d, %s", from[0], from[2], to[0], to[2], a, b, res ? (wet ? "the walking route swims - a boat helps" : "no walking route - a boat helps") : "walkable - no boat");
    return res;
}
static void squad_vehicle_tick(void)
{
    if (g_driveOff || H->frame % 60 || !g_flagCount) return;
    for (int team = 1; team <= 2; team++) {
        unsigned mask = bot_squads_mask(team);
        int sq[32], ns = 0;
        float sqD[32];
        for (int s = 1; s <= 31; s++) {
            if (!(mask & (1u << s))) continue;
            int f = squad_objective(team, s);
            if (f < 0) continue;
            float cen[3];
            squad_centre(team, s, cen);
            float d = dist2(cen, flag_pos(f));
            if (d < DRIVE_MIN_TRIP * DRIVE_MIN_TRIP) continue;
            int k = ns++;   // farthest objective first
            while (k > 0 && sqD[k - 1] < d) { sq[k] = sq[k - 1]; sqD[k] = sqD[k - 1]; k--; }
            sq[k] = s; sqD[k] = d;
        }
        for (int q = 0; q < ns; q++) {
            int s = sq[q], f = squad_objective(team, s), mem[16], nm = 0, living = 0;
            for (int j = 0; j < H->botCount && nm < 16; j++) {
                char *pl = H->bots[j], *js = *(char **)(pl + OFF_PLAYER_SOLDIER);
                if (player_team(pl) != team || player_squad(pl) != s || !js) continue;
                living++;
                BotBrain *b = bot_brain(j);
                if (!b->enabled || g_drive[j].phase || player_vehicle(pl) || b->task != TASK_NONE) continue;
                if (is_pilot(j) && pilot_has_rides(j)) continue;   // a Pilot waits for armour / a jet, not the squad's jeep
                mem[nm++] = j;
            }
            if (nm < 1) continue;
            int sameLand = -1;   // already on the flag's island: no boats (worked out when a boat comes up)
            char **vehs, *best = NULL;
            int nv = vehicle_list(&vehs), bestNear = 0;
            for (int k = 0; k < nv; k++) {
                char *v = vehs[k];
                if (!vehicle_ok(v) || !vehicle_is_light(v) || !driver_seat_free(v) || vehicle_bad(v)) continue;
                if (vehicle_is_boat(v)) {
                    if (sameLand < 0) {   // the landmass table must be this grid's (boat_mask() rebuilds it for a new map:
                        NavGrid *g = *nav_slot();   // an old map's table read with this grid's size ran off its end)
                        float cen[3];
                        sameLand = 0;
                        squad_centre(team, s, cen);
                        if (g && g->phase == 2) sameLand = !need_boat(cen, flag_pos(f));
                    }
                    if (sameLand) continue;
                }
                int vt = 0, other = 3 - team, nNear = 0, taken = 0;
                float *vp = vehicle_pos(v, &vt);
                if (!vp || (vt && vt != team)) continue;   // (players near it do not matter: by the time bots spawn, theirs are long gone)
                if (g_hqPos[team][0] != 0 && g_hqPos[other][0] != 0 && dist2(vp, g_hqPos[team]) > dist2(vp, g_hqPos[other])) continue;
                for (int j = 0; j < H->botCount && !taken; j++) if (g_drive[j].phase && g_drive[j].veh == v) taken = 1;
                if (taken) continue;
                for (int m = 0; m < nm; m++) {
                    char *js = *(char **)(H->bots[mem[m]] + OFF_PLAYER_SOLDIER);
                    if (dist2((float *)(js + OFF_SOLDIER_POSITION), vp) < SQUAD_VEH_RADIUS * SQUAD_VEH_RADIUS) nNear++;
                }
                if (nNear > bestNear) { bestNear = nNear; best = v; }
            }
            if (!best || bestNear < (living >= 2 ? 2 : 1) || bestNear * 2 < living) continue;
            float *vp = vehicle_pos(best, NULL);
            int seats = free_seat_count(best), boarding = 0;
            for (int m = 0; m < nm && boarding < seats; m++) {
                int j = mem[m];
                char *js = *(char **)(H->bots[j] + OFF_PLAYER_SOLDIER);
                if (dist2((float *)(js + OFF_SOLDIER_POSITION), vp) >= SQUAD_VEH_RADIUS * SQUAD_VEH_RADIUS) continue;   // too far: left behind
                if (!plan_to_vehicle(j, js, vp)) continue;
                DriveState *d = &g_drive[j];
                memset(d, 0, sizeof *d);
                d->phase = 5; d->veh = best; d->at = H->frame; d->flag = f;
                memcpy(d->start, vp, 12);
                memcpy(d->goal, flag_pos(f), 12);
                bot_nav(j)->mode = 3;
                boarding++;
            }
            if (boarding) bot_log("squad: %s squad %d boards the %s for flag %d (%d of %d, %.0f m to go)", team == 1 ? "US" : "RU", s,
                                  vehicle_name(best), f, boarding, living, sqrtf(sqD[q]));
        }
    }
}

// Walk up to a vehicle: its own spot, else the nearest reachable point a few metres from it (a docked boat lies in
// the water beside the pier / beach). Returns the path's point count (0 = no way there).
static int plan_to_vehicle(int i, char *soldier, const float *vp)
{
    if (!plan_budget_left()) return 0;
    int n = plan_path(i, soldier, vp[0], vp[1], vp[2]);
    for (float r = 3.0f; r <= 9.0f && !n; r += 3.0f)
        for (int a = 0; a < 4 && !n; a++) {
            if (!plan_budget_left()) return 0;
            float ang = a * 1.5707963f + r * 0.26f;
            n = plan_path(i, soldier, vp[0] + sinf(ang) * r, vp[1] + 1.0f, vp[2] + cosf(ang) * r);
        }
    return n;
}

// ---- Pilots (2026-10-13): a Pilot on foot takes a free tank / IFV / jet of its own side near it, walks (not runs)
// to it and gets in; in a tank it then drives flag to flag (pilot_hold_tick), in a jet it takes off and flies the
// flag circuit. With rides on the map but none free it waits at the HQ; on a map without any it plays infantry. ----
static int vehicle_kind(char *veh)
{
    static const char *const jets[] = {"F18", "FA18", "F-18", "F16", "Su35", "Su-35", "A10", "A-10", "Su25", "Su-25", "F35", "F-35", "Su37", NULL};   // (case-insensitive: Caspian's US jet is "f16")
    static const char *const helis[] = {"AH1Z", "AH-1Z", "Mi28", "Mi-28", "Z11", "Z-11", "AH6", "AH-6", "MH6", "MH-6", "Littlebird", "Little Bird", "Ka60",
                                        "Ka-60", "UH1Y", "UH-1Y", "Venom", "Viper", "Havoc", NULL};   // (a Pilot took an "AH-6 Littlebird" as armour)
    static const char *const tanks[] = {"M1A2", "Abrams", "T90", "T-90", NULL};
    const char *n0 = vehicle_name(veh);
    if (!n0 || !n0[0]) return VK_OTHER;   // (no name: Wake Island's bulldozer - it counted as armour, Pilots drove it, 2026-10-13)
    char n[64], lk[16];
    int q = 0;
    for (; n0[q] && q < 63; q++) n[q] = (char)toupper((unsigned char)n0[q]);
    n[q] = 0;
    #define VK_HAS(list) ({ int hit = 0; for (int k = 0; list[k] && !hit; k++) { int z = 0; for (; list[k][z] && z < 15; z++) lk[z] = (char)toupper((unsigned char)list[k][z]); lk[z] = 0; if (strstr(n, lk)) hit = 1; } hit; })
    static const char *const fixed[] = {"TOW", "Kornet", "Centurion", "CRAM", "Phalanx", "Pantsir", "Stationary", "Emplacement", NULL};   // (Caspian: Pantsir-S1 / CRAM are trailer AA guns)   // static guns: no ride
    if (VK_HAS(fixed)) return VK_OTHER;   // (a Pilot took a TOW launcher on Caspian and tried to drive it)
    if (VK_HAS(jets)) return VK_JET;
    if (VK_HAS(helis)) return VK_HELI;
    if (VK_HAS(tanks)) return VK_TANK;
    #undef VK_HAS
    if (vehicle_is_boat(veh)) return VK_BOAT;
    if (vehicle_is_light(veh)) return VK_LIGHT;
    return VK_IFV;   // LAV / BMP / M1128 / SPRUT / AA tracks ... armour that is not a main battle tank
}
static int vehicle_ours_side(char *v, int team)   // its team, or a neutral one on our half of the map (by the HQs)
{
    int vt = 0, other = 3 - team;
    float *vp = vehicle_pos(v, &vt);
    if (!vp || (vt && vt != team)) return 0;
    if (g_hqPos[team][0] != 0 && g_hqPos[other][0] != 0 && dist2(vp, g_hqPos[team]) > dist2(vp, g_hqPos[other])) return 0;
    return 1;
}
// Whose vehicle it is, by name (a parked one has no team: RU pilots flew off in US F-35s, US pilots in Su-35s, 2026-10-13)
static int vehicle_nation(char *v)
{
    static const char *const us[] = {"F35", "F18", "FA18", "F16", "A10", "AH1Z", "Viper", "UH1Y", "Venom", "AH6", "MH6", "LittleBird", "M1A2",
                                     "Abrams", "M1128", "LAV", "AAV", "Growler", "HMMWV", "Humvee", "DPV", "M142", "Centurion", "CRAM", "RHIB", NULL};
    static const char *const ru[] = {"Su35", "Su-35", "Su25", "Su-25", "Su37", "Mi28", "Havoc", "Ka60", "KA60", "Z11", "Z-11", "T90", "T-90", "SPRUT",
                                     "BMP", "BTR", "9K22", "Tunguska", "Pantsir", "VDV", "GAZ", "Vodnik", "BM21", "BM-21", "Kornet", NULL};
    const char *n0 = vehicle_name(v);
    if (!n0) return 0;
    char n[64];
    int q = 0;
    for (; n0[q] && q < 63; q++) n[q] = (char)toupper((unsigned char)n0[q]);
    n[q] = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *const *L = pass ? ru : us;
        for (int k = 0; L[k]; k++) {
            char u[16]; int z = 0;
            for (; L[k][z] && z < 15; z++) u[z] = (char)toupper((unsigned char)L[k][z]);
            u[z] = 0;
            if (strstr(n, u)) return pass ? 2 : 1;
        }
    }
    return 0;
}
static int pilot_ride_kind(int k) { return k == VK_TANK || k == VK_IFV || k == VK_JET; }
static int vehicle_is_mobile_aa(char *v)   // LAV-AD / 9K22 Tunguska-M / Vodnik AA: a Pilot's last pick before a taxi (the user)
{
    const char *n0 = vehicle_name(v);
    char n[64];
    int q = 0;
    if (!n0) return 0;
    for (; n0[q] && q < 63; q++) n[q] = (char)toupper((unsigned char)n0[q]);
    n[q] = 0;
    return strstr(n, "LAV-AD") || strstr(n, "LAVAD") || strstr(n, "LAV_AD") || strstr(n, "9K22") || strstr(n, "TUNGUSKA") ||
           (strstr(n, "VODNIK") && strstr(n, "AA"));
}
static int pilot_ride_any(int k) { return pilot_ride_kind(k) || k == VK_LIGHT || k == VK_BOAT; }   // + a jeep / boat: a taxi for the infantry
static int pilot_has_rides(int i)
{
    if (!is_pilot(i)) return 0;
    int team = player_team(H->bots[i]);
    char **vehs;
    int nv = vehicle_list(&vehs);
    for (int k = 0; k < nv; k++)
        if (vehicle_ok(vehs[k]) && pilot_ride_any(vehicle_kind(vehs[k])) && pilot_ride_ours(vehs[k], team)) return 1;
    return 0;
}
#define PILOT_PICKUP_RADIUS 150.0f
// A jet nobody has taken for 60 s: the next Pilot needing a ride walks to it first, however far (the user, 2026-10-13)
static char *g_jetIdle[32];
static unsigned g_jetIdleSince[32];
static unsigned jet_idle_frames(char *v)
{
    int slot = -1;
    for (int k = 0; k < 32; k++) if (g_jetIdle[k] == v) { slot = k; break; }
    if (slot < 0) {
        for (int k = 0; k < 32; k++) if (!g_jetIdle[k] || !vehicle_ok(g_jetIdle[k]) || !driver_seat_free(g_jetIdle[k])) { slot = k; break; }
        if (slot < 0) return 0;
        g_jetIdle[slot] = v; g_jetIdleSince[slot] = H->frame;
    }
    return H->frame - g_jetIdleSince[slot];
}
// Last time a jet of each side was seen in the air (anyone's: a human's counts) - with none up for 60 s a Pilot that is
// due to respawn spawns straight into a jet (the user, 2026-10-13: carrier maps never had bot jets up). 0 = never.
static unsigned g_jetUpAt[3], g_jetUpLevelSerial;
static void jet_idle_tick(void)   // free jets keep their clock; a taken / destroyed one is forgotten
{
    if (H->frame % 30) return;
    {
        NavGrid *gg = *nav_slot();
        unsigned ser = gg ? gg->serial : 0;
        if (ser != g_jetUpLevelSerial) { g_jetUpLevelSerial = ser; g_jetUpAt[1] = g_jetUpAt[2] = H->frame - 61 * 30; }   // (a new map: none up yet)
        char **vs;
        int nv = vehicle_list(&vs);
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER;
        for (int k = 0; k < nv; k++) {
            char *v = vs[k];
            if (!vehicle_ok(v) || vehicle_kind(v) != VK_JET || driver_seat_free(v)) continue;
            int vt = 0;
            float *vp = vehicle_pos(v, &vt), h[3];
            if (!vp) continue;
            if (vt != 1 && vt != 2) { const char *vn = vehicle_name(v); vt = vn && (strstr(vn, "Su") || strstr(vn, "SU")) ? 2 : 1; }
            float a[3] = {vp[0], vp[1] - 2.0f, vp[2]}, b[3] = {vp[0], vp[1] - 20.0f, vp[2]};
            if (!ray_cast(a, b, h, NULL)) g_jetUpAt[vt] = H->frame;   // nothing within 20 m below: flying
        }
        g_rayFlags = saved;
    }
    for (int k = 0; k < 32; k++)
        if (g_jetIdle[k] && (!vehicle_ok(g_jetIdle[k]) || !driver_seat_free(g_jetIdle[k]))) g_jetIdle[k] = NULL;
    char **vs;
    int nv = vehicle_list(&vs);
    for (int k = 0; k < nv; k++) if (vehicle_ok(vs[k]) && vehicle_kind(vs[k]) == VK_JET && driver_seat_free(vs[k])) jet_idle_frames(vs[k]);
}
static unsigned char g_pilotWaiting[MAX_BOTS];
static char *g_pilotRide[MAX_BOTS];      // the vehicle a Pilot on foot is heading for
static unsigned g_pilotRideAt[MAX_BOTS];
static float g_pilotRideBest[MAX_BOTS];      // nearest it has got to the ride (m)
static char *g_rideNo[MAX_BOTS][4];          // rides it found no walking route to (2 min each)
static unsigned g_rideNoUntil[MAX_BOTS][4];
// Where to walk to for a vehicle: the grid was built with the vehicles parked, so the ground under one is no walkable
// node - a route "to the vehicle" failed even 68 m off (2026-10-13). A walkable node 3-9 m from it instead.
static void ride_approach(const float *vp, float *out)
{
    NavGrid *g = *nav_slot();
    memcpy(out, vp, 12);
    if (!g || g->phase != 2) return;
    float t[3];
    // the floor under it first: a jet's origin sits ~2 m up, and snapping at that height found isolated nodes on top of
    // something in Wake Island's hangar (no route - the RU pilots took a buggy / the bulldozer instead, 2026-10-13)
    for (float r = 3.0f; r <= 9.0f; r += 2.0f)
        for (int a = 0; a < 8; a++) {
            float ang = a * 0.785398f;
            if (nav_snap(g, vp[0] + sinf(ang) * r, vp[1] - 2.0f, vp[2] + cosf(ang) * r, t) && t[1] < vp[1] + 0.5f && t[1] > vp[1] - 4.0f) { memcpy(out, t, 12); return; }
        }
    for (float r = 3.0f; r <= 9.0f; r += 2.0f)
        for (int a = 0; a < 8; a++) {
            float ang = a * 0.785398f;
            if (nav_snap(g, vp[0] + sinf(ang) * r, vp[1] + 1.0f, vp[2] + cosf(ang) * r, t) && fabsf(t[1] - vp[1]) < 3.0f) { memcpy(out, t, 12); return; }
        }
}
// A jet: board from beside the cockpit (forward of its origin, either side) - Pilot RU 4 stood 7 m behind an Su-35's tail
// and never got in (the user, 2026-10-13). Other vehicles: the ring round the origin.
static void ride_approach_v(char *veh, const float *vp, float *out)
{
    NavGrid *g = *nav_slot();
    float *xf = veh && vehicle_kind(veh) == VK_JET ? vehicle_xf(veh) : NULL;
    if (g && g->phase == 2 && xf) {
        float fx = xf[8], fz = xf[10], fl = sqrtf(fx * fx + fz * fz);
        if (fl > 0.1f) {
            fx /= fl; fz /= fl;
            static const float fwd[3] = {2.5f, 1.0f, 4.0f}, side[3] = {3.5f, 4.5f, 5.5f};
            float t[3];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++)
                    for (int sg = -1; sg <= 1; sg += 2) {
                        float x = vp[0] + fx * fwd[a] + fz * side[b] * sg, z = vp[2] + fz * fwd[a] - fx * side[b] * sg;
                        if (nav_snap(g, x, vp[1] - 2.0f, z, t) && t[1] < vp[1] + 0.5f && t[1] > vp[1] - 4.0f &&
                            (t[0] - x) * (t[0] - x) + (t[2] - z) * (t[2] - z) < 1.5f * 1.5f) { memcpy(out, t, 12); return; }
                    }
        }
    }
    ride_approach(vp, out);
}
static int ride_unreachable(int i, char *v)
{
    for (int k = 0; k < 4; k++) if (g_rideNo[i][k] == v && H->frame < g_rideNoUntil[i][k]) return 1;
    return 0;
}
static unsigned g_pilotRideProgAt[MAX_BOTS]; // when it last got 5 m nearer
// Ours: built by our side (by name), and not the other side's in use; a vehicle of unknown make - on our half of the map.
// (By the HQs alone, tanks waiting at a taken flag on the enemy's half never counted.)
static int pilot_heading_for_ride(int i) { return g_pilotRide[i] != NULL; }
static void pilot_status_tick(void)   // every 30 s: what each Pilot is doing (the user: track them all round)
{
    if (H->frame % (30 * 30)) return;
    for (int t = 1; t <= 2; t++) {
        char line[900];
        int n = snprintf(line, sizeof line, "pilots %s:", t == 1 ? "US" : "RU"), any = 0;
        for (int i = 0; i < H->botCount && n < (int)sizeof line - 80; i++) {
            char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
            if (!is_pilot(i) || player_team(pl) != t) continue;
            any = 1;
            const char *nm = pl + OFF_PLAYER_ONLINE_NAME;
            const char *shortn = strstr(nm, "Pilot") ? strstr(nm, "Pilot") + 6 : nm;
            if (!s) n += snprintf(line + n, sizeof line - n, " [%s dead]", shortn);
            else if (player_vehicle(pl)) n += snprintf(line + n, sizeof line - n, " [%s in %s%s]", shortn, vehicle_name(player_vehicle(pl)), g_flying[i] ? " flying" : "");
            else if (g_pilotRide[i]) {
                float *rp = vehicle_pos(g_pilotRide[i], NULL);
                n += snprintf(line + n, sizeof line - n, " [%s -> %s %.0f m%s]", shortn, vehicle_name(g_pilotRide[i]),
                              rp ? sqrtf(dist2(rp, (float *)(s + OFF_SOLDIER_POSITION))) : -1.0f, bot_brain(i)->task == TASK_FIGHT ? " fighting" : "");
            }
            else n += snprintf(line + n, sizeof line - n, " [%s %s]", shortn, g_pilotWaiting[i] ? "waiting at HQ" : "infantry");
        }
        if (any) bot_log("%s", line);
    }
}
static int pilot_ride_ours(char *v, int team)
{
    int vt = 0, nat = vehicle_nation(v);
    vehicle_pos(v, &vt);
    if (vt && vt != team) return 0;
    if (nat) return nat == team;
    return vehicle_ours_side(v, team);
}
static void pilot_pickup_tick(int i)
{
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!s || player_vehicle(pl) || !is_pilot(i)) g_pilotWaiting[i] = 0;
    if (s && is_pilot(i) && player_vehicle(pl) && !g_drive[i].phase && !g_flying[i] && g_driving[i] >= 0) {   // at the controls with no plan
        char *v = player_vehicle(pl);                                                                          // (a core reload): carry on
        char **b;
        int nat = vehicle_nation(v);
        if (nat && nat != player_team(pl)) {   // sitting in the other side's vehicle: respawn properly
            bot_log("pilot: %s is in the other side's %s - killed to respawn", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(v));
            kill_bot(pl);
            return;
        }
        if (!pilot_ride_any(vehicle_kind(v))) {   // a helicopter / gun it cannot work: out, on foot (one sat in an AH-6)
            bot_log("pilot: %s gets out of the %s (not a ride it can drive / fly)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(v));
            g_pilotForceExit = 1; bot_exit_vehicle(i, 1); g_pilotForceExit = 0;
            return;
        }
        if (vehicle_seats(v, &b) && (*(char **)(b[0] + 0x4c) == s || *(char **)(b[0] + 0x4c) == pl)) {
            if (vehicle_kind(v) == VK_JET) {
                g_driving[i] = 1; g_flying[i] = 1; g_flyAuto[i] = 1; g_flyCircuit[i] = 1; g_flyOrbit[i] = -1; g_flyChase[i] = NULL; g_flyFlag[i] = -1;
                VehTrack *vt0 = vehicle_sample(v);   // parked (an HQ jet spawn on Caspian): take-off run first
                float sp0 = vt0 ? vt0->speed : 0;
                if (sp0 < 40.0f) { g_flyTakeoff[i] = 1; g_flyTakeoffAlt[i] = 0; }
            } else { g_driving[i] = 1; g_drive[i].phase = 8; g_drive[i].veh = v; g_drive[i].at = H->frame; }
            bot_log("pilot: %s carries on in its %s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(v));
        }
        return;
    }
    // ---- on foot (2026-10-13, the user): a ride anywhere on the map - the nearest free one of ours (a tank at a taken
    // flag too); the brain walks there as a "post" task (TASK_DEFEND), so it fights on the way like any bot and goes
    // back to it after; sprints when over 100 m off, plain run under that; gets in from 7 m. ----
    if (!s || !is_pilot(i) || player_vehicle(pl) || g_drive[i].phase || g_flying[i] || !bot_brain(i)->enabled) { if (!s) g_pilotRide[i] = NULL; return; }
    if (g_intoJet[i] && H->frame > g_intoJet[i]) { g_intoJet[i] = 0; bot_log("pilot: %s - no free jet of ours to go straight into (15 s)", pl + OFF_PLAYER_ONLINE_NAME); }
    if (g_intoJet[i]) {   // spawned to fly: the nearest free jet of ours, straight into the seat (waits here for one, 15 s)
        if ((H->frame + i) % 15) return;
        char **vs, *jet = NULL;
        int nv = vehicle_list(&vs), tm = player_team(pl);
        float jd = 1e30f, *mp = (float *)(s + OFF_SOLDIER_POSITION);
        for (int k = 0; k < nv; k++) {
            char *v = vs[k];
            if (!vehicle_ok(v) || vehicle_kind(v) != VK_JET || !driver_seat_free(v) || !pilot_ride_ours(v, tm)) continue;
            float *vp = vehicle_pos(v, NULL), q = vp ? dist2(vp, mp) : 1e30f;
            if (q < jd) { jd = q; jet = v; }
        }
        if (jet) {
            g_intoJet[i] = 0;
            enter_vehicle(pl, jet, 0);
            if (player_vehicle(pl) == jet) {
                BotBrain *bb = bot_brain(i);
                bb->task = TASK_NONE; bb->crouched = 0;
                g_pilotRide[i] = NULL; bot_nav(i)->mode = 0;
                H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
                g_driving[i] = 1; g_flying[i] = 1; g_flyAuto[i] = 1; g_flyCircuit[i] = 1; g_flyOrbit[i] = -1; g_flyChase[i] = NULL; g_flySeek[i] = 0;
                g_flyFlag[i] = -1; g_flyLogAt[i] = 0; g_flyAglAt[i] = 0; g_flyTakeoff[i] = 1; g_flyTakeoffAlt[i] = 0;
                bot_log("pilot: %s is put straight into the %s (%.0f m away) - take-off", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(jet), sqrtf(jd));
                return;
            }
            bot_log("pilot: %s could not be put into the %s (%.0f m away)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(jet), sqrtf(jd));
        }
        else return;   // (no jet yet: wait here, not off to another ride)
    }
    if (!pilot_has_rides(i)) { g_pilotRide[i] = NULL; g_pilotWaiting[i] = 0; return; }   // none on this map for us: infantry
    BotBrain *b = bot_brain(i);
    int team = player_team(pl);
    float *me = (float *)(s + OFF_SOLDIER_POSITION);
    char *r = g_pilotRide[i];
    if (r) {   // on its way: still free and ours?
        float *rp = vehicle_ok(r) ? vehicle_pos(r, NULL) : NULL;
        if (!rp || !driver_seat_free(r) || H->frame - g_pilotRideAt[i] > 240 * 30) {
            bot_log("pilot: %s gives up on the %s (%s)", pl + OFF_PLAYER_ONLINE_NAME, rp ? vehicle_name(r) : "vehicle", !rp ? "gone" : !driver_seat_free(r) ? "taken" : "4 min");
            if (!rp) vehicle_mark_bad(r, 60);
            else if (H->frame - g_pilotRideAt[i] > 240 * 30) {   // 4 min: this Pilot's problem - its own list only
                int sl = 0;
                for (int k = 1; k < 4; k++) if (g_rideNoUntil[i][k] < g_rideNoUntil[i][sl]) sl = k;
                g_rideNo[i][sl] = r; g_rideNoUntil[i][sl] = H->frame + 120 * 30;
            }
            g_pilotRide[i] = NULL;
            if (b->task == TASK_DEFEND) { b->task = TASK_NONE; b->enroute = b->waiting = 0; b->crouched = 0; }
        } else {
            float d2 = dist2(rp, me), dd = sqrtf(d2);
            if (g_straightAt[i] && H->frame - g_straightAt[i] < 15 && b->task == TASK_DEFEND && dd > 40.0f) {   // no walking route to it (a jet past
                int s = 0;                                                                       // a wall: Pilot RU 3 face-planted it)
                for (int k = 1; k < 4; k++) if (g_rideNoUntil[i][k] < g_rideNoUntil[i][s]) s = k;
                g_rideNo[i][s] = r; g_rideNoUntil[i][s] = H->frame + 120 * 30;
                bot_log("pilot: %s has no way on foot to the %s (%.0f m) - another ride", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(r), dd);
                g_pilotRide[i] = NULL; g_straightAt[i] = 0;
                b->task = TASK_NONE; b->enroute = b->waiting = 0; b->crouched = 0;
                bot_nav(i)->mode = 1;
                return;
            }
            if (dd < g_pilotRideBest[i] - 5.0f) { g_pilotRideBest[i] = dd; g_pilotRideProgAt[i] = H->frame; }
            else if (b->task != TASK_FIGHT && H->frame - g_pilotRideProgAt[i] > 30 * 30) {   // no nearer in 30 s (US 4 stood 167 m
                bot_log("pilot: %s cannot get to the %s (%.0f m, no nearer in 30 s) - another ride", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(r), dd);
                {   // (this Pilot's way there, not the tank: off its own list only - marking the tank bad for everyone kept
                    //  Pilot RU 2 off the T90s beside its spawn, it took a buggy, 2026-10-13)
                    int sl = 0;                                        // from an M1A2 up a hill, and its claim kept the other pilots off it)
                    for (int k = 1; k < 4; k++) if (g_rideNoUntil[i][k] < g_rideNoUntil[i][sl]) sl = k;
                    g_rideNo[i][sl] = r; g_rideNoUntil[i][sl] = H->frame + 120 * 30;
                }
                g_pilotRide[i] = NULL;
                if (b->task == TASK_DEFEND) { b->task = TASK_NONE; b->enroute = b->waiting = 0; b->crouched = 0; }
                return;
            }
            float hx = rp[0] - me[0], hz = rp[2] - me[2], px = b->postPos[0] - me[0], pz = b->postPos[2] - me[2];
            int there = d2 < 7.0f * 7.0f || hx * hx + hz * hz < 6.5f * 6.5f || px * px + pz * pz < 2.5f * 2.5f;   // (flat: a jet's origin is 2 m up)
            static unsigned thereSince[MAX_BOTS];
            if (!there || g_pilotRide[i] != r) thereSince[i] = 0;
            else if (!thereSince[i]) thereSince[i] = H->frame;
            else if (H->frame - thereSince[i] > 8 * 30 && b->task != TASK_FIGHT) {   // at it 8 s and still not in: another ride (Pilot US 2 knelt
                int sl = 0;                                                       //  3 min by an M1A2 on Seine, 2026-10-13)
                for (int k = 1; k < 4; k++) if (g_rideNoUntil[i][k] < g_rideNoUntil[i][sl]) sl = k;
                g_rideNo[i][sl] = r; g_rideNoUntil[i][sl] = H->frame + 120 * 30;
                bot_log("pilot: %s cannot get into the %s (%.1f m flat from it, %.1f m from its spot, %.1f m up) - another ride", pl + OFF_PLAYER_ONLINE_NAME,
                        vehicle_name(r), sqrtf(hx * hx + hz * hz), sqrtf(px * px + pz * pz), rp[1] - me[1]);
                thereSince[i] = 0;
                g_pilotRide[i] = NULL;
                b->task = TASK_NONE; b->enroute = b->waiting = 0; b->crouched = 0;
                return;
            }
            if (there && b->task != TASK_FIGHT && vehicle_board_safe(r)) {   // there: in
                enter_vehicle(pl, r, 0);
                if (player_vehicle(pl) == r) {
                    if (b->task == TASK_DEFEND) b->task = TASK_NONE;
                    b->crouched = 0;
                    g_pilotRide[i] = NULL;
                    bot_nav(i)->mode = 0;
                    H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
                    if (vehicle_kind(r) == VK_JET) {
                        g_driving[i] = 1; g_flying[i] = 1; g_flyAuto[i] = 1; g_flyCircuit[i] = 1; g_flyOrbit[i] = -1; g_flyChase[i] = NULL; g_flySeek[i] = 0;
                        g_flyFlag[i] = -1; g_flyLogAt[i] = 0; g_flyAglAt[i] = 0; g_flyTakeoff[i] = 1; g_flyTakeoffAlt[i] = 0;
                        bot_log("pilot: %s is in the %s - take-off", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(r));
                    } else {
                        DriveState *d = &g_drive[i];
                        memset(d, 0, sizeof *d);
                        g_driving[i] = 1; d->phase = 8; d->veh = r; d->at = H->frame + (unsigned)(i % 6) * 75;
                        bot_log("pilot: %s takes the %s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(r));
                    }
                    return;
                }
            }
            if (b->task == TASK_NONE || b->task == TASK_DEFEND) {   // keep the post on the vehicle (it may roll / be pushed)
                if (b->task != TASK_DEFEND || dist2(b->postPos, rp) > 12.0f * 12.0f) { ride_approach_v(r, rp, b->postPos); b->replanAt = 0; }   // (it moved)
                b->task = TASK_DEFEND; b->taskUntil = NEVER; b->postFacing = 0; b->crouched = 0; b->stepAt = 0;
            }
            g_pilotWaiting[i] = 0;
            return;
        }
    }
    if ((H->frame + i * 7) % 30) return;   // (waiting keeps its last value between looks)
    char **vehs, *best = NULL;
    int nv = vehicle_list(&vehs);
    float bestD = 1e30f, lightD = 150.0f * 150.0f;
    int idleJet = 0;
    char *light = NULL;   // a jeep / boat near it: the taxi, when no armour / jet is free (the user, 2026-10-13)
    char *aa = NULL;      // a mobile AA truck: only when no tank / IFV / jet is free - before a taxi (the user, 2026-10-13)
    float aaD = 1e30f;
    for (int k = 0; k < nv; k++) {
        char *v = vehs[k];
        int isAA = vehicle_ok(v) && vehicle_is_mobile_aa(v);
        if (!vehicle_ok(v) || (!pilot_ride_any(vehicle_kind(v)) && !isAA) || !driver_seat_free(v) || vehicle_bad(v) || !pilot_ride_ours(v, team)) continue;
        if (ride_unreachable(i, v)) continue;
        if (isAA) {
            int tk = 0;
            for (int j = 0; j < H->botCount && !tk; j++) if (j != i && ((g_drive[j].phase && g_drive[j].veh == v) || g_pilotRide[j] == v)) tk = 1;
            float *ap = vehicle_pos(v, NULL);
            float ad = ap && !tk ? dist2(ap, me) : 1e30f;
            if (ad < aaD) { aaD = ad; aa = v; }
            continue;
        }
        if (vehicle_is_boat(v)) {   // a boat: only if the nearest flag not ours is across the water
            int nf = -1; float fd = 1e30f;
            for (int f = 0; f < g_flagCount; f++) if (flag_owner(f) != team) { float q = dist2(flag_pos(f), me); if (q < fd) { fd = q; nf = f; } }
            if (nf < 0 || !need_boat(me, flag_pos(nf))) continue;
        }
        if (!pilot_ride_kind(vehicle_kind(v))) {
            int tk = 0;
            for (int j = 0; j < H->botCount && !tk; j++) if (j != i && ((g_drive[j].phase && g_drive[j].veh == v) || g_pilotRide[j] == v)) tk = 1;
            float *lp = vehicle_pos(v, NULL);
            float ld = lp && !tk ? dist2(lp, me) : 1e30f;
            if (ld < lightD) { lightD = ld; light = v; }
            continue;
        }
        int idle = vehicle_kind(v) == VK_JET && jet_idle_frames(v) > 60 * 30;   // an idle jet first, at any distance
        if (idleJet && !idle) continue;
        int taken = 0;
        for (int j = 0; j < H->botCount && !taken; j++) if (j != i && ((g_drive[j].phase && g_drive[j].veh == v) || g_pilotRide[j] == v)) taken = 1;
        if (taken) continue;
        float *vp = vehicle_pos(v, NULL);
        float d = vp ? dist2(vp, me) : 1e30f;
        if (idle && !idleJet) { idleJet = 1; bestD = 1e30f; }
        if (d < bestD) { bestD = d; best = v; }
    }
    if (!best && aa) { best = aa; bestD = aaD; }
    if (!best && light) { best = light; bestD = lightD; }
    if (!best) {   // all taken: fight on as infantry (the brain's flag loop) until one is free - never idle at the HQ
        g_pilotWaiting[i] = 0;   // (four pilots stood waiting at the US HQ on Damavand, 2026-10-13)
        return;
    }
    g_pilotWaiting[i] = 0;
    g_pilotRide[i] = best; g_pilotRideAt[i] = H->frame; g_pilotRideBest[i] = sqrtf(bestD); g_pilotRideProgAt[i] = H->frame;
    float *vp = vehicle_pos(best, NULL);
    ride_approach_v(best, vp, b->postPos);
    b->task = TASK_DEFEND; b->taskUntil = NEVER; b->postFacing = 0; b->crouched = 0; b->stepAt = 0; b->replanAt = 0;
    b->enroute = b->waiting = 0;
    bot_log("pilot: %s heads for the %s (%.0f m%s)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(best), sqrtf(bestD), idleJet ? ", nobody has taken it for over a minute" : "");
}

// Jet spawners: every CharacterSpawnEntity (vtable 025bbbc0, spawn slot 0x6c = 00ddbbd0) in the entity heap, found
// once a level (read-only scan of this process). On Air Superiority each holds a jet in the air.
#define CSE_VTABLE 0x025bbbc0u
static void *g_cse[64];
static int g_cseCount;
static char g_cseLevel[64];
static void cse_scan(void)
{
    g_cseCount = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (unsigned char *a = (unsigned char *)0xf0000000u; (unsigned)a < 0xf8000000u && g_cseCount < 64; a = (unsigned char *)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE) continue;
        unsigned *p = (unsigned *)mbi.BaseAddress, *e = (unsigned *)((char *)mbi.BaseAddress + mbi.RegionSize);
        for (; p < e && g_cseCount < 64; p++)
            if (*p == CSE_VTABLE && *(unsigned *)(CSE_VTABLE + 0x6c) == ADDR_CSE_SPAWN) g_cse[g_cseCount++] = p;
    }
    strncpy(g_cseLevel, g_level, sizeof g_cseLevel - 1);
    bot_log("air: %d spawn entities on %s", g_cseCount, g_level);
}
static int air_spawn(int i)
{
    char *bot = H->bots[i];
    int team = player_team(bot);
    if (strcmp(g_cseLevel, g_level) || !g_cseCount) cse_scan();
    typedef void *(__attribute__((thiscall)) *Get_t)(void *);
    typedef char (__attribute__((thiscall)) *ChkP_t)(void *, void *);
    static int rr;
    for (int n = 0; n < g_cseCount; n++) {
        void *e = g_cse[(rr + n) % g_cseCount];
        if (IsBadReadPtr(e, 0x100) || *(unsigned *)e != CSE_VTABLE) { g_cseLevel[0] = 0; continue; }   // gone: rescan next time
        int st = spawn_team(e), gt = cse_group_team(e);
        if ((st && st != team) || (gt && gt != team)) continue;   // the other side's jets
        char *veh = ((Get_t)0x00dd7140)(e);
        if (!veh) continue;   // (before a spawn the spawner's vehicle is not a live vehicle object yet: the game's own checks only)
        if (g_airSpawnJetsOnly && (!vehicle_ok(veh) || vehicle_kind(veh) != VK_JET || !driver_seat_free(veh))) continue;
        typedef char (__attribute__((thiscall)) *Chk_t)(void *);
        if (!((Chk_t)0x00e44ea0)(e)) continue;   // enabled
        if (!((ChkP_t)0x00dd8f90)(e, bot)) continue;   // the spawner's own team / seat check
        apply_kit(bot);
        ((CseSpawn_t)ADDR_CSE_SPAWN)(e, bot, 0xffffffffu, 1, 0);
        char *s = *(char **)(bot + OFF_PLAYER_SOLDIER), *v = player_vehicle(bot);
        if (!s || !v) continue;
        rr = (rr + n + 1) % g_cseCount;
        {   // the jet tells the group's side (Su-xx: RU, else US) - learnt for the next spawns
            const char *vn = vehicle_name(v);
            int side = vn && (strstr(vn, "Su") || strstr(vn, "SU") || strstr(vn, "su")) ? 2 : 1;
            cse_group_learn(e, side);
            if (side != team) bot_log("air: '%s' got the other side's jet (%s) - its group is known now", bot + OFF_PLAYER_ONLINE_NAME, vn);
        }
        g_driving[i] = 1;
        g_flying[i] = 1; g_flyAuto[i] = 1; g_flyCircuit[i] = 1; g_flyOrbit[i] = -1; g_flyChase[i] = NULL; g_flySeek[i] = 0;
        g_flyFlag[i] = -1; g_flyLogAt[i] = 0; g_flyAglAt[i] = 0; g_flyTakeoff[i] = 0;
        bot_log("spawn: '%s' team=%d in the air in a %s (spawner %p, group %08x team %d)", bot + OFF_PLAYER_ONLINE_NAME, team, vehicle_name(v), e,
                *(unsigned *)((char *)e + 0x14), gt);
        return 1;
    }
    return 0;
}
static float g_leashFor = -1;
static char g_leashLevel[64];
static float air_leash(float *cx, float *cz)
{
    if (!g_flagCount) return 0;
    float sx = 0, sz = 0;
    for (int f = 0; f < g_flagCount; f++) { sx += flag_pos(f)[0]; sz += flag_pos(f)[2]; }
    *cx = sx / g_flagCount; *cz = sz / g_flagCount;
    if (strcmp(g_leashLevel, g_level)) {
        strncpy(g_leashLevel, g_level, sizeof g_leashLevel - 1);
        g_leashFor = 1500.0f;
        char dir[MAX_PATH], path[MAX_PATH + 40], line[160], lv[64];
        GetModuleFileNameA(NULL, dir, sizeof dir);
        *strrchr(dir, '\\') = 0;
        char *up = strrchr(dir, '\\');
        if (up) *up = 0;
        snprintf(path, sizeof path, "%s\\BotMod\\BotMod_bounds.txt", dir);
        FILE *fl = fopen(path, "r");
        float r;
        if (fl) { while (fgets(line, sizeof line, fl)) if (line[0] != '#' && sscanf(line, "%63s %f", lv, &r) == 2 && !_stricmp(lv, g_level)) g_leashFor = r; fclose(fl); }
        bot_log("fly: %s - aircraft stay within %.0f m of the flags' centre (%.0f, %.0f)", g_level, g_leashFor, *cx, *cz);
    }
    return g_leashFor;
}
// A Pilot flying the flag circuit hunts (the user's order): enemy jets, then helicopters, then - with no flag left to
// take - enemy ground vehicles (cannon strafing runs). Seek mode does the rest (seekers, cannon, ground runs); back to
// the circuit when the target is down / out of its vehicle / far away.
static void pilot_air_tick(int i)
{
    if (!is_pilot(i) || !g_flying[i] || !g_flyAuto[i] || g_flyTakeoff[i] || (H->frame + i * 11) % 60) return;
    char *pl = H->bots[i], *veh = player_vehicle(pl);
    float *m = veh ? vehicle_xf(veh) : NULL;
    if (!m) return;
    int team = player_team(pl);
    if (g_flyChase[i]) {   // hunting: still worth it?
        char *t = g_flyChase[i], *tv = player_exists(t) ? player_vehicle(t) : NULL;
        float *tm = tv ? vehicle_xf(tv) : NULL;
        float lcx, lcz, lr = air_leash(&lcx, &lcz);
        int outT = tm && lr > 0 && (tm[12] - lcx) * (tm[12] - lcx) + (tm[14] - lcz) * (tm[14] - lcz) > (lr + 200.0f) * (lr + 200.0f);
        if (tm && !outT && dist2(&tm[12], &m[12]) < 3000.0f * 3000.0f) return;
        if (outT) bot_log("air: %s lets %s go - past the map edge", pl + OFF_PLAYER_ONLINE_NAME, t + OFF_PLAYER_ONLINE_NAME);
        bot_log("air: %s breaks off %s (%s) - back to the flags", pl + OFF_PLAYER_ONLINE_NAME, t + OFF_PLAYER_ONLINE_NAME, tm ? "too far" : "gone");
        g_flyChase[i] = NULL; g_flySeek[i] = 0; g_flyCircuit[i] = 1; g_flyFlag[i] = -1;
        return;
    }
    if (!g_flyCircuit[i]) return;
    int toTake = 0;
    for (int f = 0; f < g_flagCount; f++) if (flag_owner(f) != team) toTake++;
    char *pm = player_manager(), *best = NULL;
    void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
    unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
    int bestRank = 99;
    float bestD = 2000.0f * 2000.0f;
    for (unsigned k = 0; arr && k < count && k < 64; k++) {
        char *p = arr[k], *pv;
        if (!p || p == pl || player_team(p) == team || !(pv = player_vehicle(p)) || !vehicle_ok(pv)) continue;
        char gmode[64];
        chat_game_mode(gmode, sizeof gmode);
        int kind = vehicle_kind(pv), rank = kind == VK_JET ? 0 : kind == VK_HELI ? 1 : (!toTake || _strnicmp(gmode, "AirSuperiority", 14) ? 2 : 99);   // ground: always, but in Air Superiority only once nothing is left to take
        if (rank == 99) continue;
        float *pm2 = vehicle_xf(pv);
        if (!pm2) continue;
        {   // not one out past the leash
            float lcx, lcz, lr = air_leash(&lcx, &lcz);
            if (lr > 0 && (pm2[12] - lcx) * (pm2[12] - lcx) + (pm2[14] - lcz) * (pm2[14] - lcz) > lr * lr) continue;
        }
        float d = dist2(&pm2[12], &m[12]);
        if (rank < bestRank || (rank == bestRank && d < bestD)) { if (d < 2000.0f * 2000.0f) { bestRank = rank; bestD = d; best = p; } }
    }
    if (!best) return;
    g_flyChase[i] = best; g_flySeek[i] = 1; g_seekFire[i] = 1; g_flyTgtAt[i] = 0; memset(g_flyTgtVel[i], 0, 12);
    g_seekSince[i] = 0; g_seekSelAt[i] = 0; g_seekFiredAt[i] = 0; g_seekShots[i] = 0; g_seekGunsUntil[i] = 0; g_seekNoLockSince[i] = 0;
    bot_log("air: %s goes for %s's %s (%.0f m)", pl + OFF_PLAYER_ONLINE_NAME, best + OFF_PLAYER_ONLINE_NAME, vehicle_name(player_vehicle(best)), sqrtf(bestD));
}

// On the way: friendly bots on foot by the vehicle are called in ("Get in!"). One heading the same way answers
// "Roger" and walks over (squad boarding), one going elsewhere "Negative". The driver stops until they are in.
static unsigned g_askedAt[MAX_BOTS];
static int drive_pickup(int i, DriveState *d, const float *vp)
{
    char *pl = H->bots[i];
    int boarding = 0;
    for (int j = 0; j < H->botCount; j++) if (j != i && g_drive[j].phase == 5 && g_drive[j].veh == d->veh) boarding++;
    if (d->pickupUntil) {
        if (boarding && H->frame < d->pickupUntil) return 1;
        bot_log("drive: %s drives on (%s)", pl + OFF_PLAYER_ONLINE_NAME, boarding ? "waited long enough" : "everyone is in");
        d->pickupUntil = 0;
        return 0;
    }
    if (vehicle_is_boat(d->veh) || H->frame - d->pickupAskAt < 30) return 0;
    d->pickupAskAt = H->frame;
    int seats = free_seat_count(d->veh), team = player_team(pl);
    if (seats <= 0 || dist2(vp, d->goal) < 120.0f * 120.0f) return 0;   // full, or nearly there
    int yes = 0, asked = 0;
    for (int j = 0; j < H->botCount && yes < seats; j++) {
        char *jp = H->bots[j], *js = *(char **)(jp + OFF_PLAYER_SOLDIER);
        BotBrain *b = bot_brain(j);
        if (j == i || !js || player_team(jp) != team || player_vehicle(jp) || g_drive[j].phase || !b->enabled) continue;
        if (b->task != TASK_NONE || g_fightTarget[j] || H->frame - g_askedAt[j] < 60 * 30) continue;
        float *jpos = (float *)(js + OFF_SOLDIER_POSITION);
        if (dist2(jpos, vp) > 25.0f * 25.0f) continue;
        g_askedAt[j] = H->frame;
        if (!asked++) bot_say(i, ORDER_GETIN);
        int tf = b->targetFlag;
        // worth it: the ride ends nearer its own flag than it is now, by a good margin
        int useful = tf >= 0 && sqrtf(dist2(d->goal, flag_pos(tf))) < sqrtf(dist2(jpos, flag_pos(tf))) - 60.0f;
        if (!useful || !plan_to_vehicle(j, js, vp)) {
            bot_say_later(j, ORDER_NEGATIVE, 25 + 10 * asked);
            bot_log("drive: %s offers %s a ride - Negative (going elsewhere)", pl + OFF_PLAYER_ONLINE_NAME, jp + OFF_PLAYER_ONLINE_NAME);
            continue;
        }
        DriveState *dj = &g_drive[j];
        memset(dj, 0, sizeof *dj);
        dj->phase = 5; dj->veh = d->veh; dj->at = H->frame; dj->flag = tf;
        memcpy(dj->start, vp, 12);
        memcpy(dj->goal, flag_pos(tf), 12);
        bot_nav(j)->mode = 3;
        bot_say_later(j, ORDER_ROGERTHAT, 25 + 10 * asked);
        bot_log("drive: %s offers %s a ride - Roger, it walks over", pl + OFF_PLAYER_ONLINE_NAME, jp + OFF_PLAYER_ONLINE_NAME);
        yes++;
    }
    if (!yes) return 0;
    d->pickupUntil = H->frame + 15 * 30;
    bot_log("drive: %s stops to pick up %d bot(s)", pl + OFF_PLAYER_ONLINE_NAME, yes);
    return 1;
}

// Brain hook: a bot heading for a far flag looks for a vehicle to take.
static void drive_consider(int i)
{
    BotBrain *b = bot_brain(i);
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (g_driveOff || !s || g_drive[i].phase || player_vehicle(pl) || !b->enabled || b->task != TASK_NONE || !b->enroute || b->targetFlag < 0) return;
    int chance = H->frame < g_vehChanceUntil[i];   // just got out of a vehicle / just took a flag: anyone may grab one nearby
    if (!chance && (player_squad(pl) > 0 || squad_objective(player_team(pl), player_squad(pl)) >= 0)) return;   // squads decide together
    if (!chance && H->frame - g_lastSpawnedAt[i] < 10 * 30) return;   // (a fresh spawn joins its squad half a second later)
    if (H->frame < g_driveCooldown[i] || (H->frame + i) % 30) return;
    float *me = (float *)(s + OFF_SOLDIER_POSITION), *fp = flag_pos(b->targetFlag);
    float minTrip = chance ? 200.0f : DRIVE_MIN_TRIP, findR = chance ? 60.0f : DRIVE_FIND_RADIUS;
    if (dist2(me, fp) < minTrip * minTrip) return;
    int team = player_team(pl), other = 3 - team;
    char **vehs, *best = NULL;
    int nv = vehicle_list(&vehs);
    float bestD = findR * findR;
    for (int k = 0; k < nv; k++) {
        char *v = vehs[k];
        if (!vehicle_ok(v) || !vehicle_is_light(v) || !driver_seat_free(v) || vehicle_bad(v)) continue;
        if (chance && vehicle_is_boat(v)) continue;   // (on the way: ground vehicles)
        if (vehicle_is_boat(v) && !need_boat(me, fp)) continue;   // (walkable: no boat)
        int vt = 0;
        float *vp = vehicle_pos(v, &vt);
        if (!vp || (vt && vt != team)) continue;
        float d2 = dist2(vp, me);
        if (d2 >= bestD) continue;
        if (!chance && g_hqPos[team][0] != 0 && g_hqPos[other][0] != 0 && dist2(vp, g_hqPos[team]) > dist2(vp, g_hqPos[other])) continue;   // their side
        int taken = 0;
        for (int j = 0; j < H->botCount && !taken; j++) if (j != i && g_drive[j].phase && g_drive[j].veh == v) taken = 1;
        if (taken) continue;
        bestD = d2; best = v;
    }
    if (!best) { if (!chance) g_driveCooldown[i] = H->frame + 10 * 30; return; }
    g_vehChanceUntil[i] = 0;
    float *vp = vehicle_pos(best, NULL);
    if (!plan_to_vehicle(i, s, vp)) { g_driveCooldown[i] = H->frame + 20 * 30; return; }
    DriveState *d = &g_drive[i];
    memset(d, 0, sizeof *d);
    d->phase = 1; d->veh = best; d->at = H->frame; d->flag = b->targetFlag;
    memcpy(d->start, vp, 12);
    memcpy(d->goal, fp, 12);
    bot_nav(i)->mode = 3;
    bot_log("drive: %s goes for the %s %.0f m away (flag %d is %.0f m off)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(best), sqrtf(bestD),
            b->targetFlag, sqrtf(dist2(me, fp)));
}
// Every frame for a bot with a drive in progress (before steer_bot).
// A Pilot's vehicle under half health: "Need repair" once (engineer bots nearby answer it like a player's call). Once it
// has been repaired back over PILOT_REPAIRED_FRAC it may call again the next time it drops under half.
#define PILOT_REPAIR_BELOW_FRAC  0.5f
#define PILOT_REPAIRED_FRAC      0.75f
static float *vehicle_xf(char *veh);
static void fly_tick(int i)
{
    char *pl = H->bots[i], *veh = player_vehicle(pl);
    if (!veh) {
        bot_log("fly: %s lost its aircraft (crashed / shot down / got out)", pl + OFF_PLAYER_ONLINE_NAME);
        g_flying[i] = 0; g_flyAuto[i] = 0; g_driving[i] = 0; return;
    }
    g_driving[i] = 1;
    if (g_flyAuto[i]) {   // ---- autopilot ----
        float *m = vehicle_xf(veh);
        if (!m) return;
        float hdg = atan2f(m[8], m[10]) * 57.29578f, pitch = asinf(fmaxf(-1.0f, fminf(1.0f, m[9]))) * 57.29578f;
        float bank = atan2f(m[1], m[5]) * 57.29578f, alt = m[13];
        static float lastPos[MAX_BOTS][3]; static unsigned lastAt[MAX_BOTS]; static float spd[MAX_BOTS];
        if (H->frame - lastAt[i] >= 10) {
            if (lastAt[i]) {
                float dtv = (H->frame - lastAt[i]) / 30.0f;
                for (int k = 0; k < 3; k++) g_flyVel[i][k] = (m[12 + k] - lastPos[i][k]) / dtv;
                spd[i] = sqrtf(g_flyVel[i][0] * g_flyVel[i][0] + g_flyVel[i][1] * g_flyVel[i][1] + g_flyVel[i][2] * g_flyVel[i][2]);
            }
            lastPos[i][0] = m[12]; lastPos[i][1] = m[13]; lastPos[i][2] = m[14]; lastAt[i] = H->frame;
        }
        if (H->frame >= g_flyAglAt[i]) {   // height above the ground under it
            g_flyAglAt[i] = H->frame + 15;
            float from[3] = {m[12], m[13] - 4.0f, m[14]}, to[3] = {m[12], m[13] - 2000.0f, m[14]}, hit[3], nrm[3];
            g_flyAgl[i] = ray_cast(from, to, hit, nrm) ? m[13] - hit[1] : 2000.0f;
            // ahead along the flight path (its motion over the last moment), ~3 s out: a hillside in a low, hard turn
            float vx = m[12] - lastPos[i][0], vy = m[13] - lastPos[i][1], vz = m[14] - lastPos[i][2], vl = sqrtf(vx * vx + vy * vy + vz * vz);
            if (vl < 0.5f) { vx = m[8]; vy = m[9]; vz = m[10]; vl = 1.0f; }
            float look = fmaxf(250.0f, spd[i] * 5.0f);   // (3 s was too late: a hillside, climbing at 25 degrees - CFIT)
            float a0[3] = {m[12] + vx / vl * 8.0f, m[13] + vy / vl * 8.0f, m[14] + vz / vl * 8.0f};
            float a1[3] = {m[12] + vx / vl * look, m[13] + vy / vl * look, m[14] + vz / vl * look};
            g_flyAhead[i] = ray_cast(a0, a1, hit, nrm) ? sqrtf((hit[0] - m[12]) * (hit[0] - m[12]) + (hit[1] - m[13]) * (hit[1] - m[13]) + (hit[2] - m[14]) * (hit[2] - m[14])) : 1e9f;
            if (g_flyAhead[i] < 1e8f) memcpy(g_flyAheadHit[i], hit, 12);
            if (g_flyAhead[i] < 1e8f && g_flyChase[i] && player_exists(g_flyChase[i])) {   // the ray hit the jet it is chasing, not a hill
                char *tv = player_vehicle(g_flyChase[i]);                                  // (the ray cannot skip vehicles: it pulled
                float *tm = tv ? vehicle_xf(tv) : NULL;                                     // up off every lined-up shot)
                if (tm && (hit[0] - tm[12]) * (hit[0] - tm[12]) + (hit[1] - tm[13]) * (hit[1] - tm[13]) + (hit[2] - tm[14]) * (hit[2] - tm[14]) < 30.0f * 30.0f)
                    g_flyAhead[i] = 1e9f;
            }
            g_flyEscape[i] = 0;
            if (g_flyAhead[i] < 1e8f) {   // would a 40-degree climb clear it? if not: the clearer side, 60 degrees off the nose
                float hl = sqrtf(m[8] * m[8] + m[10] * m[10]) + 1e-3f, fx = m[8] / hl, fz = m[10] / hl;
                float c1[3] = {m[12] + fx * look * 0.77f, m[13] + look * 0.64f, m[14] + fz * look * 0.77f};
                if (ray_cast(a0, c1, hit, nrm)) {
                    float best = -1, side = 0;
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        float ca = 0.5f, sa = 0.866f * sgn;   // 60 degrees: rotate (fx, fz) about the vertical
                        float sx = fx * ca + fz * sa, sz = -fx * sa + fz * ca;
                        float s1[3] = {m[12] + sx * look, m[13] + look * 0.2f, m[14] + sz * look};
                        float d = ray_cast(a0, s1, hit, nrm) ? sqrtf((hit[0] - m[12]) * (hit[0] - m[12]) + (hit[2] - m[14]) * (hit[2] - m[14])) : 1e9f;
                        if (d > best) { best = d; side = (float)sgn; }
                    }
                    g_flyEscape[i] = side;
                }
            }
        }
        if (g_flyTakeoff[i]) {   // take-off run (the user's way): full power + boost (Shift), pulling up, wings level - until clear
            if (g_flyTakeoffAlt[i] == 0 && g_flyAgl[i] > 20.0f && g_flyAgl[i] < 1999.0f) {   // a real height under it: already airborne
                g_flyTakeoff[i] = 0;                                                                 // (a reload mid-flight) - no take-off
                bot_log("fly: %s is already airborne (%.0f m up) - no take-off", pl + OFF_PLAYER_ONLINE_NAME, g_flyAgl[i]);
            }
            if (g_flyTakeoff[i] && g_flyTakeoffAlt[i] == 0) g_flyTakeoffAlt[i] = alt;
            if (!g_flyTakeoff[i]) {}
            else if (alt - g_flyTakeoffAlt[i] > 100.0f && spd[i] > 60.0f) { g_flyTakeoff[i] = 0;
                bot_log("fly: %s airborne (%.0f m above the runway, %.0f m/s)", pl + OFF_PLAYER_ONLINE_NAME, alt - g_flyTakeoffAlt[i], spd[i]); g_flyTakeoffAlt[i] = 0; }
            else {
                g_seatYaw[i] = fmaxf(-1.0f, fminf(1.0f, -bank * 0.04f));
                g_seatPitch[i] = pitch < 25.0f ? -0.8f : 0.0f;   // (pitch + = nose down)
                H->botMove[i][0] = 1.0f; H->botMove[i][1] = 0; H->botMove[i][2] = 0;
                g_sprintUntil[i] = H->frame + 3;
                if (H->frame >= g_flyLogAt[i]) { g_flyLogAt[i] = H->frame + 30;
                    bot_log("fly: %s take-off run: %.0f m/s, %.0f m up, pitch %.0f", pl + OFF_PLAYER_ONLINE_NAME, spd[i], g_flyAgl[i], pitch); }
                return;
            }
        }
        int airSup;
        { char gm[64]; chat_game_mode(gm, sizeof gm); airSup = !_strnicmp(gm, "AirSuperiority", 14); }
        if (g_flyCircuit[i] && !g_flyChase[i] && !airSup && g_flagCount > 0) {
            // not Air Superiority (the user, 2026-10-13: an F-35B circled a flag tight instead of hunting): jets take no
            // flags - a wide patrol (800 m round the flags' centre, ~300 m over them) while pilot_air_tick looks for prey
            float cx, cz;
            air_leash(&cx, &cz);
            float fy = 0;
            for (int f = 0; f < g_flagCount; f++) fy += flag_pos(f)[1];
            fy = g_flagCount ? fy / g_flagCount : m[13];
            float ox = m[12] - cx, oz = m[14] - cz, a = atan2f(ox, oz) + 0.5f;
            g_flyGoal[i][0] = cx + sinf(a) * 800.0f; g_flyGoal[i][1] = cz + cosf(a) * 800.0f; g_flyGoal[i][2] = fy + 300.0f;
        }
        if (g_flyCircuit[i] && !g_flyChase[i] && airSup && g_flagCount > 0 && H->frame % 30 == (unsigned)i % 30) {
            // the flag circuit, decided once a second: a flag not ours (the nearest) - fly there and circle in its zone until it
            // turns; all ours: guard them, flying flag to flag (on to the next within 350 m), breaking off for any that is lost
            int team = player_team(pl), f = g_flyFlag[i], pick = -1;
            int valid = f >= 0 && f < g_flagCount;
            float bestD = 1e30f;
            for (int k = 0; k < g_flagCount; k++) {
                if (flag_owner(k) == team) continue;
                float d = (flag_pos(k)[0] - m[12]) * (flag_pos(k)[0] - m[12]) + (flag_pos(k)[2] - m[14]) * (flag_pos(k)[2] - m[14]);
                if (d < bestD) { bestD = d; pick = k; }
            }
            int capturing = valid && flag_owner(f) != team && H->frame < g_flyFlagUntil[i];
            if (capturing) pick = f;                         // keep circling the one it is taking
            else if (pick < 0) {                             // all ours: guard - on to the next flag once near this one
                float d = valid ? sqrtf((flag_pos(f)[0] - m[12]) * (flag_pos(f)[0] - m[12]) + (flag_pos(f)[2] - m[14]) * (flag_pos(f)[2] - m[14])) : 0;
                pick = !valid ? 0 : d < 350.0f ? (f + 1) % g_flagCount : f;
            }
            if (pick != f) {
                g_flyFlag[i] = pick;
                g_flyFlagUntil[i] = H->frame + 120 * 30;   // the flight there plus a minute circling at most
                bot_log("fly: %s %s flag %d (owner %d) at %.0f m, %.0f m away", pl + OFF_PLAYER_ONLINE_NAME, flag_owner(pick) == team ? "guards" : "goes to take",
                        pick, flag_owner(pick), flag_pos(pick)[1],
                        sqrtf((flag_pos(pick)[0] - m[12]) * (flag_pos(pick)[0] - m[12]) + (flag_pos(pick)[2] - m[14]) * (flag_pos(pick)[2] - m[14])));
            }
        }
        if (g_flyCircuit[i] && !g_flyChase[i] && airSup && g_flyFlag[i] >= 0 && g_flyFlag[i] < g_flagCount) {
            float *fp0 = flag_pos(g_flyFlag[i]), fp[3] = {fp0[0], fp0[1], fp0[2]};
            float ox = m[12] - fp0[0], oz = m[14] - fp0[2], od = sqrtf(ox * ox + oz * oz);
            if (od < 400.0f && od > 1.0f) {   // near it: a point on a 150 m circle round the balloon, a little ahead of us
                float a = atan2f(ox, oz) + 0.6f;
                fp[0] = fp0[0] + sinf(a) * 150.0f; fp[2] = fp0[2] + cosf(a) * 150.0f;
            }
            g_flyGoal[i][0] = fp[0]; g_flyGoal[i][1] = fp[2]; g_flyGoal[i][2] = fp[1] + 20.0f;
        }
        if (g_flyOrbit[i] >= 0 && g_flyOrbit[i] < g_flagCount && !g_flyChase[i]) {
            float *fp0 = flag_pos(g_flyOrbit[i]), r = g_flyOrbitR[i];
            float ox = m[12] - fp0[0], oz = m[14] - fp0[2];
            float a = atan2f(ox, oz) + fminf(0.8f, 250.0f / r);
            g_flyGoal[i][0] = fp0[0] + sinf(a) * r; g_flyGoal[i][1] = fp0[2] + cosf(a) * r; g_flyGoal[i][2] = fp0[1] + g_flyOrbitUp[i];
        }
        float chaseElev = 0; int chasing = 0, fire = 0;
        if (!g_flyChase[i]) g_flyDive[i] = g_flyPull[i] = 0;
        if (g_flyChase[i]) {   // hunting an aircraft: its lead point (where it will be when the rounds get there)
            char *tv = player_exists(g_flyChase[i]) ? player_vehicle(g_flyChase[i]) : NULL;
            float *tm = tv ? vehicle_xf(tv) : NULL;
            if (!tm) { g_flyDive[i] = g_flyPull[i] = 0; g_strafeRun[i] = 0; }   // its target got out (on foot): no run to finish -
            if (tm) {                                                            // a pull-out left on stalled it at the ceiling
                float *tp = &tm[12];
                if (g_flyTgtAt[i] && H->frame - g_flyTgtAt[i] >= 3) {   // its velocity, between real position changes
                    float dt = (H->frame - g_flyTgtAt[i]) / 30.0f, mv = 0;
                    for (int k = 0; k < 3; k++) mv += (tp[k] - g_flyTgtPos[i][k]) * (tp[k] - g_flyTgtPos[i][k]);
                    if (mv > 0.01f || dt > 1.0f) {
                        for (int k = 0; k < 3; k++) { float v = (tp[k] - g_flyTgtPos[i][k]) / dt; if (fabsf(v) < 400.0f) g_flyTgtVel[i][k] = 0.5f * g_flyTgtVel[i][k] + 0.5f * v; }
                        memcpy(g_flyTgtPos[i], tp, 12); g_flyTgtAt[i] = H->frame;
                    }
                } else if (!g_flyTgtAt[i]) { memcpy(g_flyTgtPos[i], tp, 12); g_flyTgtAt[i] = H->frame; }
                float dx = tp[0] - m[12], dy = tp[1] - m[13], dz = tp[2] - m[14], dist = sqrtf(dx * dx + dy * dy + dz * dz);
                float tSpeed = sqrtf(g_flyTgtVel[i][0] * g_flyTgtVel[i][0] + g_flyTgtVel[i][1] * g_flyTgtVel[i][1] + g_flyTgtVel[i][2] * g_flyTgtVel[i][2]);
                static unsigned char wasGround[MAX_BOTS];   // (25 / 40 m/s hysteresis: a jet taking off flipped cannon <-> seekers every tick)
                int ground = wasGround[i] ? tSpeed < 40.0f : tSpeed < 25.0f;   // parked / taxiing / a ground vehicle: strafing runs instead of a dogfight (seekers too: they cannot lock it)
                wasGround[i] = (unsigned char)ground;
                float hd = sqrtf(dx * dx + dz * dz);
                if (!ground) g_strafeRun[i] = 0;
                int obstacle = g_strafeRun[i] == 1 && dist <= DIVE_START_DIST && g_flyAhead[i] < 1e8f &&
                               (g_flyAheadHit[i][0] - tp[0]) * (g_flyAheadHit[i][0] - tp[0]) + (g_flyAheadHit[i][2] - tp[2]) * (g_flyAheadHit[i][2] - tp[2]) > 60.0f * 60.0f;
                if (obstacle) bot_log("fly: %s - something in the way of the dive (%.0f m ahead)", pl + OFF_PLAYER_ONLINE_NAME, g_flyAhead[i]);
                if (g_strafeRun[i] == 1 && (dist < DIVE_PULL_DIST || g_flyAgl[i] < DIVE_MIN_AGL || obstacle)) {
                    g_strafeRun[i] = 2; g_strafeAt[i] = H->frame;
                    bot_log("fly: %s pulls out of its dive (%.0f m from %s, %.0f m up)", pl + OFF_PLAYER_ONLINE_NAME, dist, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, g_flyAgl[i]); }
                else if (g_strafeRun[i] == 2 && g_flyAgl[i] > 350.0f && pitch > 5.0f && H->frame - g_strafeAt[i] > 60) {
                    g_strafeRun[i] = 0; bot_log("fly: %s climbs out - reassesses and sets up again", pl + OFF_PLAYER_ONLINE_NAME); }
                else if (g_strafeRun[i] == 0 && hd > 1200.0f && g_flyAgl[i] > 400.0f) {
                    g_strafeRun[i] = 1; bot_log("fly: %s rolls in on %s (%.0f m, %.0f m up)", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist, g_flyAgl[i]); }
                g_flyDive[i] = ground && g_strafeRun[i] == 1 && dist <= DIVE_START_DIST;
                g_flyPull[i] = ground && g_strafeRun[i] == 2;
                int seekGuns = g_flySeek[i] && (H->frame < g_seekGunsUntil[i] || ground);   // out of seekers / a target on the ground: the cannon
                float t = dist / (g_flySeek[i] && !seekGuns ? FLY_MSL_SPEED : FLY_GUN_SPEED);   // the lead (as the tank's turret): where it will be
                float lp[3] = {tp[0] + g_flyTgtVel[i][0] * t, tp[1] + g_flyTgtVel[i][1] * t, tp[2] + g_flyTgtVel[i][2] * t};
                if (ground) lp[1] += g_diveAimUp;   // bullet drop: aim the dive above it (console "diveaim")
                float lp0[3] = {lp[0], lp[1], lp[2]};   // the true lead point (lp may become a pass-by / climb-over point below)
                if (g_flySeek[i] && !ground) {   // seekers want its tail: not yet in its rear cone - a point 350 m behind it
                    float ux = g_flyTgtVel[i][0] / tSpeed, uy = g_flyTgtVel[i][1] / tSpeed, uz = g_flyTgtVel[i][2] / tSpeed;
                    float aspect = -(dx * ux + dy * uy + dz * uz) / (dist + 1e-3f);   // 1 = right behind it, -1 = head-on
                    float noseOn = (m[8] * dx + m[9] * dy + m[10] * dz) / (dist + 1e-3f);   // (only both noses pointing at each other is head-on:
                    float closing = ((g_flyVel[i][0] - g_flyTgtVel[i][0]) * dx + (g_flyVel[i][1] - g_flyTgtVel[i][1]) * dy + (g_flyVel[i][2] - g_flyTgtVel[i][2]) * dz) / (dist + 1e-3f);
                    if (aspect < -0.7f && noseOn > 0.8f && closing > 50.0f && dist <= 800.0f) {   // (+ really closing: it stayed 'head-on' while opening)   // -0.3 alone circled the user for 20 s) head-on and close (it rammed the user head-on at ~230 m/s
                        float hl = sqrtf(dx * dx + dz * dz) + 1e-3f;   // closing): pass 200 m to the side and 100 m above
                        lp[0] = tp[0] + dz / hl * 200.0f; lp[1] = tp[1] + 100.0f; lp[2] = tp[2] - dx / hl * 200.0f;
                        static unsigned passAt[MAX_BOTS];
                        if (H->frame - passAt[i] > 90) { passAt[i] = H->frame; bot_log("seek: %s meets %s head-on (%.0f m) - passes to the side", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist); }
                    } else if ((aspect < 0.3f && dist > 800.0f) || dist > 1500.0f) {   // within 800 m: straight at it (the offset point kept it 17 deg off in a turning fight)   // (0.8 left it flying a parallel circle 660 m out, 35 deg off an orbiting target)
                         lp[0] = tp[0] - ux * 200.0f; lp[1] = tp[1] - uy * 200.0f; lp[2] = tp[2] - uz * 200.0f; }
                }
                if (g_flySeek[i] && !ground && dist < (seekGuns ? 60.0f : 110.0f)) {   // (guns want it close: 60 m)   // too close (it rammed the user, 2026-10-13): climb over it, no shots
                    lp[0] = tp[0]; lp[1] = tp[1] + 150.0f; lp[2] = tp[2];
                    static unsigned overAt[MAX_BOTS];
                    if (H->frame - overAt[i] > 90) { overAt[i] = H->frame; bot_log("seek: %s too close to %s (%.0f m) - climbs over it", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist); }
                }
                g_flyGoal[i][0] = lp[0]; g_flyGoal[i][1] = lp[2]; g_flyGoal[i][2] = lp[1];
                if (ground && g_strafeRun[i] != 1) {   // out to 1.8 km from it - towards the middle of the flags (the far side of the
                    // target from us could be outside the combat area: a jet lost there 10 s after setting up), high, then turn in
                    float cx = 0, cz = 0;
                    for (int k = 0; k < g_flagCount; k++) { cx += flag_pos(k)[0]; cz += flag_pos(k)[2]; }
                    if (g_flagCount) { cx /= g_flagCount; cz /= g_flagCount; } else { cx = m[12]; cz = m[14]; }
                    float wx = cx - tp[0], wz = cz - tp[2], wl = sqrtf(wx * wx + wz * wz);
                    float ux = wl > 50.0f ? wx / wl : (hd > 1.0f ? -dx / hd : 1.0f), uz = wl > 50.0f ? wz / wl : (hd > 1.0f ? -dz / hd : 0.0f);
                    // 1.3 km out, 650 m above it: a ~25 degree dive (from 1.8 km / 450 m it was ~14 and ran out of height first)
                    g_flyGoal[i][0] = tp[0] + ux * 1300.0f; g_flyGoal[i][1] = tp[2] + uz * 1300.0f; g_flyGoal[i][2] = tp[1] + 650.0f;
                }
                float lx = lp[0] - m[12], ly = lp[1] - m[13], lz = lp[2] - m[14], ll = sqrtf(lx * lx + ly * ly + lz * lz) + 1e-3f;
                chaseElev = asinf(fmaxf(-1.0f, fminf(1.0f, ly / ll))) * 57.29578f;
                float cosOff = (m[8] * lx + m[9] * ly + m[10] * lz) / ll;   // nose (forward row) against the lead direction
                float gunReach = (FLY_GUN_SPEED + spd[i]) * FLY_GUN_TTL * 0.9f;   // how far its rounds really get (90%)
                fire = ground ? (g_flyDive[i] && dist < gunReach && cosOff > 0.998f)   // the dive: nose within ~3.5 degrees of it
                              : (dist < fminf(FLY_GUN_RANGE, gunReach) && cosOff > 0.9994f);   // a dogfight: within ~2 degrees
                chasing = !ground || g_strafeRun[i] == 1;   // (setting up a run flies to its point like any waypoint)
                g_seekDist[i] = dist;
                if (seekGuns) {   // the cannon: selected once, the guns chase's own fire rule (fire, computed above)
                    g_seekDist[i] = dist; memcpy(g_seekAim[i], lp, 12);
                    if (g_seekSelAt[i] != 1) { g_seekSelAt[i] = 1; press_button(i, 16, 3); if (ground) bot_log("seek: %s switches to the cannon - %s is on the ground (strafing runs)", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME);
                        else bot_log("seek: %s switches to the cannon for %.0f s", pl + OFF_PLAYER_ONLINE_NAME, (g_seekGunsUntil[i] - H->frame) / 30.0f); }
                    if (dist < 60.0f) fire = 0;
                    {   // guns only with the nose on the TRUE lead point (against the pass-by / climb-over point it fired into the
                        // sky off the user's upper left, 2026-10-13)
                        float qx = lp0[0] - m[12], qy = lp0[1] - m[13], qz = lp0[2] - m[14], ql = sqrtf(qx * qx + qy * qy + qz * qz) + 1e-3f;
                        float cos0 = (m[8] * qx + m[9] * qy + m[10] * qz) / ql;
                        if (!ground) fire = dist < fminf(FLY_GUN_RANGE, gunReach) && cos0 > 0.9976f;   // snap shots: within ~4 deg (2 never came in a turn fight)
                    }
                    if (dist < 60.0f) fire = 0;
                    g_seekTurnFight[i] = !ground && cosOff < 0.707f;   // nose > 45 deg off it: throttle back to turn tighter (as the user does)
                } else if ((g_seekTurnFight[i] = 0), g_flySeek[i]) {   // heat-seekers: selected (again every 10 s), no guns; BotMod's own lock timer
                    fire = 0;
                    g_seekDist[i] = dist; memcpy(g_seekAim[i], lp, 12);
                    if (g_seekSelAt[i] <= 1 || H->frame - g_seekSelAt[i] > 300) {
                        if (g_seekSelAt[i] == 1) bot_log("seek: %s - missiles back, seekers selected", pl + OFF_PLAYER_ONLINE_NAME);
                        g_seekSelAt[i] = H->frame; press_button(i, 17, 3); }
                    float cosT = (m[8] * dx + m[9] * dy + m[10] * dz) / (dist + 1e-3f);   // the nose against the target itself
                    float offT = acosf(fmaxf(-1.0f, fminf(1.0f, cosT))) * 57.29578f;
                    float rx0 = lp0[0] - m[12], ry0 = lp0[1] - m[13], rz0 = lp0[2] - m[14], rl0 = sqrtf(rx0 * rx0 + ry0 * ry0 + rz0 * rz0) + 1e-3f;
                    float off = acosf(fmaxf(-1.0f, fminf(1.0f, (m[8] * rx0 + m[9] * ry0 + m[10] * rz0) / rl0))) * 57.29578f;    // ... and against the TRUE lead point
                    if (dist < g_seekRange && dist >= 110.0f && off < g_seekCone && offT < 2.0f * g_seekCone) { if (!g_seekSince[i]) g_seekSince[i] = H->frame; }
                    else if (g_seekSince[i]) {
                        bot_log("seek: %s loses the lock on %s after %.1f s (%.0f m, %.1f deg off)", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME,
                                (H->frame - g_seekSince[i]) / 30.0f, dist, off);
                        g_seekSince[i] = 0;
                    }
                    float held = g_seekSince[i] ? (H->frame - g_seekSince[i]) / 30.0f : 0.0f;
                    // a turn fight (the user kept it turning, never 4 s on the nose): within 400 m and no lock for 8 s - guns for 10 s
                    if (dist > 400.0f || g_seekSince[i]) g_seekNoLockSince[i] = 0;
                    else if (!g_seekNoLockSince[i]) g_seekNoLockSince[i] = H->frame;
                    else if (H->frame - g_seekNoLockSince[i] > 8 * 30) {
                        g_seekNoLockSince[i] = 0; g_seekGunsUntil[i] = H->frame + 10 * 30;
                        bot_log("seek: %s - a turn fight, no lock in 8 s at %.0f m: cannon for 10 s", pl + OFF_PLAYER_ONLINE_NAME, dist);
                    }
                    static unsigned char announced[MAX_BOTS];
                    if (held >= g_seekTime) {
                        if (!announced[i]) { announced[i] = 1; bot_log("seek: %s LOCKED on %s (%.0f m, %.1f deg off, %.1f s)", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist, off, held); }
                        if (g_seekFire[i] && H->frame - g_seekFiredAt[i] > 90) { g_seekFiredAt[i] = H->frame; press_button(i, g_seekFireEia, 3);   // one missile, then 3 s (every 0.5 s was a ripple of two)
                            bot_log("seek: %s fires (EIA %d)", pl + OFF_PLAYER_ONLINE_NAME, g_seekFireEia); }
                    } else announced[i] = 0;
                    static unsigned seekLogAt[MAX_BOTS];
                    if (H->frame >= seekLogAt[i]) { seekLogAt[i] = H->frame + 30;
                        bot_log("seek: %s on %s: %.0f m, nose %.1f deg off the lead (%.1f off it), lock %.1f/%.1f s", pl + OFF_PLAYER_ONLINE_NAME, g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist, off, offT, held, g_seekTime); }
                }
                static unsigned logAt[MAX_BOTS];
                if (H->frame >= logAt[i]) { logAt[i] = H->frame + 30;
                    bot_log("fly: %s chases %s: %.0f m, target v (%.0f %.0f %.0f), nose off the lead by %.1f deg%s", pl + OFF_PLAYER_ONLINE_NAME,
                            g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, dist, g_flyTgtVel[i][0], g_flyTgtVel[i][1], g_flyTgtVel[i][2],
                            acosf(fmaxf(-1.0f, fminf(1.0f, cosOff))) * 57.29578f, fire ? " FIRING" : ""); }
            }
        }
        {   // the leash: out past it - back towards the centre, no chasing / shooting until well inside again
            float cx, cz, lr = air_leash(&cx, &cz);
            static unsigned char outside[MAX_BOTS];
            float ox = m[12] - cx, oz = m[14] - cz, od = sqrtf(ox * ox + oz * oz);
            if (lr > 0 && (od > lr || (outside[i] && od > lr - 300.0f))) {
                if (!outside[i]) bot_log("fly: %s is %.0f m from the map centre (leash %.0f) - turns back", pl + OFF_PLAYER_ONLINE_NAME, od, lr);
                outside[i] = 1;
                g_flyGoal[i][0] = cx; g_flyGoal[i][1] = cz;
                chasing = 0; fire = 0; g_flyDive[i] = g_flyPull[i] = 0;
            } else outside[i] = 0;
        }
        float want = atan2f(g_flyGoal[i][0] - m[12], g_flyGoal[i][1] - m[14]) * 57.29578f, e = want - hdg;
        while (e > 180.0f) e -= 360.0f;
        while (e < -180.0f) e += 360.0f;
        int evading = cm_beam_heading(i, hdg, &want);
        if (evading) {   // a missile on it and the flares reloading: break across the missile's path (beam it), full power
            e = want - hdg;
            while (e > 180.0f) e -= 360.0f;
            while (e < -180.0f) e += 360.0f;
            chasing = 0; fire = 0; g_flyDive[i] = g_flyPull[i] = 0;
        }
        // bank towards the turn (pulling turns the heading towards the low wing), up to 80 degrees
        // ~80 degrees to turn (the user's way); chasing, twice as eager - a gentle bank for a small error left it 15-20 degrees
        // behind a turning target, turning no faster than it
        float wantBank = -g_flyTurnSign * fmaxf(-80.0f, fminf(80.0f, e * (chasing ? 4.0f : 2.0f)));
        if (strcmp(g_flyCeilingLevel, g_level)) { strncpy(g_flyCeilingLevel, g_level, sizeof g_flyCeilingLevel - 1); g_flyCeiling = 1e9f; }
        {   // stalled against the map's ceiling (BF3 caps altitude: slow, nose up, height not changing) - remember it
            static float stallAlt[MAX_BOTS]; static unsigned stallSince[MAX_BOTS];
            if (spd[i] > 0 && spd[i] < 30.0f && pitch > 10.0f && fabsf(alt - stallAlt[i]) < 4.0f) {
                if (!stallSince[i]) stallSince[i] = H->frame;
                else if (H->frame - stallSince[i] > 60 && alt < g_flyCeiling) {
                    g_flyCeiling = alt;
                    bot_log("fly: %s found the ceiling of %s at %.0f m - flies at least 150 m below it", pl + OFF_PLAYER_ONLINE_NAME, g_level, alt);
                }
            } else { stallSince[i] = 0; stallAlt[i] = alt; }
        }
        float goalAlt = fminf(g_flyGoal[i][2], g_flyCeiling - 150.0f);
        float wantPitch = fmaxf(-15.0f, fminf(15.0f, (goalAlt - alt) * 0.15f));
        if (chasing) wantPitch = fmaxf(g_strafeRun[i] ? -45.0f : -35.0f, fminf(30.0f, chaseElev));
        if (evading) wantPitch = fmaxf(-10.0f, fminf(0.0f, wantPitch));   // no climbing in the break: keep the speed
        float balloonSide = 0, balloonUp = 0;
        {   // a flag balloon on the flight path in the next 3 s, within 60 m: avoid it (it flew into one chasing)
            float fx = m[8], fy = m[9], fz = m[10], v = fmaxf(spd[i], 60.0f);
            for (int k = 0; k < g_flagCount; k++) {
                float *b = flag_pos(k), bx = b[0] - m[12], by = b[1] - m[13], bz = b[2] - m[14];
                float t = (bx * fx + by * fy + bz * fz) / v;              // seconds until abeam of it
                if (t < 0 || t > 3.0f) continue;
                float cx = bx - fx * t * v, cy = by - fy * t * v, cz = bz - fz * t * v;   // where it is off our line then
                if (cx * cx + cy * cy + cz * cz > 60.0f * 60.0f) continue;
                float rx = m[0], rz = m[2];                                // our right axis (horizontal part)
                balloonSide = (cx * rx + cz * rz) > 0 ? -1.0f : 1.0f;      // it is to the right: go left
                balloonUp = cy > 0 ? -1.0f : 1.0f;                         // it is above: go under
                break;
            }
        }
        float floorAgl = g_flyChase[i] ? 200.0f : 120.0f;
        if (g_strafeRun[i]) floorAgl = 180.0f;
        int danger = g_flyAgl[i] < floorAgl || pitch < (g_strafeRun[i] ? -50.0f : -30.0f) || (g_flyAhead[i] < 1e8f && !(g_strafeRun[i] && g_flyAgl[i] > 250.0f));
        if (!danger && balloonSide != 0) {   // a balloon ahead: bank away from its side, climb / dive past it
            wantBank = -g_flyTurnSign * 60.0f * balloonSide;
            wantPitch = balloonUp > 0 ? 20.0f : -10.0f;
            if (H->frame >= g_flyLogAt[i]) bot_log("fly: %s avoids a balloon (%s, %s)", pl + OFF_PLAYER_ONLINE_NAME, balloonSide > 0 ? "left" : "right", balloonUp > 0 ? "over" : "under");
        }
        if (danger) {
            wantBank = 0; wantPitch = g_flyAhead[i] < 1e8f ? 40.0f : 25.0f;
            // a climb will not clear it: bank hard towards the clearer side and pull (pulling turns towards the low wing)
            if (g_flyEscape[i] != 0) wantBank = -g_flyTurnSign * 70.0f * (g_flyEscape[i] > 0 ? 1.0f : -1.0f);
        }
        // too slow (a hard pull at a steep bank bled a Su-35 down to 19 m/s): wings level, nose a little down, no pulling
        int slow = !danger && spd[i] > 0.0f && spd[i] < 55.0f;
        if (slow) { wantBank = 0; wantPitch = -10.0f; }
        float roll = fmaxf(-1.0f, fminf(1.0f, (wantBank - bank) * 0.04f));
        float cb = cosf(bank / 57.29578f), sb = fabsf(sinf(bank / 57.29578f));
        // level part: nose to the wanted pitch (only as much as the wings are level); turning part: pull while banked into it
        // (the pitch hold only with the wings fairly level - banked, it fought the turn's pull to a standstill)
        float levelPull = (fabsf(bank) < 50.0f || danger) ? -(wantPitch - pitch) * 0.06f * cb * cb : 0.0f;
        // a hard turn: pull firmly and hold the nose about level with the bank angle - nose rising: bank steeper (up to 90),
        // nose dropping: shallower (down to 60). (Pulling less instead left a Su-35 circling 1 km wide, never closing.)
        if (fabsf(wantBank) > 40.0f && !danger && !slow) {
            float turnPitch = fmaxf(-5.0f, fminf(5.0f, wantPitch));
            float mag = fmaxf(60.0f, fminf(90.0f, 80.0f + (pitch - turnPitch) * 1.5f));
            wantBank = (wantBank > 0 ? 1.0f : -1.0f) * mag;
        }
        roll = fmaxf(-1.0f, fminf(1.0f, (wantBank - bank) * 0.04f));
        float turnPull = (!danger && !slow && fabsf(e) > (chasing ? 1.5f : 3.0f) && fabsf(bank) > (chasing ? 15.0f : 25.0f))
                       ? -fminf(1.0f, (chasing ? 0.4f + fabsf(e) * 0.05f : 0.6f + fabsf(e) * 0.02f)) * sb : 0.0f;
        float pin = fmaxf(-1.0f, fminf(1.0f, levelPull + turnPull));
        float rudder = 0;
        if (chasing && !danger && !slow && balloonSide == 0 && fabsf(e) < (g_flySeek[i] ? 4.0f : 15.0f)) {   // (seek: the fine aim's ~6 deg/s fell behind an orbiting jet, 20 deg off)
            // fine aim (2026-10-12): bank-and-pull wobbled 22..82 degrees for a 10-degree error and fought the nose-down;
            // near the target: a small bank, the rudder (measured: + turns the heading down, ~6 deg/s at 0.8) and pitch straight
            float fineBank = -g_flyTurnSign * fmaxf(-25.0f, fminf(25.0f, e * 1.5f));
            roll = fmaxf(-1.0f, fminf(1.0f, (fineBank - bank) * 0.05f));
            rudder = fmaxf(-1.0f, fminf(1.0f, -e * 0.15f));
            pin = fmaxf(-1.0f, fminf(1.0f, -(wantPitch - pitch) * 0.08f));
        }
        if (g_flySeek[i] && chasing && !danger && !slow && !evading && balloonSide == 0) {
            // the seekers' nose tracker (as the tank turret's smooth tracking with lead, the user's idea): the lead point in
            // the jet's own axes - roll it onto the canopy's up line, pull by how far it is off the nose, rudder the last
            // degrees. Continuous, no bank-then-pull switching (that swung the nose 6..16 deg every 2 s at 166 m behind)
            float ax = g_seekAim[i][0] - m[12], ay = g_seekAim[i][1] - m[13], az = g_seekAim[i][2] - m[14];
            float bx = ax * m[0] + ay * m[1] + az * m[2], by = ax * m[4] + ay * m[5] + az * m[6], bz = ax * m[8] + ay * m[9] + az * m[10];
            float offAll = atan2f(sqrtf(bx * bx + by * by), bz) * 57.29578f;
            if (offAll < 45.0f) {
                float phi = atan2f(bx, by) * 57.29578f;              // round the nose from the canopy's up line (+ = to the right)
                float elev = atan2f(by, bz) * 57.29578f;              // above the nose in the jet's own frame
                float azim = atan2f(bx, bz) * 57.29578f;              // right of the nose
                if (offAll < 1.0f) phi = 0;                           // on it: no rolling about
                roll = fmaxf(-1.0f, fminf(1.0f, -g_flyTurnSign * phi * 0.03f));
                pin = fmaxf(-1.0f, fminf(1.0f, -elev * (fabsf(phi) < 60.0f ? 0.12f : 0.03f)));   // (pitch + = nose down)
                rudder = fmaxf(-1.0f, fminf(1.0f, -azim * 0.15f));
            }
        }
        if (g_flyPull[i] && !slow) {   // pulling out: wings level, hard back on the stick until climbing at 20 degrees, full power (not when slow)
            roll = fmaxf(-1.0f, fminf(1.0f, (0.0f - bank) * 0.05f));
            pin = pitch < 20.0f ? -1.0f : fmaxf(-1.0f, fminf(1.0f, -(20.0f - pitch) * 0.08f));
            rudder = 0;
        } else if (g_flyDive[i]) {   // the dive: the rounds' path straight at the target - small bank + rudder, pitch onto it
            // where the rounds go = nose * muzzle speed + the jet's own velocity (rudder skids put a nose-only aim ~30 m wide)
            float rx = m[8] * FLY_GUN_SPEED + g_flyVel[i][0], ry = m[9] * FLY_GUN_SPEED + g_flyVel[i][1], rz = m[10] * FLY_GUN_SPEED + g_flyVel[i][2];
            float rh = sqrtf(rx * rx + rz * rz) + 1e-3f;
            float pathHdg = atan2f(rx, rz) * 57.29578f, pathPitch = atan2f(ry, rh) * 57.29578f;
            float pe = want - pathHdg;
            while (pe > 180.0f) pe -= 360.0f;
            while (pe < -180.0f) pe += 360.0f;
            float fineBank = -g_flyTurnSign * fmaxf(-25.0f, fminf(25.0f, pe * 1.5f));
            float diveElev = fmaxf(-70.0f, fminf(10.0f, chaseElev));
            roll = fmaxf(-1.0f, fminf(1.0f, (fineBank - bank) * 0.05f));
            rudder = fmaxf(-1.0f, fminf(1.0f, -pe * 0.15f));
            pin = fmaxf(-1.0f, fminf(1.0f, -(diveElev - pathPitch) * 0.08f));
            fire = fire && fabsf(pe) < 2.0f && fabsf(diveElev - pathPitch) < 2.0f;   // the rounds' path on it, not just the nose
        }
        g_seatYaw[i] = roll;
        g_seatPitch[i] = pin;
        H->botMove[i][0] = g_flyPull[i] ? 1.0f : g_flyDive[i] ? 0.3f : (evading || spd[i] < 90.0f || danger ? 1.0f : 0.4f);
        if (g_flyChase[i] && chasing && !danger && !slow && !g_flyDive[i] && !g_flyPull[i] &&   // far behind it: boost (Shift - the
            (g_flySeek[i] ? g_seekDist[i] > 400.0f : g_seekDist[i] > 600.0f))                      // sprint bits; the user: closes gaps)
            g_sprintUntil[i] = H->frame + 3;
        if (evading) g_sprintUntil[i] = H->frame + 3;   // boost through the break too
        if (g_flySeek[i] && chasing && !evading && !danger && !slow && !g_flyDive[i] && !g_flyPull[i] && !g_strafeRun[i])   // seekers: sit 200-350 m behind it
            H->botMove[i][0] = 0 && g_seekTurnFight[i] && spd[i] > 70.0f ? 0.0f :   // (off: the user preferred it without - it fell off their tail)
                               g_seekDist[i] > 350.0f ? 1.0f : g_seekDist[i] > 200.0f ? 0.4f : -1.0f;   // (120-250 m kept overrunning a human to 109 m)
        H->botMove[i][1] = 0;
        H->botMove[i][2] = rudder;
        if (fire && (!danger || g_flyDive[i])) press_button(i, EIA_FIRE, 2);
        if (H->frame >= g_flyLogAt[i]) {
            g_flyLogAt[i] = H->frame + 15;
            bot_log("fly: %s alt %.0f agl %.0f speed %.0f hdg %.0f->%.0f (err %.0f) pitch %.0f->%.0f bank %.0f->%.0f | roll %.2f pitch %.2f thr %.2f%s%s",
                    pl + OFF_PLAYER_ONLINE_NAME, alt, g_flyAgl[i], spd[i], hdg, want, e, pitch, wantPitch, bank, wantBank, roll, pin, H->botMove[i][0],
                    g_flyPull[i] ? " PULLING OUT" : g_flyDive[i] ? " DIVING" : danger ? (g_flyAhead[i] < 1e8f ? " PULL UP (terrain ahead)" : " PULL UP") : slow ? " TOO SLOW" : "", evading ? " BREAK (missile, no flares)" : H->frame < g_sprintUntil[i] ? " BOOST" : "");
        }
        return;
    }
    int on = H->frame < g_flyUntil[i];
    H->botMove[i][0] = on ? g_flyIn[i][2] : 0.0f;
    H->botMove[i][1] = 0;
    H->botMove[i][2] = on ? g_flyIn[i][3] : 0.0f;
    g_seatYaw[i] = on ? g_flyIn[i][0] : 0.0f;
    g_seatPitch[i] = on ? g_flyIn[i][1] : 0.0f;
    if (H->frame < g_flyLogAt[i]) return;
    g_flyLogAt[i] = H->frame + 8;
    float *m = vehicle_xf(veh);
    if (!m) return;
    static float lastPos[MAX_BOTS][3]; static unsigned lastAt[MAX_BOTS];
    float spd = lastAt[i] ? sqrtf((m[12] - lastPos[i][0]) * (m[12] - lastPos[i][0]) + (m[13] - lastPos[i][1]) * (m[13] - lastPos[i][1]) +
                                  (m[14] - lastPos[i][2]) * (m[14] - lastPos[i][2])) / ((H->frame - lastAt[i]) / 30.0f) : 0.0f;
    lastPos[i][0] = m[12]; lastPos[i][1] = m[13]; lastPos[i][2] = m[14]; lastAt[i] = H->frame;
    float hdg = atan2f(m[8], m[10]) * 57.29578f, pitch = asinf(fmaxf(-1.0f, fminf(1.0f, m[9]))) * 57.29578f;
    float bank = atan2f(m[1], m[5]) * 57.29578f;   // right axis y over up axis y
    bot_log("fly: %s alt %.0f speed %.0f hdg %.0f pitch %.0f bank %.0f | in roll %.2f pitch %.2f thr %.2f yaw %.2f%s", pl + OFF_PLAYER_ONLINE_NAME,
            m[13], spd, hdg, pitch, bank, g_seatYaw[i], g_seatPitch[i], H->botMove[i][0], H->botMove[i][2], on ? "" : " (released)");
}
static void pilot_repair_tick(int i, char *veh)
{
    static char *askedFor[MAX_BOTS];
    static unsigned char asked[MAX_BOTS], bailed[MAX_BOTS];
    if (!veh || !vehicle_ok(veh)) return;
    if (askedFor[i] != veh) { askedFor[i] = veh; asked[i] = 0; bailed[i] = 0; }   // a new vehicle (a new life)
    VehTrack *t = vehicle_sample(veh);
    float h = *(float *)(veh + OFF_VEHICLE_HEALTH), mx = t && t->maxSeen > 0 ? t->maxSeen : 1000.0f, frac = h / mx;
    char *pl = H->bots[i];
    if (!asked[i] && frac < PILOT_REPAIR_BELOW_FRAC) {
        asked[i] = 1;
        bot_log("pilot: %s's %s is at %.0f%% - Need repair", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), frac * 100.0f);
        bot_say(i, ORDER_REQUESTREPAIR);
        order_reaction(pl, ORDER_REQUESTREPAIR, NULL);
        press_button(i, g_flareEia, 3);   // ...and pops smoke (the vehicle countermeasure, X - as a jet's flares; the user, 2026-10-13)
        bot_log("pilot: %s pops smoke", pl + OFF_PLAYER_ONLINE_NAME);
    }
    if (!bailed[i] && frac < 0.30f) {   // under 30%: "Get out!" - its gunners / riders bail (they need not die with it); the Pilot
        bailed[i] = 1;                    // itself stays at the controls (order_reaction skips the one who called it - the user)
        int riders = 0;
        for (int j = 0; j < H->botCount; j++) if (j != i && player_vehicle(H->bots[j]) == veh) riders++;
        bot_say(i, ORDER_GETOUT);
        order_reaction(pl, ORDER_GETOUT, NULL);
        bot_log("pilot: %s's %s is at %.0f%% - \"Get out!\" (%d bot rider(s) bail, it stays in)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), frac * 100.0f, riders);
    }
    if (asked[i] && frac >= PILOT_REPAIRED_FRAC) {
        asked[i] = 0; bailed[i] = 0;
        bot_log("pilot: %s's %s repaired to %.0f%% - may call for repairs again", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), frac * 100.0f);
    }
}
// Overwatch (the user, 2026-10-13): a Pilot's tank whose flag is taken hides 60-140 m from it - drivable ground with a
// clear view of the flag and cover round it (horizontal rays that hit something within 14 m, the flag's side left open).
// Worked out once per bot and flag.
static float g_owSpot[MAX_BOTS][3];
static int g_owFlag[MAX_BOTS] = {[0 ... MAX_BOTS - 1] = -1};
static unsigned g_owSerial[MAX_BOTS];
static int overwatch_spot(int i, int f, float *out)
{
    NavGrid *g = *nav_slot();
    if (!g || g->phase != 2 || !g_driveMask || g_driveMaskFor != g->serial || f < 0 || f >= g_flagCount) return 0;
    if (g_owFlag[i] == f && g_owSerial[i] == g->serial) { memcpy(out, g_owSpot[i], 12); return 1; }
    float *fp = flag_pos(f), best[3] = {0, 0, 0}, bestScore = -1e9f, hit[3], nrm[3];
    int bestCover = 0;
    for (float r = 60.0f; r <= 140.0f; r += 20.0f)
        for (int a = 0; a < 16; a++) {
            float ang = a * 0.3926991f + r * 0.01f, c[3];
            if (!nav_mask_nearest(g, g_driveMask, fp[0] + sinf(ang) * r, fp[2] + cosf(ang) * r, 12.0f, c)) continue;
            float dx = fp[0] - c[0], dz = fp[2] - c[2];
            if (dx * dx + dz * dz < 50.0f * 50.0f) continue;
            float eye[3] = {c[0], c[1] + 2.5f, c[2]}, tgt[3] = {fp[0], fp[1] + 2.0f, fp[2]};
            if (ray_cast(eye, tgt, hit, nrm) && dist2(hit, tgt) > 15.0f * 15.0f) continue;   // no view of the flag
            float toF = atan2f(dx, dz), s0[3] = {c[0], c[1] + 1.5f, c[2]};
            int cover = 0;
            for (int k = 0; k < 12; k++) {
                float a2 = k * 0.5235988f, df = a2 - toF;
                while (df > 3.14159265f) df -= 6.2831853f;
                while (df < -3.14159265f) df += 6.2831853f;
                if (fabsf(df) < 0.87f) continue;   // the flag's side stays open (that is where it looks)
                float e[3] = {c[0] + sinf(a2) * 14.0f, c[1] + 1.5f, c[2] + cosf(a2) * 14.0f};
                if (ray_cast(s0, e, hit, nrm)) cover++;
            }
            float score = cover * 10.0f - fabsf(r - 80.0f) * 0.05f;
            int team = player_team(H->bots[i]), other = 3 - team;   // our side of the flag (both sides picked one spot)
            if (g_hqPos[team][0] != 0 && g_hqPos[other][0] != 0 && dist2(c, g_hqPos[team]) < dist2(c, g_hqPos[other])) score += 25.0f;
            for (int j = 0; j < H->botCount; j++)   // not on a team-mate's spot
                if (j != i && g_owFlag[j] == f && g_owSerial[j] == g->serial && player_team(H->bots[j]) == team && dist2(c, g_owSpot[j]) < 30.0f * 30.0f) score -= 40.0f;
            if (score > bestScore) { bestScore = score; memcpy(best, c, 12); bestCover = cover; }
        }
    if (bestScore < -1e8f) return 0;
    memcpy(g_owSpot[i], best, 12); g_owFlag[i] = f; g_owSerial[i] = g->serial;
    memcpy(out, best, 12);
    bot_log("pilot: %s overwatch for flag %d at (%.0f, %.0f), %.0f m from it, cover %d of 9 directions", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, f,
            best[0], best[2], sqrtf(dist2(best, flag_pos(f))), bestCover);
    return 1;
}
// A Pilot's jeep / boat is a taxi (the user, 2026-10-13): at its pickup spot it calls "Get in!" (bots nearby board) for up
// to 20 s or until full; with riders it drives to the uncaptured flag nearest the vehicle and calls "Get out!" ~60 m from
// it; empty, it drives back to its pickup spot to load again. It never gets out itself.
static void pilot_drive_to(int i, DriveState *d, char *veh, const float *goal, int flag)
{
    float *vp = vehicle_pos(veh, NULL);
    if (!vp) return;
    float spd = d->speed, np[3];
    int same = d->veh == veh && dist2(d->goal, goal) < 50.0f * 50.0f, rr = d->reroutes, nu = d->nudges;   // (the same trip: it
    memcpy(np, d->nudgePos, 12);                                                                         //  remembers the corners it hit)
    memset(d, 0, sizeof *d);
    d->veh = veh; d->phase = 2; d->at = H->frame; d->flag = flag; d->speed = spd; d->manual = 1;
    d->taxi = !vehicle_is_boat(veh);   // (boats: as before)
    if (same) { d->reroutes = rr; d->nudges = nu; memcpy(d->nudgePos, np, 12); }
    memcpy(d->goal, goal, 12);
    memcpy(d->start, vp, 12);
}
// A Pilot's jeep / boat (only taken when no armour / jet was free) is a taxi with a job, never parked (the user, 2026-10-13):
// drive to the nearest group of friendly bots on foot (500 m), stop beside them and call "Get in!" (10 s at most; a
// group that does not board is skipped for 30 s), drive them to the nearest uncaptured flag and get out WITH them there
// (the Pilot is infantry again until a ride). Nobody to pick up: to the nearest uncaptured flag alone, out there. Empty
// and a tank / IFV / jet of ours is free: out, it goes for that.
static void pilot_leave_vehicle(int i, char *veh, const char *why)
{
    char *pl = H->bots[i];
    bot_log("pilot: %s gets out of its %s (%s)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), why);
    g_pilotForceExit = 1; bot_exit_vehicle(i, 1); g_pilotForceExit = 0;
    DriveState *d = &g_drive[i];
    d->phase = 0; d->veh = NULL; g_driving[i] = 0;
    vehicle_mark_bad(veh, 45);   // not straight back into it
    bot_brain(i)->task = TASK_NONE; bot_brain(i)->enroute = bot_brain(i)->waiting = 0;
    choose_local_objective(i, why);
}
// All the flags ours (the user, 2026-10-13): Pilots' tanks and taxis guard the 2 flags nearest the enemy's HQ (no
// enemy HQ - Conquest Assault: the 2 farthest from ours), shared out by bot number. -1 while any flag is not ours.
static int pilot_front_flag(int i, int team)
{
    if (g_flagCount <= 0) return -1;
    for (int f = 0; f < g_flagCount; f++) if (flag_owner(f) != team) return -1;
    const float *eh = g_hqPos[team == 1 ? 2 : 1], *oh = g_hqPos[team == 1 ? 1 : 2];
    int useEnemy = eh[0] != 0 || eh[2] != 0;
    int a = -1, b = -1;
    float da = 1e30f, db = 1e30f;
    for (int f = 0; f < g_flagCount; f++) {
        float q = useEnemy ? dist2(flag_pos(f), eh) : -dist2(flag_pos(f), oh);
        if (q < da) { b = a; db = da; a = f; da = q; } else if (q < db) { b = f; db = q; }
    }
    return b >= 0 && (i & 1) ? b : a;
}
static int pilot_heavy_free(int i)   // a tank / IFV / jet of ours nobody has or is heading for
{
    char *pl = H->bots[i];
    int team = player_team(pl);
    char **vs;
    int nv = vehicle_list(&vs);
    for (int k = 0; k < nv; k++) {
        char *v = vs[k];
        if (!vehicle_ok(v) || !pilot_ride_kind(vehicle_kind(v)) || !driver_seat_free(v) || vehicle_bad(v) || !pilot_ride_ours(v, team) || ride_unreachable(i, v)) continue;
        int taken = 0;
        for (int j = 0; j < H->botCount && !taken; j++) if (j != i && ((g_drive[j].phase && g_drive[j].veh == v) || g_pilotRide[j] == v)) taken = 1;
        if (!taken) return 1;
    }
    return 0;
}
static char *pilot_heavy_nearest(int i, const float *from, float *outD2)   // the nearest free tank / IFV of ours (not AA)
{
    char *pl = H->bots[i], **vs, *best = NULL;
    int team = player_team(pl), nv = vehicle_list(&vs);
    float bd = 1e30f;
    for (int k = 0; k < nv; k++) {
        char *v = vs[k];
        // (no jets: a boat cannot land on a carrier deck - the jets are the respawn rule's)
        if (!vehicle_ok(v) || !pilot_ride_kind(vehicle_kind(v)) || vehicle_kind(v) == VK_JET || vehicle_is_mobile_aa(v) || !driver_seat_free(v) || vehicle_bad(v) ||
            !pilot_ride_ours(v, team) || ride_unreachable(i, v)) continue;
        int taken = 0;
        for (int j = 0; j < H->botCount && !taken; j++) if (j != i && ((g_drive[j].phase && g_drive[j].veh == v) || g_pilotRide[j] == v)) taken = 1;
        if (taken) continue;
        float *vp = vehicle_pos(v, NULL), q = vp ? dist2(vp, from) : 1e30f;
        if (q < bd) { bd = q; best = v; }
    }
    if (outD2) *outD2 = bd;
    return best;
}
static void pilot_ferry_tick(int i, DriveState *d, char *veh)
{
    static char *grp[MAX_BOTS], *skip[MAX_BOTS][4];
    static unsigned skipUntil[MAX_BOTS][4], loadAt[MAX_BOTS], callAt[MAX_BOTS];
    char *pl = H->bots[i];
    float *vp = vehicle_pos(veh, NULL);
    if (!vp) return;
    int riders = 0, team = player_team(pl), best = -1;
    for (int j = 0; j < H->botCount; j++) if (j != i && player_vehicle(H->bots[j]) == veh) riders++;
    float bd = 1e30f;
    for (int f = 0; f < g_flagCount; f++)
        if (flag_owner(f) != team && (f >= 16 || H->frame >= g_taxiNoFlagUntil[i][f])) { float q = dist2(flag_pos(f), vp); if (q < bd) { bd = q; best = f; } }
    if (best < 0 && (best = pilot_front_flag(i, team)) >= 0) bd = dist2(flag_pos(best), vp);   // all ours: to the front
    if (!riders && !vehicle_is_boat(veh) && (H->frame + i) % 30 == 0 && pilot_heavy_free(i)) { pilot_leave_vehicle(i, veh, "a tank / jet is free"); return; }
    if (vehicle_bad(veh)) {   // the drive gave up on it (keeps getting stuck / no road route): everyone out, on foot from here
        if (riders) { bot_say(i, ORDER_GETOUT); order_reaction(pl, ORDER_GETOUT, NULL); }
        pilot_leave_vehicle(i, veh, "the taxi keeps getting stuck / has no road");
        return;
    }
    if (vehicle_is_boat(veh)) {   // a Pilot's boat wants a better ride (the user, 2026-10-13): sail for the nearest free tank / IFV
        static char *boatFor[MAX_BOTS];   // of ours, land by it, everyone out - the Pilot takes it (riders come along if aboard)
        float rd2;
        char *ride = pilot_heavy_nearest(i, vp, &rd2);
        if (ride) {
            float *rp = vehicle_pos(ride, NULL);
            if (boatFor[i] == ride || rd2 < 60.0f * 60.0f) {   // (the sail for it ended: landed)
                boatFor[i] = NULL;
                if (riders) { bot_say(i, ORDER_GETOUT); order_reaction(pl, ORDER_GETOUT, NULL); }
                bot_log("pilot: %s lands %.0f m from a free %s - goes for it", pl + OFF_PLAYER_ONLINE_NAME, sqrtf(rd2), vehicle_name(ride));
                pilot_leave_vehicle(i, veh, "landed by a better ride");
                return;
            }
            if (H->frame < d->at) return;
            d->at = H->frame + 3 * 30;
            boatFor[i] = ride;
            pilot_drive_to(i, d, veh, rp, -1);
            bot_log("pilot: %s sails for the free %s (%.0f m)%s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(ride), sqrtf(rd2),
                    riders ? " with its riders" : "");
            return;
        }
        boatFor[i] = NULL;
    }
    if (riders > 0) {   // delivering
        if (loadAt[i] && H->frame - loadAt[i] < 10 * 30 && free_seat_count(veh) > 0 && grp[i]) {   // still boarding
            H->botMove[i][0] = d->speed > 0.5f ? -0.4f : 0.0f;
            return;
        }
        loadAt[i] = 0; grp[i] = NULL;
        if (best < 0) { pilot_leave_vehicle(i, veh, "no flag left to take"); return; }
        static int boatTo[MAX_BOTS];   // a boat: the flag it already sailed for (+1) - stopped again = landed: everyone out
        int landed = vehicle_is_boat(veh) && boatTo[i] == best + 1;
        if (vehicle_is_boat(veh) && !landed && H->frame >= d->at) boatTo[i] = best + 1;
        if (landed) boatTo[i] = 0;
        if (bd < 60.0f * 60.0f || landed) {   // there: everyone out - the Pilot too
            bot_say(i, ORDER_GETOUT);
            order_reaction(pl, ORDER_GETOUT, NULL);
            bot_log("pilot: %s drops %d rider(s) at flag %d and goes with them", pl + OFF_PLAYER_ONLINE_NAME, riders, best);
            pilot_leave_vehicle(i, veh, "takes the flag with its riders");
            return;
        }
        if (H->frame < d->at) return;
        d->at = H->frame + 3 * 30;
        pilot_drive_to(i, d, veh, flag_pos(best), best);
        bot_log("pilot: %s drives %d rider(s) to flag %d (%.0f m)", pl + OFF_PLAYER_ONLINE_NAME, riders, best, sqrtf(bd));
        return;
    }
    // empty: the nearest friendly bot on foot (not a Pilot, not one of the groups that would not board)
    char *nbot = NULL;
    float nd = 25.0f * 25.0f;   // earshot only: a taxi keeps to the roads and takes who is by it (the user, 2026-10-13)
    for (int j = 0; j < H->botCount; j++) {
        char *jp = H->bots[j], *js = *(char **)(jp + OFF_PLAYER_SOLDIER);
        if (j == i || !js || player_team(jp) != team || player_vehicle(jp) || is_pilot(j) || !bot_brain(j)->enabled || g_drive[j].phase) continue;
        int sk = 0;
        for (int k = 0; k < 4; k++) if (skip[i][k] == jp && H->frame < skipUntil[i][k]) sk = 1;
        if (sk) continue;
        float q = dist2((float *)(js + OFF_SOLDIER_POSITION), vp);
        if (q < nd) { nd = q; nbot = jp; }
    }
    if (!nbot) {   // nobody to carry: to the nearest flag alone, out there
        if (best < 0) { pilot_leave_vehicle(i, veh, "nothing to do with it"); return; }
        if (bd < 60.0f * 60.0f) { pilot_leave_vehicle(i, veh, "at the flag - takes it on foot"); return; }
        if (H->frame < d->at) return;
        d->at = H->frame + 3 * 30;
        pilot_drive_to(i, d, veh, flag_pos(best), best);
        bot_log("pilot: %s has nobody to carry - drives to flag %d (%.0f m)", pl + OFF_PLAYER_ONLINE_NAME, best, sqrtf(bd));
        return;
    }
    float *np = (float *)(*(char **)(nbot + OFF_PLAYER_SOLDIER) + OFF_SOLDIER_POSITION);
    if (nd > 22.0f * 22.0f) {   // to them
        if (grp[i] != nbot) { grp[i] = nbot; loadAt[i] = 0; }
        if (H->frame < d->at) return;
        d->at = H->frame + 4 * 30;
        pilot_drive_to(i, d, veh, np, -1);
        bot_log("pilot: %s drives to pick up %s's group (%.0f m)", pl + OFF_PLAYER_ONLINE_NAME, nbot + OFF_PLAYER_ONLINE_NAME, sqrtf(nd));
        return;
    }
    // beside them: "Get in!" (10 s)
    H->botMove[i][0] = d->speed > 0.5f ? -0.4f : 0.0f;
    if (grp[i] != nbot || !loadAt[i]) { grp[i] = nbot; loadAt[i] = H->frame; callAt[i] = 0; }
    if (H->frame >= callAt[i]) {
        callAt[i] = H->frame + 4 * 30;
        bot_say(i, ORDER_GETIN);
        order_reaction(pl, ORDER_GETIN, NULL);
    }
    if (H->frame - loadAt[i] > 10 * 30) {   // nobody came: skip this group a while
        int s = 0;
        for (int k = 1; k < 4; k++) if (skipUntil[i][k] < skipUntil[i][s]) s = k;
        skip[i][s] = nbot; skipUntil[i][s] = H->frame + 30 * 30;
        bot_log("pilot: %s's group would not get in - tries another", nbot + OFF_PLAYER_ONLINE_NAME);
        grp[i] = NULL; loadAt[i] = 0;
    }
}
static void pilot_hold_tick(int i, DriveState *d)
{
    char *pl = H->bots[i], *veh = player_vehicle(pl);
    H->botMove[i][1] = H->botMove[i][2] = 0;
    H->botMove[i][0] = d->speed > 0.5f ? -0.4f : 0.0f;   // brake to a stop (a still gun shoots straight)
    // with a target: pivot the hull to face it - the front armour towards the enemy (the turret aims on its own)
    if (veh && g_gunWeapon[i] && (g_gunTarget[i] || g_gunTargetVeh[i])) {
        float *hx = vehicle_xf(veh);
        if (hx) {
            float he = g_gunTgtBearing[i] - atan2f(hx[8], hx[10]);
            while (he > 3.14159265f) he -= 6.2831853f;
            while (he < -3.14159265f) he += 6.2831853f;
            if (fabsf(he) > 0.1f) H->botMove[i][2] = fmaxf(-1.0f, fminf(1.0f, -he * 1.5f));
        }
    }
    if (!veh) { d->phase = 0; d->veh = NULL; g_driving[i] = 0; return; }
    d->veh = veh;
    { int vk = vehicle_kind(veh); if ((vk == VK_LIGHT || vk == VK_BOAT) && vehicle_ok(veh) && !vehicle_is_mobile_aa(veh)) { pilot_ferry_tick(i, d, veh); return; } }   // a taxi
    if (H->frame < d->at || !vehicle_ok(veh)) return;
    d->at = H->frame + 8 * 30;
    float *vp = vehicle_pos(veh, NULL);
    if (!vp) return;
    int team = player_team(pl), best = -1;
    float bestD = 1e30f;
    for (int f = 0; f < g_flagCount; f++) {   // nearest flag that is not ours; with all ours, one of the 2 nearest the enemy
        float dd = dist2(flag_pos(f), vp) + (flag_owner(f) == team ? 1e12f : 0.0f);
        if (dd < bestD) { bestD = dd; best = f; }
    }
    { int ff = pilot_front_flag(i, team); if (ff >= 0) best = ff; }
    if (best < 0) return;
    float *fp = flag_pos(best);
    if (flag_owner(best) == team) {   // taken: overwatch from a hidden spot
        float ow[3];
        if (overwatch_spot(i, best, ow)) {
            if (dist2(ow, vp) < 12.0f * 12.0f) return;   // there: hold and watch
            float spd = d->speed;
            memset(d, 0, sizeof *d);
            d->veh = veh; d->phase = 2; d->at = H->frame; d->flag = -1; d->speed = spd; d->manual = 1;
            memcpy(d->goal, ow, 12);
            memcpy(d->start, vp, 12);
            bot_log("pilot: %s drives its %s to overwatch flag %d (%.0f m)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), best, sqrtf(dist2(ow, vp)));
            return;
        }
    }
    if (dist2(fp, vp) < (PILOT_STOP_SHORT + 10.0f) * (PILOT_STOP_SHORT + 10.0f)) return;   // already there: hold the flag
    float spd = d->speed;
    memset(d, 0, sizeof *d);
    d->veh = veh; d->phase = 2; d->at = H->frame; d->flag = best; d->speed = spd;
    d->manual = 1;   // a Pilot drives on its own (not by the infantry brain - which may be off)
    memcpy(d->goal, fp, 12);
    memcpy(d->start, vp, 12);
    bot_log("pilot: %s drives its %s to flag %d (%.0f m, owner %d)", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(veh), best, sqrtf(dist2(fp, vp)), flag_owner(best));
}
static void drive_tick(int i)
{
    DriveState *d = &g_drive[i];
    char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
    if (!d->phase) return;
    if (!s || (!d->manual && !bot_brain(i)->enabled)) { drive_stop(i, "stops driving (dead / switched off)", 10); return; }
    if (is_pilot(i)) pilot_repair_tick(i, player_vehicle(pl));
    if (d->phase == 8) { pilot_hold_tick(i, d); return; }
    if (!vehicle_ok(d->veh)) { drive_stop(i, "lost the vehicle (destroyed)", 20); return; }
    float *vp = vehicle_pos(d->veh, NULL);
    if (!vp) { drive_stop(i, "lost the vehicle", 20); return; }
    if (d->phase == 7) {   // riding: until the vehicle is left
        if (player_vehicle(pl) != d->veh) { d->phase = 0; d->veh = NULL; choose_local_objective(i, "got out of the squad's vehicle"); return; }
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        return;
    }
    if (d->phase == 5) {   // squad boarding: walk up; first one in takes the wheel
        float *me = (float *)(s + OFF_SOLDIER_POSITION);
        if (H->frame - d->at > (is_pilot(i) ? 120 : 30) * 30) { drive_stop(i, "could not reach the squad's vehicle", 20); return; }
        if (!driver_seat_free(d->veh) && !free_seat_count(d->veh)) { drive_stop(i, "finds the vehicle full", 20); return; }
        float reach = vehicle_is_boat(d->veh) ? 9.0f : is_pilot(i) ? 7.0f : 4.5f;   // (a boat: from the pier / beach; a Pilot's jet / tank: its centre is metres from the door)
        if (dist2(me, vp) > reach * reach) return;
        if (!vehicle_board_safe(d->veh)) return;   // someone just died in it: wait
        int seat = driver_seat_free(d->veh) ? 0 : free_seat(d->veh);
        if (seat < 0) { drive_stop(i, "finds the vehicle full", 20); return; }
        enter_vehicle(pl, d->veh, seat);
        if (player_vehicle(pl) != d->veh) { drive_stop(i, "could not get in", 20); return; }
        bot_nav(i)->mode = 0;
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        if (seat == 0 && is_pilot(i) && vehicle_kind(d->veh) == VK_JET) {   // a Pilot in a jet: take-off, then the flag circuit
            g_driving[i] = 1; d->phase = 0; d->veh = NULL;
            g_flying[i] = 1; g_flyAuto[i] = 1; g_flyCircuit[i] = 1; g_flyOrbit[i] = -1; g_flyChase[i] = NULL; g_flySeek[i] = 0;
            g_flyFlag[i] = -1; g_flyLogAt[i] = 0; g_flyAglAt[i] = 0; g_flyTakeoff[i] = 1; g_flyTakeoffAlt[i] = 0;
            bot_log("pilot: %s is in the %s - take-off", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(player_vehicle(pl)));
            return;
        }
        if (seat == 0 && is_pilot(i)) {   // a Pilot in a tank / IFV: straight to its flags (pilot_hold_tick decides)
            g_driving[i] = 1; d->phase = 8; d->at = H->frame + (unsigned)(i % 6) * 75;   // (leaving the HQ in turn: they jammed)
            bot_log("pilot: %s takes the %s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(d->veh));
            return;
        }
        if (seat == 0) { g_driving[i] = 1; d->phase = 6; d->at = H->frame; bot_log("drive: %s takes the wheel of the %s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(d->veh)); }
        else { d->phase = 7; bot_log("drive: %s climbs into seat %d of the %s", pl + OFF_PLAYER_ONLINE_NAME, seat, vehicle_name(d->veh)); }
        return;
    }
    if (d->phase == 6) {   // at the wheel: wait for the squadmates still walking up (15 s at most)
        if (player_vehicle(pl) != d->veh) { drive_stop(i, "is out of the vehicle", 20); return; }
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        // the whole squad: those walking up, those still to spawn (they come in on the driver - into a seat), and any
        // squadmate nearby not yet on its way (sent over). 25 s at most, or until the seats are full.
        int waiting = 0, team = player_team(pl), squad = player_squad(pl), seats = free_seat_count(d->veh);
        for (int j = 0; j < H->botCount && seats > 0; j++) {
            char *jp = H->bots[j], *js = *(char **)(jp + OFF_PLAYER_SOLDIER);
            if (j == i || player_team(jp) != team || squad <= 0 || player_squad(jp) != squad || player_vehicle(jp) == d->veh) continue;
            if (!bot_brain(j)->enabled) continue;
            if (!js) { waiting++; continue; }   // dead: respawning
            if (g_drive[j].phase == 5 && g_drive[j].veh == d->veh) { waiting++; continue; }
            if (g_drive[j].phase || player_vehicle(jp) || bot_brain(j)->task != TASK_NONE || is_pilot(j)) continue;   // (not a Pilot)
            if (dist2((float *)(js + OFF_SOLDIER_POSITION), vp) > (SQUAD_VEH_RADIUS + 20.0f) * (SQUAD_VEH_RADIUS + 20.0f)) continue;
            if (!plan_to_vehicle(j, js, vp)) continue;
            DriveState *dj = &g_drive[j];   // a squadmate that spawned after the others boarded: over here
            memset(dj, 0, sizeof *dj);
            dj->phase = 5; dj->veh = d->veh; dj->at = H->frame; dj->flag = d->flag;
            memcpy(dj->start, vp, 12);
            memcpy(dj->goal, d->goal, 12);
            bot_nav(j)->mode = 3;
            waiting++;
            bot_log("squad: %s waits for squadmate %s to board", pl + OFF_PLAYER_ONLINE_NAME, jp + OFF_PLAYER_ONLINE_NAME);
        }
        if (waiting && seats > 0 && H->frame - d->at < 25 * 30) return;
        d->phase = 2; d->boardAt = 0;   // "Get in" + 3 s for stragglers / players, then off
        return;
    }
    if (d->phase == 1) {   // walking up to it
        float *me = (float *)(s + OFF_SOLDIER_POSITION);
        if (H->frame - d->at > 35 * 30) { drive_stop(i, "gave up walking to the vehicle", 30); return; }
        if (!driver_seat_free(d->veh) || dist2(vp, d->start) > 10.0f * 10.0f) { drive_stop(i, "found the vehicle taken", 20); return; }
        float reach = vehicle_is_boat(d->veh) ? 9.0f : is_pilot(i) ? 7.0f : 4.5f;   // (a boat: from the pier / beach; a Pilot's jet / tank: its centre is metres from the door)
        if (dist2(me, vp) > reach * reach) return;
        if (!vehicle_board_safe(d->veh)) return;   // someone just died in it: wait
        enter_vehicle(pl, d->veh, 0);
        if (player_vehicle(pl) != d->veh) { drive_stop(i, "could not get in", 30); return; }
        g_driving[i] = 1;
        bot_nav(i)->mode = 0;
        d->phase = 2;
        bot_log("drive: %s is at the wheel of the %s", pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(d->veh));
        return;
    }
    if (player_vehicle(pl) != d->veh) { drive_stop(i, "is out of the vehicle", 20); return; }
    if (d->phase == 2 && !d->boardAt) {   // "Get in!" - teammates nearby take the free seats; 3 s to climb in
        d->boardAt = H->frame ? H->frame : 1;
        bot_say(i, ORDER_GETIN);
        order_reaction(pl, ORDER_GETIN, NULL);
        H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
        return;
    }
    if (d->phase == 2 && H->frame - d->boardAt < 90) return;
    // (phase 9, not 8: 8 is a Pilot's hold - a Pilot's RHIB re-planned every 3 s and never left the shore, Wake 2026-10-13)
    if (d->phase == 2 && vehicle_is_boat(d->veh) && !d->backedOut) { d->phase = 9; d->at = H->frame; d->backedOut = 1; }
    if (d->phase == 9) {   // a boat leaving its mooring: straight away from the land around it (the mean of all of it -
        // inside an L of pier and shore it heads out of the L, not at either arm), until 8 m clear; then the route
        NavGrid *g = *nav_slot();
        const unsigned char *mask = boat_mask();
        float *xf = vehicle_xf(d->veh), mx = 0, mz = 0, nearest = 1e30f;
        int nl = g && mask ? nav_land_around(g, mask, vp[0], vp[2], 30.0f, &mx, &mz, &nearest) : 0;
        if (!xf || !nl || nearest >= 8.0f || H->frame - d->at > 20 * 30) {
            bot_log("drive: %s's boat is %s (%.0f m from land) - plotting the route", pl + OFF_PLAYER_ONLINE_NAME,
                    !nl || nearest >= 8.0f ? "clear of the shore" : "still close in after 20 s", nl ? nearest : 99.0f);
            d->phase = 2;
            H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0;
            return;
        }
        if (H->frame - d->at == 0 || (H->frame - d->at) % 60 == 0)
            bot_log("drive: %s eases the boat away from the land around it (centre (%.0f, %.0f), nearest %.0f m)", pl + OFF_PLAYER_ONLINE_NAME,
                    mx, mz, nearest);
        float heading = atan2f(xf[8], xf[10]), want = atan2f(vp[0] - mx, vp[2] - mz), err;
        {   // walled in all round (a carrier's well deck - the land's middle is the deck itself): for the nearest open water
            static float ow[MAX_BOTS][3];   // instead (straight out of the stern), found once
            static unsigned owFor[MAX_BOTS];
            if (owFor[i] != d->at + 1) { owFor[i] = d->at + 1; if (!nav_open_water_near(g, mask, vp[0], vp[2], 120.0f, ow[i])) ow[i][0] = 1e30f; }
            float cx = mx - vp[0], cz = mz - vp[2];
            if (cx * cx + cz * cz < 6.0f * 6.0f) {   // walled in: the way it is pointing if that is clear (the RHIBs in Wake's LHD
                // well deck point at the opening - steering turned them into the hull, 2026-10-13), else straight back, else open water
                float hl = sqrtf(xf[8] * xf[8] + xf[10] * xf[10]) + 1e-6f, fx = xf[8] / hl, fz = xf[10] / hl, h3[3];
                unsigned sv = g_rayFlags;
                g_rayFlags = RAY_DONT_CHECK_CHARACTER;
                float a3[3] = {vp[0] + fx * 4.0f, vp[1] + 1.4f, vp[2] + fz * 4.0f}, b3[3] = {vp[0] + fx * 16.0f, vp[1] + 1.4f, vp[2] + fz * 16.0f};
                int frontHit = ray_cast(a3, b3, h3, NULL);
                float a4[3] = {vp[0] - fx * 4.0f, vp[1] + 1.4f, vp[2] - fz * 4.0f}, b4[3] = {vp[0] - fx * 16.0f, vp[1] + 1.4f, vp[2] - fz * 16.0f};
                int backHit = ray_cast(a4, b4, h3, NULL);
                g_rayFlags = sv;
                if (!frontHit) { H->botMove[i][0] = 0.6f; H->botMove[i][1] = 0; H->botMove[i][2] = 0; return; }
                if (!backHit) { H->botMove[i][0] = -0.7f; H->botMove[i][1] = 0; H->botMove[i][2] = 0; return; }
                if (ow[i][0] < 1e29f) want = atan2f(ow[i][0] - vp[0], ow[i][2] - vp[2]);
            }
        }
        err = want - heading;
        while (err > 3.14159265f) err -= 6.2831853f;
        while (err < -3.14159265f) err += 6.2831853f;
        float throttle, steer;
        if (fabsf(err) > 1.75f) {   // nose at the land: back away, swinging the nose round (reversing steers the other way)
            float back = err > 0 ? err - 3.14159265f : err + 3.14159265f;   // the stern's error
            throttle = -0.8f;
            steer = fmaxf(-1.0f, fminf(1.0f, -back * 1.5f));
        } else {
            throttle = fabsf(err) > 0.5f ? 0.5f : 0.9f;
            steer = fmaxf(-1.0f, fminf(1.0f, -err * 1.5f));
        }
        H->botMove[i][0] = throttle; H->botMove[i][1] = 0; H->botMove[i][2] = steer;
        return;
    }
    if (d->phase == 2 && d->taxi && !vehicle_is_boat(d->veh)) {   // a taxi: roads, 3 m clear - nothing else
        NavGrid *g = *nav_slot();
        taxi_masks();
        if (!g || !g_taxiRoad) { vehicle_mark_bad(d->veh, 90); drive_stop(i, "has no road map", 60); return; }
        const char *which = "the roads";
        d->n = 0;
        // the road map; the same without its stuck-spot strikes (a few bumps at a narrow HQ exit sealed both of Seine's HQs);
        // the ordinary vehicle map; the walking route (as a Pilot's tank) - drive somehow rather than get out
        static const char *const kPass[4] = {"road", "road (strikes ignored)", "vehicle-map", "walking"};
        int ss = strike_slot_of(g_taxiRoad);
        for (int pass = 0; pass < 4 && !d->n; pass++) {
            const unsigned char *m = pass == 0 ? g_taxiRoad : pass == 1 ? (ss >= 0 ? g_strikeBase[ss] : NULL) : pass == 2 ? drive_mask() : NULL;
            if (pass == 1 && !m) continue;
            float s[3], tgt[3];
            if (pass < 3) {
                int hs = m && nav_mask_nearest(g, m, vp[0], vp[2], 30.0f, s), ht = m && nav_mask_nearest(g, m, d->goal[0], d->goal[2], 80.0f, tgt);
                if (!hs || !ht) { bot_log("drive: %s's taxi: no %s ground %s", pl + OFF_PLAYER_ONLINE_NAME, kPass[pass], !hs ? "within 30 m of it" : "within 80 m of the goal"); continue; }
                nav_set_path_mask(m);
                nav_set_vehicle_half(2.5f);
            } else { memcpy(s, vp, 12); memcpy(tgt, d->goal, 12); }
            d->n = nav_find_path(g, ray_cast, vp[0], vp[1], vp[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS);
            if (!d->n && pass < 3) d->n = nav_find_path(g, ray_cast, s[0], s[1], s[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS);   // (from the road beside it)
            if (!d->n) bot_log("drive: %s's taxi: no %s route (%s)", pl + OFF_PLAYER_ONLINE_NAME, kPass[pass], nav_last_error());
            nav_set_vehicle_half(0);
            nav_set_path_mask(NULL);
            if (d->n) which = pass == 0 ? "the roads" : pass == 1 ? "the roads (past stuck spots)" : pass == 2 ? "the ordinary vehicle map" : "the walking route";
        }
        if (!d->n && d->flag >= 0 && d->flag < 16) {   // no road to this flag (Seine's quay): the others first for 2 min
            g_taxiNoFlagUntil[i][d->flag] = H->frame + 120 * 30;
            int other = 0;
            for (int f = 0; f < g_flagCount && f < 16; f++) if (f != d->flag && H->frame >= g_taxiNoFlagUntil[i][f]) other = 1;
            if (other) { bot_log("drive: %s's taxi has no road to flag %d - another flag", pl + OFF_PLAYER_ONLINE_NAME, d->flag); drive_stop(i, "no road to that flag", 1); return; }
        }
        if (!d->n) { vehicle_mark_bad(d->veh, 90); drive_stop(i, "found no road route", 60); return; }
        float stopS = d->flag < 0 ? 4.0f : PILOT_STOP_SHORT;
        for (int k = 0; k < d->n; k++) if (dist2(&d->pts[k * 3], d->goal) < stopS * stopS) { d->n = k + 1; break; }
        d->next = 0; d->phase = 3; d->at = d->checkAt = H->frame; d->stuckCount = 0;
        memcpy(d->lastPos, vp, 12);
        bot_log("drive: %s's taxi takes %s - %d waypoint(s) towards flag %d", pl + OFF_PLAYER_ONLINE_NAME, which, d->n, d->flag);
        return;
    }
    if (d->phase == 2) {   // route on the drivable grid
        NavGrid *g = *nav_slot();
        int boat = vehicle_is_boat(d->veh);
        const unsigned char *mask = boat ? boat_mask() : drive_mask();
        if (!g || !mask) { drive_stop(i, "has no drivable map", 60); return; }
        static unsigned char *clearMask;   // boats: the mask without the shallows (except at the start and the landing)
        static size_t clearSize;
        nav_set_path_mask(mask);
        // the drivable spot nearest the goal, at the goal's height (a town flag's own spot is often too cramped); a boat:
        // the water nearest it (the beach run - the last bit is swum / walked)
        float tgt[3] = {d->goal[0], d->goal[1], d->goal[2]}, gy = isnan(d->goal[1]) ? vp[1] : d->goal[1];
        int have = 0;
        if (boat) {   // the landing: the beach of the flag's own landmass that best trades boat time for the walk
            int goalLab = g_landLabels ? nav_label_at(g, g_landLabels, d->goal[0], isnan(d->goal[1]) ? vp[1] : d->goal[1], d->goal[2]) : 0;
            if (goalLab) have = nav_landing_on(g, mask, g_landLabels, goalLab, vp[0], vp[2], d->goal[0], d->goal[2], 0.6f, 60.0f, tgt);
            if (!have) have = nav_mask_nearest(g, mask, d->goal[0], d->goal[2], 300.0f, tgt);
        }
        if (boat && !have) { nav_set_path_mask(NULL); vehicle_mark_bad(d->veh, 120); drive_stop(i, "has no water near its flag", 60); return; }
        if (boat) bot_log("drive: %s lands the boat at (%.0f, %.0f), %.0f m from its flag", pl + OFF_PLAYER_ONLINE_NAME, tgt[0], tgt[2],
                          sqrtf(dist2(tgt, d->goal)));
        if (boat) {
            size_t n = (size_t)g->w * g->h * NAV_LAYERS;
            if (clearSize != n) { if (clearMask) HeapFree(GetProcessHeap(), 0, clearMask); clearMask = HeapAlloc(GetProcessHeap(), 0, n); clearSize = clearMask ? n : 0; }
            if (clearMask) {
                float keep[4] = {vp[0], vp[2], tgt[0], tgt[2]};
                memcpy(clearMask, mask, n);
                nav_mask_strip(g, clearMask, 4, keep, 2, 40.0f);
                nav_set_path_mask(clearMask);
            }
        }
        for (float r = 0; r <= 40.0f && !have; r += 10.0f)
            for (int a = 0; a < (r > 0 ? 8 : 1) && !have; a++) {
                float x = d->goal[0] + sinf(a * 0.785398f) * r, z = d->goal[2] + cosf(a * 0.785398f) * r;
                if (nav_snap(g, x, gy, z, tgt) && fabsf(tgt[1] - gy) < 4.0f) have = 1;
            }
        nav_set_long_legs(boat);
        double planT0 = perf_now();
        d->n = have ? nav_find_path(g, ray_cast, vp[0], vp[1], vp[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS) : 0;
        if (perf_now() - planT0 > 50.0) bot_log("perf: %s's vehicle route took %.0f ms (%d points)", pl + OFF_PLAYER_ONLINE_NAME, perf_now() - planT0, d->n);
        if (have && !d->n && boat) {   // no way round with 6 m to spare: the shallows too
            bot_log("drive: %s finds no route 6 m clear of land - allows the shallows", pl + OFF_PLAYER_ONLINE_NAME);
            nav_set_path_mask(mask);
            d->n = nav_find_path(g, ray_cast, vp[0], vp[1], vp[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS);
        }
        float ow[3];
        if (have && !d->n && boat && nav_open_water_near(g, mask, vp[0], vp[2], 80.0f, ow)) {   // docked in a pocket (pier posts):
            d->n = nav_find_path(g, ray_cast, ow[0], ow[1], ow[2], tgt[0], tgt[1], tgt[2], d->pts + 3, DRIVE_MAX_PTS - 1);   // out to open water first
            if (d->n) { memcpy(d->pts, ow, 12); d->n++; bot_log("drive: %s backs out to open water at (%.0f, %.0f) first", pl + OFF_PLAYER_ONLINE_NAME, ow[0], ow[2]); }
        }
        nav_set_path_mask(NULL);
        nav_set_long_legs(0);
        int keep = d->n;
        float stopShort = is_pilot(i) ? (d->flag < 0 ? 4.0f : PILOT_STOP_SHORT) : DRIVE_STOP_SHORT;   // (an overwatch spot: onto it)
        for (int k = 0; k < d->n && !boat; k++) if (dist2(&d->pts[k * 3], d->goal) < stopShort * stopShort) { keep = k + 1; break; }
        d->n = keep;
        if (!d->n && is_pilot(i) && !boat && have) {   // a Pilot's tank: no drivable-mask route - the looser tank mask (steeper ground,
            // routes through the middle of open ground, off rocks and walls)
            if (!tank_mask_now()) {
                int tn = 0;
                g_tankMask = nav_tank_mask(g, &tn);
                g_tankMaskFor = g->serial;
                bot_log("drive: tank mask for %s - %d node(s)", g->level, tn);
            }
            if (g_tankMask) {
                const char *why0 = nav_last_error();
                // other vehicles (the parked rows at the HQ, a team-mate stopped on the way) are no road: a copy of the
                // mask with 6 m round each struck off (the RU tanks jammed in their own tank park all round, 2026-10-13)
                int nn = g->w * g->h * NAV_LAYERS;
                unsigned char *vm = HeapAlloc(GetProcessHeap(), 0, (size_t)nn);
                if (vm) {
                    memcpy(vm, g_tankMask, (size_t)nn);
                    char **vs;
                    int nv = vehicle_list(&vs);
                    for (int k = 0; k < nv; k++) {
                        if (vs[k] == d->veh || !vehicle_ok(vs[k])) continue;
                        float *op = vehicle_pos(vs[k], NULL);
                        if (op && dist2(op, vp) > 8.0f * 8.0f && dist2(op, tgt) > 15.0f * 15.0f) nav_mask_block(g, vm, op[0], op[2], 6.0f);
                    }
                }
                nav_set_path_mask(vm ? vm : g_tankMask);
                d->n = nav_find_path(g, ray_cast, vp[0], vp[1], vp[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS);
                nav_set_path_mask(NULL);
                if (vm) HeapFree(GetProcessHeap(), 0, vm);
                float stopS = d->flag < 0 ? 4.0f : PILOT_STOP_SHORT;
                for (int k = 0; k < d->n; k++) if (dist2(&d->pts[k * 3], d->goal) < stopS * stopS) { d->n = k + 1; break; }
                if (d->n) bot_log("drive: %s has no drivable-mask route (%s) - takes the tank-mask route (%d points)", pl + OFF_PLAYER_ONLINE_NAME, why0, d->n);
            }
        }
        if (!d->n && is_pilot(i) && !boat && have) {   // a Pilot's tank: no drivable-mask route - the infantry route (it goes round
            // the mountains; on open ground a tank follows it fine: Alborz's steep snow left only drivable islands)
            const char *why = nav_last_error();
            d->n = nav_find_path(g, ray_cast, vp[0], vp[1], vp[2], tgt[0], tgt[1], tgt[2], d->pts, DRIVE_MAX_PTS);
            float stopS = d->flag < 0 ? 4.0f : PILOT_STOP_SHORT;
            for (int k = 0; k < d->n; k++) if (dist2(&d->pts[k * 3], d->goal) < stopS * stopS) { d->n = k + 1; break; }
            if (d->n) bot_log("drive: %s has no drivable-mask route (%s) - takes the infantry route (%d points)", pl + OFF_PLAYER_ONLINE_NAME, why, d->n);
        }
        if (!d->n && is_pilot(i) && !boat) {   // a Pilot's tank: no mapped route (Alborz's steep snow left only drivable islands,
            // every tank sat at its HQ) - cross-country straight at the goal in 30 m legs; the obstacle sensors and the
            // stuck handling steer round what is in the way
            float gx = d->goal[0] - vp[0], gz = d->goal[2] - vp[2], gl = sqrtf(gx * gx + gz * gz);
            float stopS = d->flag < 0 ? 4.0f : PILOT_STOP_SHORT;
            if (gl > stopS + 5.0f) {
                int legs = (int)((gl - stopS) / 30.0f) + 1;
                if (legs > DRIVE_MAX_PTS) legs = DRIVE_MAX_PTS;
                for (int k = 0; k < legs; k++) {
                    float t = (float)(k + 1) / legs * (gl - stopS) / gl;
                    d->pts[k * 3] = vp[0] + gx * t; d->pts[k * 3 + 1] = vp[1] + (d->goal[1] - vp[1]) * t; d->pts[k * 3 + 2] = vp[2] + gz * t;
                }
                d->n = legs;
                bot_log("drive: %s has no mapped route (%s) - goes cross-country, %d legs, %.0f m", pl + OFF_PLAYER_ONLINE_NAME, nav_last_error(), legs, gl);
            }
        }
        if (!d->n) {
            bot_log("drive: no drivable route from (%.0f, %.0f) to (%.0f, %.0f): %s", vp[0], vp[2], d->goal[0], d->goal[2], nav_last_error());
            vehicle_mark_bad(d->veh, 90);
            drive_stop(i, "found no drivable route", 60);
            return;
        }
        d->next = 0; d->phase = 3; d->at = d->checkAt = H->frame; d->stuckCount = 0;
        memcpy(d->lastPos, vp, 12);
        bot_log("drive: %s drives %d waypoint(s) towards flag %d", pl + OFF_PLAYER_ONLINE_NAME, d->n, d->flag);
        return;
    }
    // driving
    if (H->frame - d->at > 180 * 30) { drive_stop(i, "took too long - gets out", 30); return; }
    {
        VehTrack *t = vehicle_sample(d->veh);
        float hp = *(float *)(d->veh + OFF_VEHICLE_HEALTH), mx = t && t->maxSeen > 0 ? t->maxSeen : 0;
        if (mx > 0 && hp < 0.1f * mx) { drive_stop(i, "bails out - the vehicle is nearly wrecked", 20); return; }
    }
    {
        float *hx = vehicle_xf(d->veh);
        while (d->next < d->n) {
            float *w = &d->pts[d->next * 3], dx = w[0] - vp[0], dz = w[2] - vp[2], dd = dx * dx + dz * dz;
            int behind = hx && dd < 15.0f * 15.0f && dx * hx[8] + dz * hx[10] < 0;   // already driven past it
            float wr = vehicle_is_boat(d->veh) ? 12.0f : DRIVE_WAYPOINT;   // boats: no fussing over a waypoint
            if (dd < wr * wr || behind) d->next++;
            else break;
        }
    }
    float stopAt = is_pilot(i) ? (d->flag < 0 ? 4.0f : PILOT_STOP_SHORT) : DRIVE_STOP_SHORT;
    if (d->next >= d->n || (!vehicle_is_boat(d->veh) && dist2(vp, d->goal) < stopAt * stopAt)) {
        if (vehicle_is_boat(d->veh)) vehicle_mark_bad(d->veh, 180);   // (landed: the rest is on foot / by land vehicle)
        drive_stop(i, "arrived - gets out", 20);
        return;
    }
    float *xf = vehicle_xf(d->veh);
    if (!xf) { drive_stop(i, "cannot read the vehicle's heading", 60); return; }
    float *wp = &d->pts[d->next * 3];
    float aim[3];
    {   // the point `look` metres along the route from here (through the waypoints ahead)
        int boatAim = vehicle_is_boat(d->veh);
        float look = fmaxf(boatAim ? 14.0f : 10.0f, fminf(boatAim ? 40.0f : 28.0f, 8.0f + d->speed * 1.2f));
        float from[3] = {vp[0], vp[1], vp[2]};
        memcpy(aim, wp, 12);
        for (int k = d->next; k < d->n; k++) {
            float *w = &d->pts[k * 3], seg = sqrtf((w[0] - from[0]) * (w[0] - from[0]) + (w[2] - from[2]) * (w[2] - from[2]));
            if (seg >= look) {
                float t = look / fmaxf(seg, 1e-3f);
                aim[0] = from[0] + (w[0] - from[0]) * t; aim[1] = w[1]; aim[2] = from[2] + (w[2] - from[2]) * t;
                break;
            }
            look -= seg;
            memcpy(aim, w, 12); memcpy(from, w, 12);
        }
    }
    float heading = atan2f(xf[8], xf[10]), want = atan2f(aim[0] - vp[0], aim[2] - vp[2]);
    float err = want - heading;
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
    float steer = fmaxf(-1.0f, fminf(1.0f, -err * 1.5f));
    // speed (m/s) from the position every 1/3 s
    if (!d->spAt) { memcpy(d->spPos, vp, 12); d->spAt = H->frame; }
    else if (H->frame - d->spAt >= 10) {
        d->speed = sqrtf(dist2(vp, d->spPos)) / ((H->frame - d->spAt) / 30.0f);
        memcpy(d->spPos, vp, 12); d->spAt = H->frame;
    }
    // how fast to go: no limit on a straight - flat out - only slowing in time for the next bend (and the end of the
    // route): every waypoint ahead allows a speed for its turn, and the bot brakes so as to get there at that speed.
    int isBoat = vehicle_is_boat(d->veh);
    float ae = fabsf(err), vwant = 99.0f;
    if (ae > 0.6f) vwant = isBoat ? 10.0f : 8.0f;    // more than 35 degrees off: turning now (small corrections keep speed)
    if (ae > 1.05f) vwant = isBoat ? 5.0f : 3.0f;    // more than 60 degrees off: slow round
    {
        const float decel = isBoat ? 3.0f : 4.0f;    // m/s per second it can shed (rough)
        float along = sqrtf(dist2(wp, vp)), prevA = atan2f(wp[0] - vp[0], wp[2] - vp[2]);
        for (int k = d->next; k < d->n && along < 200.0f; k++) {
            float *w = &d->pts[k * 3], vt;
            if (k == d->n - 1) vt = isBoat ? 6.0f : 4.0f;   // the end: arrive slowly
            else {
                float *w2 = &d->pts[(k + 1) * 3], a2 = atan2f(w2[0] - w[0], w2[2] - w[2]), turn = a2 - prevA;
                while (turn > 3.14159265f) turn -= 6.2831853f;
                while (turn < -3.14159265f) turn += 6.2831853f;
                float at = fabsf(turn);
                vt = at > 1.05f ? (isBoat ? 8.0f : 5.0f) : at > 0.6f ? (isBoat ? 12.0f : 9.0f) : at > 0.3f ? (isBoat ? 18.0f : 14.0f) : 99.0f;
                prevA = a2;
            }
            float lead = fmaxf(0.0f, along - (isBoat ? 12.0f : 6.0f));   // (it turns a little before the waypoint)
            float v = sqrtf(vt * vt + 2.0f * decel * lead);
            if (v < vwant) vwant = v;
            if (k + 1 < d->n) along += sqrtf(dist2(&d->pts[(k + 1) * 3], w));
        }
    }
    // a driver lining up its main gun on a target slows to a crawl - a rocking hull throws the shot off
    if (g_driving[i] && g_gunWeapon[i] && (g_gunTarget[i] || g_gunTargetVeh[i]) && g_gunAimErr[i] < 0.25f && g_gunTgtDist[i] < 120.0f)
        vwant = fminf(vwant, is_pilot(i) && d->flag >= 0 && flag_owner(d->flag) != player_team(pl) ? 8.0f : 3.0f);   // a Pilot taking a flag
        // shoots on the move (crawling at 3 m/s in every tank fight, it never got to the flag - the user, 2026-10-13)
    {   // bumper: anything ahead at 1 m up?
        float hx = xf[8], hz = xf[10], hl = sqrtf(hx * hx + hz * hz) + 1e-6f, hit[3];
        unsigned saved = g_rayFlags;
        g_rayFlags = RAY_DONT_CHECK_CHARACTER | RAY_CHECK_DETAIL_MESH;   // (trees too)
        float fx = hx / hl, fz = hz / hl, rx = fz, rz = -fx;   // forward, right (in the ground plane)
        float sideNear[3] = {99, 99, 99};                       // nearest thing ahead on the left, in the middle, on the right
        static const float kOff[7] = {-1.2f, -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 1.2f};   // 7 sensors across the front (palm trunks, poles)
        void *ownBody = !IsBadReadPtr(d->veh + 0x30, 4) ? *(void **)(d->veh + 0x30) : NULL;
        void *lastBody = NULL;
        unsigned lastVt = 0, lastMat = 0;
        float lastNy = 0, nrm[3];
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 2; k++) {
                float off = kOff[r], up = isBoat ? (k ? 2.2f : 1.4f) : k ? 1.0f : 0.5f;
                int s = r < 3 ? 0 : r == 3 ? 1 : 2;
                float lead = isBoat ? 5.0f + d->speed * 0.15f : 2.5f;   // (from in front of the hull - a RHIB is ~7 m long; at speed its position lags)
                float a[3] = {vp[0] + fx * lead + rx * off, vp[1] + up, vp[2] + fz * lead + rz * off};
                float b[3] = {a[0] + fx * 10.0f, a[1], a[2] + fz * 10.0f};
                if (ray_cast(a, b, hit, nrm)) {
                    char *body = *(char **)g_lastHitRest;
                    unsigned bvt = body && !IsBadReadPtr(body, 4) ? *(unsigned *)body : 0;
                    if (body == ownBody || bvt == ADDR_WATER_PHYSICS_VTABLE) continue;   // its own hull / the water (waves)
                    if (nrm[1] > 0.65f) continue;   // ground rising ahead (a slope, a ramp, a kerb top): drivable, not a wall
                    float dh = sqrtf(dist2(hit, a));
                    if (dh < sideNear[s]) { sideNear[s] = dh; lastBody = body; lastVt = bvt; lastMat = hit_material(); lastNy = nrm[1]; }
                }
            }
        static unsigned landSeenAt[MAX_BOTS];   // (not at its landing, not still by where it set off - inside the LHD the hull read as land
                                                //  and it reversed into the same corner twice a second; once in 5 s)
        if (isBoat && d->phase == 3 && (H->frame + i) % 15 == 0 && g_boatMask && d->n > 0 && dist2(vp, &d->pts[(d->n - 1) * 3]) > 45.0f * 45.0f &&
            dist2(vp, d->start) > 40.0f * 40.0f && H->frame - landSeenAt[i] > 5 * 30) {   // land ahead under the bow (a low beach: the bumper
            // takes rising ground for a slope - Wake's small spawn island beached two RHIBs, 2026-10-13): off the water map, re-route
            float ahead = 12.0f + d->speed * 0.6f;
            for (int r = -1; r <= 1; r++) {
                float px = vp[0] + fx * ahead + rx * r * 4.0f, pz = vp[2] + fz * ahead + rz * r * 4.0f;
                float a2[3] = {px, vp[1] + 6.0f, pz}, b2[3] = {px, vp[1] - 8.0f, pz}, h2[3];
                g_rayFlags = RAY_DONT_CHECK_CHARACTER;
                int hit = ray_cast(a2, b2, h2, NULL);
                char *bd = hit ? *(char **)g_lastHitRest : NULL;
                int water = bd && !IsBadReadPtr(bd, 4) && *(unsigned *)bd == ADDR_WATER_PHYSICS_VTABLE;
                if (hit && !water && h2[1] > vp[1] - 1.5f) {
                    int k = drive_strike(g_boatMask, px, pz, 8.0f);
                    bot_log("drive: %s's boat sees land %.0f m ahead at (%.0f, %.0f) - %d node(s) off the water map, re-routes", pl + OFF_PLAYER_ONLINE_NAME,
                            ahead, px, pz, k);
                    d->phase = 2; d->backOutUntil = H->frame + 45; landSeenAt[i] = H->frame;
                    break;
                }
            }
        }
        g_rayFlags = saved;
        float nearest = fminf(sideNear[1], fminf(sideNear[0], sideNear[2]));
        d->frontNear = nearest;
        d->sideL = sideNear[0]; d->sideR = sideNear[2]; d->midNear = sideNear[1];
        if (isBoat && nearest >= 5.0f) { nearest = 99; sideNear[0] = sideNear[1] = sideNear[2] = 99; }   // boats: only what is right ahead
        if (nearest < (isBoat ? 5.0f : 6.0f) && H->frame - d->sensorLogAt > 60) {
            d->sensorLogAt = H->frame;
            bot_log("drive: %s's %s sees something ahead at (%.0f, %.0f): left %.1f mid %.1f right %.1f m (vt %08x mat %08x normal-y %.2f, %.0f m/s)",
                    pl + OFF_PLAYER_ONLINE_NAME, vehicle_name(d->veh), vp[0], vp[2], sideNear[0], sideNear[1], sideNear[2], lastVt, lastMat, lastNy, d->speed);
        }
        if (isBoat && !d->backedOut) {   // (departure, phase 9, has done this)
            d->backedOut = 1;
            if (nearest < 8.0f) {
                d->backOutUntil = H->frame + 90;
                bot_log("drive: %s backs the boat out of its mooring", pl + OFF_PLAYER_ONLINE_NAME);
            }
        }
        if (nearest < 10.0f) vwant = fminf(vwant, nearest < 4.0f ? 1.5f : 3.0f);
        // a pole / corner off to one side: ease away from it (right corner blocked -> steer left, and the other way)
        if (sideNear[1] > 6.0f && (sideNear[0] < 6.0f) != (sideNear[2] < 6.0f)) {
            float away = sideNear[2] < 6.0f ? 0.6f : -0.6f;   // positive EIAYaw turns towards a lower heading (left of a +right vector)
            steer = fmaxf(-1.0f, fminf(1.0f, steer + away));
        }
        // something dead ahead (a tree, a post): swerve to the clearer side; boxed in on all three: stop (the stuck check reverses)
        if (sideNear[1] < 8.0f) {
            if (sideNear[0] < 4.0f && sideNear[2] < 4.0f) vwant = 0.0f;
            else steer = sideNear[0] > sideNear[2] ? 0.85f : -0.85f;   // left clearer -> left (positive), right clearer -> right
        }
    }
    float throttle = d->speed < vwant - 1.0f ? (ae < 0.6f ? 1.0f : 0.6f) : d->speed > vwant + 3.0f ? -0.4f : 0.6f;
    if (vwant <= 0.0f) throttle = d->speed > 0.5f ? -0.4f : 0.0f;   // boxed in: stop
    // (dead ahead only: a building alongside the front corner while pulling away slowly is not "nosed into it")
    if (H->frame >= d->reverseUntil && H->frame >= d->backOutUntil && d->speed < 1.0f && (vwant <= 0.0f || d->midNear < 2.0f)) {
        if (!d->boxedSince) d->boxedSince = H->frame;
        else if (H->frame - d->boxedSince >= 15) {   // nose against something: back off straight away (not after the 3 s check)
            d->boxedSince = 0;
            d->reverseUntil = H->frame + 60;
            d->checkAt = H->frame; memcpy(d->lastPos, vp, 12);
            // swing the nose ~60 deg away from the blocked side while reversing (pointing it back along the route just
            // lined up the same corner again: a buggy nosed into one building corner 11 times in a minute)
            int leftBlocked = d->sideL < d->sideR;
            d->awayOn = 1;
            d->awayHeading = heading + (leftBlocked ? 1.0f : -1.0f);   // (+ = towards the right side vector)
            if (d->nudges && dist2(vp, d->nudgePos) < 8.0f * 8.0f) d->nudges++;   // (8 m: rocking off a corner moves it a few m)
            else { d->nudges = 1; memcpy(d->nudgePos, vp, 12); }
            bot_log("drive: %s is nosed up against something at (%.0f, %.0f) - backs off, nose to the %s (%d here)", pl + OFF_PLAYER_ONLINE_NAME,
                    vp[0], vp[2], leftBlocked ? "right" : "left", d->nudges);
            unsigned char *sm = vehicle_is_boat(d->veh) ? g_boatMask : g_driveMask;
            if (d->nudges >= 2 && sm) {   // again here: that front corner is no road - off the map, new route
                d->nudges = 0;
                if (++d->reroutes > 2) { vehicle_mark_bad(d->veh, 60); drive_stop(i, "keeps getting stuck - gets out and walks", 30); return; }
                float hx = xf[8], hz = xf[10], hl = sqrtf(hx * hx + hz * hz) + 1e-6f, fx = hx / hl, fz = hz / hl, rx = fz, rz = -fx;
                float side = leftBlocked ? -1.0f : 1.0f;
                float ax = vp[0] + fx * 4.0f + rx * side * 1.5f, az = vp[2] + fz * 4.0f + rz * side * 1.5f;
                int k = drive_strike(sm, ax, az, 3.0f);
                if (tank_mask_now() && !vehicle_is_boat(d->veh)) k += drive_strike(g_tankMask, ax, az, 4.0f);   // (a pilot's route map too)
                if (d->taxi) { k += drive_strike(g_taxiRoad, ax, az, 4.0f); k += drive_strike(g_taxiAny, ax, az, 4.0f); }
                bot_log("drive: (%.0f, %.0f) struck off the drivable map (%d node(s)); %s re-routes round it", ax, az, k, pl + OFF_PLAYER_ONLINE_NAME);
                d->phase = 2;
            }
        }
    } else d->boxedSince = 0;
    if (drive_pickup(i, d, vp)) {   // stopped for pick-ups: brake, hold still (not "stuck"), the drive clock paused
        throttle = d->speed > 0.5f ? -0.6f : 0.0f;
        steer = 0;
        d->checkAt = H->frame; memcpy(d->lastPos, vp, 12);
        d->at++;
    }
    if (H->frame < d->backOutUntil) { throttle = -0.9f; steer = 0; d->checkAt = H->frame; memcpy(d->lastPos, vp, 12); }   // straight back
    else if (H->frame < d->reverseUntil) {   // backing off: steer so the nose swings round to the route (the road) ahead
        throttle = -0.8f;
        float e = err;
        if (d->awayOn) {   // (a quick back-off: away from what it hit instead)
            e = d->awayHeading - heading;
            while (e > 3.14159265f) e -= 6.2831853f;
            while (e < -3.14159265f) e += 6.2831853f;
        }
        steer = fmaxf(-1.0f, fminf(1.0f, e * 1.5f));   // (reversing turns the nose the other way round)
    }
    if (H->frame >= d->reverseUntil) d->awayOn = 0;
    // every 3 s while driving forwards (not mid back-off): getting anywhere? (2026-10-12: this was chained as the "else" of
    // the line above, so it only ran while already reversing - a vehicle wedged going forwards held full throttle forever)
    if (H->frame >= d->reverseUntil && H->frame - d->checkAt >= 90) {
        float dx = vp[0] - d->lastPos[0], dz = vp[2] - d->lastPos[2];
        if (dx * dx + dz * dz < 1.5f * 1.5f) {
            if (dist2(vp, d->goal) < 60.0f * 60.0f) { drive_stop(i, "is close enough - gets out", 20); return; }   // (the last bit on foot)
            if (++d->stuckCount > 4) { drive_stop(i, "is stuck - gets out and walks", 30); return; }
            d->reverseUntil = H->frame + 75;
            d->reverseSteer = -steer;
            unsigned char *sm = vehicle_is_boat(d->veh) ? g_boatMask : g_driveMask;   // the map this vehicle drives on
            // a stuck back-off is a bump at this spot too: forward into a corner, back off 2 m, forward into it again
            // never made two stuck checks in a row (Pilot RU 4's Vodnik on a big coal truck's corners, 2026-10-13)
            if (d->nudges && dist2(vp, d->nudgePos) < 8.0f * 8.0f) d->nudges++;
            else { d->nudges = 1; memcpy(d->nudgePos, vp, 12); }
            if ((d->stuckCount == 2 || d->nudges >= 2) && sm) {   // stuck again: the spot is no road - strike it off for everyone, re-route
                d->nudges = 0;
                if (++d->reroutes > 2) { vehicle_mark_bad(d->veh, 60); drive_stop(i, "keeps getting stuck - gets out and walks", 30); return; }
                float ax = vp[0] + xf[8] * 6.0f, az = vp[2] + xf[10] * 6.0f;   // ahead of it (its own spot stays drivable)
                int k = drive_strike(sm, ax, az, 3.0f);
                if (tank_mask_now() && !vehicle_is_boat(d->veh)) k += drive_strike(g_tankMask, ax, az, 4.0f);   // (a pilot's route map too)
                if (d->taxi) { k += drive_strike(g_taxiRoad, ax, az, 5.0f); k += drive_strike(g_taxiAny, ax, az, 5.0f); }
                if (k == 0) {   // already struck and still stuck there: what holds it is beside / under the front - wider
                    ax = vp[0] + xf[8] * 4.5f; az = vp[2] + xf[10] * 4.5f;
                    k = drive_strike(sm, ax, az, 5.0f);
                    if (tank_mask_now() && !vehicle_is_boat(d->veh)) k += drive_strike(g_tankMask, ax, az, 6.0f);
                }
                d->reverseUntil = H->frame + (d->reroutes > 1 ? 150 : 105);   // a longer back-off before the new route
                bot_log("drive: (%.0f, %.0f) struck off the drivable map (%d node(s)); %s re-routes", ax, az, k, pl + OFF_PLAYER_ONLINE_NAME);
                d->phase = 2;
            }
            bot_log("drive: %s is stuck at (%.0f, %.0f) - reversing (wanted %.1f m/s, throttle %.2f, steer %.2f)", pl + OFF_PLAYER_ONLINE_NAME, vp[0], vp[2],
                    vwant, throttle, steer);
        } else d->stuckCount = 0;
        memcpy(d->lastPos, vp, 12);
        d->checkAt = H->frame;
    }
    H->botMove[i][0] = throttle;
    H->botMove[i][1] = 0;
    H->botMove[i][2] = steer;
    if (g_gunDebug && H->frame % 30 == 0)
        bot_log("drivedbg: %s phase %d wp %d/%d speed %.1f want %.1f throttle %.2f steer %.2f err %.2f | front L %.1f M %.1f R %.1f near %.1f boxed %u rev %d back %d",
                pl + OFF_PLAYER_ONLINE_NAME, d->phase, d->next, d->n, d->speed, vwant, throttle, steer, err, d->sideL, d->midNear, d->sideR, d->frontNear,
                d->boxedSince ? (H->frame - d->boxedSince) / 30 : 0, H->frame < d->reverseUntil, H->frame < d->backOutUntil);
}

// ---- Tickets (ServerTicketCounterEntity, one per team; +0xa0 = tickets, float - what RCON serverInfo reports).
// The game runs the flag bleed itself only in some rounds (not after a rotation with bots only, seen overnight), and
// it never charges bot deploys (bots spawn through BotMod, not the deploy screen). So BotMod charges every bot deploy
// (a bot with a new soldier; revives keep the soldier and stay free), runs the Conquest bleed itself while the game's
// own is dormant (a team holding a majority of the flags bleeds the other 15 tickets/min per flag of difference,
// double when it holds none), and ends the round at 0 if the game has not done so 10 s later. ----
#define ADDR_TICKET_COUNTER_VTABLE 0x0268bc30
#define OFF_TC_DATA                0x14
#define OFF_TCDATA_TEAM            0x6c
#define OFF_TC_TICKETS             0xa0
#define OFF_TC_INITIAL             0xa8        // int: the round's starting ticket count
#define TICKET_BLEED_PER_MIN       15.0f
#define GAME_BLEED_GRACE_FRAMES    (20 * 30)   // the game moved the counter this recently: its bleed is running
static char *g_ticketCounter[3];
static float g_ticketsSet[3];                  // value after BotMod's last change: any other change is the game's
static unsigned g_gameTicketsAt[3];            // frame the game last changed a team's tickets
static int g_ticketsOn, g_roundEnded;
static unsigned g_zeroAt, g_ticketsLookAt, g_ticketsFoundAt, g_roundEndedAt;

static int g_ticketsTries;
static char *g_lastSoldierOf[64];
static char *g_lastPlayerAt[64];
static void tickets_reset(void)
{
    g_ticketCounter[1] = g_ticketCounter[2] = NULL;
    g_ticketsOn = 0; g_roundEnded = 0; g_zeroAt = 0; g_ticketsTries = 0; g_roundEndedAt = 0;

    g_gameTicketsAt[1] = g_gameTicketsAt[2] = 0;
    memset(g_lastSoldierOf, 0, sizeof g_lastSoldierOf);
    memset(g_lastPlayerAt, 0, sizeof g_lastPlayerAt);
}
static void tickets_find(void)   // (level ready: a quick scan for two objects)
{
    char *tc[16];
    int n = scan_objects(ADDR_TICKET_COUNTER_VTABLE, tc, 16);   // (stale copies from earlier rounds come first)
    for (int k = 0; k < n; k++) {
        if (IsBadReadPtr(tc[k], OFF_TC_TICKETS + 4)) continue;
        char *data = *(char **)(tc[k] + OFF_TC_DATA);
        int team = data && !IsBadReadPtr(data + OFF_TCDATA_TEAM, 4) && !IsBadReadPtr(data, 4) && *(unsigned *)data == 0x0250bf04 ?
                   *(int *)(data + OFF_TCDATA_TEAM) : 0;   // (data must really be a TicketCounterEntityData)
        if (team == 1 || team == 2) { g_ticketCounter[team] = tc[k]; g_ticketsSet[team] = *(float *)(tc[k] + OFF_TC_TICKETS); }
    }
    g_ticketsOn = g_ticketCounter[1] && g_ticketCounter[2];
    g_ticketsFoundAt = H->frame;
    bot_log("tickets: %s (US %.0f, RU %.0f)", g_ticketsOn ? "found - BotMod charges bot deploys (and bleeds while the game does not)" :
            "no ticket counters on this level", g_ticketsOn ? g_ticketsSet[1] : 0.0f, g_ticketsOn ? g_ticketsSet[2] : 0.0f);
}
static float *tickets_of(int team) { return (float *)(g_ticketCounter[team] + OFF_TC_TICKETS); }
static void tickets_take(int team, float n)
{
    if (!g_ticketsOn || g_roundEnded || (team != 1 && team != 2)) return;
    float *t = tickets_of(team);
    *t = *t - n > 0 ? *t - n : 0;
    g_ticketsSet[team] = *t;
}
static void tickets_tick(void)
{
    if (!g_ticketsOn && !g_roundEnded && g_level[0] && H->frame - g_ticketsLookAt > 5 * 30) {   // counters not there at level ready
        g_ticketsLookAt = H->frame;                                                              // (round restart): look again
        if (g_ticketsTries++ < 12) tickets_find();
    }
    if (g_roundEnded && g_roundEndedAt && H->frame - g_roundEndedAt > 15 * 30) {   // endRound is ignored on this server
        g_roundEndedAt = 0;                                                         // (pre-round disabled): next map
        bot_log("tickets: the round did not end - starting the next map");
        chat_rcon("mapList.runNextRound");
    }
    if (!g_ticketsOn || g_roundEnded) return;
    char *pm = player_manager();
    if (!pm) return;
    for (int team = 1; team <= 2; team++) {   // the game's own changes (its bleed, human deploys)
        float v = *tickets_of(team);
        if (fabsf(v - g_ticketsSet[team]) > 0.001f) { g_gameTicketsAt[team] = H->frame ? H->frame : 1; g_ticketsSet[team] = v; }
    }
    void **arr = *(void ***)(pm + 0xac);
    unsigned count = *(unsigned *)(pm + 8);
    for (unsigned k = 0; k < count && k < 64; k++) {   // bot deploys: a bot with a new soldier
        char *pl = arr[k], *s = pl ? *(char **)(pl + OFF_PLAYER_SOLDIER) : NULL;
        if (pl != g_lastPlayerAt[k]) { g_lastPlayerAt[k] = pl; g_lastSoldierOf[k] = s; continue; }
        if (s && s != g_lastSoldierOf[k] && is_bot(pl)) tickets_take(player_team(pl), 0.5f);   // half a human's (bots die easier and the
                                                                                                // rounds went too fast - the user, 2026-10-13)
        g_lastSoldierOf[k] = s;
    }
    if (H->frame % 30) return;
    int held[3] = {0, 0, 0}, total = 0;   // bleed, once a second, while the game's is not running
    for (int f = 0; f < g_flagCount; f++) { int o = flag_owner(f); total++; if (o == 1 || o == 2) held[o]++; }
    for (int team = 1; team <= 2; team++) {
        int other = 3 - team;
        if (held[other] * 2 <= total || held[other] <= held[team]) continue;   // no majority against it
        if (g_gameTicketsAt[team] && H->frame - g_gameTicketsAt[team] < GAME_BLEED_GRACE_FRAMES) continue;   // the game bleeds it
        float perMin = TICKET_BLEED_PER_MIN * (float)(held[other] - held[team]) * (held[team] == 0 ? 2.0f : 1.0f);
        tickets_take(team, perMin / 60.0f);
    }
    for (int team = 1; team <= 2; team++) {   // out of tickets: the game should end the round; if not, BotMod does
        int initial = *(int *)(g_ticketCounter[team] + OFF_TC_INITIAL);   // a team that has tickets at all (starting count)
        if (*tickets_of(team) > 0 || initial <= 0 || H->frame - g_ticketsFoundAt < 10 * 30) continue;
        if (!g_zeroAt) { g_zeroAt = H->frame ? H->frame : 1; bot_log("tickets: %s out of tickets", team == 1 ? "US" : "RU"); }
        else if (H->frame - g_zeroAt > 10 * 30) {
            int winner = 3 - team;
            g_roundEnded = 1;
            g_roundEndedAt = H->frame ? H->frame : 1;
            bot_log("tickets: the round did not end by itself - ending it (%s win)", winner == 1 ? "US" : "RU");
            chat_say("[BotMod] %s are out of tickets - %s win!", team == 1 ? "US" : "RU", winner == 1 ? "US" : "RU");
            chat_rcon("mapList.endRound\t%d", winner);
        }
        break;
    }
}

// ---- Swimming: bot-seconds spent in the water (nav water nodes), per level, on the perf line ----
static unsigned g_swimSeconds, g_swimSecondsLevel;
static void swim_tick(void)
{
    NavGrid *g = *nav_slot();
    if (H->frame % 30 || !g || g->phase != 2) return;
    for (int i = 0; i < H->botCount; i++) {
        char *pl = H->bots[i], *s = *(char **)(pl + OFF_PLAYER_SOLDIER);
        if (!s || player_vehicle(pl)) continue;
        float *p = (float *)(s + OFF_SOLDIER_POSITION);
        if (nav_is_water(g, p[0], p[1], p[2])) { g_swimSeconds++; g_swimSecondsLevel++; }
    }
}

static void inwatch_tick(void)
{
    if (!g_inWatchPl || H->frame > g_inWatchUntil || !player_exists(g_inWatchPl)) { g_inWatchPl = NULL; return; }
    char *in = *(char **)(g_inWatchPl + OFF_PLAYER_INPUT), *s = *(char **)(g_inWatchPl + OFF_PLAYER_SOLDIER);
    if (!in || IsBadReadPtr(in, 0x10 + 80 * 4)) return;
    char out[600];
    int k = 0;
    for (int a = 0; a < 80; a++) {
        float v = *(float *)(in + 0x10 + a * 4);
        if (fabsf(v - g_inWatchPrev[a]) > 0.01f && k < (int)sizeof out - 30) k += snprintf(out + k, sizeof out - k, " [%d] %.2f->%.2f", a, g_inWatchPrev[a], v);
        g_inWatchPrev[a] = v;
    }
    if (!IsBadReadPtr(in, 0x400))   // raw words outside the level floats (+0x10..+0x150): flags / edges
        for (int w = 0; w < 256; w++) {
            unsigned v = *(unsigned *)(in + w * 4);
            if ((w * 4 < 0x10 || w * 4 >= 0x150) && v != g_inWatchRaw[w] && k < (int)sizeof out - 30) k += snprintf(out + k, sizeof out - k, " +%03x:%08x->%08x", w * 4, g_inWatchRaw[w], v);
            g_inWatchRaw[w] = v;
        }
    int pose = s ? *(int *)(s + OFF_SOLDIER_POSE) : -1, want = s ? *(int *)(s + OFF_SOLDIER_POSE + 4) : -1;
    if (pose * 10 + want != g_inWatchPose && k < (int)sizeof out - 40) { k += snprintf(out + k, sizeof out - k, " | stance %d (to %d)", pose, want); g_inWatchPose = pose * 10 + want; }
    if (k) bot_log("inwatch:%s", out);
}

__declspec(dllexport) void botmod_tick(void)
{
    inwatch_tick();
    {
        char who[64], text[256];
        for (int k = 0; k < 4 && chat_poll(who, sizeof who, text, sizeof text); k++) chat_command(who, text);
    }
    double tStart = perf_now(), tMark = tStart;
    perf_report();
    validate_bots();
    // No bots wanted and none here (e.g. a vehicles-only instance with botcount_<port>.txt = 0): idle - chat commands
    // (above) and the roster (so "!bots 8" still works) only; no nav building, surveys, level discovery or scans.
    if (*wanted_bots() == 0 && H->botCount == 0) {
        static unsigned idleLogged;
        if (!idleLogged) { idleLogged = 1; bot_log("idle: no bots wanted on this server - BotMod stays out of the way (\"!bots <n>\" to start)"); }
        roster_tick();
        return;
    }
    missile_tick();
    for (int i = 0; i < H->botCount; i++) {
        void *in = H->botInput[i];
        if (!in) continue;
        if (g_flying[i]) fly_tick(i);
        else if (g_drive[i].phase) drive_tick(i);
        steer_bot(i, *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER));
        if (g_drive[i].phase == 5 && is_pilot(i) && !player_vehicle(H->bots[i])) H->botMove[i][0] *= 0.5f;   // a Pilot walks to its ride
        if (level_ready()) { gunner_tick(i); fall_tick(i); }
        if (player_vehicle(H->bots[i]) && g_driving[i]) {   // a driver: S is the brake / reverse (EIABrake), not a negative throttle
            float t = H->botMove[i][0];
            ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_THROTTLE, t > 0 ? t : 0.0f);
            ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_BRAKE, t < 0 ? -t : 0.0f);
        } else ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_THROTTLE, H->botMove[i][0]);
        ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_STRAFE, H->botMove[i][1]);
        ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_YAW, H->botMove[i][2]);
        ((SetFloat_t)ADDR_ENTRYINPUT_SET_DT)(in, BOT_TICK_DT);
        apply_presses(i, in);
        if (H->frame < g_sprintUntil[i]) { *(unsigned *)((char *)in + OFF_INPUT_FLAGS_A) |= SPRINT_BIT_A; *(unsigned *)((char *)in + OFF_INPUT_FLAGS_B) |= SPRINT_BIT_B; }
        else { *(unsigned *)((char *)in + OFF_INPUT_FLAGS_A) &= ~SPRINT_BIT_A; *(unsigned *)((char *)in + OFF_INPUT_FLAGS_B) &= ~SPRINT_BIT_B; }
        {   // repairing: trigger held down continuously while in contact with the vehicle, released the moment that ends
            static unsigned char fireHeld[MAX_BOTS];
            BotBrain *rb = bot_brain(i);
            int hold = ((rb->task == TASK_REPAIR || rb->task == TASK_REVIVE) && rb->step == 2 && rb->waiting) || (rb->task == TASK_FIGHT && rb->step == 2) || g_gunFire[i];
            if (hold) { ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_FIRE, 1.0f); g_botFiredAt[i] = H->frame ? H->frame : 1; }
            else if (fireHeld[i]) ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_FIRE, 0.0f);
            fireHeld[i] = (unsigned char)hold;
            // aim down the sights (hold to zoom) while fighting on foot at more than ADS_MIN_RANGE: hip fire spreads badly
            // at range (a sniper's from the hip is a lottery); close in, the hip is quicker. From lining up (step 1)
            // through the bursts and pauses until the fight ends.
            #define ADS_MIN_RANGE 50.0f
            static unsigned char zoomHeld[MAX_BOTS];
            char *ms = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
            int zoom = rb->task == TASK_FIGHT && rb->step >= 1 && !player_vehicle((char *)H->bots[i]) && ms;
            if (zoom) {
                char *t = g_fightTarget[i], *ts = t && player_exists(t) ? *(char **)(t + OFF_PLAYER_SOLDIER) : NULL;
                zoom = ts && dist2((float *)(ts + OFF_SOLDIER_SPAWN_POSITION), (float *)(ms + OFF_SOLDIER_POSITION)) > ADS_MIN_RANGE * ADS_MIN_RANGE;
            }
            if (zoom) ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_ZOOM, 1.0f);
            else if (zoomHeld[i]) ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_ZOOM, 0.0f);
            zoomHeld[i] = (unsigned char)zoom;
            // look down at the hull while repairing; level again otherwise
            char *s = *(char **)((char *)H->bots[i] + OFF_PLAYER_SOLDIER);
            float want = g_pitchTestOn[i] ? g_pitchTest[i] : (rb->task == TASK_REPAIR && rb->step >= 1) ? REPAIR_PITCH
                       : (rb->task == TASK_REVIVE && rb->step >= 1) ? REVIVE_PITCH
                       : (rb->task == TASK_ROCKET || rb->task == TASK_FIGHT) ? g_aimPitch[i] : 0.0f;
            if (player_vehicle((char *)H->bots[i]) && g_vehInput) {   // in a seat: turret turn / elevation levels
                ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_SEAT_YAW, g_seatYaw[i]);
                ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_PITCH, g_seatPitch[i]);
            } else if (s) {
                float err = want - *(float *)(s + OFF_SOLDIER_PITCH);
                float dead = (rb->task == TASK_FIGHT && g_fineAim[i]) ? 0.003f : rb->task == TASK_FIGHT ? 0.008f : 0.02f;
                float lvl = fabsf(err) < dead ? 0.0f : fmaxf(-1.0f, fminf(1.0f, err * PITCH_INPUT_SIGN * 3.0f));
                ((SetLevel_t)ADDR_ENTRYINPUT_SET_LEVEL)(in, EIA_PITCH, lvl);
            }
        }
        if (g_vehInput) {   // in a seat: nobody feeds a bot's input to the vehicle (humans' arrives from their client) - do it
            char *pl = H->bots[i], *veh = player_vehicle(pl);
            int seat = veh ? vehicle_seat_of(veh, pl) : -1;
            if (!veh) g_driving[i] = 0;
            if ((seat > 0 || (seat == 0 && g_driving[i])) && !IsBadReadPtr(veh, 4) && *(unsigned *)veh == ADDR_VEHICLE_VTABLE) {
                typedef void (__attribute__((thiscall)) *EntryUpdate_t)(void *ent, unsigned entry, void *input);
                ((EntryUpdate_t)(*(void ***)veh)[0x144 / 4])(veh, (unsigned)seat, in);
                static unsigned logged;
                if (logged++ < 5) bot_log("vinput: fed %s's input to seat %d of %p", pl + OFF_PLAYER_ONLINE_NAME, seat, veh);
            }
        }
        if (g_vaimOn[i]) {
            *(int *)((char *)in + OFF_INPUT_AIM_VALID) = 1;
            *(float *)((char *)in + OFF_INPUT_AIM_YAW) = g_vaimYaw[i];
            *(float *)((char *)in + OFF_INPUT_AIM_PITCH) = g_vaimPitch[i];
        }
        if (*aim_mode()) {
            *(int *)((char *)in + OFF_INPUT_AIM_VALID) = 1;
            *(float *)((char *)in + OFF_INPUT_AIM_YAW) = bot_aim(i)[0];
            *(float *)((char *)in + OFF_INPUT_AIM_PITCH) = bot_aim(i)[1];
        }
    }
    { double t = perf_now(); perf_add(PF_INPUT, t - tMark); tMark = t; }
    level_tick();
    { double t = perf_now(); perf_add(PF_LEVEL, t - tMark); tMark = t; }
    if (!level_ready()) { perf_add(PF_TOTAL, perf_now() - tStart); return; }   // level loading / changing: leave everything alone
    hq_learn_tick();
    if (g_levelReady && H->frame % 30 == 7 && *nav_slot() && (*nav_slot())->phase == 2) {
        int back = nav_unblock_expired(*nav_slot(), 0);
        if (back) bot_log("nav: %d link(s) blocked at stuck spots 3 minutes ago are open again", back);
    }
    if (g_levelReady) { tickets_tick(); swim_tick(); tree_tick(); if (!g_driveOff) { drive_mask(); drive_pave_tick(); drive_strikes_tick(); mat_survey_tick(); roadwatch_tick(); squad_vehicle_tick(); } }
    if (g_readdCount) readd_bots();
    // After a server start, bring up the 8-bot roster automatically (unless BotMod_autoroster.off exists).
    static int autoRosterDone;
    if (!autoRosterDone && H->botCount == 0) {
        autoRosterDone = 1;
        char dir[MAX_PATH], path[MAX_PATH + 32];
        GetModuleFileNameA(NULL, dir, sizeof dir);
        *strrchr(dir, '\\') = 0;
        snprintf(path, sizeof path, "%s\\BotMod_autoroster.off", dir);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            char c1[] = "step 1";
            botmod_command(c1);
            g_rosterAuto = 1;
            bot_log("autoroster: %d bots wanted", *wanted_bots());
            chat_say("[BotMod] %d bots joining (US %d, RU %d) - type !help for bot commands", *wanted_bots(), (*wanted_bots() + 1) / 2,
                     *wanted_bots() / 2);
        }
    }
    // Autobalance / round swaps move live players: a bot on the wrong team for its name dies and respawns on the right one.
    if (H->frame % 30 == 0)
        for (int i = 0; i < H->botCount; i++) {
            const char *nm = H->bots[i] + OFF_PLAYER_ONLINE_NAME;
            int want = strstr(nm, " US") ? 1 : strstr(nm, " RU") ? 2 : 0;
            if (want && player_team(H->bots[i]) != want && *(void **)(H->bots[i] + OFF_PLAYER_SOLDIER)) {
                bot_log("team: %s was moved to team %d, respawning it on team %d", nm, player_team(H->bots[i]), want);
                kill_bot(H->bots[i]);
            }
        }
    // Safety net: a bot that should be moving (no task, not holding a flag) but has not covered 3 m in 45 s is wedged
    // somewhere the grid cannot get it out of - respawn it.
    {
        static float lastPos[MAX_BOTS][3];
        static unsigned lastMove[MAX_BOTS];
        if (H->frame % 30 == 0)
            for (int i = 0; i < H->botCount; i++) {
                char *s = *(char **)(H->bots[i] + OFF_PLAYER_SOLDIER);
                BotBrain *b = bot_brain(i);
                if (!s || !b->enabled || b->task != TASK_NONE || b->waiting || player_vehicle(H->bots[i])) { lastMove[i] = H->frame; continue; }
                float *p = (float *)(s + OFF_SOLDIER_POSITION);
                float dx = p[0] - lastPos[i][0], dz = p[2] - lastPos[i][2];
                if (dx * dx + dz * dz > 9.0f || !lastMove[i]) { memcpy(lastPos[i], p, 12); lastMove[i] = H->frame; continue; }
                if (H->frame - lastMove[i] > 45 * 30) {
                    bot_log("brain: %s has not moved for 45s at (%.1f, %.1f) - respawning it", H->bots[i] + OFF_PLAYER_ONLINE_NAME, p[0], p[2]);
                    kill_bot(H->bots[i]);
                    lastMove[i] = H->frame;
                }
            }
    }
    if (g_testVictim && H->frame % 15 == 0) {
        char *s = player_exists(g_testVictim) ? *(char **)(g_testVictim + OFF_PLAYER_SOLDIER) : NULL;
        static unsigned until;
        if (!until) until = H->frame + 20 * 30;
        bot_log("test: %s health %s", g_testVictim + OFF_PLAYER_ONLINE_NAME, s ? "alive" : "dead");
        if (s) bot_log("test:   %.1f", *(float *)(s + 0x128));
        if (!s || H->frame > until) { g_testVictim = NULL; until = 0; }
    }
    g_where = "roster/squads/downed/nav";
    roster_tick();
    { double t = perf_now(); perf_add(PF_ROSTER, t - tMark); tMark = t; }
    squad_tick();
    { double t = perf_now(); perf_add(PF_SQUAD, t - tMark); tMark = t; }
    downed_tick();
    downed_follow_bodies();
    { double t = perf_now(); perf_add(PF_DOWNED, t - tMark); tMark = t; }
    if (H->stepBots) step_bot_soldiers();   // (the engine's simulation of the bot soldiers: no client steps them)
    { double t = perf_now(); perf_add(PF_STEP, t - tMark); tMark = t; }
    nav_tick();
    { double t = perf_now(); perf_add(PF_NAV, t - tMark); tMark = t; }
    double brainMs = 0, detectMs = 0;
    for (int i = 0; i < H->botCount; i++) {
        g_whereBot = i;
        g_where = "brain_tick";
        double a = perf_now();
        if (level_ready()) { if (i == 0) { jet_idle_tick(); pilot_status_tick(); } pilot_pickup_tick(i); pilot_air_tick(i); }
        if (g_pilotWaiting[i]) { bot_nav(i)->mode = 0; H->botMove[i][0] = H->botMove[i][1] = H->botMove[i][2] = 0; }   // at the HQ for a ride
        if (((H->frame + i) & 1) == 0 && !g_drive[i].phase && !g_flying[i] && !g_pilotWaiting[i]) {   // each bot thinks every other frame (15 Hz), alternating halves
            brain_tick(i);
            if (level_ready()) drive_consider(i);
            g_where = "say_tick";
            say_tick(i);
        }
        double b = perf_now();
        if (b - a > 20.0) {
            static unsigned slowLogged;
            if (slowLogged++ < 60) bot_log("perf: slow think %.0f ms - %s task %s enroute %d waiting %d nav %d", b - a, (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                                           kTaskNames[bot_brain(i)->task], bot_brain(i)->enroute, bot_brain(i)->waiting, bot_nav(i)->mode);
        }
        g_where = "detect_tick";
        if ((H->frame + i * 2) % 15 == 0) detect_tick(i);
        else if ((H->frame + i) % 6 == 0) close_threat_tick(i);
        brainMs += b - a; detectMs += perf_now() - b;
    }
    perf_add(PF_BRAIN, brainMs); perf_add(PF_DETECT, detectMs);
    perf_add(PF_TOTAL, perf_now() - tStart);
    g_where = NULL;
    g_whereBot = -1;
}

// ---- Missiles: a bot in an aircraft fires countermeasures when a homing missile is launched at it ----
// The missile class (found live 2026-10-12, a heat-seeker fired at BOT_01's F/A-18): 0x290 bytes, vptrs at +0 02688bb8,
// +0x40, +0x130, +0x1d0 02688b64. Its constructor (012c7870) and a factory that builds one inline (012c8420) both end with
// "mov dword [esi+1d0], 02688b64": each becomes a call to msl_ctor_stub, which does the same store and notes esi.
// In the missile: +0xc0 velocity, +0x1c4 seconds in flight, +0x220 its target's position (tracks the target live).
#define MSL_VT_PRIMARY   0x02688bb8u
#define MSL_VT_1D0       0x02688b64u
#define MSL_OFF_VEL      0xc0
#define MSL_OFF_AGE      0x1c4
#define MSL_OFF_TGTPOS   0x220
#define MSL_OFF_TARGET   0x250   // weak handle to the target entity (its vehicle): set from the launch params +0x64 (00e6d1c0)
#define ADDR_WEAK_HANDLE      0x00dd75d0   // cdecl (WeakHandle *out, Entity *e): a counted handle to e (via e+4)
#define ADDR_WEAK_ASSIGN      0x00e054f0   // thiscall WeakHandle::operator=(WeakHandle *src)
#define ADDR_WEAK_FREE        0x0043ebe0   // cdecl: frees a handle whose count reached 0
#define MSL_MAX          48   // (a flare salvo is 12 objects of the missile class)
volatile unsigned g_mslNew[32], g_mslIdx;
__asm__(".text\n"
        ".globl _msl_ctor_stub\n"
        "_msl_ctor_stub:\n"
        "  movl $0x02688b64, 0x1d0(%esi)\n"
        "  pushl %eax\n"
        "  movl _g_mslIdx, %eax\n"
        "  andl $31, %eax\n"
        "  movl %esi, _g_mslNew(,%eax,4)\n"
        "  incl _g_mslIdx\n"
        "  popl %eax\n"
        "  ret\n");
void msl_ctor_stub(void);
static const unsigned char kMslStore[10] = {0xC7, 0x86, 0xD0, 0x01, 0x00, 0x00, 0x64, 0x8B, 0x68, 0x02};
static int msl_hook(unsigned addr, const char *what)
{
    unsigned char call[10] = {0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x44, 0x00, 0x00};   // call stub; 5-byte nop
    *(DWORD *)(call + 1) = (DWORD)((unsigned char *)msl_ctor_stub - (unsigned char *)(addr + 5));
    return patch_code(addr, kMslStore, call, 10, what);
}
static struct { char *obj; unsigned seen; char *warned; unsigned char dumped, given, counted; signed char shooter; } g_msl[MSL_MAX];
static unsigned g_mslRead;
// Countermeasures (verified 2026-10-13): EIA 10 releases a salvo of 12 flares - objects of the missile class, built through
// the same patched stores - and presses during the reload do nothing. So: one press, then count new missile-class objects;
// 4+ within half a second = flares out. None = reloading: break across the missile's path, try again every half second.
static char *g_cmMsl[MAX_BOTS];
static unsigned g_cmLockAt[MAX_BOTS], g_cmPressAt[MAX_BOTS], g_cmIdxAtPress[MAX_BOTS], g_cmNextTry[MAX_BOTS], g_cmFlaresAt[MAX_BOTS];
static unsigned char g_cmEvade[MAX_BOTS];
static void cm_press(int i)
{
    schedule_press(i, g_flareEia, H->frame + 1);
    g_cmPressAt[i] = H->frame + 1; g_cmIdxAtPress[i] = g_mslIdx;
}
static void msl_set_target(char *o, char *veh)   // what 00e6d1c0 does with a launch's target entity
{
    void *tmp = NULL;
    ((void (__cdecl *)(void **, char *))ADDR_WEAK_HANDLE)(&tmp, veh);
    ((void (__attribute__((thiscall)) *)(void *, void **))ADDR_WEAK_ASSIGN)(o + MSL_OFF_TARGET, &tmp);
    if (tmp && InterlockedDecrement((LONG *)((char *)tmp + 4)) == 0) ((void (__cdecl *)(void *))ADDR_WEAK_FREE)(tmp);
}
static int msl_alive(char *o)
{
    return o && !IsBadReadPtr(o, 0x290) && *(unsigned *)o == MSL_VT_PRIMARY && *(unsigned *)(o + 0x1d0) == MSL_VT_1D0;
}
// msldebug: one line per new missile-class object (speed, target position), then every dword in it that points at a
// player, its soldier or its vehicle (or inside the first 0x1000 bytes of one) - to find where a missile keeps its target
// and its shooter. Heap pointers only (f0000000..f8000000) are dereferenced.
static void msl_debug_dump(char *o)
{
    float *v = (float *)(o + MSL_OFF_VEL), *tp = (float *)(o + MSL_OFF_TGTPOS);
    bot_log("msldebug: %p speed %.0f age %.2f tgtpos (%.0f %.0f %.0f)", o, sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]),
            *(float *)(o + MSL_OFF_AGE), tp[0], tp[1], tp[2]);
    char *pm = player_manager();
    void **arr = pm ? *(void ***)(pm + 0xac) : NULL;
    unsigned count = pm ? *(unsigned *)(pm + 8) : 0;
    for (unsigned off = 0; off < 0x290; off += 4) {
        unsigned val = *(unsigned *)(o + off);
        if (val < 0xf0000000u || val >= 0xf8000000u) continue;
        for (unsigned k = 0; arr && k < count && k < 64; k++) {
            char *pl = arr[k];
            if (!pl) continue;
            char *so = *(char **)(pl + OFF_PLAYER_SOLDIER), *ve = player_vehicle(pl);
            const char *what = NULL; unsigned rel = 0;
            if (val == (unsigned)pl) what = "player";
            else if (so && val == (unsigned)so) what = "soldier";
            else if (ve && val == (unsigned)ve) what = "vehicle";
            else if (ve && val > (unsigned)ve && val < (unsigned)ve + 0x1000) { what = "vehicle+"; rel = val - (unsigned)ve; }
            else if (so && val > (unsigned)so && val < (unsigned)so + 0x1000) { what = "soldier+"; rel = val - (unsigned)so; }
            if (what) bot_log("msldebug:   +%03x = %08x  %s's %s%s%x", off, val, pl + OFF_PLAYER_ONLINE_NAME, what, rel ? "" : "", rel);
        }
    }
    unsigned h = *(unsigned *)(o + 0x250);   // the target's weak handle (set at creation from params +0x64, see 00e6d1c0)
    if (h >= 0xf0000000u && h < 0xf8000000u && !IsBadReadPtr((void *)h, 0x10)) {
        unsigned *hw = (unsigned *)h;
        bot_log("msldebug:   +250 handle %08x: %08x %08x %08x %08x", h, hw[0], hw[1], hw[2], hw[3]);
        for (int q = 0; q < 4; q++) {
            unsigned val = hw[q];
            if (val < 0xf0000000u || val >= 0xf8000000u) continue;
            for (unsigned k = 0; arr && k < count && k < 64; k++) {
                char *pl = arr[k];
                if (!pl) continue;
                char *so = *(char **)(pl + OFF_PLAYER_SOLDIER), *ve = player_vehicle(pl);
                if (ve && val >= (unsigned)ve - 0x100 && val < (unsigned)ve + 0x1000)
                    bot_log("msldebug:     handle[%d] = %s's vehicle %+d", q, pl + OFF_PLAYER_ONLINE_NAME, (int)(val - (unsigned)ve));
                if (so && val >= (unsigned)so - 0x100 && val < (unsigned)so + 0x1000)
                    bot_log("msldebug:     handle[%d] = %s's soldier %+d", q, pl + OFF_PLAYER_ONLINE_NAME, (int)(val - (unsigned)so));
            }
        }
    } else bot_log("msldebug:   +250 handle %08x (none)", h);
}
static int msl_shooter(void)   // a bot that pressed its seeker fire in the last 20 frames: this new missile is probably its own
{
    for (int i = 0; i < H->botCount; i++)
        if (g_flySeek[i] && g_seekFire[i] && g_seekFiredAt[i] && H->frame - g_seekFiredAt[i] <= 20) return i;
    return -1;
}
static void missile_tick(void)
{
    while (g_mslRead != g_mslIdx) {   // newly built missiles
        char *o = (char *)g_mslNew[g_mslRead++ & 31];
        int k, freeK = -1;
        for (k = 0; k < MSL_MAX && g_msl[k].obj != o; k++) if (!g_msl[k].obj && freeK < 0) freeK = k;
        if (k == MSL_MAX && freeK >= 0) { g_msl[freeK].obj = o; g_msl[freeK].seen = H->frame; g_msl[freeK].warned = NULL; g_msl[freeK].dumped = 0; g_msl[freeK].given = 0; g_msl[freeK].counted = 0; g_msl[freeK].shooter = msl_shooter(); }
        else if (k < MSL_MAX) { g_msl[k].seen = H->frame; g_msl[k].warned = NULL; g_msl[k].dumped = 0; g_msl[k].given = 0; g_msl[k].counted = 0; g_msl[k].shooter = msl_shooter(); }   // a new missile in a reused block
    }
    for (int k = 0; k < MSL_MAX; k++) {
        char *o = g_msl[k].obj;
        if (!o) continue;
        if (!msl_alive(o) || H->frame - g_msl[k].seen > 30 * 30) { g_msl[k].obj = NULL; continue; }
        if (g_msl[k].shooter >= 0 && g_msl[k].shooter < H->botCount && H->frame - g_msl[k].seen >= 1 && H->frame - g_msl[k].seen < 10 && !g_msl[k].counted) {
            float *v0 = (float *)(o + MSL_OFF_VEL);   // a bot's own seeker launch (not the target's flares): count it
            int s = g_msl[k].shooter;
            if (v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2] > 80.0f * 80.0f) {
                g_msl[k].counted = 1;
                if (++g_seekShots[s] >= 2) {
                    g_seekShots[s] = 0; g_seekGunsUntil[s] = H->frame + (unsigned)(g_seekReload * 30.0f);
                    bot_log("seek: %s has fired both missiles - cannon for %.0f s", (char *)H->bots[s] + OFF_PLAYER_ONLINE_NAME, g_seekReload);
                } else bot_log("seek: %s launched a missile (%u of 2)", (char *)H->bots[s] + OFF_PLAYER_ONLINE_NAME, g_seekShots[s]);
            }
        }
        if (!g_msl[k].given && H->frame - g_msl[k].seen >= 1 && H->frame - g_msl[k].seen < 10 && !*(unsigned *)(o + MSL_OFF_TARGET)) {
            // a bot's heat-seeker, just fired: it has no target (the game's lock is client-side) - give it the bot's
            float *v = (float *)(o + MSL_OFF_VEL);
            if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 80.0f * 80.0f)
                for (int i = 0; i < H->botCount; i++) {
                    if (!g_flySeek[i] || !g_seekFire[i] || !g_flyChase[i] || H->frame - g_seekFiredAt[i] > 20 || !player_exists(g_flyChase[i])) continue;
                    char *tv = player_vehicle(g_flyChase[i]);
                    if (!tv) continue;
                    msl_set_target(o, tv);
                    g_msl[k].given = 1;
                    bot_log("seek: %s's missile %p given %s's aircraft as its target (handle %08x)", (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME, o,
                            g_flyChase[i] + OFF_PLAYER_ONLINE_NAME, *(unsigned *)(o + MSL_OFF_TARGET));
                    break;
                }
        }
        if (g_mslDebug && !g_msl[k].dumped && H->frame - g_msl[k].seen >= 3) { g_msl[k].dumped = 1; msl_debug_dump(o); }
        if (g_msl[k].warned || H->frame - g_msl[k].seen < 2) continue;   // (its target is filled in after construction)
        float *tp = (float *)(o + MSL_OFF_TGTPOS);
        if (!(fabsf(tp[0]) < 1e5f && fabsf(tp[1]) < 1e5f && fabsf(tp[2]) < 1e5f) || (tp[0] == 0 && tp[2] == 0)) {
            if (H->frame - g_msl[k].seen > 30) g_msl[k].obj = NULL;   // no target after a second: a flare - free the slot
            continue;
        }
        for (int i = 0; i < H->botCount; i++) {
            char *veh = g_flying[i] && g_msl[k].shooter != i ? player_vehicle(H->bots[i]) : NULL;   // (not its own: it chased the
            float *m = veh ? vehicle_xf(veh) : NULL;                                                // user past it - wasted flares)
            if (!m) continue;
            float dx = m[12] - tp[0], dy = m[13] - tp[1], dz = m[14] - tp[2];
            if (dx * dx + dy * dy + dz * dz > 60.0f * 60.0f) continue;
            g_msl[k].warned = veh;
            g_cmMsl[i] = o; g_cmLockAt[i] = H->frame;
            if (!g_cmPressAt[i]) cm_press(i);
            float *v = (float *)(o + MSL_OFF_VEL);
            bot_log("missile: %p locked on %s (%.0f m/s, %.1f s in flight) - countermeasures", o, (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME,
                    sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), *(float *)(o + MSL_OFF_AGE));
            break;
        }
    }
    for (int i = 0; i < H->botCount; i++) {   // each bot defending against a missile
        char *o = g_cmMsl[i];
        if (!o) continue;
        const char *nm = (char *)H->bots[i] + OFF_PLAYER_ONLINE_NAME;
        float secs = (H->frame - g_cmLockAt[i]) / 30.0f;
        int flares = g_cmFlaresAt[i] >= g_cmLockAt[i];
        if (!g_flying[i] || !msl_alive(o) || secs > 20.0f) {
            if (!g_flying[i]) bot_log("missile: %s lost its aircraft %.1f s after the lock (%s)", nm, secs, flares ? "flares were out" : "no flares");
            else bot_log("missile: %p gone after %.1f s - %s still flying (%s)", o, secs, nm, flares ? "flares" : g_cmEvade[i] ? "break turn" : "no flares");
            g_cmMsl[i] = NULL; g_cmPressAt[i] = 0; g_cmEvade[i] = 0;
            continue;
        }
        if (g_cmPressAt[i] && H->frame >= g_cmPressAt[i] + 3) {
            unsigned n = g_mslIdx - g_cmIdxAtPress[i];
            if (n >= 4) {
                bot_log("missile: %s - flares out (%u objects, %.1f s after the lock)", nm, n, secs);
                g_cmFlaresAt[i] = H->frame; g_cmPressAt[i] = 0; g_cmEvade[i] = 0;
            } else if (H->frame >= g_cmPressAt[i] + 15) {
                if (!g_cmEvade[i]) bot_log("missile: %s - no flares (reloading) - breaks across the missile's path", nm);
                g_cmEvade[i] = 1; g_cmPressAt[i] = 0; g_cmNextTry[i] = H->frame + 15;
            }
        }
        if (g_cmEvade[i] && !g_cmPressAt[i] && H->frame >= g_cmNextTry[i]) cm_press(i);
    }
}
static int cm_beam_heading(int i, float hdg, float *want)
{
    char *o = g_cmMsl[i];
    if (!g_cmEvade[i] || !o || !msl_alive(o)) return 0;
    float *v = (float *)(o + MSL_OFF_VEL);
    if (v[0] * v[0] + v[2] * v[2] < 1.0f) return 0;
    float a = atan2f(v[2], -v[0]) * 57.29578f, b = atan2f(-v[2], v[0]) * 57.29578f;   // the two headings square to its path
    float da = a - hdg, db = b - hdg;
    while (da > 180.0f) da -= 360.0f;
    while (da < -180.0f) da += 360.0f;
    while (db > 180.0f) db -= 360.0f;
    while (db < -180.0f) db += 360.0f;
    *want = fabsf(da) < fabsf(db) ? a : b;   // the nearer one
    return 1;
}

__declspec(dllexport) int botmod_init(BotModHost *host)
{
    if (host->version != BOTMOD_HOST_VERSION) {
        host->log("core: host version %d != core version %d - restart the server", host->version, BOTMOD_HOST_VERSION);
        return 0;
    }
    H = host;
    int ok = 1;
    ok &= detour(ADDR_CSE_SPAWN, kCseSpawnPrologue, 6, hook_cse_spawn, "CharacterSpawnEntity::spawn");
    ok &= detour(ADDR_GHOST_ADD, kGhostAddPrologue, 6, hook_ghost_add, "ServerGhostManager::addGhost");
    ok &= detour(ADDR_PLAYER_SELECT_UNLOCKS, kSelectUnlocksPrologue, 6, hook_select_unlocks, "ServerPlayer::onSelectedUnlockAssets");
    ok &= detour(ADDR_PLAYER_SELECT_WEAPON, kSelectWeaponPrologue, 5, hook_select_weapon, "ServerPlayer::onSelectedWeapon");
    ok &= detour(ADDR_MSGMGR_DISPATCH, kMsgDispatchPrologue, 10, hook_msg_dispatch, "MessageManager::dispatch");
    ok &= detour(ADDR_SPAWN_REQ_CALL, kSpawnReqCall, 11, hook_spawn_request, "spawn request call");
    {   // Bots have no persistence stats, so the award system has no counters for them and warns on every lookup
        // ("trying to access AwardCounter for player N before added to the system"). The warning is only a log line
        // (both paths continue the same way): skip it in ServerAwardSystem::getAwardCounter (two copies).
        static const unsigned char je[2] = {0x74, 0x4F}, jmp[2] = {0xEB, 0x4F};
        ok &= patch_code(0x011ff662, je, jmp, 2, "AwardCounter warning (1)");
        ok &= patch_code(0x01201592, je, jmp, 2, "AwardCounter warning (2)");
    }
    ok &= detour(ADDR_SOLDIER_CTOR, kSoldierCtorPrologue, 6, hook_soldier_ctor, "ServerSoldierEntity ctor");
    ok &= patch_vslot(ADDR_PLAYER_VTABLE + 0x18, ADDR_PLAYER_HAS_UNLOCK, hook_has_unlock, "ServerPlayer::hasUnlock");
    ok &= patch_vslot(ADDR_SOLDIER_VTABLE + 0x74, ADDR_SOLDIER_PREPHYS, hook_soldier_prephys, "ServerSoldierEntity::prePhysicsUpdate");
    ok &= patch_vslot(ADDR_SOLDIER_VTABLE + 0x110, ADDR_SOLDIER_ON_DAMAGE, (void *)hook_soldier_on_damage, "ServerSoldierEntity::onDamage");
    ok &= detour(ADDR_PROJECTILE_PROCESS_HIT, kProcessHitPrologue, 6, hook_process_hit, "projectile processHit");
    ok &= msl_hook(0x012c789b, "missile ctor (vptr store)");
    ok &= msl_hook(0x012c8471, "missile factory (vptr store)");
    {
        static const unsigned char hc[6] = {0x0F, 0x85, 0x2D, 0x02, 0x00, 0x00};   // jne 0x11803f8
        ok &= patch_jump(0x011801c5, hc, 6, (void *)hc_stub, "bot damage acceptance (health component)");
    }
    ok &= patch_vslot(ADDR_SOLDIER_VTABLE + 0x144, ADDR_SOLDIER_PREPHYS_ENTRY, hook_soldier_apply, "ServerSoldierEntity::prePhysicsUpdateEntry");
    if (!ok) { unpatch_all(); return 0; }
    // A grid carried over from the previous core may use another layout (NAV_LAYERS): reload it from disk.
    nav_free(*nav_slot()); *nav_slot() = NULL; *nav_building() = 0;
    g_jobDoLoadouts = !loadout_lib()->count;   // the discovery worker loads/verifies the library (no main-thread stall)
    g_needLoadouts = 1;   // ...and always re-looks the kit assets up against the CURRENT level before a bot spawns: a reload
                          // during a map change (XP5_002 -> XP3_Alborz) kept the old level's freed assets and crashed in apply_kit
    { const char *l = current_level(); if (l) strncpy(g_level, l, sizeof g_level - 1); }
    for (int i = 0; i < MAX_BOTS; i++) g_botLoadout[i] = i < H->botCount ? pick_loadout(H->bots[i]) : -1;   // what they spawned with
    for (int i = 0; i < H->botCount; i++) {   // planned paths do not survive a reload: re-plan everything
        BotBrain *b = bot_brain(i);
        b->enroute = b->waiting = 0;
        b->targetFlag = -1;
        b->replanAt = 0;
        if (b->crouched) press_button(i, 14, POSE_PRESS_FRAMES);   // prone is off now: stand up any bot we put down
        b->crouched = 0;
        if (bot_nav(i)->mode == 3) bot_nav(i)->mode = 1;
    }
    g_compThread = CreateThread(NULL, 0, comp_worker, NULL, 0, NULL);
    chat_start(H->log);
    load_bot_count();
    if (H->botCount) g_rosterAuto = 1;   // reload with bots already in: keep managing them
    bot_log("core: ready (%d bot(s) carried over, %d loadout(s))", H->botCount, loadout_lib()->count);
    return 1;
}

__declspec(dllexport) void botmod_shutdown(void)
{
    nav_save_if_dirty();   // keep what the bots learned about the map across reloads
    // stop the workers first (scans abort within one memory region), then wait briefly - never long enough to stall
    InterlockedExchange(&g_abortScans, 1);
    g_compStop = 1;
    if (g_jobThread) {
        if (WaitForSingleObject(g_jobThread, 3000) == WAIT_TIMEOUT) bot_log("WARNING: discovery worker still running at unload");
        CloseHandle(g_jobThread); g_jobThread = NULL;
    }
    chat_stop();
    if (g_compThread) {
        if (WaitForSingleObject(g_compThread, 3000) == WAIT_TIMEOUT) bot_log("WARNING: scan worker still running at unload");
        CloseHandle(g_compThread); g_compThread = NULL;
    }
    unpatch_all();
    bot_log("core: shut down, all hooks removed");
}
