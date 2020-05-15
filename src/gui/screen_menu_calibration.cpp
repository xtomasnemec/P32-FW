// screen_menu_calibration.c

#include "gui.h"
#include "screen_menu.hpp"
#include "marlin_client.h"
#include "wizard/wizard.h"
#include "window_dlg_wait.h"
#include "screens.h"

#include "menu_vars.h"
#include "eeprom.h"
/*
typedef enum {
    MI_RETURN,
    MI_WIZARD,
    MI_Z_OFFSET,
    MI_AUTO_HOME,
    MI_MESH_BED,
    MI_SELFTEST,
    MI_CALIB_FIRST,
    MI_COUNT
} MI_t;

//"C inheritance" of screen_menu_data_t with data items
#pragma pack(push)
#pragma pack(1)

typedef struct
{
    screen_menu_data_t base;
    menu_item_t items[MI_COUNT];

} this_screen_data_t;

#pragma pack(pop)

void screen_menu_calibration_init(screen_t *screen) {
    marlin_vars_t *vars = marlin_update_vars(MARLIN_VAR_MSK(MARLIN_VAR_Z_OFFSET));
    screen_menu_init(screen, "CALIBRATION", ((this_screen_data_t *)screen->pdata)->items, MI_COUNT, 1, 0);
    psmd->items[MI_RETURN] = menu_item_return;
    psmd->items[MI_WIZARD] = { WindowMenuItem(WI_LABEL_t("Wizard")), SCREEN_MENU_NO_SCREEN };
    psmd->items[MI_Z_OFFSET] = { WindowMenuItem(WI_SPIN_FL_t(vars->z_offset, zoffset_fl_range, zoffset_fl_format, "Z-offset")), SCREEN_MENU_NO_SCREEN };
    psmd->items[MI_AUTO_HOME] = { WindowMenuItem(WI_LABEL_t("Auto Home")), SCREEN_MENU_NO_SCREEN };
    psmd->items[MI_MESH_BED] = { WindowMenuItem(WI_LABEL_t("Mesh Bed Level.")), SCREEN_MENU_NO_SCREEN };
    psmd->items[MI_SELFTEST] = { WindowMenuItem(WI_LABEL_t("SelfTest")), SCREEN_MENU_NO_SCREEN };
    psmd->items[MI_CALIB_FIRST] = { WindowMenuItem(WI_LABEL_t("First Layer Cal.")), SCREEN_MENU_NO_SCREEN };
}

int8_t gui_marlin_G28_or_G29_in_progress() {
    uint32_t cmd = marlin_command();
    if ((cmd == MARLIN_CMD_G28) || (cmd == MARLIN_CMD_G29))
        return -1;
    else
        return 0;
}

int screen_menu_calibration_event(screen_t *screen, window_t *window, uint8_t event, void *param) {
    if (screen_menu_event(screen, window, event, param))
        return 1;
    if ((event == WINDOW_EVENT_CHANGING) && ((int)param == MI_Z_OFFSET))
        marlin_set_z_offset(reinterpret_cast<const WI_SPIN_FL_t &>(psmd->items[MI_Z_OFFSET].item.Get()).value);
    else if ((event == WINDOW_EVENT_CHANGE) && ((int)param == MI_Z_OFFSET))
        eeprom_set_var(EEVAR_ZOFFSET, marlin_get_var(MARLIN_VAR_Z_OFFSET));
    else if (event == WINDOW_EVENT_CLICK) {
        switch ((int)param) {
        case MI_WIZARD:
            wizard_run_complete();
            break;
        case MI_AUTO_HOME:
            marlin_event_clr(MARLIN_EVT_CommandBegin);
            marlin_gcode("G28");
            while (!marlin_event_clr(MARLIN_EVT_CommandBegin))
                marlin_client_loop();
            gui_dlg_wait(gui_marlin_G28_or_G29_in_progress);
            break;
        case MI_MESH_BED:
            if (!marlin_all_axes_homed()) {
                marlin_event_clr(MARLIN_EVT_CommandBegin);
                marlin_gcode("G28");
                while (!marlin_event_clr(MARLIN_EVT_CommandBegin))
                    marlin_client_loop();
                gui_dlg_wait(gui_marlin_G28_or_G29_in_progress);
            }
            marlin_event_clr(MARLIN_EVT_CommandBegin);
            marlin_gcode("G29");
            while (!marlin_event_clr(MARLIN_EVT_CommandBegin))
                marlin_client_loop();
            gui_dlg_wait(gui_marlin_G28_or_G29_in_progress);
            break;
        case MI_SELFTEST:
            wizard_run_selftest();
            break;
        case MI_CALIB_FIRST:
            wizard_run_firstlay();
            break;
        }
    }
    return 0;
}

screen_t screen_menu_calibration = {
    0,
    0,
    screen_menu_calibration_init,
    screen_menu_done,
    screen_menu_draw,
    screen_menu_calibration_event,
    sizeof(this_screen_data_t), //data_size
    0,                          //pdata
};
*/

#include "screen_menu.hpp"
#include "WindowMenuItems.hpp"

using Screen = screen_menu_data_t<false, true, false, MI_RETURN>;

static void init(screen_t *screen) {
    Screen::Create(screen);
}

screen_t screen_menu_calibration = {
    0,
    0,
    init,
    Screen::CDone,
    Screen::CDraw,
    Screen::CEvent,
    sizeof(Screen), //data_size
    0,              //pdata
};

extern "C" screen_t *const get_scr_menu_calibration() { return &screen_menu_calibration; }
