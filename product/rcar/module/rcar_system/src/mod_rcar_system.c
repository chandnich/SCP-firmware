/*
 * Renesas SCP/MCP Software
 * Copyright (c) 2020-2021, Renesas Electronics Corporation. All rights
 * reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <mmio.h>
#include <rcar_mmap.h>
#include <rcar_pwc.h>
#include <rcar_scmi.h>

#include <mod_clock.h>
#include <mod_rcar_clock.h>
#include <mod_rcar_reg_sensor.h>
#include <mod_rcar_scif.h>
#include <mod_rcar_system.h>
#include <mod_scmi.h>
#include <mod_system_power.h>

#include <fwk_assert.h>
#include <fwk_element.h>
#include <fwk_id.h>
#include <fwk_interrupt.h>
#include <fwk_macros.h>
#include <fwk_mm.h>
#include <fwk_module.h>
#include <fwk_module_idx.h>
#include <fwk_notification.h>
#include <fwk_status.h>
#include <fwk_thread.h>

#include <arch_gic.h>
#include <arch_helpers.h>
#include <arch_system.h>

/* Device context */
struct rcar_system_dev_ctx {
    const struct mod_rcar_system_dev_config *config;
    struct mod_rcar_clock_drv_api *api;
};

/* Module context */
struct rcar_system_ctx {
    struct rcar_system_dev_ctx *dev_ctx_table;
    unsigned int dev_count;
};

static struct rcar_system_ctx module_ctx;

/*-----------------------------------------------------------*/
#define P_STATUS (_shutdown_request)

/* SCMI services required to enable the messaging stack */
static unsigned int scmi_notification_table[] = {
    RCAR_SCMI_SERVICE_IDX_PSCI,
    RCAR_SCMI_SERVICE_IDX_OSPM_0,
};

IMPORT_SYM(unsigned long, __system_ram_start__, SYSTEM_RAM_START);
IMPORT_SYM(unsigned long, __system_ram_end__, SYSTEM_RAM_END);
IMPORT_SYM(unsigned long, __sram_copy_start__, SRAM_COPY_START);

/*
 * Static helpers
 */

static int messaging_stack_ready(void)
{
    return 0;
}

bool is_available_shutdown_req(uint32_t req)
{
    bool ret;

    switch (req) {
    case R_WARMBOOT:
    case R_SUSPEND:
    case R_RESET:
    case R_OFF:
        ret = true;
        break;
    default:
        ret = false;
        break;
    }
    return ret;
}

void rcar_system_code_copy_to_system_ram(void)
{
    memcpy(
        (void *)SCP_SRAM_BASE,
        (void *)SRAM_COPY_START,
        (SYSTEM_RAM_END - SYSTEM_RAM_START));
}

void vApplicationIdleHook(void)
{
    uint32_t req;
    struct rcar_system_dev_ctx *ctx;
    fwk_id_t element_id = FWK_ID_ELEMENT_INIT(FWK_MODULE_IDX_CLOCK, 0);
    uint32_t i;

    if (is_available_shutdown_req(P_STATUS)) {
        fwk_interrupt_global_disable();
        req = P_STATUS;
        P_STATUS = R_CLEAR;
        switch (req) {
        case R_SUSPEND:
            _boot_flag = R_WARMBOOT;
            while (!(mmio_read_32(RCAR_CA57PSTR) & 0x0f))
                continue;

            _save_system();

            rcar_system_code_copy_to_system_ram();

            for (i = 0; i < module_ctx.dev_count; i++) {
                element_id.element.element_idx = i;
                ctx = module_ctx.dev_ctx_table +
                    fwk_id_get_element_idx(element_id);
                if (ctx->api->resume)
                    ctx->api->resume();
            }

            reg_sensor_resume();
            mod_rcar_scif_resume();
            gic_init();
            vConfigureTickInterrupt();
            fwk_interrupt_global_enable();
            break;
        case R_RESET:
            rcar_system_reset();
            break;
        case R_OFF:
            rcar_system_off();
            break;
        default:
            break;
        }
    }
}

/*
 * Functions fulfilling the framework's module interface
 */

static int rcar_system_shutdown(enum mod_pd_system_shutdown system_shutdown)
{
    switch (system_shutdown) {
    case MOD_PD_SYSTEM_SHUTDOWN:
        _shutdown_request = R_OFF;
        break;
    case MOD_PD_SYSTEM_COLD_RESET:
        _shutdown_request = R_RESET;
        break;
    default:
        break;
    }

    return FWK_SUCCESS;
}

static const struct mod_system_power_driver_api
    rcar_system_system_power_driver_api = { .system_shutdown =
                                                rcar_system_shutdown };

/*
 * Functions fulfilling the framework's module interface
 */

static int rcar_system_init(
    fwk_id_t module_id,
    unsigned int element_count,
    const void *data)
{
    module_ctx.dev_count = element_count;

    if (element_count == 0)
        return FWK_SUCCESS;

    module_ctx.dev_ctx_table =
        fwk_mm_calloc(element_count, sizeof(struct rcar_system_dev_ctx));
    if (module_ctx.dev_ctx_table == NULL)
        return FWK_E_NOMEM;

    return FWK_SUCCESS;
}
static int rcar_system_element(
    fwk_id_t element_id,
    unsigned int sub_element_count,
    const void *data)
{
    struct rcar_system_dev_ctx *ctx;
    const struct mod_rcar_system_dev_config *dev_config = data;

    ctx = module_ctx.dev_ctx_table + fwk_id_get_element_idx(element_id);
    ctx->config = dev_config;

    return FWK_SUCCESS;
}

static int rcar_system_bind(fwk_id_t id, unsigned int round)
{
    struct rcar_system_dev_ctx *ctx;

    if (round == 1)
        return FWK_SUCCESS;

    if (!fwk_id_is_type(id, FWK_ID_TYPE_ELEMENT))
        /* Only element binding is supported */
        return FWK_SUCCESS;

    ctx = module_ctx.dev_ctx_table + fwk_id_get_element_idx(id);

    return fwk_module_bind(
        ctx->config->driver_id, ctx->config->api_id, &ctx->api);
}

static int rcar_system_process_bind_request(
    fwk_id_t source_id,
    fwk_id_t target_id,
    fwk_id_t api_id,
    const void **api)
{
    *api = &rcar_system_system_power_driver_api;

    return FWK_SUCCESS;
}

static int rcar_system_start(fwk_id_t id)
{
    int status;
    unsigned int i;

    /*
     * Subscribe to these SCMI channels in order to know when they have all
     * initialized.
     * At that point we can consider the SCMI stack to be initialized from
     * the point of view of the PSCI agent.
     */
    if (fwk_id_get_type(id) != FWK_ID_TYPE_MODULE)
        return FWK_SUCCESS;

    for (i = 0; i < FWK_ARRAY_SIZE(scmi_notification_table); i++) {
        status = fwk_notification_subscribe(
            mod_scmi_notification_id_initialized,
            fwk_id_build_element_id(
                fwk_module_id_scmi, scmi_notification_table[i]),
            id);
        if (status != FWK_SUCCESS)
            return status;
    }

    return FWK_SUCCESS;
}

static int rcar_system_process_notification(
    const struct fwk_event *event,
    struct fwk_event *resp_event)
{
    static unsigned int scmi_notification_count = 0;

    if (!fwk_expect(fwk_id_is_type(event->target_id, FWK_ID_TYPE_MODULE)))
        return FWK_E_PARAM;

    if (fwk_id_is_equal(event->id, mod_scmi_notification_id_initialized))
        scmi_notification_count++;

    if (scmi_notification_count == FWK_ARRAY_SIZE(scmi_notification_table)) {
        messaging_stack_ready();

        scmi_notification_count = 0;
    }

    return FWK_SUCCESS;
}

const struct fwk_module module_rcar_system = {
    .name = "RCAR_SYSTEM",
    .api_count = MOD_RCAR_SYSTEM_API_COUNT,
    .type = FWK_MODULE_TYPE_DRIVER,
    .init = rcar_system_init,
    .element_init = rcar_system_element,
    .bind = rcar_system_bind,
    .start = rcar_system_start,
    .process_bind_request = rcar_system_process_bind_request,
    .process_notification = rcar_system_process_notification,
};
