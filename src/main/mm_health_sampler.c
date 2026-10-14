/* Majora's Mask (US) state sampler: reads a few save-context fields once per
 * emulator frame, logs health changes, and exports a LinkState datagram (v1: v0 fields plus the live Player)
 * to 127.0.0.1 over UDP. Read-only: it never writes emulated memory.
 * Packet layout: mm-bridge/LINKSTATE.md. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_debugger.h"
#include "api/m64p_types.h"
#include "main/mm_health_sampler.h"
#include "main/rom.h"

#define MM_ROM_MD5 "2A0A8ACB61538235BC1094D297FB6556"

/* gSaveContext (mm-decomp z64save.h). The base is both derived from the
 * verified health address and listed in Zelda64RecompSyms datasyms.toml. */
#define MM_SAVE_CONTEXT UINT32_C(0x801EF670)
#define MM_HEALTH_ADDRESS (MM_SAVE_CONTEXT + 0x36)          /* s16 saveInfo.playerData.health, verified */
#define MM_PLAYER_FORM_ADDRESS (MM_SAVE_CONTEXT + 0x20)     /* u8 save.playerForm */
#define MM_HEALTH_CAPACITY_ADDRESS (MM_SAVE_CONTEXT + 0x34) /* s16 saveInfo.playerData.healthCapacity */
#define MM_MAGIC_LEVEL_ADDRESS (MM_SAVE_CONTEXT + 0x38)     /* s8 saveInfo.playerData.magicLevel */
#define MM_MAGIC_ADDRESS (MM_SAVE_CONTEXT + 0x39)           /* s8 saveInfo.playerData.magic */
#define MM_FILE_NUM_ADDRESS (MM_SAVE_CONTEXT + 0x3CA0)      /* s32 fileNum, 0xFF = no file (z_title.c) */
#define MM_GAME_MODE_ADDRESS (MM_SAVE_CONTEXT + 0x3CA8)     /* s32 gameMode, 0 = GAMEMODE_NORMAL */

/* Frame signals (Zelda64RecompSyms datasyms.toml; candidates).
 * sGraphPrevUpdateEndTime (graph.c) is the osGetTime() stamp written once at
 * the end of every completed graph/game update; its low word changes exactly
 * once per game frame. sIrqMgrRetraceCount (irqmgr.c) counts VI retraces. */
#define MM_GRAPH_UPDATE_END_TIME_ADDRESS UINT32_C(0x801F8150) /* OSTime (u64) */
#define MM_VI_RETRACE_COUNT_ADDRESS UINT32_C(0x80096B78)      /* s32 */

#define MM_GAMEMODE_NORMAL 0
#define MM_NO_FILE 0xFF

/* The live Player actor, resolved fresh every frame (never cached):
 * sCamPlayState (Zelda64RecompSyms) -> PlayState.actorCtx.actorLists[ACTORCAT_PLAYER].
 * Offsets are from mm-decomp z64.h, z64actor.h, z64player.h and
 * z64animation.h at 5607eec18. All fields are candidates. */
#define MM_CAM_PLAY_STATE_PTR UINT32_C(0x801EDC28) /* PlayState* sCamPlayState (z_camera.c) */
#define MM_PLAY_SCENE_ID 0x000A4                   /* s16 PlayState.sceneId */
#define MM_PLAY_PLAYER_LIST_LENGTH 0x01CC8         /* s32 actorCtx(0x1CA0).actorLists[2].length */
#define MM_PLAY_PLAYER_LIST_FIRST 0x01CCC          /* Actor* actorCtx.actorLists[2].first */
#define MM_PLAY_GAMEPLAY_FRAMES 0x18840            /* u32 PlayState.gameplayFrames */
#define MM_SCENE_MAX 0x71                          /* z64scene.h SCENE_MAX */
#define MM_ACTOR_ID 0x000                          /* s16, ACTOR_PLAYER = 0 */
#define MM_ACTOR_CATEGORY 0x002                    /* u8, ACTORCAT_PLAYER = 2 */
#define MM_ACTOR_WORLD_POS 0x024                   /* Vec3f world.pos */
#define MM_ACTOR_WORLD_ROT 0x030                   /* Vec3s world.rot */
#define MM_ACTOR_VELOCITY 0x064                    /* Vec3f velocity */
#define MM_ACTOR_SPEED 0x070                       /* f32 speed */
#define MM_ACTOR_SHAPE_YAW 0x0BE                   /* s16 shape.rot.y: facing */
#define MM_PLAYER_HELD_ITEM 0x148                  /* u8 heldItemId (ItemId) */
#define MM_PLAYER_TRANSFORMATION 0x14B             /* u8 transformation */
#define MM_PLAYER_CURRENT_MASK 0x153               /* u8 currentMask (PlayerMask) */
#define MM_PLAYER_ANIMATION 0x248                  /* void* skelAnime(0x240).animation(0x08) */
#define MM_PLAYER_ANIM_FRAME 0x258                 /* f32 skelAnime.curFrame(0x18) */
#define MM_PLAYER_STATE_FLAGS1 0xA6C               /* u32 stateFlags1..3 */
#define MM_PLAYER_LINEAR_VELOCITY 0xAD0            /* f32 linearVelocity */
#define MM_PLAYER_CURRENT_YAW 0xAD4                /* s16 currentYaw: movement direction */

/* LinkState v1 wire format: 128 bytes, little-endian. Append-only: bytes
 * 0-47 are exactly v0's, the Player block follows (mm-bridge/LINKSTATE.md). */
#define MM_LINKSTATE_VERSION 1
#define MM_LINKSTATE_SIZE 128
#define MM_LINKSTATE_DEFAULT_PORT 47647

#define MM_FLAG_ROM_MATCH 0x1u
#define MM_FLAG_GAMEPLAY_ACTIVE 0x2u
#define MM_FLAG_NEW_GAME_FRAME 0x4u
#define MM_FLAG_PLAYER_VALID 0x8u

#define MM_FIELD_HEALTH 0x01u
#define MM_FIELD_HEALTH_CAPACITY 0x02u
#define MM_FIELD_MAGIC 0x04u
#define MM_FIELD_MAGIC_LEVEL 0x08u
#define MM_FIELD_SAVED_FORM 0x10u
#define MM_FIELD_PLAYER_BLOCK 0x20u

typedef struct
{
    uint32_t actor; /* 0 when no valid Player */
    uint32_t gameplay_frames;
    int16_t scene_id;
    int16_t facing;
    uint32_t pos[3]; /* raw f32 bits, as are the other f32 fields */
    int16_t world_rot[3];
    int16_t current_yaw;
    uint32_t velocity[3];
    uint32_t speed;
    uint32_t linear_velocity;
    uint32_t state_flags[3];
    uint32_t animation;
    uint32_t anim_frame;
    uint8_t transformation;
    uint8_t current_mask;
    uint8_t held_item;
} mm_player;

typedef struct
{
    uint32_t vi_count;
    uint32_t game_update_stamp;
    int32_t game_mode;
    int32_t file_num;
    int16_t health;
    int16_t health_capacity;
    int8_t magic;
    int8_t magic_level;
    uint8_t saved_form;
} mm_state;

#ifdef _WIN32
typedef SOCKET mm_socket;
#define MM_BAD_SOCKET INVALID_SOCKET
#else
typedef int mm_socket;
#define MM_BAD_SOCKET (-1)
#endif

#define MM_LINKSTATE_MAX_PORTS 4

static mm_socket l_socket = MM_BAD_SOCKET;
static int l_socket_failed;
static int l_send_error_logged;
static struct sockaddr_in l_destinations[MM_LINKSTATE_MAX_PORTS];
static int l_destination_count;
static uint32_t l_sequence;

static void read_state(mm_state* s)
{
    /* Low word of the big-endian u64 OSTime: the fast-changing half. */
    s->game_update_stamp = DebugMemRead32(MM_GRAPH_UPDATE_END_TIME_ADDRESS + 4);
    s->vi_count = DebugMemRead32(MM_VI_RETRACE_COUNT_ADDRESS);
    s->game_mode = (int32_t) DebugMemRead32(MM_GAME_MODE_ADDRESS);
    s->file_num = (int32_t) DebugMemRead32(MM_FILE_NUM_ADDRESS);
    s->health = (int16_t) DebugMemRead16(MM_HEALTH_ADDRESS);
    s->health_capacity = (int16_t) DebugMemRead16(MM_HEALTH_CAPACITY_ADDRESS);
    s->magic = (int8_t) DebugMemRead8(MM_MAGIC_ADDRESS);
    s->magic_level = (int8_t) DebugMemRead8(MM_MAGIC_LEVEL_ADDRESS);
    s->saved_form = DebugMemRead8(MM_PLAYER_FORM_ADDRESS);
}

/* A save file is loaded and the game is in ordinary play (not the title
 * demo, file select, credits or owl-save screen). Health 0 stays valid. */
static int gameplay_active(const mm_state* s)
{
    return s->game_mode == MM_GAMEMODE_NORMAL && s->file_num != MM_NO_FILE;
}

static int is_rdram_pointer(uint32_t address)
{
    /* KSEG0 over the 8 MiB of RDRAM MM requires, word aligned. */
    return address >= UINT32_C(0x80000000) && address < UINT32_C(0x80800000) && (address & 3) == 0;
}

/* Fills *p with the live Player, or leaves p->actor = 0 when any check
 * fails. Gameplay must be active first: across scene transitions
 * sCamPlayState can still point at a torn-down PlayState. */
static void read_player(const mm_state* s, mm_player* p)
{
    uint32_t play, actor;
    int32_t list_length;
    int16_t scene_id;
    int i;

    memset(p, 0, sizeof(*p));
    if (!gameplay_active(s))
        return;
    play = DebugMemRead32(MM_CAM_PLAY_STATE_PTR);
    if (!is_rdram_pointer(play))
        return;
    scene_id = (int16_t) DebugMemRead16(play + MM_PLAY_SCENE_ID);
    list_length = (int32_t) DebugMemRead32(play + MM_PLAY_PLAYER_LIST_LENGTH);
    if (scene_id < 0 || scene_id >= MM_SCENE_MAX || list_length < 1)
        return;
    actor = DebugMemRead32(play + MM_PLAY_PLAYER_LIST_FIRST);
    if (!is_rdram_pointer(actor) || DebugMemRead16(actor + MM_ACTOR_ID) != 0 ||
        DebugMemRead8(actor + MM_ACTOR_CATEGORY) != 2)
        return;

    p->actor = actor;
    p->gameplay_frames = DebugMemRead32(play + MM_PLAY_GAMEPLAY_FRAMES);
    p->scene_id = scene_id;
    p->facing = (int16_t) DebugMemRead16(actor + MM_ACTOR_SHAPE_YAW);
    for (i = 0; i < 3; i++)
    {
        p->pos[i] = DebugMemRead32(actor + MM_ACTOR_WORLD_POS + 4 * i);
        p->world_rot[i] = (int16_t) DebugMemRead16(actor + MM_ACTOR_WORLD_ROT + 2 * i);
        p->velocity[i] = DebugMemRead32(actor + MM_ACTOR_VELOCITY + 4 * i);
        p->state_flags[i] = DebugMemRead32(actor + MM_PLAYER_STATE_FLAGS1 + 4 * i);
    }
    p->current_yaw = (int16_t) DebugMemRead16(actor + MM_PLAYER_CURRENT_YAW);
    p->speed = DebugMemRead32(actor + MM_ACTOR_SPEED);
    p->linear_velocity = DebugMemRead32(actor + MM_PLAYER_LINEAR_VELOCITY);
    p->animation = DebugMemRead32(actor + MM_PLAYER_ANIMATION);
    p->anim_frame = DebugMemRead32(actor + MM_PLAYER_ANIM_FRAME);
    p->transformation = DebugMemRead8(actor + MM_PLAYER_TRANSFORMATION);
    p->current_mask = DebugMemRead8(actor + MM_PLAYER_CURRENT_MASK);
    p->held_item = DebugMemRead8(actor + MM_PLAYER_HELD_ITEM);
}

static void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void put_u32(uint8_t* p, uint32_t v)
{
    put_u16(p, (uint16_t) v);
    put_u16(p + 2, (uint16_t) (v >> 16));
}

static void add_destination(long port)
{
    struct sockaddr_in* d;

    if (port <= 0 || port >= 65536 || l_destination_count >= MM_LINKSTATE_MAX_PORTS)
        return;
    d = &l_destinations[l_destination_count++];
    memset(d, 0, sizeof(*d));
    d->sin_family = AF_INET;
    d->sin_port = htons((unsigned short) port);
    d->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DebugMessage(M64MSG_INFO, "[MMLinkState] exporting LinkState v%d to 127.0.0.1:%ld",
                 MM_LINKSTATE_VERSION, port);
}

/* Destinations: MM_LINKSTATE_PORTS (comma-separated, up to 4), else
 * MM_LINKSTATE_PORT, else the default port. Invalid entries are skipped. */
static void configure_destinations(void)
{
    const char* list = getenv("MM_LINKSTATE_PORTS");
    const char* single = getenv("MM_LINKSTATE_PORT");

    l_destination_count = 0;
    if (list != NULL)
    {
        const char* p = list;
        while (*p != '\0')
        {
            char* end;
            long port = strtol(p, &end, 10);
            if (end != p)
                add_destination(port);
            p = (*end == ',') ? end + 1 : (end != p ? end : p + 1);
        }
    }
    if (l_destination_count == 0 && single != NULL)
        add_destination(atol(single));
    if (l_destination_count == 0)
        add_destination(MM_LINKSTATE_DEFAULT_PORT);
}

/* Opens a non-blocking socket on first use. On failure it logs once and
 * stops trying, so the emulation thread never stalls on networking. */
static int open_socket(void)
{
    if (l_socket != MM_BAD_SOCKET)
        return 1;
    if (l_socket_failed)
        return 0;

#ifdef _WIN32
    {
        WSADATA wsa;
        u_long non_blocking = 1;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        {
            l_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (l_socket != MM_BAD_SOCKET && ioctlsocket(l_socket, FIONBIO, &non_blocking) != 0)
            {
                closesocket(l_socket);
                l_socket = MM_BAD_SOCKET;
            }
        }
    }
#else
    l_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (l_socket != MM_BAD_SOCKET &&
        fcntl(l_socket, F_SETFL, fcntl(l_socket, F_GETFL, 0) | O_NONBLOCK) != 0)
    {
        close(l_socket);
        l_socket = MM_BAD_SOCKET;
    }
#endif

    if (l_socket == MM_BAD_SOCKET)
    {
        l_socket_failed = 1;
        DebugMessage(M64MSG_WARNING, "[MMLinkState] UDP socket setup failed; LinkState export disabled");
        return 0;
    }

    configure_destinations();
    return 1;
}

/* Fire and forget. With nobody listening, Windows may report a connection
 * reset on a later send; that and a full buffer are expected and ignored,
 * and any other error is logged once. */
static void send_state(unsigned int frame_index, const mm_state* s, const mm_player* p,
                       uint32_t flags)
{
    uint8_t packet[MM_LINKSTATE_SIZE];
    uint32_t fields = MM_FIELD_HEALTH | MM_FIELD_HEALTH_CAPACITY | MM_FIELD_MAGIC |
                      MM_FIELD_MAGIC_LEVEL | MM_FIELD_SAVED_FORM | MM_FIELD_PLAYER_BLOCK;
    int i;

    if (!open_socket())
        return;

    memset(packet, 0, sizeof(packet));
    memcpy(packet, "MMLS", 4);
    put_u16(packet + 4, MM_LINKSTATE_VERSION);
    put_u16(packet + 6, MM_LINKSTATE_SIZE);
    put_u32(packet + 8, l_sequence++);
    put_u32(packet + 12, frame_index);
    put_u32(packet + 16, s->vi_count);
    put_u32(packet + 20, s->game_update_stamp);
    put_u32(packet + 24, flags);
    put_u32(packet + 28, fields);
    put_u16(packet + 32, (uint16_t) s->health);
    put_u16(packet + 34, (uint16_t) s->health_capacity);
    packet[36] = (uint8_t) s->magic;
    packet[37] = (uint8_t) s->magic_level;
    packet[38] = s->saved_form;
    put_u32(packet + 40, (uint32_t) s->game_mode);
    put_u32(packet + 44, (uint32_t) s->file_num);

    /* v1 Player block; all zero when flags lacks MM_FLAG_PLAYER_VALID. */
    put_u32(packet + 48, p->actor);
    put_u32(packet + 52, p->gameplay_frames);
    put_u16(packet + 56, (uint16_t) p->scene_id);
    put_u16(packet + 58, (uint16_t) p->facing);
    for (i = 0; i < 3; i++)
    {
        put_u32(packet + 60 + 4 * i, p->pos[i]);
        put_u16(packet + 72 + 2 * i, (uint16_t) p->world_rot[i]);
        put_u32(packet + 80 + 4 * i, p->velocity[i]);
        put_u32(packet + 100 + 4 * i, p->state_flags[i]);
    }
    put_u16(packet + 78, (uint16_t) p->current_yaw);
    put_u32(packet + 92, p->speed);
    put_u32(packet + 96, p->linear_velocity);
    put_u32(packet + 112, p->animation);
    put_u32(packet + 116, p->anim_frame);
    packet[120] = p->transformation;
    packet[121] = p->current_mask;
    packet[122] = p->held_item;

    for (i = 0; i < l_destination_count; i++)
    {
        if (sendto(l_socket, (const char*) packet, sizeof(packet), 0,
                   (const struct sockaddr*) &l_destinations[i], sizeof(l_destinations[i])) < 0 &&
            !l_send_error_logged)
        {
#ifdef _WIN32
            int error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAEWOULDBLOCK)
                continue;
#else
            int error = 0;
#endif
            l_send_error_logged = 1;
            DebugMessage(M64MSG_WARNING, "[MMLinkState] UDP send failed (error %d); further send errors are not logged", error);
        }
    }
}

void mm_health_sampler_sample(unsigned int frame_index)
{
    static int16_t previous_health;
    static int has_previous_health;
    static uint32_t previous_stamp;
    static int has_previous_stamp;
    static int previous_active = -1;
    static uint32_t previous_actor = 1; /* never a real value, so the first state logs */
    mm_state current;
    mm_player player;
    uint32_t flags = MM_FLAG_ROM_MATCH;
    int active;

    if (strcmp(ROM_SETTINGS.MD5, MM_ROM_MD5) != 0)
        return;

    read_state(&current);
    read_player(&current, &player);
    active = gameplay_active(&current);
    if (active)
        flags |= MM_FLAG_GAMEPLAY_ACTIVE;
    if (!has_previous_stamp || current.game_update_stamp != previous_stamp)
        flags |= MM_FLAG_NEW_GAME_FRAME;
    if (player.actor != 0)
        flags |= MM_FLAG_PLAYER_VALID;
    previous_stamp = current.game_update_stamp;
    has_previous_stamp = 1;

    send_state(frame_index, &current, &player, flags);

    if (player.actor != previous_actor)
    {
        DebugMessage(M64MSG_INFO, "[MMHealthSampler] frame=%u player_actor=0x%08X scene=%d",
                     frame_index, player.actor, (int) player.scene_id);
        previous_actor = player.actor;
    }

    if (active != previous_active)
    {
        DebugMessage(M64MSG_INFO, "[MMHealthSampler] frame=%u gameplay_active=%d game_mode=%d file_num=%d",
                     frame_index, active, (int) current.game_mode, (int) current.file_num);
        previous_active = active;
        has_previous_health = 0;
    }

    /* Health is logged only during gameplay, where 0 is a real value (death). */
    if (active && (!has_previous_health || current.health != previous_health))
    {
        DebugMessage(M64MSG_INFO,
                     "[MMHealthSampler] frame=%u address=0x%08X health_s16=%d hearts=%.2f",
                     frame_index, MM_HEALTH_ADDRESS, (int) current.health,
                     (double) current.health / 16.0);
        previous_health = current.health;
        has_previous_health = 1;
    }
}
