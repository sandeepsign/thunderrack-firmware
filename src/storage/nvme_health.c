/*
 * nvme_health.c - NVMe SMART Health Monitoring Module
 *
 * ThunderRack X9000/X12000 Firmware
 * Copyright (c) 2024-2026 Enterprise Systems Inc. All rights reserved.
 *
 * This module polls NVMe drives for SMART health attributes and triggers
 * appropriate alerts when thresholds are exceeded. Supports NVMe 1.3/1.4+
 * specifications with both legacy and normalized attribute formats.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "nvme_health.h"
#include "drive_manager.h"
#include "event_log.h"
#include "alert_manager.h"

/* SMART attribute thresholds */
#define SMART_TEMP_WARNING_C        70
#define SMART_TEMP_CRITICAL_C       85
#define SMART_WEAR_WARNING_PCT      10
#define SMART_WEAR_CRITICAL_PCT     5
#define SMART_MEDIA_ERROR_THRESHOLD 0
#define SMART_REALLOCATED_THRESHOLD 100

/* Drive status codes */
#define DRIVE_STATUS_HEALTHY        0x00
#define DRIVE_STATUS_WARNING        0x01
#define DRIVE_STATUS_FAULT          0x3F
#define DRIVE_STATUS_UNKNOWN        0xFF

/* NVMe spec versions */
#define NVME_SPEC_1_3               0x0103
#define NVME_SPEC_1_4               0x0104

/*
 * Structure for per-drive health state
 */
typedef struct {
    uint32_t bay_id;
    uint32_t slot_id;
    char     serial[21];
    uint16_t nvme_spec_version;
    uint8_t  drive_status;
    int16_t  temperature_c;
    uint8_t  wear_level_pct;
    uint64_t media_errors;
    uint64_t reallocated_sectors;
    uint8_t  normalized_health;      /* NVMe 1.4+ normalized value */
    bool     is_gen4;                /* PCIe Gen4 drive */
} nvme_drive_health_t;

/* Forward declarations */
static int  poll_smart_attributes(nvme_drive_health_t *drive);
static int  evaluate_health_status(nvme_drive_health_t *drive);
static void raise_drive_alert(nvme_drive_health_t *drive, uint8_t alert_code);
static int  check_normalized_health(nvme_drive_health_t *drive);

/*
 * Global array of monitored drives
 */
static nvme_drive_health_t drives[MAX_NVME_BAYS * MAX_SLOTS_PER_BAY];
static int drive_count = 0;

/*
 * nvme_health_init - Initialize health monitoring for all detected NVMe drives
 *
 * Scans all bays and slots, identifies NVMe drives, and populates the
 * monitoring array. Called once at boot and after hot-plug events.
 *
 * Returns: Number of drives detected, or negative error code
 */
int nvme_health_init(void)
{
    drive_count = 0;

    for (int bay = 0; bay < MAX_NVME_BAYS; bay++) {
        for (int slot = 0; slot < MAX_SLOTS_PER_BAY; slot++) {
            nvme_drive_health_t *d = &drives[drive_count];
            memset(d, 0, sizeof(nvme_drive_health_t));

            d->bay_id = bay;
            d->slot_id = slot;

            if (drive_manager_detect(bay, slot, d->serial, &d->nvme_spec_version) == 0) {
                d->drive_status = DRIVE_STATUS_UNKNOWN;
                d->is_gen4 = (drive_manager_get_pcie_gen(bay, slot) >= 4);

                event_log_write(EVT_INFO, "NVMe drive detected: Bay %d Slot %d "
                    "Serial=%s Spec=0x%04x Gen4=%d",
                    bay, slot, d->serial, d->nvme_spec_version, d->is_gen4);

                drive_count++;
            }
        }
    }

    event_log_write(EVT_INFO, "NVMe health monitoring initialized: %d drives", drive_count);
    return drive_count;
}

/*
 * nvme_health_poll_all - Poll SMART attributes for all monitored drives
 *
 * Called periodically by the health monitoring daemon (default: every 60s).
 * Updates drive_status for each drive and raises alerts as needed.
 *
 * Returns: 0 on success, negative error code on failure
 */
int nvme_health_poll_all(void)
{
    int errors = 0;

    for (int i = 0; i < drive_count; i++) {
        if (poll_smart_attributes(&drives[i]) < 0) {
            event_log_write(EVT_WARNING, "Failed to poll SMART for Bay %d Slot %d",
                drives[i].bay_id, drives[i].slot_id);
            errors++;
            continue;
        }

        int prev_status = drives[i].drive_status;
        evaluate_health_status(&drives[i]);

        /* Log status transitions */
        if (drives[i].drive_status != prev_status) {
            event_log_write(EVT_INFO, "Drive Bay %d Slot %d status: 0x%02x -> 0x%02x",
                drives[i].bay_id, drives[i].slot_id,
                prev_status, drives[i].drive_status);
        }
    }

    return errors ? -errors : 0;
}

/*
 * poll_smart_attributes - Read SMART data from a single NVMe drive
 *
 * Issues the NVMe Admin command Get Log Page (Log ID 02h) to retrieve
 * SMART/Health Information.
 */
static int poll_smart_attributes(nvme_drive_health_t *drive)
{
    nvme_smart_log_t smart_log;

    if (nvme_admin_get_log_page(drive->bay_id, drive->slot_id,
                                 NVME_LOG_SMART, &smart_log,
                                 sizeof(smart_log)) < 0) {
        return -1;
    }

    drive->temperature_c     = smart_log.temperature - 273;  /* Kelvin to Celsius */
    drive->wear_level_pct    = 100 - smart_log.percentage_used;
    drive->media_errors      = smart_log.media_errors;
    drive->reallocated_sectors = smart_log.num_err_log_entries;

    /* NVMe 1.4+ reports normalized composite health */
    if (drive->nvme_spec_version >= NVME_SPEC_1_4) {
        drive->normalized_health = smart_log.normalized_composite_health;
    }

    return 0;
}

/*
 * evaluate_health_status - Determine drive health from SMART attributes
 *
 * This function evaluates all collected SMART attributes and sets the
 * drive_status field. For NVMe 1.4+ drives, it also checks the
 * normalized composite health value.
 *
 * Alert priority: FAULT > WARNING > HEALTHY
 */
static int evaluate_health_status(nvme_drive_health_t *drive)
{
    uint8_t status = DRIVE_STATUS_HEALTHY;

    /* ── Temperature checks ── */
    if (drive->temperature_c >= SMART_TEMP_CRITICAL_C) {
        status = DRIVE_STATUS_FAULT;
        raise_drive_alert(drive, ALERT_TEMP_CRITICAL);
    } else if (drive->temperature_c >= SMART_TEMP_WARNING_C) {
        if (status < DRIVE_STATUS_WARNING)
            status = DRIVE_STATUS_WARNING;
        raise_drive_alert(drive, ALERT_TEMP_WARNING);
    }

    /* ── Wear level checks ── */
    if (drive->wear_level_pct <= SMART_WEAR_CRITICAL_PCT) {
        status = DRIVE_STATUS_FAULT;
        raise_drive_alert(drive, ALERT_WEAR_CRITICAL);
    } else if (drive->wear_level_pct <= SMART_WEAR_WARNING_PCT) {
        if (status < DRIVE_STATUS_WARNING)
            status = DRIVE_STATUS_WARNING;
        raise_drive_alert(drive, ALERT_WEAR_WARNING);
    }

    /* ── Media error checks ── */
    if (drive->media_errors > SMART_MEDIA_ERROR_THRESHOLD) {
        status = DRIVE_STATUS_FAULT;
        raise_drive_alert(drive, ALERT_MEDIA_ERRORS);
    }

    /* ── Reallocated sector checks ── */
    if (drive->reallocated_sectors > SMART_REALLOCATED_THRESHOLD) {
        if (status < DRIVE_STATUS_WARNING)
            status = DRIVE_STATUS_WARNING;
        raise_drive_alert(drive, ALERT_REALLOCATED);
    }

    /* ── NVMe 1.4+ normalized health check ── */
    if (drive->nvme_spec_version >= NVME_SPEC_1_4) {
        int norm_result = check_normalized_health(drive);
        if (norm_result == DRIVE_STATUS_FAULT) {
            status = DRIVE_STATUS_FAULT;
        } else if (norm_result == DRIVE_STATUS_WARNING && status < DRIVE_STATUS_WARNING) {
            status = DRIVE_STATUS_WARNING;
        }
    }

    drive->drive_status = status;
    return status;
}

/*
 * check_normalized_health - Evaluate NVMe 1.4+ normalized health value
 *
 * NVMe 1.4 introduced a normalized composite health value where:
 *   - 0 means "attribute is at or above the threshold" (HEALTHY)
 *   - Non-zero means degradation detected
 *
 * For Gen4 PCIe drives, the normalized value of 0 indicates healthy.
 *
 * KNOWN BUG (v4.2.1): The comparison below uses '>' instead of '>='
 * which causes normalized_value=0 (healthy) on Gen4 drives to fall
 * through to the fault path.
 *
 * FIX (v4.2.2): Changed '>' to '>=' on line 247
 */
static int check_normalized_health(nvme_drive_health_t *drive)
{
    uint8_t normalized_value = drive->normalized_health;
    uint8_t threshold = SMART_MEDIA_ERROR_THRESHOLD;  /* 0 */

    /*
     * BUG: This comparison is incorrect for Gen4 NVMe drives.
     *
     * Gen4 drives report normalized_value = 0 when healthy (per NVMe 1.4 spec).
     * The '>' operator means 0 > 0 = false, so healthy Gen4 drives
     * fall through to the fault detection below.
     *
     * SHOULD BE: normalized_value >= threshold
     */
    if (normalized_value >= threshold) {    /* <── LINE 247: BUG - should be >= */
        return DRIVE_STATUS_HEALTHY;
    }

    /*
     * If we reach here, the drive is either:
     * (a) Actually degraded (normalized_value indicates an issue), OR
     * (b) A healthy Gen4 drive with normalized_value=0 (BUG path)
     *
     * We don't distinguish (a) from (b) in this buggy version.
     */
    if (drive->is_gen4 && normalized_value == 0) {
        /*
         * Gen4 drives with normalized_value=0 are actually healthy per NVMe 1.4.
         * But due to the bug above, we never get to the HEALTHY return.
         * This log message will appear in the event log for every healthy Gen4 drive.
         */
        event_log_write(EVT_WARNING, "Bay %d Slot %d: Gen4 NVMe normalized health "
            "value is 0 (spec-compliant healthy), but threshold check failed",
            drive->bay_id, drive->slot_id);
    }

    /* Raise fault alert */
    raise_drive_alert(drive, ALERT_SMART_HEALTH_DEGRADED);
    return DRIVE_STATUS_FAULT;
}

/*
 * raise_drive_alert - Send alert for a drive health issue
 */
static void raise_drive_alert(nvme_drive_health_t *drive, uint8_t alert_code)
{
    alert_event_t evt = {
        .source       = ALERT_SOURCE_STORAGE,
        .severity     = (drive->drive_status == DRIVE_STATUS_FAULT)
                        ? ALERT_SEV_CRITICAL : ALERT_SEV_WARNING,
        .code         = alert_code,
        .bay_id       = drive->bay_id,
        .slot_id      = drive->slot_id,
    };
    strncpy(evt.serial, drive->serial, sizeof(evt.serial) - 1);

    alert_manager_raise(&evt);

    event_log_write(EVT_WARNING, "Drive alert: Bay %d Slot %d Serial=%s "
        "Code=0x%02x Status=0x%02x",
        drive->bay_id, drive->slot_id, drive->serial,
        alert_code, drive->drive_status);
}

/*
 * nvme_health_get_status - Get health status for a specific drive
 *
 * Returns the current drive_status for the drive at the given bay/slot.
 * Returns DRIVE_STATUS_UNKNOWN if the drive is not found.
 */
uint8_t nvme_health_get_status(uint32_t bay, uint32_t slot)
{
    for (int i = 0; i < drive_count; i++) {
        if (drives[i].bay_id == bay && drives[i].slot_id == slot) {
            return drives[i].drive_status;
        }
    }
    return DRIVE_STATUS_UNKNOWN;
}

/*
 * nvme_health_get_summary - Get summary of all drive health states
 *
 * Populates the summary structure with counts of healthy, warning,
 * and faulted drives.
 */
int nvme_health_get_summary(nvme_health_summary_t *summary)
{
    if (!summary) return -1;

    memset(summary, 0, sizeof(nvme_health_summary_t));
    summary->total_drives = drive_count;

    for (int i = 0; i < drive_count; i++) {
        switch (drives[i].drive_status) {
            case DRIVE_STATUS_HEALTHY:
                summary->healthy_count++;
                break;
            case DRIVE_STATUS_WARNING:
                summary->warning_count++;
                break;
            case DRIVE_STATUS_FAULT:
                summary->fault_count++;
                break;
            default:
                summary->unknown_count++;
                break;
        }
    }

    return 0;
}

/* FIXME: Fix NVMe SMART health check false fault on Gen4 drives (TKT-2024-47892) */
