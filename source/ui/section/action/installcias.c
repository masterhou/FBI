#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "action.h"
#include "../task/task.h"
#include "../../error.h"
#include "../../info.h"
#include "../../list.h"
#include "../../prompt.h"
#include "../../ui.h"
#include "../../../core/linkedlist.h"
#include "../../../core/screen.h"
#include "../../../core/util.h"

typedef struct {
    linked_list* items;
    file_info* target;

    linked_list contents;

    bool delete;

    u64 currTitleId;
    u64 startTime;

    data_op_data installInfo;
} install_cias_data;

static void action_install_cias_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_cias_data* installData = (install_cias_data*) data;

    u32 curr = installData->installInfo.processed;
    if(curr < installData->installInfo.total) {
        ui_draw_file_info(view, ((list_item*) linked_list_get(&installData->contents, curr))->data, x1, y1, x2, y2);
    } else if(installData->target != NULL) {
        ui_draw_file_info(view, installData->target, x1, y1, x2, y2);
    }
}

static Result action_install_cias_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result action_install_cias_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result action_install_cias_open_src(void* data, u32 index, u32* handle) {
    install_cias_data* installData = (install_cias_data*) data;

    file_info* info = (file_info*) ((list_item*) linked_list_get(&installData->contents, index))->data;

    Result res = 0;

    FS_Path* fsPath = util_make_path_utf8(info->path);
    if(fsPath != NULL) {
        res = FSUSER_OpenFile(handle, info->archive, *fsPath, FS_OPEN_READ, 0);

        util_free_path_utf8(fsPath);
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_install_cias_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    install_cias_data* installData = (install_cias_data*) data;

    file_info* info = (file_info*) ((list_item*) linked_list_get(&installData->contents, index))->data;

    Result res = 0;

    if(R_SUCCEEDED(res = FSFILE_Close(handle)) && installData->delete && succeeded) {
        FS_Path* fsPath = util_make_path_utf8(info->path);
        if(fsPath != NULL) {
            if(R_SUCCEEDED(FSUSER_DeleteFile(info->archive, *fsPath))) {
                installData->target->containsCias = false;
                installData->target->containsTickets = false;

                linked_list_iter iter;
                linked_list_iterate(installData->items, &iter);

                while(linked_list_iter_has_next(&iter)) {
                    list_item* item = (list_item*) linked_list_iter_next(&iter);
                    file_info* currInfo = (file_info*) item->data;

                    if(strncmp(currInfo->path, info->path, FILE_PATH_MAX) == 0) {
                        linked_list_iter_remove(&iter);
                        task_free_file(item);
                    } else if(currInfo->isCia) {
                        installData->target->containsCias = true;
                    } else if(currInfo->isTicket) {
                        installData->target->containsTickets = true;
                    }
                }
            }

            util_free_path_utf8(fsPath);
        } else {
            res = R_FBI_OUT_OF_MEMORY;
        }
    }

    return res;
}

static Result action_install_cias_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result action_install_cias_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result action_install_cias_open_dst(void* data, u32 index, void* initialReadBlock, u32* handle) {
    install_cias_data* installData = (install_cias_data*) data;

    u64 titleId = util_get_cia_title_id((u8*) initialReadBlock);

    FS_MediaType dest = ((titleId >> 32) & 0x8010) != 0 ? MEDIATYPE_NAND : MEDIATYPE_SD;

    u8 n3ds = false;
    if(R_SUCCEEDED(APT_CheckNew3DS(&n3ds)) && !n3ds && ((titleId >> 28) & 0xF) == 2) {
        return R_FBI_WRONG_SYSTEM;
    }

    // Deleting FBI before it reinstalls itself causes issues.
    if(((titleId >> 8) & 0xFFFFF) != UNIQUE_ID) {
        AM_DeleteTitle(dest, titleId);
        AM_DeleteTicket(titleId);

        if(dest == MEDIATYPE_SD) {
            AM_QueryAvailableExternalTitleDatabase(NULL);
        }
    }

    Result res = AM_StartCiaInstall(dest, handle);
    if(R_SUCCEEDED(res)) {
        installData->currTitleId = titleId;
        installData->startTime = osGetTime();
    }

    return res;
}

static Result action_install_cias_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    if(succeeded) {
        install_cias_data* installData = (install_cias_data*) data;

        Result res = 0;
        if(R_SUCCEEDED(res = AM_FinishCiaInstall(handle))) {
            util_import_seed(installData->currTitleId);

            if(installData->currTitleId == 0x0004013800000002 || installData->currTitleId == 0x0004013820000002) {
                res = AM_InstallFirm(installData->currTitleId);
            }
        }

        return res;
    } else {
        return AM_CancelCIAInstall(handle);
    }
}

static Result action_install_cias_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

bool action_install_cias_error(void* data, u32 index, Result res) {
    install_cias_data* installData = (install_cias_data*) data;

    if(res == R_FBI_CANCELLED) {
        prompt_display("Failure", "Install cancelled.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
        return false;
    } else {
        volatile bool dismissed = false;
        error_display_res(&dismissed, data, action_install_cias_draw_top, res, "Failed to install CIA file.");

        while(!dismissed) {
            svcSleepThread(1000000);
        }
    }

    return index < installData->installInfo.total - 1;
}

static void action_install_cias_free_data(install_cias_data* data) {
    task_clear_files(&data->contents);
    linked_list_destroy(&data->contents);
    free(data);
}

static void action_install_cias_update(ui_view* view, void* data, float* progress, char* text) {
    install_cias_data* installData = (install_cias_data*) data;

    if(installData->installInfo.finished) {
        if(installData->delete) {
            FSUSER_ControlArchive(installData->target->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        }

        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(installData->installInfo.result)) {
            prompt_display("Success", "Install finished.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
        }

        action_install_cias_free_data(installData);

        return;
    }

    if((hidKeysDown() & KEY_B) && !installData->installInfo.finished) {
        svcSignalEvent(installData->installInfo.cancelEvent);
    }

    *progress = installData->installInfo.currTotal != 0 ? (float) ((double) installData->installInfo.currProcessed / (double) installData->installInfo.currTotal) : 0;
    float speed = installData->installInfo.currProcessed / (osGetTime() - installData->startTime) / 1048.5f;
    snprintf(text, PROGRESS_TEXT_MAX, "%lu / %lu\n%.2f MiB / %.2f MiB\nSpeed: %.3f MiB/s", installData->installInfo.processed, installData->installInfo.total, installData->installInfo.currProcessed / 1024.0 / 1024.0, installData->installInfo.currTotal / 1024.0 / 1024.0, speed);
}

static void action_install_cias_onresponse(ui_view* view, void* data, bool response) {
    install_cias_data* installData = (install_cias_data*) data;

    if(response) {
        Result res = task_data_op(&installData->installInfo);
        if(R_SUCCEEDED(res)) {
            info_display("Installing CIA(s)", "Press B to cancel.", true, data, action_install_cias_update, action_install_cias_draw_top);
        } else {
            error_display_res(NULL, NULL, NULL, res, "Failed to initiate CIA installation.");

            action_install_cias_free_data(installData);
        }
    } else {
        action_install_cias_free_data(installData);
    }
}

static void action_install_cias_internal(linked_list* items, list_item* selected, const char* message, bool delete) {
    install_cias_data* data = (install_cias_data*) calloc(1, sizeof(install_cias_data));
    if(data == NULL) {
        error_display(NULL, NULL, NULL, "Failed to allocate install CIAs data.");

        return;
    }

    data->items = items;
    data->target = (file_info*) selected->data;

    data->delete = delete;

    data->currTitleId = 0;

    data->installInfo.data = data;

    data->installInfo.op = DATAOP_COPY;

    data->installInfo.copyEmpty = false;

    data->installInfo.isSrcDirectory = action_install_cias_is_src_directory;
    data->installInfo.makeDstDirectory = action_install_cias_make_dst_directory;

    data->installInfo.openSrc = action_install_cias_open_src;
    data->installInfo.closeSrc = action_install_cias_close_src;
    data->installInfo.getSrcSize = action_install_cias_get_src_size;
    data->installInfo.readSrc = action_install_cias_read_src;

    data->installInfo.openDst = action_install_cias_open_dst;
    data->installInfo.closeDst = action_install_cias_close_dst;
    data->installInfo.writeDst = action_install_cias_write_dst;

    data->installInfo.error = action_install_cias_error;

    data->installInfo.finished = true;

    linked_list_init(&data->contents);

    populate_files_data popData;
    popData.items = &data->contents;
    popData.base = data->target;
    popData.recursive = false;
    popData.includeBase = !data->target->isDirectory;
    popData.dirsFirst = false;

    Result listRes = task_populate_files(&popData);
    if(R_FAILED(listRes)) {
        error_display_res(NULL, NULL, NULL, listRes, "Failed to initiate CIA list population.");

        action_install_cias_free_data(data);
        return;
    }

    while(!popData.finished) {
        svcSleepThread(1000000);
    }

    if(R_FAILED(popData.result)) {
        error_display_res(NULL, NULL, NULL, popData.result, "Failed to populate CIA list.");

        action_install_cias_free_data(data);
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(&data->contents, &iter);

    while(linked_list_iter_has_next(&iter)) {
        file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;

        if(!info->isCia) {
            linked_list_iter_remove(&iter);
        }
    }

    data->installInfo.total = linked_list_size(&data->contents);
    data->installInfo.processed = data->installInfo.total;

    prompt_display("Confirmation", message, COLOR_TEXT, true, data, NULL, action_install_cias_draw_top, action_install_cias_onresponse);
}

void action_install_cia(linked_list* items, list_item* selected) {
    action_install_cias_internal(items, selected, "Install the selected CIA?", false);
}

void action_install_cia_delete(linked_list* items, list_item* selected) {
    action_install_cias_internal(items, selected, "Install and delete the selected CIA?", true);
}

void action_install_cias(linked_list* items, list_item* selected) {
    action_install_cias_internal(items, selected, "Install all CIAs in the current directory?", false);
}

void action_install_cias_delete(linked_list* items, list_item* selected) {
    action_install_cias_internal(items, selected, "Install and delete all CIAs in the current directory?", true);
}