
//484C0
void get_luciole(void) {
    if (RayEvts.firefly) {
        Bloc_lim_H1_Glob = Bloc_lim_H1;
        Bloc_lim_H2_Glob = Bloc_lim_H2;
        Bloc_lim_W1_Glob = Bloc_lim_W1;
        Bloc_lim_W2_Glob = Bloc_lim_W2;

        Bloc_lim_H1 = Bloc_lim_H1_Aff;
        Bloc_lim_H2 = Bloc_lim_H2_Aff;
        Bloc_lim_W1 = Bloc_lim_W1_Aff;
        Bloc_lim_W2 = Bloc_lim_W2_Aff;
        sprite_clipping(Bloc_lim_W1, Bloc_lim_W2, Bloc_lim_H1, Bloc_lim_H2);
    }
}

//48538
void CLEAR_FIXE_LUCIOLE(void) {
    if (RayEvts.firefly) {
        s16 width = 77;
        s16 height = 35;

        if (id_Cling_Old != 0) {
            width = 85;
            height = 65;
            id_Cling_Old = 0;
        }

        DISPLAY_BLACKBOX(13, 0, width, height, -1, 0);
        DISPLAY_BLACKBOX(241, 4, 68, 23, -1, 0);
    }
}

//485AC
void set_luciole(s32 screen_x, s32 screen_y) {
    s32 radius = rayon_luciole;

    Bloc_lim_H1_Aff = MAX(screen_y - radius, Bloc_lim_H1_Glob);
    Bloc_lim_H1_Aff = MIN(Bloc_lim_H1_Aff, Bloc_lim_H2_Glob);
    Bloc_lim_H2_Aff = MIN(screen_y + radius, Bloc_lim_H2_Glob);
    Bloc_lim_H2_Aff = MAX(Bloc_lim_H2_Aff, Bloc_lim_H1_Glob);
    Bloc_lim_W1_Aff = MAX(screen_x - radius, Bloc_lim_W1_Glob);
    Bloc_lim_W1_Aff = MIN(Bloc_lim_W1_Aff, Bloc_lim_W2_Glob);
    Bloc_lim_W2_Aff = MIN(screen_x + radius, Bloc_lim_W2_Glob);
    Bloc_lim_W2_Aff = MAX(Bloc_lim_W2_Aff, Bloc_lim_W1_Glob);
}

//48650
void init_aff_luciole(s32 screen_x, s32 screen_y) {
    rotationtxt = 0;
    coeffktxt = 5;
    ADDLUCLIP = 15;
    rayon_luciole = 0;

    Bloc_lim_H1_Glob = Bloc_lim_H1;
    Bloc_lim_H2_Glob = Bloc_lim_H2;
    Bloc_lim_W1_Glob = Bloc_lim_W1;
    Bloc_lim_W2_Glob = Bloc_lim_W2;
    set_luciole(screen_x, screen_y);
}

//486B8
void plot2line(s32 center_x, s32 center_y, s32 inner_x, s32 y_offset, u8* draw_buf) {
    s32 outer_radius = rayon_luciole + ADDLUCLIP;
    s32 left_outer = MAX(center_x - outer_radius, Bloc_lim_W1_Glob);
    s32 left_inner = MIN(center_x - inner_x, Bloc_lim_W2_Glob);
    s32 right_inner = MAX(center_x + inner_x, Bloc_lim_W1_Glob);
    s32 right_outer = MIN(center_x + outer_radius, Bloc_lim_W2_Glob);
    s32 left_count = left_inner - left_outer;
    s32 right_count = right_outer - right_inner;
    s32 rows[2] = {center_y - y_offset, center_y + y_offset};

    for (s32 i = 0; i < 2; i++) {
        s32 y = rows[i];
        if (y >= Bloc_lim_H1_Glob && y < Bloc_lim_H2_Glob) {
            if (left_count > 0) {
                memset(draw_buf + y * SCREEN_WIDTH + left_outer, 0, left_count);
            }
            if (right_count > 0) {
                memset(draw_buf + y * SCREEN_WIDTH + right_inner, 0, right_count);
            }
        }
    }
}

//48858
void aff_luciole(s32 center_x, s32 center_y, s32 radius, u8* draw_buf) {
    s32 y_offset = radius;
    for (s32 i = 0; i <= ADDLUCLIP; i++) {
        plot2line(center_x, center_y, 0, y_offset, draw_buf);
        y_offset++;
    }

    if (radius >= 0) {
        s32 x_offset = 0;
        s32 error = 0;
        s32 error_step = 1;
        s32 error_correction = radius * 2 - 1;

        while (x_offset <= radius) {
            plot2line(center_x, center_y, x_offset, radius, draw_buf);
            plot2line(center_x, center_y, radius, x_offset, draw_buf);

            x_offset++;
            error += error_step;
            error_step += 2;
            if (radius <= error) {
                radius--;
                error -= error_correction;
                error_correction -= 2;
            }
        }
    }
}

//48914
void Display_and_free_luciole(u8* draw_buf) {
    if (RayEvts.firefly) {
        s32 movement_x = Abs(old_x_luc - x_luc);
        s32 movement_y = Abs(old_y_luc - y_luc);
        ADDLUCLIP = MAX(movement_x, movement_y) + 6;

        old_x_luc = x_luc;
        old_y_luc = y_luc;
        aff_luciole(x_luc, y_luc, rayon_luciole - 4, draw_buf);

        Bloc_lim_H1 = Bloc_lim_H1_Glob;
        Bloc_lim_H2 = Bloc_lim_H2_Glob;
        Bloc_lim_W1 = Bloc_lim_W1_Glob;
        Bloc_lim_W2 = Bloc_lim_W2_Glob;
    }
}

//489D8
void free_luciole(void) {
    if (RayEvts.firefly) {
        Bloc_lim_H1 = Bloc_lim_H1_Glob;
        Bloc_lim_H2 = Bloc_lim_H2_Glob;
        Bloc_lim_W1 = Bloc_lim_W1_Glob;
        Bloc_lim_W2 = Bloc_lim_W2_Glob;
    }
}

//48A0C
void INIT_LUCIOLE(void) {
    s16 world_x = ray.x + ray.offset_bx;
    s16 world_y = ray.y + ray.offset_hy - 16;

    x_luc = world_x - xmap + 8;
    y_luc = world_y - ymap;
    old_x_luc = x_luc;
    old_y_luc = y_luc;
    vx_luc = 0;
    vy_luc = 0;

    for (s32 i = 0; i < 4; i++) {
        x_ray[i] = world_x;
        y_ray[i] = world_y;
    }

    x_main_luc = world_x;
    y_main_luc = world_y;
    n_ray = 0;
    if (GameModeVideo == MODE_NORMAL) {
        init_aff_luciole(x_luc, y_luc);
    }
}

//48AE0
void DO_LUCIOLE(void) {
    bool follow_ray =
        ray_mode != MODE_3_MORT_DE_RAYMAN &&
        !(ray.main_etat == 2 && ray.sub_etat == 9) &&
        !(ray.main_etat == 3 && (ray.sub_etat == 22 || ray.sub_etat == 32));

    if (follow_ray) {
        if (rayon_luciole < 54 && nb_fade == 0) {
            rayon_luciole = (s16)((54 * (512 - cosinus(rotationtxt))) >> 9);
            if (rotationtxt < 128) {
                rotationtxt += 2;
            }
        } else if (horloge[2] != 0) {
            rayon_luciole--;
        } else {
            rayon_luciole++;
        }

        s16 target_x = x_ray[n_ray];
        s16 target_y = y_ray[n_ray];
        if (poing.is_active && poing_obj != NULL) {
            x_ray[n_ray] = poing_obj->x + poing_obj->offset_bx;
            y_ray[n_ray] = poing_obj->y + poing_obj->offset_by;
        } else {
            s16 spr_x;
            s16 spr_y;
            s16 spr_w;
            s16 spr_h;

            x_ray[n_ray] = ray.x + ray.offset_bx;
            y_ray[n_ray] = ray.y + ray.offset_hy - 4;
            if (GET_SPRITE_POS(&ray, 2, &spr_x, &spr_y, &spr_w, &spr_h)) {
                x_ray[n_ray] = spr_x + (spr_w > 1);
                y_ray[n_ray] = spr_y + (spr_h > 1);
            }
        }
        n_ray = (n_ray + 1) & 3;

        vx_luc += sgn(target_x - x_main_luc - vx_luc);
        vy_luc += sgn(target_y - y_main_luc - vy_luc);
        vx_luc = MAX(vx_luc, -8);
        vx_luc = MIN(vx_luc, 8);
        vy_luc = MAX(vy_luc, -8);
        vy_luc = MIN(vy_luc, 8);
    } else {
        if (rayon_luciole >= 54) {
            rotationtxt = myRand(512);
            rayon_luciole -= 2;
        }

        vx_luc = (s16)((coeffktxt * cosinus(rotationtxt)) >> 9);
        vy_luc = (s16)((coeffktxt * sinus(rotationtxt)) >> 9);
        if (horloge[2] != 0) {
            rayon_luciole--;
            if (rayon_luciole < 0) {
                rayon_luciole = 0;
            }
        }

        rotationtxt += myRand(54);
        if (rotationtxt > 512) {
            rotationtxt = 0;
            coeffktxt++;
            if (coeffktxt > 10) {
                coeffktxt = 10;
            }
        }
    }

    x_main_luc += vx_luc;
    y_main_luc += vy_luc;
    x_luc = x_main_luc - xmap + 8;
    y_luc = y_main_luc - ymap;
    if (GameModeVideo == MODE_NORMAL) {
        set_luciole(x_luc, y_luc);
    }
}

