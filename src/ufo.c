

//7E760
bool test_block_chdir(obj_t* obj, s16* block_index, s16 offset_x, s16 offset_y) {
    s16 tile_x = (s16)((obj->x + offset_x) >> 4);
    s16 tile_y = (s16)((obj->y + offset_y) >> 4);

    *block_index = (s16)(tile_x + tile_y * mp.width);
    if ((s16)obj->test_block_index == *block_index) {
        return false;
    }

    obj->test_block_index = (u16)*block_index;
    if (tile_x < 0 || tile_x >= mp.width || tile_y < 0 || tile_y >= mp.height) {
        return false;
    }

    /* PS1 stores this type in the packed tile word.  The PC map format has
     * the equivalent value in map_tile_t.tile_type. */
    return mp.map[*block_index].tile_type == BTYP_CHDIR;
}

//7E838
void DO_MOVING_WITH_INDICATOR_COMMAND(obj_t* obj) {
    obj->flags.flip_x = false;
    if (obj->main_etat != 0 || obj->sub_etat != 14) {
        return;
    }

    s16 block_index;
    if (!test_block_chdir(obj, &block_index, obj->offset_bx, obj->offset_by)) {
        return;
    }

    for (s16 i = 0; i < actobj.num_active_objects; ++i) {
        obj_t* indicator = &level.objects[actobj.objects[i]];
        if (indicator->type != TYPE_163_INDICATOR) {
            continue;
        }

        s16 indicator_block = (s16)(
            ((indicator->x + indicator->offset_bx) >> 4) +
            ((indicator->y + indicator->offset_by) >> 4) * mp.width
        );
        if (indicator_block != block_index) {
            continue;
        }

        s16 speed_x = obj->speed_x;
        s16 speed_y = obj->speed_y;
        s32 speed = MAX(Abs((s32)speed_x), Abs((s32)speed_y));

        switch (indicator->sub_etat) {
            case 0:
                speed_x = (s16)speed;
                speed_y = 0;
                break;
            case 1:
                speed_x = 0;
                speed_y = (s16)speed;
                break;
            case 2:
                speed_x = (s16)-speed;
                speed_y = 0;
                break;
            case 3:
                speed_x = 0;
                speed_y = (s16)-speed;
                break;
            case 4:
                if (speed_x == 0) {
                    speed_x = (s16)speed;
                    speed_y = 0;
                } else if (speed_y == 0) {
                    speed_x = 0;
                    speed_y = (s16)-speed;
                }
                break;
            case 5:
                if (speed_x == 0) {
                    speed_x = (s16)speed;
                    speed_y = 0;
                } else if (speed_y == 0) {
                    speed_x = 0;
                    speed_y = (s16)speed;
                }
                break;
            case 6:
                if (speed_x == 0) {
                    speed_x = (s16)-speed;
                    speed_y = 0;
                } else if (speed_y == 0) {
                    speed_x = 0;
                    speed_y = (s16)-speed;
                }
                break;
            case 7:
                if (speed_x == 0) {
                    speed_x = (s16)-speed;
                    speed_y = 0;
                } else if (speed_y == 0) {
                    speed_x = 0;
                    speed_y = (s16)speed;
                }
                break;
            case 8:
                if (speed_x == 0) {
                    if (speed_y > 0) {
                        speed_y = (s16)-speed;
                    } else if (speed_y < 0) {
                        speed_y = (s16)speed;
                    }
                } else if (speed_y == 0) {
                    if (speed_x > 0) {
                        speed_x = (s16)-speed;
                    } else {
                        speed_x = (s16)speed;
                    }
                } else {
                    speed_x = (s16)-speed_x;
                    speed_y = (s16)-speed_y;
                }
                break;
            case 9:
            case 10: {
                s32 delta = indicator->sub_etat == 9 ? 1 : -1;
                if (speed_y == 0) {
                    if (speed_x > 0) {
                        speed_x = (s16)MAX(2, MIN(4, speed + delta));
                    } else if (speed_x < 0) {
                        speed_x = (s16)MAX(-4, MIN(-2, delta - speed));
                    }
                }
                if (speed_x != 0) {
                    speed_y = 0;
                }
            } break;
            case 11:
            case 12: {
                s32 delta = indicator->sub_etat == 11 ? 1 : -1;
                if (speed_x == 0) {
                    if (speed_y > 0) {
                        speed_y = (s16)MAX(2, MIN(4, speed + delta));
                    } else if (speed_y < 0) {
                        speed_y = (s16)MAX(-4, MIN(-2, delta - speed));
                    }
                }
                if (speed_y != 0) {
                    speed_x = 0;
                }
            } break;
            default:
                break;
        }

        obj->speed_x = speed_x;
        obj->speed_y = speed_y;
        return;
    }
}

//7EB84
void DO_IDC_COMMAND(obj_t* obj) {
    if (obj->sub_etat <= 3) {
        obj->display_prio = 4;
        obj->flags.flip_x = false;
    } else {
        obj->display_prio = 0;
    }
}

//7EBB8
void DO_LEV_POING_COLLISION(obj_t* obj, s16 sprite) {
    (void)sprite;
    if (obj->main_etat != 5 || obj->sub_etat != 53) {
        return;
    }

    PlaySnd(235, obj->id);
    set_main_and_sub_etat(obj, 5, 54);

    obj_t* indicator = &level.objects[link_init[obj->id]];
    if (indicator->type == TYPE_163_INDICATOR && indicator->sub_etat <= 3) {
        set_sub_etat(indicator, (indicator->sub_etat + 1) & 3);
    }
}

//7EC50
void START_UFO(obj_t* obj) {
    if (obj->main_etat == 0 && obj->sub_etat == 13) {
        set_main_and_sub_etat(obj, 0, 14);
        obj->speed_x = 2;
        obj->speed_y = 0;
    }
}
