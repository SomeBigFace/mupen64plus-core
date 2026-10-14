#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_debugger.h"
#include "api/m64p_types.h"
#include "main/mm_health_sampler.h"
#include "main/rom.h"

#ifdef MM_LINKSTATE_UDP
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
#endif

#define MM_ROM_MD5 "2A0A8ACB61538235BC1094D297FB6556"

/* gSaveContext base, derived from the verified health address minus the
 * SaveContext offset of saveInfo.playerData.health (0x24 + 0x12) in the
 * mm-decomp z64save.h. Only health is live-verified; the rest are
 * source-derived candidates until correlated in-game. */
#define MM_SAVE_CONTEXT UINT32_C(0x801EF670)
#define MM_HEALTH_ADDRESS (MM_SAVE_CONTEXT + 0x36)          /* s16, verified */
#define MM_EQUIPPED_MASK_ADDRESS (MM_SAVE_CONTEXT + 0x04)   /* u8 */
#define MM_TIME_ADDRESS (MM_SAVE_CONTEXT + 0x0C)            /* u16 */
#define MM_DAY_ADDRESS (MM_SAVE_CONTEXT + 0x18)             /* s32 */
#define MM_PLAYER_FORM_ADDRESS (MM_SAVE_CONTEXT + 0x20)     /* u8 */
#define MM_HEALTH_CAPACITY_ADDRESS (MM_SAVE_CONTEXT + 0x34) /* s16 */
#define MM_MAGIC_LEVEL_ADDRESS (MM_SAVE_CONTEXT + 0x38)     /* s8 */
#define MM_MAGIC_ADDRESS (MM_SAVE_CONTEXT + 0x39)           /* s8 */
#define MM_RUPEES_ADDRESS (MM_SAVE_CONTEXT + 0x3A)          /* s16 */

/* Live Player actor, resolved fresh every frame through
 * sCamPlayState -> PlayState.actorCtx.actorLists[ACTORCAT_PLAYER].first.
 * sCamPlayState's address is from Zelda64RecompSyms (US rev 1); the offsets
 * are from mm-decomp z64.h / z64actor.h / z64player.h. All candidates. */
#define MM_CAM_PLAY_STATE_PTR UINT32_C(0x801EDC28)
#define MM_PLAY_SCENE_ID 0xA4             /* s16 PlayState.sceneId */
#define MM_PLAY_PLAYER_LIST_FIRST 0x1CCC  /* actorCtx 0x1CA0 + lists 0x10 + 2 * 0xC + first 0x4 */
#define MM_ACTOR_ID 0x000                 /* s16, ACTOR_PLAYER = 0 */
#define MM_ACTOR_CATEGORY 0x002           /* u8, ACTORCAT_PLAYER = 2 */
#define MM_ACTOR_WORLD_POS 0x024          /* Vec3f */
#define MM_ACTOR_WORLD_ROT 0x030          /* Vec3s */
#define MM_ACTOR_VELOCITY 0x064           /* Vec3f */
#define MM_ACTOR_SPEED 0x070              /* f32 */
#define MM_ACTOR_SHAPE_YAW 0x0BE          /* s16 shape.rot.y, the facing */
#define MM_PLAYER_TRANSFORMATION 0x14B    /* u8 PlayerTransformation */
#define MM_PLAYER_CURRENT_MASK 0x153      /* u8 PlayerMask */
#define MM_PLAYER_STATE_FLAGS1 0xA6C      /* u32 x3 */
#define MM_PLAYER_LINEAR_VELOCITY 0xAD0   /* f32 */

typedef struct
{
    uint32_t actor;          /* 0 when no valid Player was found */
    int16_t scene_id;
    uint32_t pos[3];         /* raw f32 bits */
    int16_t world_rot[3];
    int16_t facing;
    uint32_t velocity[3];    /* raw f32 bits */
    uint32_t speed;          /* raw f32 bits */
    uint32_t linear_velocity;
    uint32_t state_flags[3];
    uint8_t transformation;
    uint8_t current_mask;
} mm_player_state;

static int is_rdram_pointer(uint32_t address)
{
    /* KSEG0 over 8 MiB of RDRAM, word aligned. */
    return address >= UINT32_C(0x80000000) && address < UINT32_C(0x80800000) && (address & 3) == 0;
}

static void read_player_state(mm_player_state* p)
{
    uint32_t play = DebugMemRead32(MM_CAM_PLAY_STATE_PTR);
    uint32_t actor;
    int i;

    memset(p, 0, sizeof(*p));
    if (!is_rdram_pointer(play))
        return;
    actor = DebugMemRead32(play + MM_PLAY_PLAYER_LIST_FIRST);
    if (!is_rdram_pointer(actor) || DebugMemRead16(actor + MM_ACTOR_ID) != 0 ||
        DebugMemRead8(actor + MM_ACTOR_CATEGORY) != 2)
        return;

    p->actor = actor;
    p->scene_id = (int16_t) DebugMemRead16(play + MM_PLAY_SCENE_ID);
    for (i = 0; i < 3; i++)
    {
        p->pos[i] = DebugMemRead32(actor + MM_ACTOR_WORLD_POS + 4 * i);
        p->world_rot[i] = (int16_t) DebugMemRead16(actor + MM_ACTOR_WORLD_ROT + 2 * i);
        p->velocity[i] = DebugMemRead32(actor + MM_ACTOR_VELOCITY + 4 * i);
        p->state_flags[i] = DebugMemRead32(actor + MM_PLAYER_STATE_FLAGS1 + 4 * i);
    }
    p->facing = (int16_t) DebugMemRead16(actor + MM_ACTOR_SHAPE_YAW);
    p->speed = DebugMemRead32(actor + MM_ACTOR_SPEED);
    p->linear_velocity = DebugMemRead32(actor + MM_PLAYER_LINEAR_VELOCITY);
    p->transformation = DebugMemRead8(actor + MM_PLAYER_TRANSFORMATION);
    p->current_mask = DebugMemRead8(actor + MM_PLAYER_CURRENT_MASK);
}

typedef struct
{
    int16_t health;
    int16_t health_capacity;
    int16_t rupees;
    uint16_t time;
    int32_t day;
    int8_t magic_level;
    int8_t magic;
    uint8_t player_form;
    uint8_t equipped_mask;
} mm_link_state;

static void read_link_state(mm_link_state* s)
{
    s->health = (int16_t) DebugMemRead16(MM_HEALTH_ADDRESS);
    s->health_capacity = (int16_t) DebugMemRead16(MM_HEALTH_CAPACITY_ADDRESS);
    s->rupees = (int16_t) DebugMemRead16(MM_RUPEES_ADDRESS);
    s->time = DebugMemRead16(MM_TIME_ADDRESS);
    s->day = (int32_t) DebugMemRead32(MM_DAY_ADDRESS);
    s->magic_level = (int8_t) DebugMemRead8(MM_MAGIC_LEVEL_ADDRESS);
    s->magic = (int8_t) DebugMemRead8(MM_MAGIC_ADDRESS);
    s->player_form = DebugMemRead8(MM_PLAYER_FORM_ADDRESS);
    s->equipped_mask = DebugMemRead8(MM_EQUIPPED_MASK_ADDRESS);
}

#ifdef MM_LINKSTATE_UDP
/* LinkState wire format v2: 96 bytes, little-endian, one datagram per
 * rendered frame to 127.0.0.1:MM_LINKSTATE_PORT (default 47647).
 * See mm-bridge/PROTOCOL.md. */
#define MM_LINKSTATE_VERSION 2
#define MM_LINKSTATE_SIZE 96
#define MM_LINKSTATE_DEFAULT_PORT 47647

#ifdef _WIN32
typedef SOCKET mm_socket;
#define MM_BAD_SOCKET INVALID_SOCKET
#else
typedef int mm_socket;
#define MM_BAD_SOCKET (-1)
#endif

static mm_socket l_socket = MM_BAD_SOCKET;
static int l_socket_failed;
static struct sockaddr_in l_destination;
static uint32_t l_sequence;

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

/* Opens a non-blocking socket once; on failure, logs once and stops trying
 * so the emulation thread never stalls on networking. */
static int open_socket(void)
{
    const char* port_text;
    long port = MM_LINKSTATE_DEFAULT_PORT;

    if (l_socket != MM_BAD_SOCKET)
        return 1;
    if (l_socket_failed)
        return 0;

    port_text = getenv("MM_LINKSTATE_PORT");
    if (port_text != NULL && atol(port_text) > 0 && atol(port_text) < 65536)
        port = atol(port_text);

#ifdef _WIN32
    {
        WSADATA wsa;
        u_long non_blocking = 1;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        {
            l_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (l_socket != MM_BAD_SOCKET)
                ioctlsocket(l_socket, FIONBIO, &non_blocking);
        }
    }
#else
    l_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (l_socket != MM_BAD_SOCKET)
        fcntl(l_socket, F_SETFL, fcntl(l_socket, F_GETFL, 0) | O_NONBLOCK);
#endif

    if (l_socket == MM_BAD_SOCKET)
    {
        l_socket_failed = 1;
        DebugMessage(M64MSG_WARNING, "[MMLinkState] UDP socket unavailable; LinkState export disabled");
        return 0;
    }

    memset(&l_destination, 0, sizeof(l_destination));
    l_destination.sin_family = AF_INET;
    l_destination.sin_port = htons((unsigned short) port);
    l_destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DebugMessage(M64MSG_INFO, "[MMLinkState] exporting LinkState v%d to 127.0.0.1:%ld",
                 MM_LINKSTATE_VERSION, port);
    return 1;
}

/* Fire and forget: a dropped datagram or an absent listener is not an error. */
static void send_link_state(unsigned int frame_index, const mm_link_state* s,
                            const mm_player_state* p)
{
    uint8_t packet[MM_LINKSTATE_SIZE];
    int i;

    if (!open_socket())
        return;

    memset(packet, 0, sizeof(packet));
    memcpy(packet, "MMLS", 4);
    put_u16(packet + 4, MM_LINKSTATE_VERSION);
    put_u16(packet + 6, MM_LINKSTATE_SIZE);
    put_u32(packet + 8, l_sequence++);
    put_u32(packet + 12, frame_index);
    /* flags: bit 0 = ROM MD5 matched, bit 1 = Player actor valid */
    put_u32(packet + 16, 1u | (p->actor != 0 ? 2u : 0u));
    put_u16(packet + 20, (uint16_t) s->health);
    put_u16(packet + 22, (uint16_t) s->health_capacity);
    put_u16(packet + 24, (uint16_t) s->rupees);
    put_u16(packet + 26, s->time);
    put_u32(packet + 28, (uint32_t) s->day);
    packet[32] = (uint8_t) s->magic_level;
    packet[33] = (uint8_t) s->magic;
    packet[34] = s->player_form;
    packet[35] = s->equipped_mask;
    put_u32(packet + 36, p->actor);
    for (i = 0; i < 3; i++)
    {
        put_u32(packet + 40 + 4 * i, p->pos[i]);
        put_u16(packet + 52 + 2 * i, (uint16_t) p->world_rot[i]);
        put_u32(packet + 60 + 4 * i, p->velocity[i]);
        put_u32(packet + 80 + 4 * i, p->state_flags[i]);
    }
    put_u16(packet + 58, (uint16_t) p->facing);
    put_u32(packet + 72, p->speed);
    put_u32(packet + 76, p->linear_velocity);
    packet[92] = p->transformation;
    packet[93] = p->current_mask;
    put_u16(packet + 94, (uint16_t) p->scene_id);

    sendto(l_socket, (const char*) packet, sizeof(packet), 0,
           (const struct sockaddr*) &l_destination, sizeof(l_destination));
}
#endif

void mm_health_sampler_sample(unsigned int frame_index)
{
    static mm_link_state previous;
    static int has_previous_sample;
    static uint8_t previous_transformation = 0xFF;
    mm_link_state current;
    mm_player_state player;

    if (strcmp(ROM_SETTINGS.MD5, MM_ROM_MD5) != 0)
        return;

    read_link_state(&current);
    read_player_state(&player);

#ifdef MM_LINKSTATE_UDP
    send_link_state(frame_index, &current, &player);
#endif

    if (player.actor != 0 && player.transformation != previous_transformation)
    {
        DebugMessage(M64MSG_INFO,
                     "[MMHealthSampler] frame=%u player_actor=0x%08X transformation=%u current_mask=%u scene=%d (unverified fields)",
                     frame_index, player.actor, (unsigned) player.transformation,
                     (unsigned) player.current_mask, (int) player.scene_id);
        previous_transformation = player.transformation;
    }

    /* Change log, as before: wait for a loaded save (positive health). */
    if (current.health <= 0)
        return;

    if (!has_previous_sample || current.health != previous.health)
    {
        DebugMessage(M64MSG_INFO,
                     "[MMHealthSampler] frame=%u address=0x%08X health_s16=%d hearts=%.2f",
                     frame_index, MM_HEALTH_ADDRESS, (int) current.health,
                     (double) current.health / 16.0);
    }
    if (!has_previous_sample || current.player_form != previous.player_form ||
        current.equipped_mask != previous.equipped_mask)
    {
        DebugMessage(M64MSG_INFO,
                     "[MMHealthSampler] frame=%u player_form=%u equipped_mask=0x%02X (unverified fields)",
                     frame_index, (unsigned) current.player_form, (unsigned) current.equipped_mask);
    }
    previous = current;
    has_previous_sample = 1;
}
