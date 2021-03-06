
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <SDL2/SDL.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include "core/system.h"
#include "core/vmath.h"
#include "core/gl_util.h"
#include "core/gl_text.h"
#include "core/console.h"
#include "core/redblack.h"
#include "core/polygon.h"
#include "core/obb.h"
#include "render/camera.h"
#include "render/render.h"
#include "render/frustum.h"
#include "render/bordered_texture_atlas.h"
#include "render/shader_description.h"
#include "vt/vt_level.h"

#include "audio.h"
#include "room.h"
#include "trigger.h"
#include "mesh.h"
#include "skeletal_model.h"
#include "anim_state_control.h"
#include "character_controller.h"
#include "script.h"
#include "engine.h"
#include "physics.h"
#include "inventory.h"
#include "resource.h"


typedef struct fd_command_s
{
    uint16_t    function : 5;               // 0b00000000 00011111
    uint16_t    function_value : 3;         // 0b00000000 11100000  TR_III+
    uint16_t    sub_function : 7;           // 0b01111111 00000000
    uint16_t    end_bit : 1;                // 0b10000000 00000000
}fd_command_t, *fd_command_p;

typedef struct fd_trigger_head_s
{
    uint16_t    timer : 8;                  // 0b00000000 11111111   Used as common parameter for some commands.
    uint16_t    once : 1;                   // 0b00000001 00000000
    uint16_t    mask : 5;                   // 0b00111110 00000000
    uint16_t    uncnown : 2;
}fd_trigger_head_t, *fd_trigger_head_p;

typedef struct fd_trigger_function_s
{
    uint16_t    operands : 10;              // 0b00000011 11111111
    uint16_t    function : 5;               // 0b01111100 00000000
    uint16_t    cont_bit : 1;               // 0b10000000 00000000
}fd_trigger_function_t, *fd_trigger_function_p;

typedef struct fd_slope_s
{
    uint16_t    slope_t10 : 4;              // 0b00000000 00001111
    uint16_t    slope_t11 : 4;              // 0b00000000 11110000
    uint16_t    slope_t12 : 4;              // 0b00001111 00000000
    uint16_t    slope_t13 : 4;              // 0b11110000 00000000
}fd_slope_t, *fd_slope_p;


/*
 * Helper functions to convert legacy TR structs to native OpenTomb structs.
 */
__inline void TR_vertex_to_arr(float v[3], tr5_vertex_t *tr_v)
{
    v[0] = tr_v->x;
    v[1] =-tr_v->z;
    v[2] = tr_v->y;
}


__inline void TR_color_to_arr(float v[4], tr5_colour_t *tr_c)
{
    v[0] = tr_c->r * 2;
    v[1] = tr_c->g * 2;
    v[2] = tr_c->b * 2;
    v[3] = tr_c->a * 2;
}

/*
 * Functions for getting various parameters from legacy TR structs.
 */
void     TR_GetBFrameBB_Pos(class VT_Level *tr, size_t frame_offset, struct bone_frame_s *bone_frame);
int      TR_GetNumAnimationsForMoveable(class VT_Level *tr, size_t moveable_ind);
int      TR_GetNumFramesForAnimation(class VT_Level *tr, size_t animation_ind);
uint32_t TR_GetOriginalAnimationFrameOffset(uint32_t offset, uint32_t anim, class VT_Level *tr);

// Main functions which are used to translate legacy TR floor data
// to native OpenTomb structs.
uint32_t Res_Sector_BiggestCorner(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4);
void     Res_Sector_SetTweenFloorConfig(struct sector_tween_s *tween);
void     Res_Sector_SetTweenCeilingConfig(struct sector_tween_s *tween);
int      Res_Sector_IsWall(struct room_sector_s *wall_sector, struct room_sector_s *near_sector);
/*
 * BASIC SECTOR COLLISION LAYOUT
 *
 *
 *  OY                            OZ
 *  ^   0 ___________ 1            ^  1  ___________  2
 *  |    |           |             |    |           |
 *  |    |           |             |    |   tween   |
 *  |    |   SECTOR  |             |    |  corners  |
 *  |    |           |             |    |           |
 *  |   3|___________|2            |  0 |___________| 3
 *  |-------------------> OX       |--------------------> OXY
 */


uint32_t Res_Sector_BiggestCorner(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    v1 = (v1 > v2)?(v1):(v2);
    v2 = (v3 > v4)?(v3):(v4);
    return (v1 > v2)?(v1):(v2);
}


void Res_Sector_SetTweenFloorConfig(struct sector_tween_s *tween)
{
    tween->floor_tween_inverted = 0x00;

    if(((tween->floor_corners[1][2] > tween->floor_corners[0][2]) && (tween->floor_corners[3][2] > tween->floor_corners[2][2])) ||
       ((tween->floor_corners[1][2] < tween->floor_corners[0][2]) && (tween->floor_corners[3][2] < tween->floor_corners[2][2])))
    {
        tween->floor_tween_type = TR_SECTOR_TWEEN_TYPE_2TRIANGLES;              // like a butterfly
    }
    else if((tween->floor_corners[0][2] != tween->floor_corners[1][2]) &&
       (tween->floor_corners[2][2] != tween->floor_corners[3][2]))
    {
        tween->floor_tween_type = TR_SECTOR_TWEEN_TYPE_QUAD;
    }
    else if(tween->floor_corners[0][2] != tween->floor_corners[1][2])
    {
        tween->floor_tween_type = TR_SECTOR_TWEEN_TYPE_TRIANGLE_LEFT;
    }
    else if(tween->floor_corners[2][2] != tween->floor_corners[3][2])
    {
        tween->floor_tween_type = TR_SECTOR_TWEEN_TYPE_TRIANGLE_RIGHT;
    }
    else
    {
        tween->floor_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
    }
}

void Res_Sector_SetTweenCeilingConfig(struct sector_tween_s *tween)
{
    tween->ceiling_tween_inverted = 0x00;

    if(((tween->ceiling_corners[1][2] > tween->ceiling_corners[0][2]) && (tween->ceiling_corners[3][2] > tween->ceiling_corners[2][2])) ||
       ((tween->ceiling_corners[1][2] < tween->ceiling_corners[0][2]) && (tween->ceiling_corners[3][2] < tween->ceiling_corners[2][2])))
    {
        tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_2TRIANGLES;            // like a butterfly
    }
    else if((tween->ceiling_corners[0][2] != tween->ceiling_corners[1][2]) &&
       (tween->ceiling_corners[2][2] != tween->ceiling_corners[3][2]))
    {
        tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_QUAD;
    }
    else if(tween->ceiling_corners[0][2] != tween->ceiling_corners[1][2])
    {
        tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_TRIANGLE_LEFT;
    }
    else if(tween->ceiling_corners[2][2] != tween->ceiling_corners[3][2])
    {
        tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_TRIANGLE_RIGHT;
    }
    else
    {
        tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
    }
}


int Res_Sector_IsWall(struct room_sector_s *wall_sector, struct room_sector_s *near_sector)
{
    if(!wall_sector->portal_to_room && !near_sector->portal_to_room && (wall_sector->floor_penetration_config == TR_PENETRATION_CONFIG_WALL))
    {
        return 1;
    }

    if(!near_sector->portal_to_room && wall_sector->portal_to_room && (near_sector->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
    {
        wall_sector = Sector_GetPortalSectorTarget(wall_sector);
        if((wall_sector->floor_penetration_config == TR_PENETRATION_CONFIG_WALL) || (0 == Sectors_Is2SidePortals(near_sector, wall_sector)))
        {
            return 1;
        }
    }

    return 0;
}

///@TODO: resolve floor >> ceiling case
void Res_Sector_GenTweens(struct room_s *room, struct sector_tween_s *room_tween)
{
    for(uint16_t h = 0; h < room->sectors_y - 1; h++)
    {
        for(uint16_t w = 0; w < room->sectors_x - 1; w++)
        {
            // Init X-plane tween [ | ]
            room_sector_p current_heightmap = room->sectors + (w * room->sectors_y + h);
            room_sector_p next_heightmap    = current_heightmap + 1;
            char joined_floors = 0;
            char joined_ceilings = 0;

            /* XY corners coordinates must be calculated from native room sector */
            room_tween->floor_corners[0][1] = current_heightmap->floor_corners[0][1];
            room_tween->floor_corners[1][1] = room_tween->floor_corners[0][1];
            room_tween->floor_corners[2][1] = room_tween->floor_corners[0][1];
            room_tween->floor_corners[3][1] = room_tween->floor_corners[0][1];
            room_tween->floor_corners[0][0] = current_heightmap->floor_corners[0][0];
            room_tween->floor_corners[1][0] = room_tween->floor_corners[0][0];
            room_tween->floor_corners[2][0] = current_heightmap->floor_corners[1][0];
            room_tween->floor_corners[3][0] = room_tween->floor_corners[2][0];

            room_tween->ceiling_corners[0][1] = current_heightmap->ceiling_corners[0][1];
            room_tween->ceiling_corners[1][1] = room_tween->ceiling_corners[0][1];
            room_tween->ceiling_corners[2][1] = room_tween->ceiling_corners[0][1];
            room_tween->ceiling_corners[3][1] = room_tween->ceiling_corners[0][1];
            room_tween->ceiling_corners[0][0] = current_heightmap->ceiling_corners[0][0];
            room_tween->ceiling_corners[1][0] = room_tween->ceiling_corners[0][0];
            room_tween->ceiling_corners[2][0] = current_heightmap->ceiling_corners[1][0];
            room_tween->ceiling_corners[3][0] = room_tween->ceiling_corners[2][0];

            if(w > 0)
            {
                if((next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) || (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))                                                           // Init X-plane tween [ | ]
                {
                    if(Res_Sector_IsWall(next_heightmap, current_heightmap))
                    {
                        room_tween->floor_corners[0][2] = current_heightmap->floor_corners[0][2];
                        room_tween->floor_corners[1][2] = current_heightmap->ceiling_corners[0][2];
                        room_tween->floor_corners[2][2] = current_heightmap->ceiling_corners[1][2];
                        room_tween->floor_corners[3][2] = current_heightmap->floor_corners[1][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->floor_tween_inverted = 0x01;
                        room_tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
                        joined_floors = 1;
                        joined_ceilings = 1;
                    }
                    else if(Res_Sector_IsWall(current_heightmap, next_heightmap))
                    {
                        room_tween->floor_corners[0][2] = next_heightmap->floor_corners[3][2];
                        room_tween->floor_corners[1][2] = next_heightmap->ceiling_corners[3][2];
                        room_tween->floor_corners[2][2] = next_heightmap->ceiling_corners[2][2];
                        room_tween->floor_corners[3][2] = next_heightmap->floor_corners[2][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
                        joined_floors = 1;
                        joined_ceilings = 1;
                    }
                    else
                    {
                        /************************** SECTION WITH DROPS CALCULATIONS **********************/
                        if((!current_heightmap->portal_to_room && !next_heightmap->portal_to_room) || Sectors_Is2SidePortals(current_heightmap, next_heightmap))
                        {
                            current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                            next_heightmap    = Sector_GetPortalSectorTarget(next_heightmap);
                            if(!current_heightmap->portal_to_room && !next_heightmap->portal_to_room && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                            {
                                if((current_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID) || (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                                {
                                    room_tween->floor_corners[0][2] = current_heightmap->floor_corners[0][2];
                                    room_tween->floor_corners[1][2] = next_heightmap->floor_corners[3][2];
                                    room_tween->floor_corners[2][2] = next_heightmap->floor_corners[2][2];
                                    room_tween->floor_corners[3][2] = current_heightmap->floor_corners[1][2];
                                    Res_Sector_SetTweenFloorConfig(room_tween);
                                    room_tween->floor_tween_inverted = 0x01;
                                    joined_floors = 1;
                                }
                                if((current_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID) || (next_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                                {
                                    room_tween->ceiling_corners[0][2] = current_heightmap->ceiling_corners[0][2];
                                    room_tween->ceiling_corners[1][2] = next_heightmap->ceiling_corners[3][2];
                                    room_tween->ceiling_corners[2][2] = next_heightmap->ceiling_corners[2][2];
                                    room_tween->ceiling_corners[3][2] = current_heightmap->ceiling_corners[1][2];
                                    Res_Sector_SetTweenCeilingConfig(room_tween);
                                    joined_ceilings = 1;
                                }
                            }
                        }
                    }
                }

                current_heightmap = room->sectors + (w * room->sectors_y + h);
                next_heightmap    = current_heightmap + 1;
                if((joined_floors == 0) && (!current_heightmap->portal_to_room || !next_heightmap->portal_to_room))
                {
                    char valid = 0;
                    if(next_heightmap->portal_to_room && (current_heightmap->sector_above != NULL) && (current_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        next_heightmap = Sector_GetPortalSectorTarget(next_heightmap);
                        if(next_heightmap->owner_room->id == current_heightmap->sector_above->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(current_heightmap->sector_above->owner_room, next_heightmap->pos);
                            if(rs && (rs->portal_to_room == next_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if(current_heightmap->portal_to_room && (next_heightmap->sector_above != NULL) && (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                        if(current_heightmap->owner_room->id == next_heightmap->sector_above->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(next_heightmap->sector_above->owner_room, current_heightmap->pos);
                            if(rs && (rs->portal_to_room == current_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if((valid == 1) && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                    {
                        room_tween->floor_corners[0][2] = current_heightmap->floor_corners[0][2];
                        room_tween->floor_corners[1][2] = next_heightmap->floor_corners[3][2];
                        room_tween->floor_corners[2][2] = next_heightmap->floor_corners[2][2];
                        room_tween->floor_corners[3][2] = current_heightmap->floor_corners[1][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->floor_tween_inverted = 0x01;
                    }
                }

                current_heightmap = room->sectors + (w * room->sectors_y + h);
                next_heightmap    = current_heightmap + 1;
                if((joined_ceilings == 0) && (!current_heightmap->portal_to_room || !next_heightmap->portal_to_room))
                {
                    char valid = 0;
                    if(next_heightmap->portal_to_room && (current_heightmap->sector_below != NULL) && (current_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        next_heightmap = Sector_GetPortalSectorTarget(next_heightmap);
                        if(next_heightmap->owner_room->id == current_heightmap->sector_below->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(current_heightmap->sector_below->owner_room, next_heightmap->pos);
                            if(rs && (rs->portal_to_room == next_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if(current_heightmap->portal_to_room && (next_heightmap->sector_below != NULL) && (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                        if(current_heightmap->owner_room->id == next_heightmap->sector_below->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(next_heightmap->sector_below->owner_room, current_heightmap->pos);
                            if(rs && (rs->portal_to_room == current_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if((valid == 1) && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                    {
                        room_tween->ceiling_corners[0][2] = current_heightmap->ceiling_corners[0][2];
                        room_tween->ceiling_corners[1][2] = next_heightmap->ceiling_corners[3][2];
                        room_tween->ceiling_corners[2][2] = next_heightmap->ceiling_corners[2][2];
                        room_tween->ceiling_corners[3][2] = current_heightmap->ceiling_corners[1][2];
                        Res_Sector_SetTweenCeilingConfig(room_tween);
                    }
                }
            }

            /*****************************************************************************************************
             ********************************   CENTRE  OF  THE  ALGORITHM   *************************************
             *****************************************************************************************************/

            room_tween++;
            current_heightmap = room->sectors + (w * room->sectors_y + h);
            next_heightmap    = room->sectors + ((w + 1) * room->sectors_y + h);
            room_tween->floor_corners[0][0] = current_heightmap->floor_corners[1][0];
            room_tween->floor_corners[1][0] = room_tween->floor_corners[0][0];
            room_tween->floor_corners[2][0] = room_tween->floor_corners[0][0];
            room_tween->floor_corners[3][0] = room_tween->floor_corners[0][0];
            room_tween->floor_corners[0][1] = current_heightmap->floor_corners[1][1];
            room_tween->floor_corners[1][1] = room_tween->floor_corners[0][1];
            room_tween->floor_corners[2][1] = current_heightmap->floor_corners[2][1];
            room_tween->floor_corners[3][1] = room_tween->floor_corners[2][1];

            room_tween->ceiling_corners[0][0] = current_heightmap->ceiling_corners[1][0];
            room_tween->ceiling_corners[1][0] = room_tween->ceiling_corners[0][0];
            room_tween->ceiling_corners[2][0] = room_tween->ceiling_corners[0][0];
            room_tween->ceiling_corners[3][0] = room_tween->ceiling_corners[0][0];
            room_tween->ceiling_corners[0][1] = current_heightmap->ceiling_corners[1][1];
            room_tween->ceiling_corners[1][1] = room_tween->ceiling_corners[0][1];
            room_tween->ceiling_corners[2][1] = current_heightmap->ceiling_corners[2][1];
            room_tween->ceiling_corners[3][1] = room_tween->ceiling_corners[2][1];

            joined_floors = 0;
            joined_ceilings = 0;

            if(h > 0)
            {
                if((next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) || (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                {
                    // Init Y-plane tween  [ - ]
                    if(Res_Sector_IsWall(next_heightmap, current_heightmap))
                    {
                        room_tween->floor_corners[0][2] = current_heightmap->floor_corners[1][2];
                        room_tween->floor_corners[1][2] = current_heightmap->ceiling_corners[1][2];
                        room_tween->floor_corners[2][2] = current_heightmap->ceiling_corners[2][2];
                        room_tween->floor_corners[3][2] = current_heightmap->floor_corners[2][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->floor_tween_inverted = 0x01;
                        room_tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
                        joined_floors = 1;
                        joined_ceilings = 1;
                    }
                    else if(Res_Sector_IsWall(current_heightmap, next_heightmap))
                    {
                        room_tween->floor_corners[0][2] = next_heightmap->floor_corners[0][2];
                        room_tween->floor_corners[1][2] = next_heightmap->ceiling_corners[0][2];
                        room_tween->floor_corners[2][2] = next_heightmap->ceiling_corners[3][2];
                        room_tween->floor_corners[3][2] = next_heightmap->floor_corners[3][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->ceiling_tween_type = TR_SECTOR_TWEEN_TYPE_NONE;
                        joined_floors = 1;
                        joined_ceilings = 1;
                    }
                    else
                    {
                        /************************** BIG SECTION WITH DROPS CALCULATIONS **********************/
                        if((!current_heightmap->portal_to_room && !next_heightmap->portal_to_room) || Sectors_Is2SidePortals(current_heightmap, next_heightmap))
                        {
                            current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                            next_heightmap    = Sector_GetPortalSectorTarget(next_heightmap);
                            if(!current_heightmap->portal_to_room && !next_heightmap->portal_to_room && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                            {
                                if((current_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID) || (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                                {
                                    room_tween->floor_corners[0][2] = current_heightmap->floor_corners[1][2];
                                    room_tween->floor_corners[1][2] = next_heightmap->floor_corners[0][2];
                                    room_tween->floor_corners[2][2] = next_heightmap->floor_corners[3][2];
                                    room_tween->floor_corners[3][2] = current_heightmap->floor_corners[2][2];
                                    Res_Sector_SetTweenFloorConfig(room_tween);
                                    room_tween->floor_tween_inverted = 0x01;
                                    joined_floors = 1;
                                }
                                if((current_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID) || (next_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                                {
                                    room_tween->ceiling_corners[0][2] = current_heightmap->ceiling_corners[1][2];
                                    room_tween->ceiling_corners[1][2] = next_heightmap->ceiling_corners[0][2];
                                    room_tween->ceiling_corners[2][2] = next_heightmap->ceiling_corners[3][2];
                                    room_tween->ceiling_corners[3][2] = current_heightmap->ceiling_corners[2][2];
                                    Res_Sector_SetTweenCeilingConfig(room_tween);
                                    joined_ceilings = 1;
                                }
                            }
                        }
                    }
                }

                current_heightmap = room->sectors + (w * room->sectors_y + h);
                next_heightmap    = room->sectors + ((w + 1) * room->sectors_y + h);
                if((joined_floors == 0) && (!current_heightmap->portal_to_room || !next_heightmap->portal_to_room))
                {
                    char valid = 0;
                    if(next_heightmap->portal_to_room && (current_heightmap->sector_above != NULL) && (current_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        next_heightmap = Sector_GetPortalSectorTarget(next_heightmap);
                        if(next_heightmap->owner_room->id == current_heightmap->sector_above->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(current_heightmap->sector_above->owner_room, next_heightmap->pos);
                            if(rs && (rs->portal_to_room == next_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if(current_heightmap->portal_to_room && (next_heightmap->sector_above != NULL) && (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                        if(current_heightmap->owner_room->id == next_heightmap->sector_above->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(next_heightmap->sector_above->owner_room, current_heightmap->pos);
                            if(rs && (rs->portal_to_room == current_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if((valid == 1) && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                    {
                        room_tween->floor_corners[0][2] = current_heightmap->floor_corners[1][2];
                        room_tween->floor_corners[1][2] = next_heightmap->floor_corners[0][2];
                        room_tween->floor_corners[2][2] = next_heightmap->floor_corners[3][2];
                        room_tween->floor_corners[3][2] = current_heightmap->floor_corners[2][2];
                        Res_Sector_SetTweenFloorConfig(room_tween);
                        room_tween->floor_tween_inverted = 0x01;
                    }
                }

                current_heightmap = room->sectors + (w * room->sectors_y + h);
                next_heightmap    = room->sectors + ((w + 1) * room->sectors_y + h);
                if((joined_ceilings == 0) && (!current_heightmap->portal_to_room || !next_heightmap->portal_to_room))
                {
                    char valid = 0;
                    if(next_heightmap->portal_to_room && (current_heightmap->sector_below != NULL) && (current_heightmap->ceiling_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        next_heightmap = Sector_GetPortalSectorTarget(next_heightmap);
                        if(next_heightmap->owner_room->id == current_heightmap->sector_below->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(current_heightmap->sector_below->owner_room, next_heightmap->pos);
                            if(rs && (rs->portal_to_room == next_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if(current_heightmap->portal_to_room && (next_heightmap->sector_below != NULL) && (next_heightmap->floor_penetration_config == TR_PENETRATION_CONFIG_SOLID))
                    {
                        current_heightmap = Sector_GetPortalSectorTarget(current_heightmap);
                        if(current_heightmap->owner_room->id == next_heightmap->sector_below->owner_room->id)
                        {
                            valid = 1;
                        }
                        if(valid == 0)
                        {
                            room_sector_p rs = Room_GetSectorRaw(next_heightmap->sector_below->owner_room, current_heightmap->pos);
                            if(rs && (rs->portal_to_room == current_heightmap->owner_room))
                            {
                                valid = 1;
                            }
                        }
                    }

                    if((valid == 1) && (current_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL) && (next_heightmap->floor_penetration_config != TR_PENETRATION_CONFIG_WALL))
                    {
                        room_tween->ceiling_corners[0][2] = current_heightmap->ceiling_corners[1][2];
                        room_tween->ceiling_corners[1][2] = next_heightmap->ceiling_corners[0][2];
                        room_tween->ceiling_corners[2][2] = next_heightmap->ceiling_corners[3][2];
                        room_tween->ceiling_corners[3][2] = current_heightmap->ceiling_corners[2][2];
                        Res_Sector_SetTweenCeilingConfig(room_tween);
                    }
                }
            }
            room_tween++;
        }    ///END for(uint16_t w = 0; w < room->sectors_x - 1; w++)
    }    ///END for(uint16_t h = 0; h < room->sectors_y - 1; h++)
}


int Res_Sector_In2SideOfPortal(struct room_sector_s *s1, struct room_sector_s *s2, struct portal_s *p)
{
    if((s1->pos[0] == s2->pos[0]) && (s1->pos[1] != s2->pos[1]) && (fabs(p->norm[1]) > 0.99))
    {
        float min_x, max_x, min_y, max_y, x;
        max_x = min_x = p->vertex[0];
        for(uint16_t i = 1; i < p->vertex_count; i++)
        {
            x = p->vertex[3 * i + 0];
            if(x > max_x)
            {
                max_x = x;
            }
            if(x < min_x)
            {
                min_x = x;
            }
        }
        if(s1->pos[1] > s2->pos[1])
        {
            min_y = s2->pos[1];
            max_y = s1->pos[1];
        }
        else
        {
            min_y = s1->pos[1];
            max_y = s2->pos[1];
        }

        if((s1->pos[0] < max_x) && (s1->pos[0] > min_x) && (p->centre[1] < max_y) && (p->centre[1] > min_y))
        {
            return 1;
        }
    }
    else if((s1->pos[0] != s2->pos[0]) && (s1->pos[1] == s2->pos[1]) && (fabs(p->norm[0]) > 0.99))
    {
        float min_x, max_x, min_y, max_y, y;
        max_y = min_y = p->vertex[1];
        for(uint16_t i = 1; i < p->vertex_count; i++)
        {
            y = p->vertex[3 * i + 1];
            if(y > max_y)
            {
                max_y = y;
            }
            if(y < min_y)
            {
                min_y = y;
            }
        }
        if(s1->pos[0] > s2->pos[0])
        {
            min_x = s2->pos[0];
            max_x = s1->pos[0];
        }
        else
        {
            min_x = s1->pos[0];
            max_x = s2->pos[0];
        }

        if((p->centre[0] < max_x) && (p->centre[0] > min_x) && (s1->pos[1] < max_y) && (s1->pos[1] > min_y))
        {
            return 1;
        }
    }

    return 0;
}


void Res_RoomLightCalculate(struct light_s *light, struct tr5_room_light_s *tr_light)
{
    switch(tr_light->light_type)
    {
        case 0:
            light->light_type = LT_SUN;
            break;

        case 1:
            light->light_type = LT_POINT;
            break;

        case 2:
            light->light_type = LT_SPOTLIGHT;
            break;

        case 3:
            light->light_type = LT_SHADOW;
            break;

        default:
            light->light_type = LT_NULL;
            break;
    }

    light->pos[0] = tr_light->pos.x;
    light->pos[1] =-tr_light->pos.z;
    light->pos[2] = tr_light->pos.y;
    light->pos[3] = 1.0f;

    if(light->light_type == LT_SHADOW)
    {
        light->colour[0] = -(tr_light->color.r / 255.0f) * tr_light->intensity;
        light->colour[1] = -(tr_light->color.g / 255.0f) * tr_light->intensity;
        light->colour[2] = -(tr_light->color.b / 255.0f) * tr_light->intensity;
        light->colour[3] = 1.0f;
    }
    else
    {
        light->colour[0] = (tr_light->color.r / 255.0f) * tr_light->intensity;
        light->colour[1] = (tr_light->color.g / 255.0f) * tr_light->intensity;
        light->colour[2] = (tr_light->color.b / 255.0f) * tr_light->intensity;
        light->colour[3] = 1.0f;
    }

    light->inner = tr_light->r_inner;
    light->outer = tr_light->r_outer;
    light->length = tr_light->length;
    light->cutoff = tr_light->cutoff;

    light->falloff = 0.001f / light->outer;
}


void Res_RoomSectorsCalculate(struct room_s *rooms, uint32_t rooms_count, uint32_t room_index, class VT_Level *tr)
{
    room_sector_p sector;
    room_p room = rooms + room_index;
    tr5_room_t *tr_room = &tr->rooms[room_index];

    /*
     * Sectors loading
     */
    sector = room->sectors;
    for(uint32_t i = 0; i < room->sectors_count; i++, sector++)
    {
        /*
         * Let us fill pointers to sectors above and sectors below
         */
        uint8_t rp = tr_room->sector_list[i].room_below;
        sector->sector_below = NULL;
        if(rp < rooms_count && rp != 255)
        {
            sector->sector_below = Room_GetSectorRaw(rooms + rp, sector->pos);
        }
        rp = tr_room->sector_list[i].room_above;
        sector->sector_above = NULL;
        if(rp < rooms_count && rp != 255)
        {
            sector->sector_above = Room_GetSectorRaw(rooms + rp, sector->pos);
        }

        room_sector_p near_sector = NULL;

        /**** OX *****/
        if((sector->index_y > 0) && (sector->index_y < room->sectors_y - 1) && (sector->index_x == 0))
        {
            near_sector = sector + room->sectors_y;
        }
        if((sector->index_y > 0) && (sector->index_y < room->sectors_y - 1) && (sector->index_x == room->sectors_x - 1))
        {
            near_sector = sector - room->sectors_y;
        }
        /**** OY *****/
        if((sector->index_x > 0) && (sector->index_x < room->sectors_x - 1) && (sector->index_y == 0))
        {
            near_sector = sector + 1;
        }
        if((sector->index_x > 0) && (sector->index_x < room->sectors_x - 1) && (sector->index_y == room->sectors_y - 1))
        {
            near_sector = sector - 1;
        }

        if(near_sector && sector->portal_to_room)
        {
            portal_p p = room->portals;
            for(uint16_t j = 0; j < room->portals_count; j++, p++)
            {
                if((p->norm[2] < 0.01) && ((p->norm[2] > -0.01)))
                {
                    room_sector_p dst = Room_GetSectorRaw(p->dest_room, sector->pos);
                    room_sector_p orig_dst = Room_GetSectorRaw(sector->portal_to_room, sector->pos);
                    if(dst && !dst->portal_to_room && (dst->floor != TR_METERING_WALLHEIGHT) && (dst->ceiling != TR_METERING_WALLHEIGHT) && (sector->portal_to_room != p->dest_room) && (dst->floor < orig_dst->floor) && Res_Sector_In2SideOfPortal(near_sector, dst, p))
                    {
                        sector->portal_to_room = p->dest_room;
                        orig_dst = dst;
                    }
                }
            }
        }
    }
}


int Res_Sector_TranslateFloorData(struct room_s *rooms, uint32_t rooms_count, struct room_sector_s *sector, class VT_Level *tr)
{
    int ret = 0;

    if(!sector || (sector->trig_index <= 0) || (sector->trig_index >= tr->floor_data_size))
    {
        return ret;
    }

    uint16_t *entry = tr->floor_data + sector->trig_index;
    size_t max_offset = tr->floor_data_size;
    size_t current_offset = sector->trig_index;
    fd_command_t fd_command;

    sector->flags = 0;  // Clear sector flags before parsing.
    do
    {
        fd_command = *((fd_command_p)entry);
        entry++;
        current_offset++;
        switch(fd_command.function)
        {
            case TR_FD_FUNC_PORTALSECTOR:          // PORTAL DATA
                if(fd_command.sub_function == 0x00)
                {
                    if(*entry < rooms_count)
                    {
                        sector->portal_to_room = rooms + *entry;
                        sector->floor_penetration_config   = TR_PENETRATION_CONFIG_GHOST;
                        sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_GHOST;
                    }
                    entry ++;
                    current_offset++;
                }
                break;

            case TR_FD_FUNC_FLOORSLANT:          // FLOOR SLANT
                if(fd_command.sub_function == 0x00)
                {
                    int8_t raw_y_slant =  (*entry & 0x00FF);
                    int8_t raw_x_slant = ((*entry & 0xFF00) >> 8);

                    sector->floor_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NONE;
                    sector->floor_penetration_config = TR_PENETRATION_CONFIG_SOLID;

                    if(raw_x_slant > 0)
                    {
                        sector->floor_corners[2][2] -= ((float)raw_x_slant * TR_METERING_STEP);
                        sector->floor_corners[3][2] -= ((float)raw_x_slant * TR_METERING_STEP);
                    }
                    else if(raw_x_slant < 0)
                    {
                        sector->floor_corners[0][2] -= (abs((float)raw_x_slant) * TR_METERING_STEP);
                        sector->floor_corners[1][2] -= (abs((float)raw_x_slant) * TR_METERING_STEP);
                    }

                    if(raw_y_slant > 0)
                    {
                        sector->floor_corners[0][2] -= ((float)raw_y_slant * TR_METERING_STEP);
                        sector->floor_corners[3][2] -= ((float)raw_y_slant * TR_METERING_STEP);
                    }
                    else if(raw_y_slant < 0)
                    {
                        sector->floor_corners[1][2] -= (abs((float)raw_y_slant) * TR_METERING_STEP);
                        sector->floor_corners[2][2] -= (abs((float)raw_y_slant) * TR_METERING_STEP);
                    }

                    entry++;
                    current_offset++;
                }
                break;

            case TR_FD_FUNC_CEILINGSLANT:          // CEILING SLANT
                if(fd_command.sub_function == 0x00)
                {
                    int8_t raw_y_slant =  (*entry & 0x00FF);
                    int8_t raw_x_slant = ((*entry & 0xFF00) >> 8);

                    sector->ceiling_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NONE;
                    sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_SOLID;

                    if(raw_x_slant > 0)
                    {
                        sector->ceiling_corners[3][2] += ((float)raw_x_slant * TR_METERING_STEP);
                        sector->ceiling_corners[2][2] += ((float)raw_x_slant * TR_METERING_STEP);
                    }
                    else if(raw_x_slant < 0)
                    {
                        sector->ceiling_corners[1][2] += (abs((float)raw_x_slant) * TR_METERING_STEP);
                        sector->ceiling_corners[0][2] += (abs((float)raw_x_slant) * TR_METERING_STEP);
                    }

                    if(raw_y_slant > 0)
                    {
                        sector->ceiling_corners[1][2] += ((float)raw_y_slant * TR_METERING_STEP);
                        sector->ceiling_corners[2][2] += ((float)raw_y_slant * TR_METERING_STEP);
                    }
                    else if(raw_y_slant < 0)
                    {
                        sector->ceiling_corners[0][2] += (abs((float)raw_y_slant) * TR_METERING_STEP);
                        sector->ceiling_corners[3][2] += (abs((float)raw_y_slant) * TR_METERING_STEP);
                    }

                    entry++;
                    current_offset++;
                }
                break;

            case TR_FD_FUNC_TRIGGER:          // TRIGGERS
                {
                    fd_trigger_head_t fd_trigger_head = *((fd_trigger_head_p)entry);

                    if(sector->trigger == NULL)
                    {
                        sector->trigger = (trigger_header_p)malloc(sizeof(trigger_header_t));
                    }
                    else
                    {
                        Con_AddLine("SECTOR HAS TWO OR MORE TRIGGERS!!!", FONTSTYLE_CONSOLE_WARNING);
                    }
                    sector->trigger->commands = NULL;
                    sector->trigger->function_value = fd_command.function_value;
                    sector->trigger->sub_function = fd_command.sub_function;
                    sector->trigger->mask = fd_trigger_head.mask;
                    sector->trigger->timer = fd_trigger_head.timer;
                    sector->trigger->once = fd_trigger_head.once;

                    // Now parse operand chain for trigger function!
                    fd_trigger_function_t fd_trigger_function;
                    do
                    {
                        trigger_command_p command = (trigger_command_p)malloc(sizeof(trigger_command_t));
                        trigger_command_p *last_command_ptr = &sector->trigger->commands;
                        command->next = NULL;
                        while(*last_command_ptr)
                        {
                            last_command_ptr = &((*last_command_ptr)->next);
                        }
                        *last_command_ptr = command;

                        entry++;
                        current_offset++;
                        fd_trigger_function = *((fd_trigger_function_p)entry);
                        command->function = fd_trigger_function.function;
                        command->operands = fd_trigger_function.operands;
                        command->once = 0;
                        command->cam_index = 0;
                        command->cam_timer = 0;
                        command->cam_move = 0;

                        switch(command->function)
                        {
                            case TR_FD_TRIGFUNC_SET_CAMERA:
                                {
                                    command->cam_index = (*entry) & 0x007F;
                                    entry++;
                                    current_offset++;
                                    command->cam_timer = ((*entry) & 0x00FF);
                                    command->once      = ((*entry) & 0x0100) >> 8;
                                    command->cam_move  = ((*entry) & 0x1000) >> 12;
                                    fd_trigger_function.cont_bit  = ((*entry) & 0x8000) >> 15;
                                }
                                break;

                            case TR_FD_TRIGFUNC_FLYBY:
                                {
                                    entry++;
                                    current_offset++;
                                    command->once  = ((*entry) & 0x0100) >> 8;
                                    fd_trigger_function.cont_bit  = ((*entry) & 0x8000) >> 15;
                                }
                                break;

                            case TR_FD_TRIGFUNC_OBJECT:
                            case TR_FD_TRIGFUNC_UWCURRENT:
                            case TR_FD_TRIGFUNC_FLIPMAP:
                            case TR_FD_TRIGFUNC_FLIPON:
                            case TR_FD_TRIGFUNC_FLIPOFF:
                            case TR_FD_TRIGFUNC_SET_TARGET:
                            case TR_FD_TRIGFUNC_ENDLEVEL:
                            case TR_FD_TRIGFUNC_PLAYTRACK:
                            case TR_FD_TRIGFUNC_FLIPEFFECT:
                            case TR_FD_TRIGFUNC_SECRET:
                            case TR_FD_TRIGFUNC_CLEARBODIES:
                            case TR_FD_TRIGFUNC_CUTSCENE:
                                break;

                            default: // UNKNOWN!
                                break;
                        };
                    }
                    while(!fd_trigger_function.cont_bit && (current_offset < max_offset));
                }
                break;

            case TR_FD_FUNC_DEATH:
                sector->flags |= SECTOR_FLAG_DEATH;
                break;

            case TR_FD_FUNC_CLIMB:
                // First 4 sector flags are similar to subfunction layout.
                sector->flags |= (uint32_t)fd_command.sub_function;
                break;

            case TR_FD_FUNC_MONKEY:
                sector->flags |= SECTOR_FLAG_CLIMB_CEILING;
                break;

            case TR_FD_FUNC_MINECART_LEFT:
                // Minecart left (TR3) and trigger triggerer mark (TR4-5) has the same flag value.
                // We re-parse them properly here.
                if(tr->game_version < TR_IV)
                {
                    sector->flags |= SECTOR_FLAG_MINECART_LEFT;
                }
                else
                {
                    sector->flags |= SECTOR_FLAG_TRIGGERER_MARK;
                }
                break;

            case TR_FD_FUNC_MINECART_RIGHT:
                // Minecart right (TR3) and beetle mark (TR4-5) has the same flag value.
                // We re-parse them properly here.
                if(tr->game_version < TR_IV)
                {
                    sector->flags |= SECTOR_FLAG_MINECART_RIGHT;
                }
                else
                {
                    sector->flags |= SECTOR_FLAG_BEETLE_MARK;
                }
                break;

            default:
                // Other functions are TR3+ collisional triangle functions.
                if( (fd_command.function >= TR_FD_FUNC_FLOORTRIANGLE_NW) &&
                    (fd_command.function <= TR_FD_FUNC_CEILINGTRIANGLE_NE_PORTAL_SE) )
                {
                    fd_slope_t fd_slope = *((fd_slope_p)entry);
                    float overall_adjustment = (float)Res_Sector_BiggestCorner(fd_slope.slope_t10, fd_slope.slope_t11, fd_slope.slope_t12, fd_slope.slope_t13) * TR_METERING_STEP;
                    entry++;
                    current_offset++;

                    if( (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NW)           ||
                        (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NW_PORTAL_SW) ||
                        (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NW_PORTAL_NE)  )
                    {
                        sector->floor_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NW;

                        sector->floor_corners[0][2] -= overall_adjustment - ((float)fd_slope.slope_t12 * TR_METERING_STEP);
                        sector->floor_corners[1][2] -= overall_adjustment - ((float)fd_slope.slope_t13 * TR_METERING_STEP);
                        sector->floor_corners[2][2] -= overall_adjustment - ((float)fd_slope.slope_t10 * TR_METERING_STEP);
                        sector->floor_corners[3][2] -= overall_adjustment - ((float)fd_slope.slope_t11 * TR_METERING_STEP);

                        if(fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NW_PORTAL_SW)
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_A;
                        }
                        else if(fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NW_PORTAL_NE)
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_B;
                        }
                        else
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_SOLID;
                        }
                    }
                    else if( (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NE)           ||
                             (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NE_PORTAL_NW) ||
                             (fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NE_PORTAL_SE)  )
                    {
                        sector->floor_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NE;

                        sector->floor_corners[0][2] -= overall_adjustment - ((float)fd_slope.slope_t12 * TR_METERING_STEP);
                        sector->floor_corners[1][2] -= overall_adjustment - ((float)fd_slope.slope_t13 * TR_METERING_STEP);
                        sector->floor_corners[2][2] -= overall_adjustment - ((float)fd_slope.slope_t10 * TR_METERING_STEP);
                        sector->floor_corners[3][2] -= overall_adjustment - ((float)fd_slope.slope_t11 * TR_METERING_STEP);

                        if(fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NE_PORTAL_NW)
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_A;
                        }
                        else if(fd_command.function == TR_FD_FUNC_FLOORTRIANGLE_NE_PORTAL_SE)
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_B;
                        }
                        else
                        {
                            sector->floor_penetration_config = TR_PENETRATION_CONFIG_SOLID;
                        }
                    }
                    else if( (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NW)           ||
                             (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NW_PORTAL_SW) ||
                             (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NW_PORTAL_NE)  )
                    {
                        sector->ceiling_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NW;

                        sector->ceiling_corners[0][2] += overall_adjustment - (float)(fd_slope.slope_t11 * TR_METERING_STEP);
                        sector->ceiling_corners[1][2] += overall_adjustment - (float)(fd_slope.slope_t10 * TR_METERING_STEP);
                        sector->ceiling_corners[2][2] += overall_adjustment - (float)(fd_slope.slope_t13 * TR_METERING_STEP);
                        sector->ceiling_corners[3][2] += overall_adjustment - (float)(fd_slope.slope_t12 * TR_METERING_STEP);

                        if(fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NW_PORTAL_SW)
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_A;
                        }
                        else if(fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NW_PORTAL_NE)
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_B;
                        }
                        else
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_SOLID;
                        }
                    }
                    else if( (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NE)           ||
                             (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NE_PORTAL_NW) ||
                             (fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NE_PORTAL_SE)  )
                    {
                        sector->ceiling_diagonal_type = TR_SECTOR_DIAGONAL_TYPE_NE;

                        sector->ceiling_corners[0][2] += overall_adjustment - (float)(fd_slope.slope_t11 * TR_METERING_STEP);
                        sector->ceiling_corners[1][2] += overall_adjustment - (float)(fd_slope.slope_t10 * TR_METERING_STEP);
                        sector->ceiling_corners[2][2] += overall_adjustment - (float)(fd_slope.slope_t13 * TR_METERING_STEP);
                        sector->ceiling_corners[3][2] += overall_adjustment - (float)(fd_slope.slope_t12 * TR_METERING_STEP);

                        if(fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NE_PORTAL_NW)
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_A;
                        }
                        else if(fd_command.function == TR_FD_FUNC_CEILINGTRIANGLE_NE_PORTAL_SE)
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_DOOR_VERTICAL_B;
                        }
                        else
                        {
                            sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_SOLID;
                        }
                    }
                }
                else
                {
                    // Unknown floordata function!
                }
                break;
        };
        ret++;
    }
    while(!fd_command.end_bit && (current_offset < max_offset));

    if(sector->floor == TR_METERING_WALLHEIGHT)
    {
        sector->floor_penetration_config = TR_PENETRATION_CONFIG_WALL;
    }
    if(sector->ceiling == TR_METERING_WALLHEIGHT)
    {
        sector->ceiling_penetration_config = TR_PENETRATION_CONFIG_WALL;
    }

    return ret;
}


void GenerateAnimCommandsTransform(skeletal_model_p model, int16_t *base_anim_commands_array)
{
    if(!base_anim_commands_array)
    {
        return;
    }

    //Sys_DebugLog("anim_transform.txt", "MODEL[%d]", model->id);
    for(uint16_t anim = 0; anim < model->animation_count; anim++)
    {
        if(model->animations[anim].num_anim_commands > 255)
        {
            continue;                                                           // If no anim commands or current anim has more than 255 (according to TRosettaStone).
        }

        animation_frame_p af  = model->animations + anim;
        int16_t *pointer      = base_anim_commands_array + af->anim_command;

        for(uint32_t i = 0; i < af->num_anim_commands; i++, pointer++)
        {
            switch(*pointer)
            {
                case TR_ANIMCOMMAND_SETPOSITION:
                    // This command executes ONLY at the end of animation.
                    af->frames[af->frames_count-1].move[0] = (float)(*++pointer);                          // x = x;
                    af->frames[af->frames_count-1].move[2] =-(float)(*++pointer);                          // z =-y
                    af->frames[af->frames_count-1].move[1] = (float)(*++pointer);                          // y = z
                    af->frames[af->frames_count-1].command |= ANIM_CMD_MOVE;
                    //Sys_DebugLog("anim_transform.txt", "move[anim = %d, frame = %d, frames = %d]", anim, af->frames_count-1, af->frames_count);
                    break;

                case TR_ANIMCOMMAND_JUMPDISTANCE:
                    af->frames[af->frames_count-1].v_Vertical   = *++pointer;
                    af->frames[af->frames_count-1].v_Horizontal = *++pointer;
                    af->frames[af->frames_count-1].command |= ANIM_CMD_JUMP;
                    break;

                case TR_ANIMCOMMAND_EMPTYHANDS:
                    break;

                case TR_ANIMCOMMAND_KILL:
                    break;

                case TR_ANIMCOMMAND_PLAYSOUND:
                    ++pointer;
                    ++pointer;
                    break;

                case TR_ANIMCOMMAND_PLAYEFFECT:
                    {
                        int frame = *++pointer;
                        switch(*++pointer & 0x3FFF)
                        {
                            case TR_EFFECT_CHANGEDIRECTION:
                                af->frames[frame].command |= ANIM_CMD_CHANGE_DIRECTION;
                                //Con_Printf("ROTATE: anim = %d, frame = %d of %d", anim, frame, af->frames_count);
                                //Sys_DebugLog("anim_transform.txt", "dir[anim = %d, frame = %d, frames = %d]", anim, frame, af->frames_count);
                                break;
                        }
                    }
                    break;
            }
        }
    }
}

/**   Assign animated texture to a polygon.
  *   While in original TRs we had TexInfo abstraction layer to refer texture,
  *   in OpenTomb we need to re-think animated texture concept to work on a
  *   per-polygon basis. For this, we scan all animated texture lists for
  *   same TexInfo index that is applied to polygon, and if corresponding
  *   animation list is found, we assign it to polygon.
  */
bool Res_SetAnimTexture(struct polygon_s *polygon, uint32_t tex_index, struct anim_seq_s *anim_sequences, uint32_t anim_sequences_count)
{
    polygon->anim_id = 0;                           // Reset to 0 by default.

    for(uint32_t i = 0; i < anim_sequences_count; i++)
    {
        for(uint16_t j = 0; j < anim_sequences[i].frames_count; j++)
        {
            if(anim_sequences[i].frame_list[j] == tex_index)
            {
                // If we have found assigned texture ID in animation texture lists,
                // we assign corresponding animation sequence to this polygon,
                // additionally specifying frame offset.
                polygon->anim_id       = i + 1;  // Animation sequence ID.
                polygon->frame_offset  = j;      // Animation frame offset.
                return true;
            }
        }
    }

    return false;   // No such TexInfo found in animation textures lists.
}


static void TR_CopyNormals(const polygon_p polygon, base_mesh_p mesh, const uint16_t *mesh_vertex_indices)
{
    for(int i = 0; i < polygon->vertex_count; i++)
    {
        vec3_copy(polygon->vertices[i].normal, mesh->vertices[mesh_vertex_indices[i]].normal);
    }
}


void TR_AccumulateNormals(tr4_mesh_t *tr_mesh, base_mesh_p mesh, int numCorners, const uint16_t *vertex_indices, polygon_p p)
{
    Polygon_Resize(p, numCorners);

    for(int i = 0; i < numCorners; i++)
    {
        TR_vertex_to_arr(p->vertices[i].position, &tr_mesh->vertices[vertex_indices[i]]);
    }
    Polygon_FindNormale(p);

    for(int i = 0; i < numCorners; i++)
    {
        vec3_add(mesh->vertices[vertex_indices[i]].normal, mesh->vertices[vertex_indices[i]].normal, p->plane);
    }
}

void TR_SetupColoredFace(tr4_mesh_t *tr_mesh, VT_Level *tr, const uint16_t *vertex_indices, unsigned color, polygon_p p)
{
    for(int i = 0; i < p->vertex_count; i++)
    {
        p->vertices[i].color[0] = tr->palette.colour[color].r / 255.0f;
        p->vertices[i].color[1] = tr->palette.colour[color].g / 255.0f;
        p->vertices[i].color[2] = tr->palette.colour[color].b / 255.0f;
        if(tr_mesh->num_lights == tr_mesh->num_vertices)
        {
            p->vertices[i].color[0] = p->vertices[i].color[0] * 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
            p->vertices[i].color[1] = p->vertices[i].color[1] * 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
            p->vertices[i].color[2] = p->vertices[i].color[2] * 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
        }
        p->vertices[i].color[3] = 1.0f;
    }
}

void TR_SetupTexturedFace(tr4_mesh_t *tr_mesh, const uint16_t *vertex_indices, polygon_p p)
{
    for(int i = 0; i < p->vertex_count; i++)
    {
        if(tr_mesh->num_lights == tr_mesh->num_vertices)
        {
            p->vertices[i].color[0] = 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
            p->vertices[i].color[1] = 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
            p->vertices[i].color[2] = 1.0f - (tr_mesh->lights[vertex_indices[i]] / (8192.0f));
            p->vertices[i].color[3] = 1.0f;
        }
        else
        {
            vec4_set_one(p->vertices[i].color);
        }
    }
}

void TR_GenMesh(struct base_mesh_s *mesh, size_t mesh_index, struct anim_seq_s *anim_sequences, uint32_t anim_sequences_count, class bordered_texture_atlas *atlas, class VT_Level *tr)
{
    uint16_t col;
    tr4_mesh_t *tr_mesh;
    tr4_face4_t *face4;
    tr4_face3_t *face3;
    tr4_object_texture_t *tex;
    polygon_p p;
    float n;
    vertex_p vertex;
    const uint32_t tex_mask = (tr->game_version == TR_IV)?(TR_TEXTURE_INDEX_MASK_TR4):(TR_TEXTURE_INDEX_MASK);

    /* TR WAD FORMAT DOCUMENTATION!
     * tr4_face[3,4]_t:
     * flipped texture & 0x8000 (1 bit  ) - horizontal flipping.
     * shape texture   & 0x7000 (3 bits ) - texture sample shape.
     * index texture   & $0FFF  (12 bits) - texture sample index.
     *
     * if bit [15] is set, as in ( texture and $8000 ), it indicates that the texture
     * sample must be flipped horizontally prior to be used.
     * Bits [14..12] as in ( texture and $7000 ), are used to store the texture
     * shape, given by: ( texture and $7000 ) shr 12.
     * The valid values are: 0, 2, 4, 6, 7, as assigned to a square starting from
     * the top-left corner and going clockwise: 0, 2, 4, 6 represent the positions
     * of the square angle of the triangles, 7 represents a quad.
     */

    tr_mesh = &tr->meshes[mesh_index];
    mesh->id = mesh_index;
    mesh->centre[0] = tr_mesh->centre.x;
    mesh->centre[1] =-tr_mesh->centre.z;
    mesh->centre[2] = tr_mesh->centre.y;
    mesh->R = tr_mesh->collision_size;

    mesh->vertex_count = tr_mesh->num_vertices;
    vertex = mesh->vertices = (vertex_p)calloc(mesh->vertex_count, sizeof(vertex_t));
    for(uint32_t i = 0; i < mesh->vertex_count; i++, vertex++)
    {
        TR_vertex_to_arr(vertex->position, &tr_mesh->vertices[i]);
        vec3_set_zero(vertex->normal);                                          // paranoid
    }

    BaseMesh_FindBB(mesh);

    mesh->polygons_count = tr_mesh->num_textured_triangles + tr_mesh->num_coloured_triangles + tr_mesh->num_textured_rectangles + tr_mesh->num_coloured_rectangles;
    p = mesh->polygons = Polygon_CreateArray(mesh->polygons_count);

    /*
     * textured triangles
     */
    for(int16_t i = 0; i < tr_mesh->num_textured_triangles; i++, p++)
    {
        face3 = &tr_mesh->textured_triangles[i];
        tex = &tr->object_textures[face3->texture & tex_mask];

        p->double_side = (bool)(face3->texture >> 15);    // CORRECT, BUT WRONG IN TR3-5

        Res_SetAnimTexture(p, face3->texture & tex_mask, anim_sequences, anim_sequences_count);

        if(face3->lighting & 0x01)
        {
            p->transparency = BM_MULTIPLY;
        }
        else
        {
            p->transparency = tex->transparency_flags;
        }

        TR_AccumulateNormals(tr_mesh, mesh, 3, face3->vertices, p);
        TR_SetupTexturedFace(tr_mesh, face3->vertices, p);

        atlas->getCoordinates(p, face3->texture & tex_mask, 0);
    }

    /*
     * coloured triangles
     */
    for(int16_t i = 0; i < tr_mesh->num_coloured_triangles; i++, p++)
    {
        face3 = &tr_mesh->coloured_triangles[i];
        col = face3->texture & 0xff;
        p->texture_index = 0;
        p->transparency = 0;
        p->anim_id = 0;

        TR_AccumulateNormals(tr_mesh, mesh, 3, face3->vertices, p);
        atlas->getWhiteTextureCoordinates(p);
        TR_SetupColoredFace(tr_mesh, tr, face3->vertices, col, p);
    }

    /*
     * textured rectangles
     */
    for(int16_t i = 0; i < tr_mesh->num_textured_rectangles; i++, p++)
    {
        face4 = &tr_mesh->textured_rectangles[i];
        tex = &tr->object_textures[face4->texture & tex_mask];

        p->double_side = (bool)(face4->texture >> 15);    // CORRECT, BUT WRONG IN TR3-5

        Res_SetAnimTexture(p, face4->texture & tex_mask, anim_sequences, anim_sequences_count);

        if(face4->lighting & 0x01)
        {
            p->transparency = BM_MULTIPLY;
        }
        else
        {
            p->transparency = tex->transparency_flags;
        }

        TR_AccumulateNormals(tr_mesh, mesh, 4, face4->vertices, p);
        TR_SetupTexturedFace(tr_mesh, face4->vertices, p);

        atlas->getCoordinates(p, face4->texture & tex_mask, 0);
    }

    /*
     * coloured rectangles
     */
    for(int16_t i = 0; i < tr_mesh->num_coloured_rectangles; i++, p++)
    {
        face4 = &tr_mesh->coloured_rectangles[i];
        col = face4->texture & 0xff;
        Polygon_Resize(p, 4);
        p->texture_index = 0;
        p->transparency = 0;
        p->anim_id = 0;

        TR_AccumulateNormals(tr_mesh, mesh, 4, face4->vertices, p);
        atlas->getWhiteTextureCoordinates(p);
        TR_SetupColoredFace(tr_mesh, tr, face4->vertices, col, p);
    }

    /*
     * let us normalise normales %)
     */
    p = mesh->polygons;
    vertex_p v = mesh->vertices;
    for(uint32_t i = 0; i < mesh->vertex_count; i++, v++)
    {
        vec3_norm(v->normal, n);
    }

    /*
     * triangles
     */
    for(int16_t i = 0; i < tr_mesh->num_textured_triangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_mesh->textured_triangles[i].vertices);
    }

    for(int16_t i = 0; i < tr_mesh->num_coloured_triangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_mesh->coloured_triangles[i].vertices);
    }

    /*
     * rectangles
     */
    for(int16_t i = 0; i < tr_mesh->num_textured_rectangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_mesh->textured_rectangles[i].vertices);
    }

    for(int16_t i = 0; i < tr_mesh->num_coloured_rectangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_mesh->coloured_rectangles[i].vertices);
    }

    if(mesh->vertex_count > 0)
    {
        mesh->vertex_count = 0;
        free(mesh->vertices);
        mesh->vertices = NULL;
    }

    p = mesh->polygons;
    for(uint32_t i = 0; i < mesh->polygons_count; i++, p++)
    {
        if((p->anim_id > 0) && (p->anim_id <= anim_sequences_count))
        {
            anim_seq_p seq = anim_sequences + (p->anim_id - 1);
            // set tex coordinates to the first frame for correct texture transform in renderer
            atlas->getCoordinates(p, seq->frame_list[0], false, 0, seq->uvrotate);
        }
    }
}

void TR_SetupRoomPolygonVertices(polygon_p p, base_mesh_p mesh, const tr5_room_t *tr_room, const uint16_t *vertices)
{
    for (int i = 0; i < p->vertex_count; i++)
    {
        TR_vertex_to_arr(p->vertices[i].position, &tr_room->vertices[vertices[i]].vertex);
    }
    Polygon_FindNormale(p);

    for (int i = 0; i < p->vertex_count; i++)
    {
        vec3_add(mesh->vertices[vertices[i]].normal, mesh->vertices[vertices[i]].normal, p->plane);
        vec3_copy(p->vertices[i].normal, p->plane);
        TR_color_to_arr(p->vertices[i].color, &tr_room->vertices[vertices[i]].colour);
    }
}

void TR_GenRoomMesh(struct room_s *room, size_t room_index, struct anim_seq_s *anim_sequences, uint32_t anim_sequences_count, class bordered_texture_atlas *atlas, class VT_Level *tr)
{
    tr5_room_t *tr_room;
    polygon_p p;
    base_mesh_p mesh;
    float n;
    vertex_p vertex;
    uint32_t tex_mask = (tr->game_version == TR_IV)?(TR_TEXTURE_INDEX_MASK_TR4):(TR_TEXTURE_INDEX_MASK);

    tr_room = &tr->rooms[room_index];

    if(tr_room->num_triangles + tr_room->num_rectangles == 0)
    {
        room->content->mesh = NULL;
        return;
    }

    mesh = room->content->mesh = (base_mesh_p)calloc(1, sizeof(base_mesh_t));
    mesh->id = room_index;

    mesh->vertex_count = tr_room->num_vertices;
    vertex = mesh->vertices = (vertex_p)calloc(mesh->vertex_count, sizeof(vertex_t));
    for(uint32_t i = 0; i < mesh->vertex_count; i++, vertex++)
    {
        TR_vertex_to_arr(vertex->position, &tr_room->vertices[i].vertex);
        vec3_set_zero(vertex->normal);                                          // paranoid
    }

    BaseMesh_FindBB(mesh);

    mesh->polygons_count = tr_room->num_triangles + tr_room->num_rectangles;
    p = mesh->polygons = Polygon_CreateArray(mesh->polygons_count);

    /*
     * triangles
     */
    for(uint32_t i = 0; i < tr_room->num_triangles; i++, p++)
    {
        uint32_t masked_texture = tr_room->triangles[i].texture & tex_mask;
        Polygon_Resize(p, 3);
        TR_SetupRoomPolygonVertices(p, mesh, tr_room, tr_room->triangles[i].vertices);
        p->double_side = tr_room->triangles[i].texture & 0x8000;
        p->transparency = tr->object_textures[masked_texture].transparency_flags;
        Res_SetAnimTexture(p, masked_texture, anim_sequences, anim_sequences_count);
        atlas->getCoordinates(p, masked_texture, 0);
    }

    /*
     * rectangles
     */
    for(uint32_t i = 0; i < tr_room->num_rectangles; i++, p++)
    {
        uint32_t masked_texture = tr_room->rectangles[i].texture & tex_mask;
        Polygon_Resize(p, 4);
        TR_SetupRoomPolygonVertices(p, mesh, tr_room, tr_room->rectangles[i].vertices);
        p->double_side = tr_room->rectangles[i].texture & 0x8000;
        p->transparency = tr->object_textures[masked_texture].transparency_flags;
        Res_SetAnimTexture(p, masked_texture, anim_sequences, anim_sequences_count);
        atlas->getCoordinates(p, masked_texture, 0);
    }

    /*
     * let us normalise normales %)
     */
    vertex_p v = mesh->vertices;
    for(uint32_t i = 0; i < mesh->vertex_count; i++, v++)
    {
        vec3_norm(v->normal, n);
    }

    /*
     * triangles
     */
    p = mesh->polygons;
    for(uint32_t i = 0; i < tr_room->num_triangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_room->triangles[i].vertices);
    }

    /*
     * rectangles
     */
    for(uint32_t i = 0; i < tr_room->num_rectangles; i++, p++)
    {
        TR_CopyNormals(p, mesh, tr_room->rectangles[i].vertices);
    }

    if(mesh->vertex_count > 0)
    {
        mesh->vertex_count = 0;
        free(mesh->vertices);
        mesh->vertices = NULL;
    }

    p = mesh->polygons;
    for(uint32_t i = 0; i < mesh->polygons_count; i++, p++)
    {
        if((p->anim_id > 0) && (p->anim_id <= anim_sequences_count))
        {
            anim_seq_p seq = anim_sequences + (p->anim_id - 1);
            // set tex coordinates to the first frame for correct texture transform in renderer
            atlas->getCoordinates(p, seq->frame_list[0], false, 0, seq->uvrotate);
        }
    }
}


void TR_GenSkeletalModel(struct skeletal_model_s *model, size_t model_id, struct base_mesh_s *base_mesh_array, int16_t *base_anim_commands_array, class VT_Level *tr)
{
    tr_moveable_t *tr_moveable;
    tr_animation_t *tr_animation;

    uint32_t frame_offset, frame_step;
    uint16_t temp1, temp2;
    float ang;
    float rot[3];

    bone_tag_p bone_tag;
    bone_frame_p bone_frame;
    mesh_tree_tag_p tree_tag;
    animation_frame_p anim;

    tr_moveable = &tr->moveables[model_id];                                    // original tr structure
    model->collision_map = (uint16_t*)malloc(model->mesh_count * sizeof(uint16_t));
    model->mesh_tree = (mesh_tree_tag_p)calloc(model->mesh_count, sizeof(mesh_tree_tag_t));
    tree_tag = model->mesh_tree;

    uint32_t *mesh_index = tr->mesh_indices + tr_moveable->starting_mesh;

    for(uint16_t k = 0; k < model->mesh_count; k++, tree_tag++)
    {
        model->collision_map[k] = k;
        tree_tag->mesh_base = base_mesh_array + (mesh_index[k]);
        tree_tag->mesh_skin = NULL;
        tree_tag->replace_anim = 0x00;
        tree_tag->replace_mesh = 0x00;
        tree_tag->body_part    = 0x00;
        tree_tag->offset[0] = 0.0;
        tree_tag->offset[1] = 0.0;
        tree_tag->offset[2] = 0.0;
        tree_tag->parent = 0;
        if(k == 0)
        {
            tree_tag->flag = 0x02;
        }
        else
        {
            uint32_t *tr_mesh_tree = tr->mesh_tree_data + tr_moveable->mesh_tree_index + (k-1)*4;
            tree_tag->flag = (tr_mesh_tree[0] & 0xFF);
            tree_tag->offset[0] = (float)((int32_t)tr_mesh_tree[1]);
            tree_tag->offset[1] = (float)((int32_t)tr_mesh_tree[3]);
            tree_tag->offset[2] =-(float)((int32_t)tr_mesh_tree[2]);
        }
    }

    SkeletalModel_GenParentsIndexes(model);

    for(uint16_t i = 1; i + 1 < model->mesh_count; i++)
    {
        for(uint16_t j = 1; j + i < model->mesh_count; j++)
        {
            uint16_t m1 = model->collision_map[j];
            uint16_t m2 = model->collision_map[j + 1];
            if(model->mesh_tree[m1].parent > model->mesh_tree[m2].parent)
            {
                uint16_t t = model->collision_map[j];
                model->collision_map[j] = model->collision_map[j + 1];
                model->collision_map[j + 1] = t;
            }
        }
    }

    /*
     * =================    now, animation loading    ========================
     */

    if(tr_moveable->animation_index >= tr->animations_count)
    {
        /*
         * model has no start offset and any animation
         */
        model->animation_count = 1;
        model->animations = (animation_frame_p)malloc(sizeof(animation_frame_t));
        model->animations->frames_count = 1;
        model->animations->frames = (bone_frame_p)calloc(model->animations->frames_count , sizeof(bone_frame_t));
        bone_frame = model->animations->frames;

        model->animations->id = 0;
        model->animations->next_anim = NULL;
        model->animations->next_frame = 0;
        model->animations->state_change = NULL;
        model->animations->state_change_count = 0;
        model->animations->original_frame_rate = 1;

        bone_frame->bone_tag_count = model->mesh_count;
        bone_frame->bone_tags = (bone_tag_p)malloc(bone_frame->bone_tag_count * sizeof(bone_tag_t));

        vec3_set_zero(bone_frame->pos);
        vec3_set_zero(bone_frame->move);
        bone_frame->v_Horizontal = 0.0;
        bone_frame->v_Vertical = 0.0;
        bone_frame->command = 0x00;
        for(uint16_t k = 0; k < bone_frame->bone_tag_count; k++)
        {
            tree_tag = model->mesh_tree + k;
            bone_tag = bone_frame->bone_tags + k;

            rot[0] = 0.0;
            rot[1] = 0.0;
            rot[2] = 0.0;
            vec4_SetZXYRotations(bone_tag->qrotate, rot);
            vec3_copy(bone_tag->offset, tree_tag->offset);
        }
        return;
    }
    //Sys_DebugLog(LOG_FILENAME, "model = %d, anims = %d", tr_moveable->object_id, GetNumAnimationsForMoveable(tr, model_num));
    model->animation_count = TR_GetNumAnimationsForMoveable(tr, model_id);
    if(model->animation_count <= 0)
    {
        /*
         * the animation count must be >= 1
         */
        model->animation_count = 1;
    }

    /*
     *   Ok, let us calculate animations;
     *   there is no difficult:
     * - first 9 words are bounding box and frame offset coordinates.
     * - 10's word is a rotations count, must be equal to number of meshes in model.
     *   BUT! only in TR1. In TR2 - TR5 after first 9 words begins next section.
     * - in the next follows rotation's data. one word - one rotation, if rotation is one-axis (one angle).
     *   two words in 3-axis rotations (3 angles). angles are calculated with bit mask.
     */
    model->animations = (animation_frame_p)calloc(model->animation_count, sizeof(animation_frame_t));
    anim = model->animations;
    for(uint16_t i = 0; i < model->animation_count; i++, anim++)
    {
        tr_animation = &tr->animations[tr_moveable->animation_index+i];
        frame_offset = tr_animation->frame_offset / 2;
        uint16_t l_start = 0x09;
        if(tr->game_version == TR_I || tr->game_version == TR_I_DEMO || tr->game_version == TR_I_UB)
        {
            l_start = 0x0A;
        }
        frame_step = tr_animation->frame_size;

        anim->id = i;
        anim->original_frame_rate = tr_animation->frame_rate;

        anim->speed_x = tr_animation->speed;
        anim->accel_x = tr_animation->accel;
        anim->speed_y = tr_animation->accel_lateral;
        anim->accel_y = tr_animation->speed_lateral;

        anim->anim_command = tr_animation->anim_command;
        anim->num_anim_commands = tr_animation->num_anim_commands;
        anim->state_id = tr_animation->state_id;

        anim->frames_count = TR_GetNumFramesForAnimation(tr, tr_moveable->animation_index+i);

        //Sys_DebugLog(LOG_FILENAME, "Anim[%d], %d", tr_moveable->animation_index, TR_GetNumFramesForAnimation(tr, tr_moveable->animation_index));

        // Parse AnimCommands
        // Max. amount of AnimCommands is 255, larger numbers are considered as 0.
        // See http://evpopov.com/dl/TR4format.html#Animations for details.

        if( (anim->num_anim_commands > 0) && (anim->num_anim_commands <= 255) )
        {
            // Calculate current animation anim command block offset.
            int16_t *pointer = base_anim_commands_array + anim->anim_command;

            for(uint32_t count = 0; count < anim->num_anim_commands; count++, pointer++)
            {
                switch(*pointer)
                {
                    case TR_ANIMCOMMAND_PLAYEFFECT:
                    case TR_ANIMCOMMAND_PLAYSOUND:
                        // Recalculate absolute frame number to relative.
                        ///@FIXED: was unpredictable behavior.
                        *(pointer + 1) -= tr_animation->frame_start;
                        pointer += 2;
                        break;

                    case TR_ANIMCOMMAND_SETPOSITION:
                        // Parse through 3 operands.
                        pointer += 3;
                        break;

                    case TR_ANIMCOMMAND_JUMPDISTANCE:
                        // Parse through 2 operands.
                        pointer += 2;
                        break;

                    default:
                        // All other commands have no operands.
                        break;
                }
            }
        }


        if(anim->frames_count <= 0)
        {
            /*
             * number of animations must be >= 1, because frame contains base model offset
             */
            anim->frames_count = 1;
        }
        anim->frames = (bone_frame_p)calloc(anim->frames_count, sizeof(bone_frame_t));

        /*
         * let us begin to load animations
         */
        bone_frame = anim->frames;
        for(uint16_t j = 0; j < anim->frames_count; j++, bone_frame++, frame_offset += frame_step)
        {
            bone_frame->bone_tag_count = model->mesh_count;
            bone_frame->bone_tags = (bone_tag_p)malloc(model->mesh_count * sizeof(bone_tag_t));
            vec3_set_zero(bone_frame->pos);
            vec3_set_zero(bone_frame->move);
            TR_GetBFrameBB_Pos(tr, frame_offset, bone_frame);

            if(frame_offset >= tr->frame_data_size)
            {
                //Con_Printf("Bad frame offset");
                for(uint16_t k = 0;k < bone_frame->bone_tag_count; k++)
                {
                    tree_tag = model->mesh_tree + k;
                    bone_tag = bone_frame->bone_tags + k;
                    rot[0] = 0.0;
                    rot[1] = 0.0;
                    rot[2] = 0.0;
                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                    vec3_copy(bone_tag->offset, tree_tag->offset);
                }
            }
            else
            {
                uint16_t l = l_start;
                for(uint16_t k = 0;k < bone_frame->bone_tag_count; k++)
                {
                    tree_tag = model->mesh_tree + k;
                    bone_tag = bone_frame->bone_tags + k;
                    rot[0] = 0.0;
                    rot[1] = 0.0;
                    rot[2] = 0.0;
                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                    vec3_copy(bone_tag->offset, tree_tag->offset);

                    switch(tr->game_version)
                    {
                        case TR_I:                                              /* TR_I */
                        case TR_I_UB:
                        case TR_I_DEMO:
                            temp2 = tr->frame_data[frame_offset + l];
                            l ++;
                            temp1 = tr->frame_data[frame_offset + l];
                            l ++;
                            rot[0] = (float)((temp1 & 0x3ff0) >> 4);
                            rot[2] =-(float)(((temp1 & 0x000f) << 6) | ((temp2 & 0xfc00) >> 10));
                            rot[1] = (float)(temp2 & 0x03ff);
                            rot[0] *= 360.0 / 1024.0;
                            rot[1] *= 360.0 / 1024.0;
                            rot[2] *= 360.0 / 1024.0;
                            vec4_SetZXYRotations(bone_tag->qrotate, rot);
                            break;

                        default:                                                /* TR_II + */
                            temp1 = tr->frame_data[frame_offset + l];
                            l ++;
                            if(tr->game_version >= TR_IV)
                            {
                                ang = (float)(temp1 & 0x0fff);
                                ang *= 360.0 / 4096.0;
                            }
                            else
                            {
                                ang = (float)(temp1 & 0x03ff);
                                ang *= 360.0 / 1024.0;
                            }

                            switch (temp1 & 0xc000)
                            {
                                case 0x4000:    // x only
                                    rot[0] = ang;
                                    rot[1] = 0;
                                    rot[2] = 0;
                                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                                    break;

                                case 0x8000:    // y only
                                    rot[0] = 0;
                                    rot[1] = 0;
                                    rot[2] =-ang;
                                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                                    break;

                                case 0xc000:    // z only
                                    rot[0] = 0;
                                    rot[1] = ang;
                                    rot[2] = 0;
                                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                                    break;

                                default:        // all three
                                    temp2 = tr->frame_data[frame_offset + l];
                                    rot[0] = (float)((temp1 & 0x3ff0) >> 4);
                                    rot[2] =-(float)(((temp1 & 0x000f) << 6) | ((temp2 & 0xfc00) >> 10));
                                    rot[1] = (float)(temp2 & 0x03ff);
                                    rot[0] *= 360.0 / 1024.0;
                                    rot[1] *= 360.0 / 1024.0;
                                    rot[2] *= 360.0 / 1024.0;
                                    vec4_SetZXYRotations(bone_tag->qrotate, rot);
                                    l ++;
                                    break;
                            };
                            break;
                    };
                }
            }
        }
    }

    /*
     * Animations interpolation to 1/30 sec like in original. Needed for correct state change works.
     */
    SkeletalModel_InterpolateFrames(model);
    /*
     * state change's loading
     */

#if LOG_ANIM_DISPATCHES
    if(model->animation_count > 1)
    {
        Sys_DebugLog(LOG_FILENAME, "MODEL[%d], anims = %d", model_num, model->animation_count);
    }
#endif
    anim = model->animations;
    for(uint16_t i = 0; i < model->animation_count; i++, anim++)
    {
        anim->state_change_count = 0;
        anim->state_change = NULL;

        tr_animation = &tr->animations[tr_moveable->animation_index+i];
        int16_t j = tr_animation->next_animation - tr_moveable->animation_index;
        j &= 0x7fff;
        if((j >= 0) && (j < model->animation_count))
        {
            anim->next_anim = model->animations + j;
            anim->next_frame = tr_animation->next_frame - tr->animations[tr_animation->next_animation].frame_start;
            anim->next_frame %= anim->next_anim->frames_count;
            if(anim->next_frame < 0)
            {
                anim->next_frame = 0;
            }
#if LOG_ANIM_DISPATCHES
            Sys_DebugLog(LOG_FILENAME, "ANIM[%d], next_anim = %d, next_frame = %d", i, anim->next_anim->id, anim->next_frame);
#endif
        }
        else
        {
            anim->next_anim = NULL;
            anim->next_frame = 0;
        }

        anim->state_change_count = 0;
        anim->state_change = NULL;

        if((tr_animation->num_state_changes > 0) && (model->animation_count > 1))
        {
            state_change_p sch_p;
#if LOG_ANIM_DISPATCHES
            Sys_DebugLog(LOG_FILENAME, "ANIM[%d], next_anim = %d, next_frame = %d", i, (anim->next_anim)?(anim->next_anim->id):(-1), anim->next_frame);
#endif
            anim->state_change_count = tr_animation->num_state_changes;
            sch_p = anim->state_change = (state_change_p)malloc(tr_animation->num_state_changes * sizeof(state_change_t));

            for(uint16_t j = 0;j < tr_animation->num_state_changes; j++, sch_p++)
            {
                tr_state_change_t *tr_sch;
                tr_sch = &tr->state_changes[j+tr_animation->state_change_offset];
                sch_p->id = tr_sch->state_id;
                sch_p->anim_dispatch = NULL;
                sch_p->anim_dispatch_count = 0;
                for(uint16_t l = 0; l < tr_sch->num_anim_dispatches; l++)
                {
                    tr_anim_dispatch_t *tr_adisp = &tr->anim_dispatches[tr_sch->anim_dispatch+l];
                    uint16_t next_anim = tr_adisp->next_animation & 0x7fff;
                    uint16_t next_anim_ind = next_anim - (tr_moveable->animation_index & 0x7fff);
                    if(next_anim_ind < model->animation_count)
                    {
                        sch_p->anim_dispatch_count++;
                        sch_p->anim_dispatch = (anim_dispatch_p)realloc(sch_p->anim_dispatch, sch_p->anim_dispatch_count * sizeof(anim_dispatch_t));

                        anim_dispatch_p adsp = sch_p->anim_dispatch + sch_p->anim_dispatch_count - 1;
                        uint16_t next_frames_count = model->animations[next_anim - tr_moveable->animation_index].frames_count;
                        uint16_t next_frame = tr_adisp->next_frame - tr->animations[next_anim].frame_start;

                        uint16_t low  = tr_adisp->low  - tr_animation->frame_start;
                        uint16_t high = tr_adisp->high - tr_animation->frame_start;

                        adsp->frame_low  = low  % anim->frames_count;
                        adsp->frame_high = (high - 1) % anim->frames_count;
                        adsp->next_anim = next_anim - tr_moveable->animation_index;
                        adsp->next_frame = next_frame % next_frames_count;

#if LOG_ANIM_DISPATCHES
                        Sys_DebugLog(LOG_FILENAME, "anim_disp[%d], frames_count = %d: interval[%d.. %d], next_anim = %d, next_frame = %d", l,
                                    anim->frames_count, adsp->frame_low, adsp->frame_high,
                                    adsp->next_anim, adsp->next_frame);
#endif
                    }
                }
            }
        }
    }

    GenerateAnimCommandsTransform(model, base_anim_commands_array);
}


void TR_GetBFrameBB_Pos(class VT_Level *tr, size_t frame_offset, struct bone_frame_s *bone_frame)
{
    unsigned short int *frame;

    if(frame_offset < tr->frame_data_size)
    {
        frame = tr->frame_data + frame_offset;
        bone_frame->bb_min[0] = (short int)frame[0];                            // x_min
        bone_frame->bb_min[1] = (short int)frame[4];                            // y_min
        bone_frame->bb_min[2] =-(short int)frame[3];                            // z_min

        bone_frame->bb_max[0] = (short int)frame[1];                            // x_max
        bone_frame->bb_max[1] = (short int)frame[5];                            // y_max
        bone_frame->bb_max[2] =-(short int)frame[2];                            // z_max

        bone_frame->pos[0] = (short int)frame[6];
        bone_frame->pos[1] = (short int)frame[8];
        bone_frame->pos[2] =-(short int)frame[7];
    }
    else
    {
        bone_frame->bb_min[0] = 0.0;
        bone_frame->bb_min[1] = 0.0;
        bone_frame->bb_min[2] = 0.0;

        bone_frame->bb_max[0] = 0.0;
        bone_frame->bb_max[1] = 0.0;
        bone_frame->bb_max[2] = 0.0;

        bone_frame->pos[0] = 0.0;
        bone_frame->pos[1] = 0.0;
        bone_frame->pos[2] = 0.0;
    }

    bone_frame->centre[0] = (bone_frame->bb_min[0] + bone_frame->bb_max[0]) / 2.0;
    bone_frame->centre[1] = (bone_frame->bb_min[1] + bone_frame->bb_max[1]) / 2.0;
    bone_frame->centre[2] = (bone_frame->bb_min[2] + bone_frame->bb_max[2]) / 2.0;
}


int TR_GetNumAnimationsForMoveable(class VT_Level *tr, size_t moveable_ind)
{
    int ret;
    tr_moveable_t *curr_moveable, *next_moveable;

    curr_moveable = &tr->moveables[moveable_ind];

    if(curr_moveable->animation_index == 0xFFFF)
    {
        return 0;
    }

    if(moveable_ind == tr->moveables_count-1)
    {
        ret = (int32_t)tr->animations_count - (int32_t)curr_moveable->animation_index;
        if(ret < 0)
        {
            return 1;
        }
        else
        {
            return ret;
        }
    }

    next_moveable = &tr->moveables[moveable_ind+1];
    if(next_moveable->animation_index == 0xFFFF)
    {
        if(moveable_ind + 2 < tr->moveables_count)                              // I hope there is no two neighboard movables with animation_index'es == 0xFFFF
        {
            next_moveable = &tr->moveables[moveable_ind+2];
        }
        else
        {
            return 1;
        }
    }

    ret = (next_moveable->animation_index <= tr->animations_count)?(next_moveable->animation_index):(tr->animations_count);
    ret -= (int32_t)curr_moveable->animation_index;

    return ret;
}


int TR_GetNumFramesForAnimation(class VT_Level *tr, size_t animation_ind)
{
    tr_animation_t *curr_anim, *next_anim;
    int ret;

    curr_anim = &tr->animations[animation_ind];
    if(curr_anim->frame_size <= 0)
    {
        return 1;                                                               // impossible!
    }

    if(animation_ind == tr->animations_count - 1)
    {
        ret = 2 * tr->frame_data_size - curr_anim->frame_offset;
        ret /= curr_anim->frame_size * 2;                                       /// it is fully correct!
        return ret;
    }

    next_anim = tr->animations + animation_ind + 1;
    ret = next_anim->frame_offset - curr_anim->frame_offset;
    ret /= curr_anim->frame_size * 2;

    return ret;
}


uint32_t TR_GetOriginalAnimationFrameOffset(uint32_t offset, uint32_t anim, class VT_Level *tr)
{
    tr_animation_t *tr_animation;

    if(anim >= tr->animations_count)
    {
        return -1;
    }

    tr_animation = &tr->animations[anim];
    if(anim + 1 == tr->animations_count)
    {
        if(offset < tr_animation->frame_offset)
        {
            return -2;
        }
    }
    else
    {
        if((offset < tr_animation->frame_offset) && (offset >= (tr_animation+1)->frame_offset))
        {
            return -2;
        }
    }

    return tr_animation->frame_offset;
}
