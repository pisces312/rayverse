
//3F040
void allocateSTOSKO(void) {
    obj_t* obj = &level.objects[stosko_obj_id];
    obj->flags.alive = true;
    obj->is_active = true;
    obj->x = 160;
    obj->y = ymap - 50;
    obj->y = obj->init_y = firstFloorBelow(obj) - obj->offset_by;

    /* PC starts Stoneskops 16 pixels nearer than PS1, which uses
       xmapmax + SCREEN_WIDTH here. */
    obj->x = xmapmax + SCREEN_WIDTH - 16;
    obj->init_x = xmap + SCREEN_WIDTH - obj->offset_bx - 60;
    obj->timer = 50;
    obj->flags.flip_x = false;
    RayEvts.poing = true;
    poing_obj->init_sub_etat = 8;
}

//3F0F4
void allocateMOSKITOMAMA(void) {
    /* PC biases Rayman's horizontal reference by +8; PS1 omits that bias. */
    s16 ray_half_x = ashr32(ray.x + 8 + ray.offset_bx - xmap, 1);
    YPosBBF2D = NiveauSol - (ray.offset_by - ray.offset_hy) - (ray_half_x - 90);

    obj_t* right = &level.objects[moskitomama_droite_obj_id];
    right->flags.flip_x = false;
    right->x = right->init_x = 160;
    right->y = right->init_y = ymap;
    NiveauSol = firstFloorBelow(right);
    right->speed_x = 0;
    right->speed_y = 0;

    /* PC positions both hybrids eight pixels left of the PS1 coordinates
       (272/32 instead of 280/40), matching its sprite origin. */
    right->x = right->init_x = SCREEN_WIDTH - 48 - right->offset_bx;
    right->y = right->init_y = ymap - 200;
    right->is_active = true;
    right->flags.alive = true;
    BBF2DEsk = 0;

    YPosBBF2G = NiveauSol - (ray.offset_by - ray.offset_hy) + (ray_half_x - 70);
    obj_t* left = &level.objects[moskitomama_gauche_obj_id];
    left->speed_x = 0;
    left->speed_y = 0;
    left->is_active = true;
    left->flags.alive = true;
    left->flags.flip_x = true;
    left->x = left->init_x = 32 - left->offset_bx;
    left->y = left->init_y = ymap - 200;
    BBF2GEsk = 0;

    RayEvts.poing = true;
    poing_obj->init_sub_etat = 8;
}

//3F2A0
void allocateMOSKITOSAXO(void) {
    obj_t* obj = &level.objects[moskitosaxo_obj_id];
    if (!RayEvts.tiny) {
        DO_NOVA(&ray);
        RAY_DEMIRAY();
    }
    RayEvts.run = true;
    obj->flags.alive = true;
    obj->is_active = true;
    obj->x = obj->init_x = xmap + SCREEN_WIDTH / 2 - obj->offset_bx;
    obj->y = obj->init_y = ymap - obj->offset_by;
    RayEvts.poing = true;
    poing_obj->init_sub_etat = 8;
}

//3F340
void doMOSAMScommand(obj_t* obj) {
    scrollLocked = true;

    switch (obj->cmd) {
        case GO_LEFT:
        case GO_RIGHT:
            obj->speed_x = 0;
            obj->speed_y = -8;
            scrollLocked = false;
            break;

        case GO_WAIT:
            if (!(obj->main_etat == 0 && obj->sub_etat == 2)) {
                calc_obj_dir(obj);
            }
            bossXToReach = OBJ_INVALID_XY;
            bossYToReach = OBJ_INVALID_XY;
            obj->speed_x = 0;
            obj->speed_y = 0;
            break;

        case GO_SPEED: {
            calc_obj_dir(obj);
            bossXToReach = ray.x + ray.offset_bx - obj->offset_bx;
            bossYToReach = firstFloorBelow(&ray) - obj->offset_by + 16;
            obj->speed_x = obj->iframes_timer;
            obj->speed_y = obj->follow_id;
            s32 factor = ((bossXToReach - obj->x) * 262144) / 55;
            factor /= get_eta(obj)->speed_x_right;
            bossSpeedFactor = Abs(factor);
            bossReachingAccuracyX = 0xFF;
            bossReachingAccuracyY = 0;
            bossReachingTimer = 1;
            obj->gravity_value_1 = 0;
            obj->gravity_value_2 = 0;
            break;
        }

        case GO_NOP:
            setBossReachingSpeeds(obj, bossReachingTimer,
                                  bossReachingAccuracyX, bossReachingAccuracyY);
            break;
    }
}

//3F4A4
void DoMOSAMSPoingCollision(obj_t* obj, s16 sprite) {
    if ((get_eta(obj)->flags & 1) && (sprite == 3 || sprite == 6)) {
        poing.damage = 1;
        obj_hurt(obj);
        if (obj->hit_points != 0) {
            skipToLabel(obj, 4, true);
        } else {
            skipToLabel(obj, 2, true);
            obj->param = 1;
        }
    }
}

//3F518
void allocateStoskoClaw(obj_t* stosko) {
    stosko->link = -1;
    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* claw = &level.objects[i];
        if (claw->type == TYPE_224_STOSKO_PINCE && !claw->is_active) {
            claw->x = stosko->x;
            claw->y = stosko->y;
            claw->speed_x = 0;
            calc_obj_pos(claw);
            claw->flags.alive = true;
            claw->is_active = true;
            add_alwobj(claw);
            claw->cmd_offset = -1;
            claw->nb_cmd = 0;
            stosko->link = i;
            break;
        }
    }
}

//3F598
void doSTOSKOcommand(obj_t* stosko) {
    scrollLocked = true;

    switch ((stosko->main_etat << 8) | stosko->sub_etat) {
        case 0x100:
            if (stosko->x <= stosko->init_x) {
                set_main_and_sub_etat(stosko, 0, 0);
                set_main_and_sub_etat(&ray, 0, 0);
                remoteRayXToReach = OBJ_INVALID_XY;
            } else {
                remoteRayXToReach = scroll_start_x;
                if (ray.main_etat == 3 && ray.sub_etat == 20) {
                    ray.flags.flip_x = true;
                }
            }
            SET_X_SPEED(stosko);
            break;

        case 0x008:
            stosko->speed_x = 0;
            stosko->speed_y = -8;
            break;

        case 0x000:
            if (poing.is_active || ray.main_etat == 2) {
                set_sub_etat(stosko, 5);
            } else if (stosko->timer != 0) {
                --stosko->timer;
            } else {
                set_sub_etat(stosko, 1);
            }
            break;

        case 0x001:
            if (poing.is_active) {
                if (Abs(ray.y + ray.offset_by - stosko->y - stosko->offset_by) < 48 ||
                    stosko->anim_frame >= 10) {
                    set_sub_etat(stosko, 10);
                    stosko->anim_index = stosko->eta[0][10].anim_index;
                } else {
                    set_sub_etat(stosko, 5);
                }
            } else if (EOA(stosko)) {
                allocateStoskoClaw(stosko);
            }
            break;

        case 0x004:
            stosko->timer = 50;
            /* fall through */
        case 0x002: {
            obj_t* claw = &level.objects[stosko->link];
            if (claw->speed_x > 0 && claw->x >= stosko->x) {
                stosko->link = -1;
                claw->flags.alive = false;
                set_sub_etat(stosko, 3);
            }
            break;
        }

        case 0x003:
        case 0x007:
        case 0x00C:
        case 0x00D:
            stosko->timer = 50;
            break;

        case 0x00B:
            if (!poing.is_active) {
                set_sub_etat(stosko, 12);
            }
            break;

        case 0x006:
            if (ray.main_etat != 2 && !poing.is_active) {
                set_sub_etat(stosko, 7);
            }
            break;
    }
}

//3F7EC
void DoSTOSKOPoingCollision(obj_t* obj, s16 sprite) {
    if (sprite != 6 || poing.is_returning || !(get_eta(obj)->flags & 1)) {
        return;
    }

    poing.damage = 1;
    obj_hurt(obj);
    if (obj->hit_points != 0) {
        set_sub_etat(obj, obj->sub_etat == 2 ? 4 : 13);
    } else {
        if (obj->sub_etat == 2) {
            obj_t* claw = &level.objects[obj->link];
            claw->flags.alive = false;
            DO_NOVA(claw);
        }
        set_sub_etat(obj, 8);
        obj->param = 1;
    }
}

//3F8AC
void doBBF2command(obj_t* obj) {
    s16 spr_x;
    s16 spr_y;
    s16 spr_w;
    s16 spr_h;

    if (!(obj->main_etat == 0 && obj->sub_etat == 2)) {
        s16 vertical_delta = 0;
        /* As in the PC allocator, this target calculation has a +8 bias
           that is absent from the PS1 version. */
        s16 ray_half_x = ashr32(ray.x + 8 + ray.offset_bx - xmap, 1);

        if (obj->type == TYPE_231_HYB_BBF2_D) {
            if (BBF2DEsk == 0) {
                YPosBBF2D = NiveauSol - (ray.offset_by - ray.offset_hy) - (ray_half_x - 90);
            } else {
                if (BBF2DEsk == 100) {
                    s16 boss_mid_y = obj->y + ((obj->offset_by + obj->offset_hy) >> 1);
                    GET_SPRITE_POS(TirBBF2G, 8, &spr_x, &spr_y, &spr_w, &spr_h);
                    if (spr_y + spr_h < boss_mid_y &&
                        spr_y + spr_h > obj->y + obj->offset_hy - 10) {
                        YPosBBF2D += 50;
                    } else if (spr_y > boss_mid_y && spr_y < obj->y + obj->offset_by + 10) {
                        YPosBBF2D -= 50;
                    } else if (spr_y > obj->y + obj->offset_hy - 10 &&
                               spr_y + spr_h < obj->y + obj->offset_by + 10) {
                        YPosBBF2D -= 80;
                    } else {
                        YPosBBF2D = NiveauSol - (ray.offset_by - ray.offset_hy) -
                                    (ray_half_x - 90);
                    }
                }
                --BBF2DEsk;
            }
            vertical_delta = YPosBBF2D - 150 - obj->y;
        } else if (obj->type == TYPE_232_HYB_BBF2_G) {
            if (BBF2GEsk == 0) {
                YPosBBF2G = NiveauSol - (ray.offset_by - ray.offset_hy) + (ray_half_x - 70);
            } else {
                if (BBF2GEsk == 100) {
                    s16 boss_mid_y = obj->y + ((obj->offset_by + obj->offset_hy) >> 1);
                    GET_SPRITE_POS(TirBBF2D, 8, &spr_x, &spr_y, &spr_w, &spr_h);
                    if (spr_y + spr_h < boss_mid_y &&
                        spr_y + spr_h > obj->y + obj->offset_hy - 10) {
                        YPosBBF2G += 50;
                    } else if (spr_y > boss_mid_y && spr_y < obj->y + obj->offset_by + 10) {
                        YPosBBF2G -= 50;
                    } else if (spr_y > obj->y + obj->offset_hy - 10 &&
                               spr_y + spr_h < obj->y + obj->offset_by + 10) {
                        YPosBBF2G -= 80;
                    } else {
                        YPosBBF2G = NiveauSol - (ray.offset_by - ray.offset_hy) +
                                    (ray_half_x - 70);
                    }
                }
                --BBF2GEsk;
            }
            vertical_delta = YPosBBF2G - 150 - obj->y;
        }

        /* PC uses ymap + 50 as its lower clamp; PS1 uses ymap + 90. */
        if ((obj->y > ymap + 50 && vertical_delta > 0) ||
            (obj->y < ymap - 150 && vertical_delta < 0)) {
            vertical_delta = 0;
            obj->speed_y = 0;
        }
        if (vertical_delta != 0) {
            obj->speed_y = MAX(-48, MIN(48,
                obj->speed_y + SGN(vertical_delta - obj->speed_y)));
        }
    }

    GET_SPRITE_POS(&ray, 5, &spr_x, &spr_y, &spr_w, &spr_h);
    spr_y += 15;
    s16 boss_y = obj->y + 150;
    if (obj->sub_etat == 0 && spr_y > boss_y - 15 && spr_y < boss_y + 15) {
        set_sub_etat(obj, 5);
    } else if ((obj->sub_etat == 6 || obj->sub_etat == 4) &&
               (spr_y < boss_y - 20 || spr_y > boss_y + 20)) {
        set_sub_etat(obj, 7);
    } else if ((obj->sub_etat == 5 || obj->sub_etat == 6) &&
               spr_y > boss_y - 7 && spr_y < boss_y + 7) {
        set_sub_etat(obj, 3);
    } else if (obj->sub_etat == 3 && obj->anim_frame == 3) {
        AllocateTirBBF2(obj);
        if (obj->type == TYPE_231_HYB_BBF2_D) {
            BBF2GEsk = 100;
        } else {
            BBF2DEsk = 100;
        }
    }
}

//3FD78
void DO_HYB_BBF2_POING_COLLISION(obj_t* obj, s16 sprite) {
    (void)sprite;
    if (obj->sub_etat == 2 || obj->sub_etat == 1) {
        return;
    }

    obj_t* linked = &level.objects[obj->link];
    if (linked->sub_etat == 1) {
        return;
    }

    --obj->hit_points;
    linked->hit_points = obj->hit_points;
    if (obj->hit_points != 0) {
        set_main_and_sub_etat(obj, 0, 1);
        return;
    }

    set_main_and_sub_etat(obj, 0, 2);
    obj->speed_y = -128;
    obj->speed_x = 0;
    obj->flags.read_commands = false;
    obj->param = 1;

    set_main_and_sub_etat(linked, 0, 2);
    linked->speed_y = -128;
    linked->speed_x = 0;
    linked->flags.read_commands = false;
    linked->param = 1;
}

//3FE8C
void AllocateTirBBF2(obj_t* boss) {
    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* laser = &level.objects[i];
        if (laser->type == TYPE_233_HYB_BBF2_LAS && !laser->is_active) {
            laser->display_prio = 4;
            laser->x = laser->init_x = boss->x;
            laser->y = laser->init_y = boss->y;
            if (boss->type == TYPE_231_HYB_BBF2_D) {
                laser->speed_x = -5;
                TirBBF2D = laser;
                laser->flags.flip_x = false;
            } else {
                laser->speed_x = 5;
                TirBBF2G = laser;
                laser->flags.flip_x = true;
            }
            laser->speed_y = 0;
            laser->is_active = true;
            laser->flags.alive = true;
            add_alwobj(laser);
            laser->link = boss->link;
            break;
        }
    }
}

//3FF5C
void DO_HYB_BBF2_LAS(obj_t* laser) {
    obj_t* target = &level.objects[laser->link];
    if ((target->type == TYPE_231_HYB_BBF2_D || target->type == TYPE_232_HYB_BBF2_G) &&
        OBJ_IN_COL_ZDC(laser, target)) {
        for (s16 i = 0; i < level.nb_objects; ++i) {
            obj_t* explosion = &level.objects[i];
            if (explosion->type == TYPE_11_BOUM && !explosion->is_active) {
                s16 laser_x;
                s16 laser_y;
                s16 laser_w;
                s16 laser_h;
                s16 unused_x;
                s16 unused_y;
                s16 unused_w;
                s16 unused_h;
                GET_ANIM_POS(laser, &laser_x, &laser_y, &laser_w, &laser_h);
                GET_ANIM_POS(explosion, &unused_x, &unused_y, &unused_w, &unused_h);
                explosion->anim_frame = 0;
                explosion->x = laser_x - explosion->offset_bx;
                if (laser->speed_x > 0) {
                    explosion->x += laser_w;
                }
                explosion->y = laser_y + (laser_h >> 1) -
                               ((explosion->offset_by + explosion->offset_hy) >> 1);
                calc_obj_pos(explosion);
                explosion->flags.alive = true;
                explosion->is_active = true;
                add_alwobj(explosion);
                break;
            }
        }

        DO_HYB_BBF2_POING_COLLISION(target, 0);
        laser->flags.alive = false;
        laser->is_active = false;
    }

    if (laser->speed_y == 0) {
        for (s16 i = 0; i < actobj.num_active_objects; ++i) {
            obj_t* other = &level.objects[actobj.objects[i]];
            if (other->type == TYPE_233_HYB_BBF2_LAS && other->id != laser->id &&
                OBJ_IN_COL_ZDC(laser, other)) {
                other->speed_x = 0;
                laser->speed_x = 0;
                other->speed_y = 3;
                laser->speed_y = 3;
                laser->link = other->id;
                other->link = laser->id;
                break;
            }
        }
    }
}

//40168
void DoHybBBF2LasRaymanCollision(obj_t* obj) {
    obj->is_active = false;
    obj->flags.alive = false;
}

//40184
s32 OBJ_IN_COL_ZDC(obj_t* obj1, obj_t* obj2) {
    zdc_t* zdc1 = get_zdc(obj1, 0);
    zdc_t* zdc2 = get_zdc(obj2, 0);
    s16 x1;
    s16 y1;
    s16 w1;
    s16 h1;
    s16 x2;
    s16 y2;
    s16 w2;
    s16 h2;

    GET_SPRITE_POS(obj1, zdc1->sprite, &x1, &y1, &w1, &h1);
    GET_SPRITE_POS(obj2, zdc2->sprite, &x2, &y2, &w2, &h2);

    if (!obj1->flags.flip_x) {
        x1 += zdc1->x_pos;
    } else {
        x1 = x1 + w1 - (zdc1->x_pos + zdc1->width);
    }
    if (!obj2->flags.flip_x) {
        x2 += zdc2->x_pos;
    } else {
        x2 = x2 + w2 - (zdc2->x_pos + zdc2->width);
    }
    y1 += zdc1->y_pos;
    y2 += zdc2->y_pos;

    return inter_box(x1, y1, zdc1->width, zdc1->height,
                     x2, y2, zdc2->width, zdc2->height);
}

//402C0
void AllocateDarkPhase2(obj_t* mr_dark_obj) {
    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* obj = &level.objects[i];
        if (obj->type == TYPE_32_DARK_PHASE2 && !obj->is_active) {
            obj->x = obj->init_x = mr_dark_obj->x;
            obj->y = obj->init_y = mr_dark_obj->y;
            obj->speed_x = 0;
            obj->speed_y = 0;
            obj->flags.alive = true;
            obj->is_active = true;
            set_main_and_sub_etat(obj, 0, 38);

            dark_obj = mr_dark_obj;
            phase_dark2 = 0;
            mr_dark_obj->flags.alive = false;
            mr_dark_obj->is_active = false;
            break;
        }
    }
}

//40364
void DO_DARK2_AFFICHE_TEXT(void) {
    if (!TextDark2_Affiche) {
        return;
    }

    let_shadow = true;

    /* PC/Android run this final title pass after Mr Dark. PS1's routine only
       draws the scrolling taunt used during phase two. */
    if (finBosslevel.mr_dark) {
        if (NBRE_SAVE != 0) {
            return;
        }

        if (FinalPassF == 0) {
            if (FinalPassX > 66) {
                FinalPassA = FinalPassX;
                FinalPassF = 1;
            } else if (FinalPassX > 40) {
                FinalPassX += 4;
            } else {
                FinalPassX += 2;
            }
        } else {
            s16 offset = ashr16(expsin[FinalPassN], 7);
            if (offset != 255) {
                FinalPassX += offset;
                ++FinalPassN;
            }
        }
        return;
    }

    display_text_sin(txt_dark2, XText, YText, temps_text, 2, 0);
    if (gele == 0) {
        temps_text += 3;
    }
}

//404B0
void DO_DARK_PHASE2_COMMAND(obj_t* obj) {
    s16 dark_x;
    s16 dark_y;
    s16 dark_w;
    s16 dark_h;

    if (obj->main_etat == 0 && obj->sub_etat == 38) {
        RayEvts.poing = false;
        sinus_actif = 0;
        flammes_actives = 0;
        phase_dark2 = 0;
        TextDark2_Affiche = false;
        scroll_start_x = 0;
        scroll_end_x = 0;
        ToonJustGivePoing = 0;
        dark_obj->is_active = false;
        dark_obj->flags.alive = false;
    }

    switch (phase_dark2) {
        case 0:
            if (sinus_actif == 0) {
                sinus_actif = 1;
                set_sub_etat(obj, 30);
            } else if (sinus_actif == 2) {
                phase_dark2 = 1;
            }
            break;

        case 1:
            if (flammes_actives == 0) {
                flammes_actives = 1;
                set_sub_etat(obj, 31);
            } else if (flammes_actives == 2 &&
                       level.objects[flamme_gauche_id].cmd == GO_NOP &&
                       level.objects[flamme_droite_id].cmd == GO_NOP) {
                skipToLabel(&level.objects[flamme_droite_id], 2, true);
                skipToLabel(&level.objects[flamme_gauche_id], 2, true);
                phase_dark2 = 2;
            }
            break;

        case 2:
            if (level.objects[flamme_gauche_id].cmd == GO_NOP &&
                level.objects[flamme_droite_id].cmd == GO_NOP) {
                phase_dark2 = 3;
                sinus_actif = 0;
            }
            break;

        case 3:
            if (sinus_actif == 0) {
                sinus_actif = 1;
                set_sub_etat(obj, 30);
            } else if (sinus_actif == 2) {
                phase_dark2 = 4;
            }
            break;

        case 4:
            skipToLabel(&level.objects[flamme_droite_id], 3, true);
            skipToLabel(&level.objects[flamme_gauche_id], 3, true);
            phase_dark2 = 5;
            break;

        case 5:
            if (level.objects[flamme_gauche_id].cmd == GO_NOP &&
                level.objects[flamme_droite_id].cmd == GO_NOP) {
                if (sinus_actif == 0) {
                    sinus_actif = 1;
                    set_sub_etat(obj, 30);
                } else if (sinus_actif == 2) {
                    phase_dark2 = 6;
                    sinus_actif = 0;
                }
            } else {
                sinus_actif = 0;
            }
            break;

        case 6:
            if (sinus_actif == 0) {
                skipToLabel(&level.objects[flamme_droite_id], 2, true);
                skipToLabel(&level.objects[flamme_gauche_id], 2, true);
                sinus_actif = 1;
                set_sub_etat(obj, 30);
            } else if (sinus_actif == 2 &&
                       level.objects[flamme_gauche_id].cmd == GO_NOP &&
                       level.objects[flamme_droite_id].cmd == GO_NOP) {
                phase_dark2 = 7;
                sinus_actif = 0;
            }
            break;

        case 7:
            if (obj->sub_etat != 33) {
                set_sub_etat(obj, 33);
                num_dark2_phrase = 0;
                TextDark2_Affiche = true;
            } else if (num_dark2_phrase == 2) {
                set_sub_etat(obj, 32);
                phase_dark2 = 8;
                TextDark2_Affiche = false;
            }
            break;

        case 8:
            skipToLabel(&level.objects[flamme_gauche_id], 4, true);
            phase_dark2 = 9;
            PosArXToon1 = -1;
            break;

        case 9:
            if (RayEvts.poing) {
                phase_dark2 = 10;
            } else if (PosArXToon1 == -1 && level.objects[flamme_gauche_id].speed_x == 4) {
                AllocateToons();
                PosArXToon1 = poing_obj->x + 64;
                PosArYToon1 = poing_obj->y + 50;
                PosArXToon2 = poing_obj->x + 48;
                PosArYToon2 = poing_obj->y + 50;
            }
            break;

        case 10:
            set_main_and_sub_etat(&level.objects[flamme_gauche_id], 0, 4);
            set_main_and_sub_etat(&level.objects[flamme_droite_id], 0, 4);
            phase_dark2 = 11;
            break;

        case 11:
            if (ToonJustGivePoing == 1 &&
                (block_flags[mp.map[ray.ray_dist].tile_type] & (1 << BLOCK_SOLID)) &&
                !(ray.main_etat == 3 && ray.sub_etat == 23)) {
                ToonJustGivePoing = 2;
                set_main_and_sub_etat(&ray, 3, 23);
                ray.speed_x = 0;
                ray.speed_y = 0;
            }

            if (!level.objects[flamme_gauche_id].is_active &&
                !(obj->main_etat == 0 && obj->sub_etat == 34) &&
                !(obj->main_etat == 2 && obj->sub_etat == 1) &&
                !(ray.main_etat == 3 && ray.sub_etat == 23) &&
                ToonJustGivePoing == 2) {
                ToonJustGivePoing = 0;
                set_main_and_sub_etat(obj, 0, 34);
            } else if (obj->anim_frame == 4 &&
                       obj->main_etat == 0 && obj->sub_etat == 34) {
                obj->speed_y = -4;
                phase_dark2 = 12;
                goto_phase3(dark_obj);
            }
            break;
    }

    if (obj->main_etat != 0) {
        return;
    }

    switch (obj->sub_etat) {
        case 31:
            if (obj->anim_frame == 22) {
                GET_SPRITE_POS(obj, 3, &dark_x, &dark_y, &dark_w, &dark_h);
                dark2_rayon_dx_1 = 0;
                dark2_rayon_dx_2 = 0;
                dark2_rayon_dy_1 = -15;
                dark2_rayon_dy_2 = -15;
                s16 sort_x = dark_x + (dark_w >> 1) - 96;
                s16 sort_y = dark_y + (dark_h >> 1) - 128;
                allocate_DARK2_SORT(sort_x, sort_y, 37, 1);
                allocate_DARK2_SORT(sort_x, sort_y, 37, 0);
            }
            break;

        case 30:
            if (obj->anim_frame == 22) {
                GET_SPRITE_POS(obj, 3, &dark_x, &dark_y, &dark_w, &dark_h);
                dark2_rayon_dx_1 = -16;
                dark2_rayon_dx_2 = -16;
                dark2_rayon_dy_1 = 76;
                dark2_rayon_dy_2 = -76;
                sens_sinus_1 = 0;
                sens_sinus_2 = 1;
                PosXSin1 = PosXSin2 = dark_x + (dark_w >> 1) - 96;
                PosYSin1 = PosYSin2 = dark_y + (dark_h >> 1) - 128;
                allocate_DARK2_SORT(PosXSin1, PosYSin1, 35, 1);
                allocate_DARK2_SORT(PosXSin2, PosYSin2, 35, 0);
            }
            break;

        case 33:
            if (num_dark2_phrase == 0) {
                strcpy(txt_dark2, language_txt[95]);
                XText = SCREEN_WIDTH / 2;
                YText = -10;
                VitesseYText = 40;
                temps_text = 0;
                num_dark2_phrase = 1;
            } else if (YText < -20) {
                TextDark2_Affiche = false;
                temps_text = 0;
                XText = SCREEN_WIDTH / 2;
                YText = -10;
                VitesseYText = 40;
                ++num_dark2_phrase;
            } else {
                YText += ashr16(VitesseYText, 4);
                if ((VitesseYText != 0 && horloge[4] == 0) ||
                    (VitesseYText == 0 && horloge[6] == 0)) {
                    --VitesseYText;
                }
            }
            break;
    }
}

//40E58
void DO_DARK2_SORT_COMMAND(obj_t* obj) {
    if (obj->hit_points == 0) {
        return;
    }

    if (obj->sub_etat == 35 || obj->sub_etat == 36) {
        if (obj->x < -obj->offset_bx) {
            sinus_actif = 2;
            obj->hit_points = 0;
            return;
        }

        if (obj->iframes_timer != 0) {
            if (sens_sinus_1) {
                dark2_rayon_dy_1 += 3;
                if (dark2_rayon_dy_1 > 76) {
                    sens_sinus_1 = 0;
                }
            } else {
                dark2_rayon_dy_1 -= 3;
                if (dark2_rayon_dy_1 < -76) {
                    sens_sinus_1 = 1;
                }
            }
            PosXSin1 += ashr16(dark2_rayon_dx_1, 4);
            PosYSin1 += ashr16(dark2_rayon_dy_1, 4);
            if (obj->anim_frame == 1) {
                allocate_DARK2_SORT(PosXSin1, PosYSin1, obj->sub_etat, obj->iframes_timer);
                obj->hit_points = 0;
            }
        } else {
            if (sens_sinus_2) {
                dark2_rayon_dy_2 += 3;
                if (dark2_rayon_dy_2 > 76) {
                    sens_sinus_2 = 0;
                }
            } else {
                dark2_rayon_dy_2 -= 3;
                if (dark2_rayon_dy_2 < -76) {
                    sens_sinus_2 = 1;
                }
            }
            PosXSin2 += ashr16(dark2_rayon_dx_2, 4);
            PosYSin2 += ashr16(dark2_rayon_dy_2, 4);
            if (obj->anim_frame == 1) {
                allocate_DARK2_SORT(PosXSin2, PosYSin2, obj->sub_etat, obj->iframes_timer);
                obj->hit_points = 0;
            }
        }
    } else if (obj->sub_etat == 37) {
        s16 target_dx = (obj->iframes_timer != 0 ? SCREEN_WIDTH - 80 : 5) -
                        (obj->x + obj->offset_bx);
        s16 target_dy = firstFloorBelow(obj) - (obj->y + obj->offset_by);
        s16 distance = Abs(target_dx) + Abs(target_dy);

        if (distance < 10) {
            obj->hit_points = 0;
            AllocateFlammes(obj->iframes_timer);
            return;
        }

        if (distance > 0) {
            target_dx = (s16)((s32)target_dx * 16 / distance);
            target_dy = (s16)((s32)target_dy * 16 / distance);
        }

        if (obj->anim_frame == 1) {
            s16* ray_dx = obj->iframes_timer != 0 ? &dark2_rayon_dx_1 : &dark2_rayon_dx_2;
            s16* ray_dy = obj->iframes_timer != 0 ? &dark2_rayon_dy_1 : &dark2_rayon_dy_2;
            *ray_dx += SGN(target_dx - *ray_dx);
            *ray_dy += SGN(target_dy - *ray_dy);
            allocate_DARK2_SORT(obj->x + *ray_dx, obj->y + *ray_dy,
                                obj->sub_etat, obj->iframes_timer);
            obj->hit_points = 0;
        }
    }
}

//41208
void allocate_DARK2_SORT(s16 x, s16 y, s16 sub_etat, s16 iframes) {
    obj_t* first_inactive = NULL;
    s16 first_index = -1;

    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* obj = &level.objects[i];
        if (obj->type == TYPE_33_DARK2_SORT && !obj->is_active) {
            first_inactive = obj;
            first_index = i;
            break;
        }
    }

    if (first_inactive == NULL) {
        return;
    }

    /* The PC allocator tries to continue after the first active projectile
       following the first free slot, then wraps to that first free slot. */
    obj_t* selected = first_inactive;
    s16 active_index = -1;
    for (s16 i = first_index; i < level.nb_objects; ++i) {
        obj_t* obj = &level.objects[i];
        if (obj->type == TYPE_33_DARK2_SORT && obj->is_active) {
            active_index = i;
            break;
        }
    }
    if (active_index != -1) {
        for (s16 i = active_index; i < level.nb_objects; ++i) {
            obj_t* obj = &level.objects[i];
            if (obj->type == TYPE_33_DARK2_SORT && !obj->is_active) {
                selected = obj;
                break;
            }
        }
    }

    selected->flags.alive = true;
    if (!selected->is_active) {
        add_alwobj(selected);
    }
    selected->is_active = true;
    selected->hit_points = 1;
    selected->x = x;
    selected->y = y;
    selected->iframes_timer = iframes;
    selected->param = 0;
    set_main_and_sub_etat(selected, 0, sub_etat);
}

//41328
void DoFlammeCommand(obj_t* obj) {
    if (obj->main_etat == 0 && obj->sub_etat == 5) {
        obj->is_active = false;
        obj->flags.alive = false;
    } else {
        DO_ONE_CMD(obj);
    }
}

//41354
void DoFlammeRaymanCollision(obj_t* obj) {
    s16 old_iframes = ray.iframes_timer;

    if (old_iframes < 10 && old_iframes != -1) {
        RAY_HIT(false, obj);
        ray.iframes_timer = old_iframes;
    } else {
        RAY_HIT(true, obj);
        if (ray_mode != 3) {
            ray.iframes_timer = 10;
        }
    }
}

//413B4
void AllocateFlammes(s16 side) {
    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* obj = &level.objects[i];
        if (side == 0 && obj->type == TYPE_209_FIRE_LEFT && !obj->is_active) {
            obj->display_prio = 4;
            obj->y = obj->init_y = 100;
            obj->x = obj->init_x = 5 - obj->offset_bx;
            obj->y = obj->init_y = firstFloorBelow(obj) - obj->offset_by;
            obj->speed_x = 0;
            obj->speed_y = 0;
            obj->flags.alive = true;
            obj->is_active = true;
            add_alwobj(obj);
            flamme_gauche_id = obj->id;
            flammes_actives = 2;
            break;
        }
        if (side == 1 && obj->type == TYPE_210_FIRE_RIGHT && !obj->is_active) {
            obj->display_prio = 4;
            obj->y = obj->init_y = 100;
            obj->x = obj->init_x = SCREEN_WIDTH - 80 - obj->offset_bx;
            obj->y = obj->init_y = firstFloorBelow(obj) - obj->offset_by;
            obj->speed_x = 0;
            obj->speed_y = 0;
            obj->flags.alive = true;
            obj->is_active = true;
            add_alwobj(obj);
            flamme_droite_id = obj->id;
            flammes_actives = 2;
            break;
        }
    }
}

//41510
void AllocateToons(void) {
    s16 first_id = -1;

    for (s16 i = 0; i < level.nb_objects; ++i) {
        obj_t* obj = &level.objects[i];
        if (obj->type != TYPE_108_DARK2_PINK_FLY || obj->is_active) {
            continue;
        }

        obj->display_prio = 4;
        obj->speed_x = 0;
        obj->speed_y = 0;
        obj->param = 0;
        obj->flags.alive = true;
        obj->is_active = true;
        add_alwobj(obj);

        if (first_id == -1) {
            obj->x = obj->init_x = SCREEN_WIDTH;
            obj->y = obj->init_y = ymap + 100;
            obj->hit_points = 0;
            first_id = i;
        } else {
            obj->x = obj->init_x = -20;
            obj->y = obj->init_y = ymap + 100;
            obj->hit_points = 1;
            obj->link = first_id;
            level.objects[first_id].link = i;
            break;
        }
    }
}

//41650
void DO_DARK2_TOONS_COMMAND(obj_t* obj) {
    s16 diff_x;
    s16 diff_y;

    if (obj->hit_points == 0) {
        diff_x = PosArXToon1 - (obj->x + obj->offset_bx);
        diff_y = PosArYToon1 - (obj->y + obj->offset_by);
    } else {
        diff_x = PosArXToon2 - (obj->x + obj->offset_bx);
        diff_y = PosArYToon2 - (obj->y + obj->offset_by);
    }

    obj_t* linked = &level.objects[obj->link];
    if (obj->param != 0 && linked->param != 0) {
        if (obj->is_active) {
            if (linked->is_active) {
                poing_obj->x = ((obj->x + obj->offset_bx + linked->x + linked->offset_bx) >> 1) -
                               poing_obj->offset_bx;
                poing_obj->y = ((obj->y + obj->offset_by + linked->y + linked->offset_by) >> 1) -
                               poing_obj->offset_by;
            } else {
                poing_obj->x = obj->x + obj->offset_bx - poing_obj->offset_bx;
                poing_obj->y = obj->y + obj->offset_by - poing_obj->offset_by;
            }
        } else if (linked->is_active) {
            poing_obj->x = linked->x + linked->offset_bx - poing_obj->offset_bx;
            poing_obj->y = linked->y + linked->offset_by - poing_obj->offset_by;
        }

        s16 ray_x = ray.x + ray.offset_bx;
        s16 ray_y = ray.y + (ray.offset_by >> 1);
        PosArXToon1 = ray_x + (obj->offset_bx >> 1);
        PosArYToon1 = ray_y;
        PosArXToon2 = ray_x - (obj->offset_bx >> 1);
        PosArYToon2 = ray_y;
    } else if (!RayEvts.poing) {
        PosArXToon1 = poing_obj->x + 64;
        PosArYToon1 = poing_obj->y + 50;
        PosArXToon2 = poing_obj->x + 48;
        PosArYToon2 = poing_obj->y + 50;
    }

    s16 distance = Abs(diff_x) + Abs(diff_y);
    if (distance < 2 && obj->param == 0) {
        obj->param = 1;
        if (!linked->is_active) {
            linked->param = 1;
        }
    }
    if (distance > 0) {
        diff_x = (s16)((s32)diff_x * 32 / distance);
        diff_y = (s16)((s32)diff_y * 32 / distance);
    }

    obj->speed_y = MAX(-40, MIN(40, obj->speed_y + SGN(diff_y - obj->speed_y)));
    obj->speed_x = MAX(-40, MIN(40, obj->speed_x + SGN(diff_x - obj->speed_x)));

    if (obj->speed_x > 0 && !obj->flags.flip_x) {
        obj->flags.flip_x = true;
    } else if (obj->speed_x < 0 && obj->flags.flip_x) {
        obj->flags.flip_x = false;
    }

    if (obj->y < ymap - 200) {
        obj->flags.alive = false;
        obj->is_active = false;
    }
}

//419E4
void ToonDonnePoing(obj_t* obj) {
    obj_t* linked = &level.objects[obj->link];
    if (obj->param != 0 && linked->param != 0) {
        RayEvts.poing = true;
        obj->param = 0;
        linked->param = 0;
        PosArXToon1 = 10;
        PosArXToon2 = SCREEN_WIDTH - 20;
        PosArYToon1 = ymap - 200;
        PosArYToon2 = ymap - 200;
        DO_NOVA(poing_obj);
        ToonJustGivePoing = 1;
        poing_obj->init_sub_etat = 8;
        poing_obj->is_active = false;
        poing_obj->flags.alive = false;
    }
}

