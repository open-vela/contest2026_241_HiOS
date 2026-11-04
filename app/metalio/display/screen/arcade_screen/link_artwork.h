#pragma once
// Small vector artwork: no external image decoding or per-tile child objects.
inline void DrawLinkArtwork(lv_event_t *event) {
    const int type=*static_cast<int*>(lv_event_get_user_data(event));
    if(type<1 || type>8)return;
    lv_area_t bounds;lv_obj_get_coords(lv_event_get_target_obj(event),&bounds);
    auto *layer=lv_event_get_layer(event);
    auto put=[&](int,int x,int y,int w,int h,uint32_t color,int radius){
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
        d.bg_color=lv_color_hex(color);d.bg_opa=LV_OPA_COVER;d.radius=radius;
        lv_area_t r={bounds.x1+x,bounds.y1+y+3,bounds.x1+x+w-1,bounds.y1+y+h+2};
        lv_draw_rect(layer,&d,&r);
    };
    switch (type) {
    case 1: // glossy red apple
        put(0, 18, 17, 36, 34, 0xAE3548, 17);
        put(1, 20, 16, 32, 31, 0xF04452, 16);
        put(2, 21, 18, 14, 21, 0xFF6870, 8);
        put(3, 38, 18, 12, 21, 0xD93349, 7);
        put(4, 34, 8, 5, 13, 0x68422F, 2);
        put(5, 38, 6, 17, 9, 0x51B879, 7);
        put(6, 25, 21, 5, 12, 0xFFE0D2, 2);
        put(7, 26, 38, 8, 3, 0xF7A6A2, 2);
        break;
    case 2: // golden pear
        put(0, 20, 19, 34, 34, 0xB77B2B, 17);
        put(1, 26, 10, 22, 28, 0xF4C83F, 11);
        put(2, 20, 27, 34, 25, 0xF1B72D, 16);
        put(3, 27, 13, 11, 18, 0xFFE16B, 6);
        put(4, 35, 7, 4, 12, 0x68422F, 2);
        put(5, 39, 6, 16, 8, 0x54B56D, 7);
        put(6, 27, 27, 4, 12, 0xFFF1A0, 2);
        put(7, 47, 34, 3, 4, 0xD9952A, 2);
        break;
    case 3: // blush peach
        put(0, 18, 16, 37, 36, 0xBB5471, 18);
        put(1, 20, 15, 33, 34, 0xF58DA7, 17);
        put(2, 21, 17, 15, 25, 0xFFB1BC, 9);
        put(3, 37, 18, 14, 25, 0xE96C8D, 8);
        put(4, 35, 7, 4, 13, 0x68422F, 2);
        put(5, 39, 6, 16, 8, 0x54B56D, 7);
        put(6, 35, 20, 2, 21, 0xCE5C7C, 1);
        put(7, 25, 22, 4, 9, 0xFFF0DD, 2);
        break;
    case 4: // purple grape bunch
        put(0, 24, 13, 15, 15, 0x6045A5, 8);
        put(1, 39, 13, 15, 15, 0x503994, 8);
        put(2, 17, 26, 15, 15, 0x7454C6, 8);
        put(3, 32, 26, 15, 15, 0x5941AA, 8);
        put(4, 47, 26, 12, 15, 0x7554C4, 7);
        put(5, 24, 39, 15, 13, 0x5B43AD, 8);
        put(6, 39, 39, 15, 13, 0x704FC0, 8);
        put(7, 29, 7, 23, 10, 0x51B56C, 7);
        put(8, 27, 16, 4, 7, 0xC5B4FF, 2);
        put(9, 42, 17, 4, 7, 0xB5A1F4, 2);
        put(10, 22, 29, 4, 7, 0xC5B4FF, 2);
        put(11, 34, 29, 4, 7, 0xB5A1F4, 2);
        break;
    case 5: // sun-ripened orange
        put(0, 19, 14, 37, 38, 0xBB5F24, 19);
        put(1, 20, 13, 36, 36, 0xF58A24, 18);
        put(2, 24, 17, 27, 27, 0xFFAA35, 14);
        put(3, 36, 6, 4, 12, 0x68422F, 2);
        put(4, 39, 6, 17, 9, 0x4FAC68, 7);
        put(5, 26, 21, 5, 11, 0xFFF0B8, 2);
        put(6, 22, 35, 5, 4, 0xFFD071, 2);
        put(7, 47, 30, 4, 4, 0xFFD071, 2);
        break;
    case 6: // ripe strawberry
        put(0, 21, 16, 31, 37, 0xAD314C, 16);
        put(1, 20, 17, 32, 27, 0xF0445E, 15);
        put(2, 23, 30, 26, 20, 0xE93857, 12);
        put(3, 24, 8, 26, 11, 0x43A864, 6);
        put(4, 31, 4, 12, 10, 0x59BD74, 6);
        put(5, 27, 21, 3, 4, 0xFFE493, 2);
        put(6, 40, 22, 3, 4, 0xFFE493, 2);
        put(7, 23, 30, 3, 4, 0xFFE493, 2);
        put(8, 34, 32, 3, 4, 0xFFE493, 2);
        put(9, 45, 31, 3, 4, 0xFFE493, 2);
        put(10, 31, 43, 3, 4, 0xFFE493, 2);
        put(11, 36, 17, 4, 9, 0xFF8590, 2);
        break;
    case 7: // kiwi slice
        put(0, 16, 12, 42, 42, 0x956438, 21);
        put(1, 19, 15, 36, 36, 0x9DCC47, 18);
        put(2, 29, 25, 16, 16, 0xF7EFC6, 8);
        for (int i=0;i<4;++i) {
            put(3+i, 24+i*7, 20, 2, 4, 0x504127, 1);
            put(7+i, 24+i*7, 44, 2, 4, 0x504127, 1);
        }
        break;
    case 8: // cherries
        put(0, 19, 30, 23, 23, 0xBD254E, 12);
        put(1, 39, 28, 23, 23, 0xEE4165, 12);
        put(2, 29, 12, 3, 21, 0x4E8E4A, 2);
        put(3, 48, 10, 3, 21, 0x4E8E4A, 2);
        put(4, 29, 9, 21, 5, 0x4E8E4A, 2);
        put(5, 34, 5, 17, 9, 0x7BC56D, 5);
        put(6, 24, 34, 4, 8, 0xFF9BB0, 2);
        put(7, 45, 32, 4, 8, 0xFFD0DC, 2);
        break;
    default:
        break;
    }
}
