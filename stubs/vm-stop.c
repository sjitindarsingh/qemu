#include "qemu-common.h"
#include "sysemu/sysemu.h"

void __vm_stop(RunState state, bool silent)
{
    abort();
}
