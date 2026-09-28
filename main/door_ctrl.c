#include "door_ctrl.h"

/* 多任务跨核共享的标志位，用 volatile 保证可见性（读写都是单字，原子性足够）。 */
static volatile bool s_door_open = false;
static volatile bool s_stranger  = false;
static volatile bool s_waving    = false;

void door_ctrl_set_open(bool open) { s_door_open = open; }
bool door_ctrl_is_open(void)       { return s_door_open; }

void door_ctrl_set_alert(bool stranger, bool waving) { s_stranger = stranger; s_waving = waving; }
bool door_ctrl_alert_stranger(void)                   { return s_stranger; }
bool door_ctrl_alert_waving(void)                     { return s_waving; }
