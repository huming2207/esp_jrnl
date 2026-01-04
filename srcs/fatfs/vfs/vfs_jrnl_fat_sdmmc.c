/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_vfs_jrnl_fat.h"
#include "diskio_impl.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#include "../diskio/diskio_jrnl.h"
#include "private_include/esp_vfs_jrnl_fat_private.h"
#include "vfs_fat_internal.h"

static const char* TAG = "vfs_jrnl_fat_sdmmc";

static esp_err_t jrnl_sdmmc_read(int32_t handle, size_t src_addr, void *dest, size_t size)
{
    sdmmc_card_t* card = (sdmmc_card_t*)handle;
    size_t sector_size = card->csd.sector_size;
    if (src_addr % sector_size != 0 || size % sector_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdmmc_read_sectors(card, dest, src_addr / sector_size, size / sector_size);
}

static esp_err_t jrnl_sdmmc_write(int32_t handle, size_t dest_addr, const void *src, size_t size)
{
    sdmmc_card_t* card = (sdmmc_card_t*)handle;
    size_t sector_size = card->csd.sector_size;
    if (dest_addr % sector_size != 0 || size % sector_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdmmc_write_sectors(card, src, dest_addr / sector_size, size / sector_size);
}

static esp_err_t jrnl_sdmmc_erase(int32_t handle, size_t start_addr, size_t size)
{
    sdmmc_card_t* card = (sdmmc_card_t*)handle;
    size_t sector_size = card->csd.sector_size;
    if (start_addr % sector_size != 0 || size % sector_size != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return sdmmc_erase_sectors(card, start_addr / sector_size, size / sector_size, SDMMC_ERASE_ARG);
}

esp_err_t esp_vfs_fat_sdmmc_mount_jrnl(const char* base_path,
                                       const sdmmc_host_t* host_config,
                                       const void* slot_config,
                                       const esp_vfs_fat_mount_config_t* mount_config,
                                       sdmmc_card_t** out_card,
                                       const esp_jrnl_config_t* jrnl_config,
                                       esp_jrnl_handle_t* jrnl_handle)
{
    esp_err_t err = ESP_OK;
    sdmmc_card_t* card = NULL;
    esp_jrnl_handle_t jrnl_handle_temp = JRNL_INVALID_HANDLE;
    BYTE pdrv = 0xFF;

    if (base_path == NULL || host_config == NULL || mount_config == NULL || 
        out_card == NULL || jrnl_config == NULL || jrnl_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    card = (sdmmc_card_t*)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Copy host configuration to the card structure
    memcpy(&card->host, host_config, sizeof(sdmmc_host_t));

    if (card->host.init) {
        err = card->host.init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "host init failed (0x%x)", err);
            goto fail;
        }
    }

    if (card->host.flags & SDMMC_HOST_FLAG_SPI) {
        err = sdspi_host_init_device((const sdspi_device_config_t*)slot_config, &card->host.slot);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sdspi_host_init_device failed (0x%x)", err);
            goto fail;
        }
    } else {
#ifdef SOC_SDMMC_HOST_SUPPORTED
        err = sdmmc_host_init_slot(card->host.slot, (const sdmmc_slot_config_t*)slot_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "sdmmc_host_init_slot failed (0x%x)", err);
            goto fail;
        }
#else
        ESP_LOGE(TAG, "SDMMC Host isn't supported!");
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
#endif
    }

    err = sdmmc_card_init(&card->host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed (0x%x)", err);
        goto fail;
    }
    
    *out_card = card;

    esp_jrnl_diskio_t diskio_cfg = {
        .diskio_ctrl_handle = (int32_t)card,
        .disk_read = jrnl_sdmmc_read,
        .disk_write = jrnl_sdmmc_write,
        .disk_erase_range = jrnl_sdmmc_erase
    };

    esp_jrnl_volume_t volume_cfg = {
        .volume_size = (size_t)card->csd.capacity * card->csd.sector_size,
        .disk_sector_size = card->csd.sector_size
    };

    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    char drv[3] = {(char) ('0' + pdrv), ':', 0};

    esp_jrnl_config_extended_t jrnl_config_ext = {
        .user_cfg = *jrnl_config,
        .fs_volume_id = pdrv,
        .volume_cfg = volume_cfg,
        .diskio_cfg = diskio_cfg
    };

    err = esp_jrnl_mount(&jrnl_config_ext, &jrnl_handle_temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_jrnl_mount failed (0x%x)", err);
        goto fail;
    }

    err = ff_diskio_register_jrnl(pdrv, jrnl_handle_temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ff_diskio_register_jrnl failed (0x%x)", err);
        goto fail;
    }

    FATFS *fs;
    esp_vfs_fat_conf_t conf = {
        .base_path = base_path,
        .fat_drive = drv,
        .max_files = mount_config->max_files,
    };
    err = vfs_fat_register_cfg_jrnl(&conf, &fs);
    if (err != ESP_ERR_INVALID_STATE && err != ESP_OK) {
        ESP_LOGE(TAG, "vfs_fat_register failed (0x%x)", err);
        goto fail;
    }

    err = vfs_fat_register_pdrv_jrnl_handle(pdrv, jrnl_handle_temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vfs_fat_register_pdrv_jrnl_handle failed (0x%x)", err);
        goto fail;
    }

    bool need_mount_again = jrnl_config->force_fs_format;
    if (!need_mount_again) {
        FRESULT fres = f_mount(fs, drv, 1);
        if (fres != FR_OK) {
            need_mount_again = (fres == FR_NO_FILESYSTEM || fres == FR_INT_ERR) && mount_config->format_if_mount_failed;
            if (!need_mount_again) {
                ESP_LOGE(TAG, "f_mount failed (%d)", fres);
                err = ESP_FAIL;
                goto fail;
            }
        }
    }

    if (need_mount_again) {
        size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(card->csd.sector_size, mount_config->allocation_unit_size);
        const size_t workbuf_size = 4096;
        void *workbuf = ff_memalloc(workbuf_size);
        if (workbuf == NULL) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        
        const MKFS_PARM opt = {(BYTE)(FM_ANY | FM_SFD), 0, 0, 0, alloc_unit_size};
        FRESULT fres = f_mkfs(drv, &opt, workbuf, workbuf_size);
        ff_memfree(workbuf);
        if (fres != FR_OK) {
            ESP_LOGE(TAG, "f_mkfs failed (%d)", fres);
            err = ESP_FAIL;
            goto fail;
        }
        
        fres = f_mount(fs, drv, 0);
        if (fres != FR_OK) {
            ESP_LOGE(TAG, "f_mount after format failed (%d)", fres);
            err = ESP_FAIL;
            goto fail;
        }
    }

    err = esp_jrnl_set_direct_io(jrnl_handle_temp, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_jrnl_set_direct_io failed (0x%x)", err);
        goto fail;
    }

    *jrnl_handle = jrnl_handle_temp;
    return ESP_OK;

fail:
    if (jrnl_handle_temp != JRNL_INVALID_HANDLE) {
        esp_vfs_fat_sdmmc_unmount_jrnl(&jrnl_handle_temp, base_path);
    } else {
        if (card) {
            if (card->host.flags & SDMMC_HOST_FLAG_SPI) {
                sdspi_host_remove_device(card->host.slot);
            }
            if (card->host.deinit) {
                card->host.deinit();
            }
            free(card);
        }
    }
    return err;
}

esp_err_t esp_vfs_fat_sdmmc_unmount_jrnl(esp_jrnl_handle_t* jrnl_handle, const char* base_path)
{
    esp_err_t err = ESP_OK;
    if (jrnl_handle == NULL || *jrnl_handle == JRNL_INVALID_HANDLE || base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t card_handle_int;
    err = esp_jrnl_get_diskio_handle(*jrnl_handle, &card_handle_int);
    if (err != ESP_OK) {
        return err;
    }
    sdmmc_card_t* card = (sdmmc_card_t*)card_handle_int;

    vfs_fat_unregister_pdrv_jrnl_handle(*jrnl_handle);

    BYTE pdrv = ff_diskio_get_pdrv_jrnl(*jrnl_handle);
    if (pdrv != 0xff) {
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        f_mount(0, drv, 0);
        ff_diskio_clear_pdrv_jrnl(*jrnl_handle);
        ff_diskio_unregister(pdrv);
    }

    err = esp_jrnl_unmount(*jrnl_handle);
    *jrnl_handle = JRNL_INVALID_HANDLE;

    vfs_fat_unregister_path_jrnl(base_path);

    if (card) {
        if (card->host.flags & SDMMC_HOST_FLAG_SPI) {
            sdspi_host_remove_device(card->host.slot);
        }
        if (card->host.deinit) {
            card->host.deinit();
        }
        free(card);
    }

    return err;
}