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

    data_op_data deleteInfo;
} delete_contents_data;

static void action_delete_contents_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    delete_contents_data* deleteData = (delete_contents_data*) data;

    u32 curr = deleteData->deleteInfo.processed;
    if(curr < deleteData->deleteInfo.total) {
        ui_draw_file_info(view, ((list_item*) linked_list_get(&deleteData->contents, curr))->data, x1, y1, x2, y2);
    } else if(deleteData->target != NULL) {
        ui_draw_file_info(view, deleteData->target, x1, y1, x2, y2);
    }
}

static Result action_delete_contents_delete(void* data, u32 index) {
    delete_contents_data* deleteData = (delete_contents_data*) data;

    Result res = 0;

    file_info* info = (file_info*) ((list_item*) linked_list_get(&deleteData->contents, index))->data;

    FS_Path* fsPath = util_make_path_utf8(info->path);
    if(fsPath != NULL) {
        if(util_is_dir(deleteData->target->archive, info->path)) {
            res = FSUSER_DeleteDirectory(deleteData->target->archive, *fsPath);
        } else {
            res = FSUSER_DeleteFile(deleteData->target->archive, *fsPath);
        }

        util_free_path_utf8(fsPath);
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    if(R_SUCCEEDED(res)) {
        deleteData->target->containsCias = false;
        deleteData->target->containsTickets = false;

        linked_list_iter iter;
        linked_list_iterate(deleteData->items, &iter);

        while(linked_list_iter_has_next(&iter)) {
            list_item* item = (list_item*) linked_list_iter_next(&iter);
            file_info* currInfo = (file_info*) item->data;

            if(strncmp(currInfo->path, info->path, FILE_PATH_MAX) == 0) {
                linked_list_iter_remove(&iter);
                task_free_file(item);
            } else if(currInfo->isCia) {
                deleteData->target->containsCias = true;
            } else if(currInfo->isTicket) {
                deleteData->target->containsTickets = true;
            }
        }
    }

    return res;
}

static bool action_delete_contents_error(void* data, u32 index, Result res) {
    delete_contents_data* deleteData = (delete_contents_data*) data;

    if(res == R_FBI_CANCELLED) {
        prompt_display("Failure", "Delete cancelled.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
        return false;
    } else {
        volatile bool dismissed = false;
        error_display_res(&dismissed, data, action_delete_contents_draw_top, res, "Failed to delete content.");

        while(!dismissed) {
            svcSleepThread(1000000);
        }
    }

    return index < deleteData->deleteInfo.total - 1;
}

static void action_delete_contents_free_data(delete_contents_data* data) {
    task_clear_files(&data->contents);
    linked_list_destroy(&data->contents);
    free(data);
}

static void action_delete_contents_update(ui_view* view, void* data, float* progress, char* text) {
    delete_contents_data* deleteData = (delete_contents_data*) data;

    if(deleteData->deleteInfo.finished) {
        FSUSER_ControlArchive(deleteData->target->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);

        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(deleteData->deleteInfo.result)) {
            prompt_display("Success", "Contents deleted.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
        }

        action_delete_contents_free_data(deleteData);

        return;
    }

    if((hidKeysDown() & KEY_B) && !deleteData->deleteInfo.finished) {
        svcSignalEvent(deleteData->deleteInfo.cancelEvent);
    }

    *progress = deleteData->deleteInfo.total > 0 ? (float) deleteData->deleteInfo.processed / (float) deleteData->deleteInfo.total : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%lu / %lu", deleteData->deleteInfo.processed, deleteData->deleteInfo.total);
}

static void action_delete_contents_onresponse(ui_view* view, void* data, bool response) {
    delete_contents_data* deleteData = (delete_contents_data*) data;

    if(response) {
        Result res = task_data_op(&deleteData->deleteInfo);
        if(R_SUCCEEDED(res)) {
            info_display("Deleting Contents", "Press B to cancel.", true, data, action_delete_contents_update, action_delete_contents_draw_top);
        } else {
            error_display_res(NULL, deleteData->target, ui_draw_file_info, res, "Failed to initiate delete operation.");

            action_delete_contents_free_data(deleteData);
        }
    } else {
        action_delete_contents_free_data(deleteData);
    }
}

static void action_delete_contents_internal(linked_list* items, list_item* selected, const char* message, bool recursive, bool includeBase, bool ciasOnly, bool ticketsOnly) {
    delete_contents_data* data = (delete_contents_data*) calloc(1, sizeof(delete_contents_data));
    if(data == NULL) {
        error_display(NULL, NULL, NULL, "Failed to allocate delete contents data.");

        return;
    }

    data->items = items;
    data->target = (file_info*) selected->data;

    data->deleteInfo.data = data;

    data->deleteInfo.op = DATAOP_DELETE;

    data->deleteInfo.delete = action_delete_contents_delete;

    data->deleteInfo.error = action_delete_contents_error;

    data->deleteInfo.finished = false;

    linked_list_init(&data->contents);

    populate_files_data popData;
    popData.items = &data->contents;
    popData.base = data->target;
    popData.recursive = recursive;
    popData.includeBase = includeBase;
    popData.dirsFirst = false;

    Result listRes = task_populate_files(&popData);
    if(R_FAILED(listRes)) {
        error_display_res(NULL, NULL, NULL, listRes, "Failed to initiate content list population.");

        action_delete_contents_free_data(data);
        return;
    }

    while(!popData.finished) {
        svcSleepThread(1000000);
    }

    if(R_FAILED(popData.result)) {
        error_display_res(NULL, NULL, NULL, popData.result, "Failed to populate content list.");

        action_delete_contents_free_data(data);
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(&data->contents, &iter);

    while(linked_list_iter_has_next(&iter)) {
        file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;

        if((ciasOnly && (info->isDirectory || !info->isCia)) || (ticketsOnly && (info->isDirectory || !info->isTicket))) {
            linked_list_iter_remove(&iter);
        }
    }

    data->deleteInfo.total = linked_list_size(&data->contents);
    data->deleteInfo.processed = data->deleteInfo.total;

    prompt_display("Confirmation", message, COLOR_TEXT, true, data, NULL, action_delete_contents_draw_top, action_delete_contents_onresponse);
}

void action_delete_file(linked_list* items, list_item* selected) {
    action_delete_contents_internal(items, selected, "Delete the selected file?", false, true, false, false);
}

void action_delete_dir(linked_list* items, list_item* selected) {
    action_delete_contents_internal(items, selected, "Delete the current directory?", true, true, false, false);
}

void action_delete_dir_contents(linked_list* items, list_item* selected) {
    action_delete_contents_internal(items, selected, "Delete all contents of the current directory?", true, false, false, false);
}

void action_delete_dir_cias(linked_list* items, list_item* selected) {
    action_delete_contents_internal(items, selected, "Delete all CIAs in the current directory?", false, false, true, false);
}

void action_delete_dir_tickets(linked_list* items, list_item* selected) {
    action_delete_contents_internal(items, selected, "Delete all tickets in the current directory?", false, false, false, true);
}