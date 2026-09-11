// The PS5 version of src/pc/controller/controller_entry_point.c: only the
// DualSense (ps5/controller_ps5.c), instead of the port's SDL, XInput,
// keyboard and WUP list.

#include "macros.h"
#include "lib/src/libultra_internal.h"
#include "lib/src/osContInternal.h"
#include "controller_api.h"

extern struct ControllerAPI controller_ps5;

s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status) {
    controller_ps5.init();
    *controllerBits = 1;
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *mesg) {
    return 0;
}

void osContGetReadData(OSContPad *pad) {
    controller_ps5.read(pad);
}
