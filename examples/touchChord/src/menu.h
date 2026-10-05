// Menu object types + state shared between main.cpp (tables, navigation)
// and ui.cpp (rendering).
#ifndef HC_MENU_H
#define HC_MENU_H

#include <Arduino.h>

// A menu is a table of MenuItems. Types:
//   MI_ACTION  — running a command (R fires it)
//   MI_VALUE   — get()/adjust(±1) pair; L/R changes the value, shown after the label
//   MI_SUBMENU — pushes another Menu (R enters, L goes back)
//   MI_INSTLIST / MI_TRACKLIST — dynamic lists rendered with list_count/list_label
enum MIType : uint8_t { MI_ACTION, MI_VALUE, MI_SUBMENU, MI_INSTLIST, MI_TRACKLIST };

struct Menu;
struct MenuItem {
    const char *label;
    MIType      type;
    int         (*get)(int idx);        // MI_VALUE: current value (idx = item context, e.g. drum button)
    const char **vals;                 // MI_VALUE: optional value names
    int         nvals;
    void        (*adjust)(int idx, int d);  // MI_VALUE: d = +1 / -1
    const Menu *sub;               // MI_SUBMENU
    void        (*act)(void);      // MI_ACTION
    int         idx;               // context passed to get/adjust (0 for plain items)
    void        (*fmt)(int idx, int v, char *buf, int bl);  // optional custom value text
};
struct Menu { const char *title; const MenuItem *items; int n; };

extern bool        menu_open;   // menu system active?
extern int         menu_sel;    // cursor row
extern const Menu *menu_cur;    // current menu
// dynamic list menus (their items arrays are built on the fly)
extern const Menu  MENU_INST, MENU_TRACK, MENU_FX;

#endif  // HC_MENU_H
