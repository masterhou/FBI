#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "section.h"
#include "task/task.h"
#include "../error.h"
#include "../info.h"
#include "../prompt.h"
#include "../ui.h"
#include "../../core/screen.h"

static Result dumpnand_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result dumpnand_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result dumpnand_open_src(void* data, u32 index, u32* handle) {
    return FSUSER_OpenFileDirectly(handle, ARCHIVE_NAND_W_FS, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_UTF16, u"/"), FS_OPEN_READ, 0);
}

static Result dumpnand_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result dumpnand_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result dumpnand_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result dumpnand_open_dst(void* data, u32 index, void* initialReadBlock, u32* handle) {
    return FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_UTF16, u"/NAND.bin"), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
}

static Result dumpnand_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result dumpnand_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static bool dumpnand_error(void* data, u32 index, Result res) {
    if(res == R_FBI_CANCELLED) {
        prompt_display("Failure", "Dump cancelled.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
    } else {
        error_display_res(NULL, NULL, NULL, res, "Failed to dump NAND.");
    }

    return false;
}

static void dumpnand_update(ui_view* view, void* data, float* progress, char* text) {
    data_op_data* dumpData = (data_op_data*) data;

    if(dumpData->finished) {
        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(dumpData->result)) {
            prompt_display("Success", "NAND dumped.", COLOR_TEXT, false, NULL, NULL, NULL, NULL);
        }

        free(dumpData);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(dumpData->cancelEvent);
    }

    *progress = dumpData->currTotal != 0 ? (float) ((double) dumpData->currProcessed / (double) dumpData->currTotal) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%.2f MiB / %.2f MiB", dumpData->currProcessed / 1024.0f / 1024.0f, dumpData->currTotal / 1024.0f / 1024.0f);
}

static void dumpnand_onresponse(ui_view* view, void* data, bool response) {
    if(response) {
        data_op_data* dumpData = (data_op_data*) data;

        Result res = task_data_op(dumpData);
        if(R_SUCCEEDED(res)) {
            info_display("Dumping NAND", "Press B to cancel.", true, data, dumpnand_update, NULL);
        } else {
            error_display_res(NULL, NULL, NULL, res, "Failed to initiate NAND dump.");
        }
    } else {
        free(data);
    }
}

void dumpnand_open() {
    data_op_data* data = (data_op_data*) calloc(1, sizeof(data_op_data));
    if(data == NULL) {
        error_display(NULL, NULL, NULL, "Failed to allocate dump NAND data.");

        return;
    }

    data->data = data;

    data->op = DATAOP_COPY;

    data->copyEmpty = true;

    data->total = 1;

    data->isSrcDirectory = dumpnand_is_src_directory;
    data->makeDstDirectory = dumpnand_make_dst_directory;

    data->openSrc = dumpnand_open_src;
    data->closeSrc = dumpnand_close_src;
    data->getSrcSize = dumpnand_get_src_size;
    data->readSrc = dumpnand_read_src;

    data->openDst = dumpnand_open_dst;
    data->closeDst = dumpnand_close_dst;
    data->writeDst = dumpnand_write_dst;

    data->error = dumpnand_error;

    data->finished = true;

    prompt_display("Confirmation", "Dump raw NAND image to the SD card?", COLOR_TEXT, true, data, NULL, NULL, dumpnand_onresponse);
}