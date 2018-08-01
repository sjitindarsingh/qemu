/*
 * QEMU PowerPC pSeries Logical Partition capabilities handling
 *
 * Copyright (c) 2017 David Gibson, Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

#include "hw/ppc/spapr.h"
#include "kvm_ppc.h"

typedef struct sPAPRCapPossible {
    int num;            /* size of vals array below */
    const char *help;   /* help text for vals */
    /*
     * Note:
     * - because of the way compatibility is determined vals MUST be ordered
     *   such that later options are a superset of all preceding options.
     * - the order of vals must be preserved, that is their index is important,
     *   however vals may be added to the end of the list so long as the above
     *   point is observed
     */
    const char *vals[];
} sPAPRCapPossible;

typedef struct sPAPRCapabilityInfo {
    const char *name;
    const char *description;
    int index;

    /* Getter and Setter Function Pointers */
    ObjectPropertyAccessor *get;
    ObjectPropertyAccessor *set;
    const char *type;
    /* Possible values if this is a custom string type */
    sPAPRCapPossible *possible;
    /* Make sure the virtual hardware can support this capability */
    void (*apply)(sPAPRMachineState *spapr, uint8_t val, Error **errp);
} sPAPRCapabilityInfo;

static void ATTRIBUTE_UNUSED spapr_cap_get_bool(Object *obj, Visitor *v,
                                                void *opaque, const char *name,
                                                Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    bool value = spapr_get_cap(spapr, cap->index) == SPAPR_CAP_ON;

    visit_type_bool(v, &value, name, errp);
}

static void ATTRIBUTE_UNUSED spapr_cap_set_bool(Object *obj, Visitor *v,
                                                void *opaque, const char *name,
                                                Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    bool value;
    Error *local_err = NULL;

    visit_type_bool(v, &value, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    spapr->cmd_line_caps[cap->index] = true;
    spapr->eff.caps[cap->index] = value ? SPAPR_CAP_ON : SPAPR_CAP_OFF;
}

static void spapr_cap_get_tristate(Object *obj, Visitor *v, void *opaque,
                                   const char *name, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    char *val = NULL;
    uint8_t value = spapr_get_cap(spapr, cap->index);

    switch (value) {
    case SPAPR_CAP_BROKEN:
        val = g_strdup("broken");
        break;
    case SPAPR_CAP_WORKAROUND:
        val = g_strdup("workaround");
        break;
    case SPAPR_CAP_FIXED:
        val = g_strdup("fixed");
        break;
    default:
        error_setg(errp, "Invalid value (%d) for cap-%s", value, cap->name);
        return;
    }

    visit_type_str(v, &val, name, errp);
    g_free(val);
}

static void spapr_cap_set_tristate(Object *obj, Visitor *v, void *opaque,
                                   const char *name, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    char *val;
    Error *local_err = NULL;
    uint8_t value;

    visit_type_str(v, &val, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (!strcasecmp(val, "broken")) {
        value = SPAPR_CAP_BROKEN;
    } else if (!strcasecmp(val, "workaround")) {
        value = SPAPR_CAP_WORKAROUND;
    } else if (!strcasecmp(val, "fixed")) {
        value = SPAPR_CAP_FIXED;
    } else {
        error_setg(errp, "Invalid capability mode \"%s\" for cap-%s", val,
                   cap->name);
        goto out;
    }

    spapr->cmd_line_caps[cap->index] = true;
    spapr->eff.caps[cap->index] = value;
out:
    g_free(val);
}

static void spapr_cap_get_string(Object *obj, Visitor *v, void *opaque,
                                 const char *name, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    char *val = NULL;
    uint8_t value = spapr_get_cap(spapr, cap->index);

    if (value >= cap->possible->num) {
        error_setg(errp, "Invalid value (%d) for cap-%s", value, cap->name);
        return;
    }

    val = g_strdup(cap->possible->vals[value]);

    visit_type_str(v, &val, name, errp);
    g_free(val);
}

static void spapr_cap_set_string(Object *obj, Visitor *v, void *opaque,
                                 const char *name, Error **errp)
{
    sPAPRCapabilityInfo *cap = opaque;
    sPAPRMachineState *spapr = SPAPR_MACHINE(obj);
    Error *local_err = NULL;
    uint8_t i;
    char *val;

    visit_type_str(v, &val, name, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    if (!strcmp(val, "?")) {
        error_setg(errp, "%s", cap->possible->help);
        goto out;
    }
    for (i = 0; i < cap->possible->num; i++) {
        if (!strcasecmp(val, cap->possible->vals[i])) {
            spapr->cmd_line_caps[cap->index] = true;
            spapr->eff.caps[cap->index] = i;
            goto out;
        }
    }

    error_setg(errp, "Invalid capability mode \"%s\" for cap-%s", val,
               cap->name);
out:
    g_free(val);
}

sPAPRCapPossible cap_cfpc_possible = {
    .num = 3,
    .vals = {"broken", "workaround", "fixed"},
    .help = "broken - no protection, workaround - workaround available,"
            " fixed - fixed in hardware",
};

static void cap_safe_cache_apply(sPAPRMachineState *spapr, uint8_t val,
                                 Error **errp)
{
    uint8_t kvm_val =  kvmppc_get_cap_safe_cache();

    if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp,
"Requested safe cache capability level not supported by tcg, try a different value for cap-cfpc");
    } else if (kvm_enabled() && (val > kvm_val)) {
        error_setg(errp,
"Requested safe cache capability level not supported by kvm, try cap-cfpc=%s",
                   cap_cfpc_possible.vals[kvm_val]);
    }
}

static void cap_safe_bounds_check_apply(sPAPRMachineState *spapr, uint8_t val,
                                        Error **errp)
{
    if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp, "Requested safe bounds check capability level not supported by tcg, try a different value for cap-sbbc");
    } else if (kvm_enabled() && (val > kvmppc_get_cap_safe_bounds_check())) {
        error_setg(errp, "Requested safe bounds check capability level not supported by kvm, try a different value for cap-sbbc");
    }
}

static void cap_safe_indirect_branch_apply(sPAPRMachineState *spapr,
                                           uint8_t val, Error **errp)
{
    if (val == SPAPR_CAP_WORKAROUND) { /* Can only be Broken or Fixed */
        error_setg(errp, "Requested safe indirect branch capability level \"workaround\" not valid, try cap-ibs=fixed");
    } else if (tcg_enabled() && val) {
        /* TODO - for now only allow broken for TCG */
        error_setg(errp, "Requested safe indirect branch capability level not supported by tcg, try a different value for cap-ibs");
    } else if (kvm_enabled() && (val > kvmppc_get_cap_safe_indirect_branch())) {
        error_setg(errp, "Requested safe indirect branch capability level not supported by kvm, try a different value for cap-ibs");
    }
}

#define VALUE_DESC_TRISTATE     " (broken, workaround, fixed)"

sPAPRCapabilityInfo capability_table[SPAPR_CAP_NUM] = {
    [SPAPR_CAP_CFPC] = {
        .name = "cfpc",
        .description = "Cache Flush on Privilege Change" VALUE_DESC_TRISTATE,
        .index = SPAPR_CAP_CFPC,
        .get = spapr_cap_get_string,
        .set = spapr_cap_set_string,
        .type = "string",
        .possible = &cap_cfpc_possible,
        .apply = cap_safe_cache_apply,
    },
    [SPAPR_CAP_SBBC] = {
        .name = "sbbc",
        .description = "Speculation Barrier Bounds Checking" VALUE_DESC_TRISTATE,
        .index = SPAPR_CAP_SBBC,
        .get = spapr_cap_get_tristate,
        .set = spapr_cap_set_tristate,
        .type = "string",
        .apply = cap_safe_bounds_check_apply,
    },
    [SPAPR_CAP_IBS] = {
        .name = "ibs",
        .description = "Indirect Branch Serialisation (broken, fixed)",
        .index = SPAPR_CAP_IBS,
        .get = spapr_cap_get_tristate,
        .set = spapr_cap_set_tristate,
        .type = "string",
        .apply = cap_safe_indirect_branch_apply,
    },
};

static sPAPRCapabilities default_caps_with_cpu(sPAPRMachineState *spapr,
                                               CPUState *cs)
{
    sPAPRMachineClass *smc = SPAPR_MACHINE_GET_CLASS(spapr);
    sPAPRCapabilities caps;

    caps = smc->default_caps;

    return caps;
}

int spapr_caps_pre_load(void *opaque)
{
    sPAPRMachineState *spapr = opaque;

    /* Set to default so we can tell if this came in with the migration */
    spapr->mig = spapr->def;
    return 0;
}

void spapr_caps_pre_save(void *opaque)
{
    sPAPRMachineState *spapr = opaque;

    spapr->mig = spapr->eff;
}

/* This has to be called from the top-level spapr post_load, not the
 * caps specific one.  Otherwise it wouldn't be called when the source
 * caps are all defaults, which could still conflict with overridden
 * caps on the destination */
int spapr_caps_post_migration(sPAPRMachineState *spapr)
{
    int i;
    bool ok = true;
    sPAPRCapabilities dstcaps = spapr->eff;
    sPAPRCapabilities srccaps;

    srccaps = default_caps_with_cpu(spapr, first_cpu);
    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        /* If not default value then assume came in with the migration */
        if (spapr->mig.caps[i] != spapr->def.caps[i]) {
            srccaps.caps[i] = spapr->mig.caps[i];
        }
    }

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        sPAPRCapabilityInfo *info = &capability_table[i];

        if (srccaps.caps[i] > dstcaps.caps[i]) {
            error_report("cap-%s higher level (%d) in incoming stream than on destination (%d)",
                         info->name, srccaps.caps[i], dstcaps.caps[i]);
            ok = false;
        }

        if (srccaps.caps[i] < dstcaps.caps[i]) {
            fprintf(stderr, "info: cap-%s lower level (%d) in incoming stream than on destination (%d)",
                    info->name, srccaps.caps[i], dstcaps.caps[i]);
        }
    }

    return ok ? 0 : -EINVAL;
}

/* Used to generate the migration field and needed function for a spapr cap */
#define SPAPR_CAP_MIG_STATE(sname, cap)                 \
static bool spapr_cap_##sname##_needed(void *opaque)    \
{                                                       \
    sPAPRMachineState *spapr = opaque;                  \
                                                        \
    return spapr->cmd_line_caps[cap] &&                 \
           (spapr->eff.caps[cap] !=                     \
            spapr->def.caps[cap]);                      \
}                                                       \
                                                        \
const VMStateDescription vmstate_spapr_cap_##sname = {  \
    .name = "spapr/cap/" #sname,                        \
    .version_id = 1,                                    \
    .minimum_version_id = 1,                            \
    .needed = spapr_cap_##sname##_needed,               \
    .fields = (VMStateField[]) {                        \
        VMSTATE_UINT8(mig.caps[cap],                    \
                      sPAPRMachineState),               \
        VMSTATE_END_OF_LIST()                           \
    },                                                  \
}

SPAPR_CAP_MIG_STATE(cfpc, SPAPR_CAP_CFPC);
SPAPR_CAP_MIG_STATE(sbbc, SPAPR_CAP_SBBC);
SPAPR_CAP_MIG_STATE(ibs, SPAPR_CAP_IBS);

void spapr_caps_reset(sPAPRMachineState *spapr)
{
    sPAPRCapabilities default_caps;
    int i;

    /* First compute the actual set of caps we're running with.. */
    default_caps = default_caps_with_cpu(spapr, first_cpu);

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        /* Store the defaults */
        spapr->def.caps[i] = default_caps.caps[i];
        /* If not set on the command line then apply the default value */
        if (!spapr->cmd_line_caps[i]) {
            spapr->eff.caps[i] = default_caps.caps[i];
        }
    }

    /* .. then apply those caps to the virtual hardware */

    for (i = 0; i < SPAPR_CAP_NUM; i++) {
        sPAPRCapabilityInfo *info = &capability_table[i];

        /*
         * If the apply function can't set the desired level and thinks it's
         * fatal, it should cause that.
         */
        info->apply(spapr, spapr->eff.caps[i], &error_fatal);
    }
}

void spapr_caps_add_properties(Object *obj, Error **errp)
{
    Error *local_err = NULL;
    int i;

    for (i = 0; i < ARRAY_SIZE(capability_table); i++) {
        sPAPRCapabilityInfo *cap = &capability_table[i];
        const char *name = g_strdup_printf("cap-%s", cap->name);
        char *desc;

        object_property_add(obj, name, cap->type,
                            cap->get, cap->set, NULL,
                            cap, &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }

        desc = g_strdup_printf("%s", cap->description);
        object_property_set_description(obj, name, desc, &local_err);
        g_free(desc);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }
}
